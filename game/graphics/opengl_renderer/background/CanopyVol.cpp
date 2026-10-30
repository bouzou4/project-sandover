#include "CanopyVol.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "common/log/log.h"
#include "common/util/FileUtil.h"

#include "game/graphics/gfx.h"
#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/opengl_renderer/GlQueries.h"
#include "game/graphics/opengl_renderer/LevelHeightfield.h"
#include "game/graphics/opengl_renderer/loader/Loader.h"

namespace {
constexpr float kMeter = 4096.f;
constexpr int kBit = PERF_OPT_GEOMETRY + 2;  // cached uniform lookups
constexpr int kMaxMaps = 2;
constexpr u32 kVersion = 3;
constexpr u32 kFlagSky = 1;
std::atomic<u64> g_serial{1};

fs::path vol_dir() {
  if (const char* p = std::getenv("SANDOVER_CANOPY_VOL")) {
    return fs::path(p);
  }
  if (const char* d = std::getenv("SANDOVER_DATA")) {
    return fs::path(d) / "lighting" / "canopy";
  }
  return file_util::get_jak_project_dir() / "sandover" / "data" / "lighting" / "canopy";
}

struct Frame {
  GLuint tex = 0;
  int w = 0, h = 0;  // allocated cells per layer
  const CanopyVolMap* slot[kMaxMaps] = {nullptr, nullptr};
  u64 serial[kMaxMaps] = {0, 0};
  int n = 0;
  float rect[kMaxMaps][4] = {};  // x0, z0 (game units), cells per game unit, -
  float hgt[kMaxMaps][2] = {};   // y0, span (game units)
  int size[kMaxMaps][2] = {};    // cells of the map
};
Frame g_frame;

bool wanted() {
  const auto& gs = Gfx::g_global_settings;
  return gs.pbr_transmit_occl && gs.pbr_enable && gs.pbr_background;
}

GLint begin_unit() {
  GLint active = GL_TEXTURE0;  // the renderer's convention between passes
  if (!Gfx::perf_opt(kBit)) {
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
  }
  glActiveTexture(GL_TEXTURE0 + kCanopyVolUnit);
  return active;
}

std::pair<float, float> camera_key(float x, float z, const CanopyVolMap& m) {
  const float x1 = m.x0 + m.texel * m.width, z1 = m.z0 + m.texel * m.height;
  const float dx = std::max({m.x0 - x, 0.f, x - x1});
  const float dz = std::max({m.z0 - z, 0.f, z - z1});
  const float cx = 0.5f * (m.x0 + x1) - x, cz = 0.5f * (m.z0 + z1) - z;
  return {std::sqrt(dx * dx + dz * dz), std::sqrt(cx * cx + cz * cz)};
}

void upload_layer(const Frame& f, int layer, const CanopyVolMap* m) {
  std::vector<u16> pad((size_t)f.w * f.h * 4, 0);
  if (m) {
    for (u32 y = 0; y < m->ch; y++) {
      memcpy(&pad[(size_t)y * f.w * 4], &m->cells[(size_t)y * m->cw * 4], (size_t)m->cw * 8);
    }
  }
  glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, layer, f.w, f.h, 1, GL_RGBA, GL_UNSIGNED_SHORT,
                  pad.data());
}
}  // namespace

void canopy_vol_cells(const u16* d, u32 w, u32 h, CanopyVolMap* m) {
  // texel i lies in cell i >> 1; channels g, b, a of the file (lowest, highest, volume)
  m->cw = (w + 1) / 2;
  m->ch = (h + 1) / 2;
  m->cells.assign((size_t)m->cw * m->ch * 4, 0);
  for (u32 cy = 0; cy < m->ch; cy++) {
    for (u32 cx = 0; cx < m->cw; cx++) {
      u32 sum[3] = {0, 0, 0}, k = 0;
      for (u32 y = 2 * cy; y < std::min(2 * cy + 2, h); y++) {
        for (u32 x = 2 * cx; x < std::min(2 * cx + 2, w); x++) {
          const u16* t = &d[((size_t)y * w + x) * 4];
          sum[0] += t[1];
          sum[1] += t[2];
          sum[2] += t[3];
          k++;
        }
      }
      u16* c = &m->cells[((size_t)cy * m->cw + cx) * 4];
      for (int i = 0; i < 3; i++) {
        c[i] = (u16)((sum[i] + k / 2) / k);
      }
    }
  }
}

std::unique_ptr<CanopyVolMap> canopy_vol_load(const std::string& level_name) {
  const auto path = vol_dir() / (level_name + ".canopy");
  if (!fs::exists(path)) {
    return nullptr;
  }
  try {
    const auto bytes = file_util::read_binary_file(path);
    struct {
      char magic[4];
      u32 version, w, h;
      float x0, z0, texel, y0, y1;
      u32 flags, pad[2];
    } hdr;
    static_assert(sizeof(hdr) == 48);
    if (bytes.size() < sizeof(hdr)) {
      throw std::runtime_error("short file");
    }
    memcpy(&hdr, bytes.data(), sizeof(hdr));
    if (memcmp(hdr.magic, "SOVC", 4) != 0 || hdr.version != kVersion) {
      throw std::runtime_error("not a canopy volume map v3");
    }
    const size_t n = (size_t)hdr.w * hdr.h * 4;
    if (hdr.w == 0 || hdr.h == 0 || hdr.w > 8192 || hdr.h > 8192 ||
        bytes.size() != sizeof(hdr) + n * 2) {
      throw std::runtime_error("bad size");
    }
    if (!(hdr.texel > 0.f) || !(hdr.y1 > hdr.y0) || !std::isfinite(hdr.x0) ||
        !std::isfinite(hdr.z0)) {
      throw std::runtime_error("bad header values");
    }
    std::vector<u16> d(n);
    memcpy(d.data(), bytes.data() + sizeof(hdr), n * 2);
    auto m = std::make_unique<CanopyVolMap>();
    m->width = hdr.w;
    m->height = hdr.h;
    m->x0 = hdr.x0;
    m->z0 = hdr.z0;
    m->texel = hdr.texel;
    m->y0 = hdr.y0;
    m->y1 = hdr.y1;
    m->sky = (hdr.flags & kFlagSky) != 0;
    canopy_vol_cells(d.data(), hdr.w, hdr.h, m.get());
    m->serial = g_serial++;
    lg::info("canopy volume: {} {}x{} cells of {:.2f} m, {:.1f} MB", level_name, m->cw, m->ch,
             2 * hdr.texel, m->cells.size() * 2 / 1e6);
    return m;
  } catch (const std::exception& e) {
    lg::warn("canopy volume: {} unreadable, {}", path.string(), e.what());
    return nullptr;
  }
}

void canopy_vol_prepare(SharedRenderState* rs) {
  auto& f = g_frame;
  std::vector<std::pair<std::pair<float, float>, const CanopyVolMap*>> cand;
  if (wanted() && rs->loader && rs->version == GameVersion::Jak1) {
    const float cx = rs->camera_pos.x() / kMeter, cz = rs->camera_pos.z() / kMeter;
    for (auto* lev : rs->loader->get_in_use_levels()) {
      if (lev->canopy_vol) {
        cand.push_back({camera_key(cx, cz, *lev->canopy_vol), lev->canopy_vol.get()});
      }
    }
  }
  std::sort(cand.begin(), cand.end(), [](const auto& a, const auto& b) {
    return a.first != b.first ? a.first < b.first : a.second->serial < b.second->serial;
  });
  const CanopyVolMap* want[kMaxMaps] = {nullptr, nullptr};
  for (size_t i = 0; i < cand.size() && (int)i < kMaxMaps; i++) {
    want[i] = cand[i].second;
  }
  // stable slots: a chosen map already in a slot keeps it
  const CanopyVolMap* next[kMaxMaps] = {nullptr, nullptr};
  bool placed[kMaxMaps] = {false, false};
  for (int s = 0; s < kMaxMaps; s++) {
    for (int i = 0; i < kMaxMaps; i++) {
      if (want[i] && !placed[i] && f.slot[s] == want[i] && f.serial[s] == want[i]->serial) {
        next[s] = want[i];
        placed[i] = true;
      }
    }
  }
  for (int i = 0; i < kMaxMaps; i++) {
    for (int s = 0; s < kMaxMaps && want[i] && !placed[i]; s++) {
      if (!next[s]) {
        next[s] = want[i];
        placed[i] = true;
      }
    }
  }
  int need_w = 1, need_h = 1;
  for (auto* m : next) {
    if (m) {
      need_w = std::max(need_w, (int)m->cw);
      need_h = std::max(need_h, (int)m->ch);
    }
  }
  const bool any = next[0] || next[1];
  if (!any && f.tex) {
    glDeleteTextures(1, &f.tex);
    f.tex = 0;
    f.w = f.h = 0;
  } else if (any && (!f.tex || need_w > f.w || need_h > f.h)) {
    if (!f.tex) {
      glGenTextures(1, &f.tex);
    }
    f.w = std::max(need_w, f.w);
    f.h = std::max(need_h, f.h);
    const GLint active = begin_unit();
    glBindTexture(GL_TEXTURE_2D_ARRAY, f.tex);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA16, f.w, f.h, kMaxMaps, 0, GL_RGBA,
                 GL_UNSIGNED_SHORT, nullptr);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
    for (int s = 0; s < kMaxMaps; s++) {
      upload_layer(f, s, next[s]);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    glActiveTexture(active);
  } else if (f.tex) {
    bool bound = false;
    GLint active = GL_TEXTURE0;
    for (int s = 0; s < kMaxMaps; s++) {
      if (next[s] == f.slot[s] && (next[s] ? next[s]->serial : 0) == f.serial[s]) {
        continue;
      }
      if (!bound) {
        active = begin_unit();
        glBindTexture(GL_TEXTURE_2D_ARRAY, f.tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
        bound = true;
      }
      upload_layer(f, s, next[s]);
    }
    if (bound) {
      glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
      glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
      glActiveTexture(active);
    }
  }
  f.n = 0;
  for (int s = 0; s < kMaxMaps; s++) {
    f.slot[s] = next[s];
    f.serial[s] = next[s] ? next[s]->serial : 0;
    f.rect[s][0] = f.rect[s][1] = f.rect[s][2] = f.rect[s][3] = 0.f;
    f.hgt[s][0] = f.hgt[s][1] = 0.f;
    f.size[s][0] = f.size[s][1] = 0;
    if (next[s]) {
      const auto* m = next[s];
      f.n = s + 1;
      f.rect[s][0] = m->x0 * kMeter;
      f.rect[s][1] = m->z0 * kMeter;
      f.rect[s][2] = 1.f / (2.f * m->texel * kMeter);
      f.hgt[s][0] = m->y0 * kMeter;
      f.hgt[s][1] = (m->y1 - m->y0) * kMeter;
      f.size[s][0] = (int)m->cw;
      f.size[s][1] = (int)m->ch;
    }
  }
}

void canopy_set_backlight_uniforms(GLuint program) {
  const auto& f = g_frame;
  const auto& gs = Gfx::g_global_settings;
  const bool on = gs.pbr_transmit_occl;
  const int n = on && f.tex ? f.n : 0;
  // levels without a volume map (no tree crowns) skip both tests: no map, no cost
  const int solid = n > 0 && heightfield_bound() > 0 ? 1 : 0;
  glUniform1i(GLQ_LOC(kBit, program, "pbr_bl_n"), n);
  glUniform1i(GLQ_LOC(kBit, program, "pbr_bl_solid"), solid);
  if (n == 0) {
    return;
  }
  // leaf optical depth scale, march step (m), solid margin (m)
  glUniform4f(GLQ_LOC(kBit, program, "pbr_bl_par"), gs.pbr_transmit_density, 1.5f, 1.0f, 0.f);
  if (solid) {
    heightfield_set_uniforms(program);
  }
  glUniform4fv(GLQ_LOC(kBit, program, "pbr_bl_rect"), kMaxMaps, &f.rect[0][0]);
  glUniform2fv(GLQ_LOC(kBit, program, "pbr_bl_h"), kMaxMaps, &f.hgt[0][0]);
  glUniform2iv(GLQ_LOC(kBit, program, "pbr_bl_size"), kMaxMaps, &f.size[0][0]);
  const GLint active = begin_unit();
  glBindTexture(GL_TEXTURE_2D_ARRAY, f.tex);
  glActiveTexture(active);
}
