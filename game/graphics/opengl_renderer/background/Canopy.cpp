#include "Canopy.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "common/log/log.h"
#include "common/util/FileUtil.h"

#include "game/graphics/gfx.h"
#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/opengl_renderer/EmissiveTable.h"
#include "game/graphics/opengl_renderer/GlQueries.h"
#include "game/graphics/opengl_renderer/loader/Loader.h"

namespace {
constexpr float kMeter = 4096.f;
constexpr int kCanopyUnit = 30;  // scratchpad ALLOCATIONS: 30, explicit sampler uniform
constexpr int kBit = PERF_OPT_GEOMETRY + 2;  // cached uniform lookups, as the shadow receivers
constexpr int kMaxMaps = 2;
constexpr u32 kVersion = 2;
std::atomic<u64> g_serial{1};

fs::path canopy_dir() {
  if (const char* p = std::getenv("SANDOVER_CANOPY")) {
    return fs::path(p);
  }
  if (const char* d = std::getenv("SANDOVER_DATA")) {
    return fs::path(d) / "lighting";
  }
  return file_util::get_jak_project_dir() / "sandover" / "data" / "lighting";
}

// render-thread state for this frame
struct Frame {
  GLuint tex = 0;
  int layer_w = 0, layer_h = 0;
  const CanopyMap* slot[kMaxMaps] = {nullptr, nullptr};  // stable layer slots
  u64 serial[kMaxMaps] = {0, 0};
  int n = 0;  // highest filled slot + 1
  float rect[kMaxMaps][4] = {};  // x0, z0 (game units), uv per game unit in x, z
  float hgt[kMaxMaps][2] = {};   // y0, span (game units)
  float sun[4] = {0, 1, 0, 0};   // toward the sun; w: strength x elevation fade
  float c2w[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};
Frame g_frame;

GLint begin_unit() {
  GLint active = GL_TEXTURE0;  // the renderer's convention between passes
  if (!Gfx::perf_opt(kBit)) {
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
  }
  glActiveTexture(GL_TEXTURE0 + kCanopyUnit);
  return active;
}

// the map the camera is over first (distance to its extent), then centre distance
std::pair<float, float> camera_key(float x, float z, const CanopyMap& m) {
  const float x1 = m.x0 + m.texel * m.width, z1 = m.z0 + m.texel * m.height;
  const float dx = std::max({m.x0 - x, 0.f, x - x1});
  const float dz = std::max({m.z0 - z, 0.f, z - z1});
  const float cx = 0.5f * (m.x0 + x1) - x, cz = 0.5f * (m.z0 + z1) - z;
  return {std::sqrt(dx * dx + dz * dz), std::sqrt(cx * cx + cz * cz)};
}

void write_layer(int layer, const CanopyMap* m, int lw, int lh) {
  std::vector<u16> zero((size_t)lw * lh * 2, 0);
  glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, layer, lw, lh, 1, GL_RG, GL_UNSIGNED_SHORT,
                  zero.data());
  if (m) {
    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, layer, m->width, m->height, 1, GL_RG,
                    GL_UNSIGNED_SHORT, m->data.data());
  }
}

float smoothstep(float a, float b, float x) {
  const float t = std::clamp((x - a) / (b - a), 0.f, 1.f);
  return t * t * (3.f - 2.f * t);
}
}  // namespace

std::unique_ptr<CanopyMap> canopy_load(const std::string& level_name) {
  const auto path = canopy_dir() / (level_name + ".canopy");
  if (!fs::exists(path)) {
    return nullptr;
  }
  try {
    const auto bytes = file_util::read_binary_file(path);
    struct {
      char magic[4];
      u32 version, w, h;
      float x0, z0, texel, y0, y1;
      u32 pad[3];
    } hdr;
    static_assert(sizeof(hdr) == 48);
    if (bytes.size() < sizeof(hdr)) {
      throw std::runtime_error("short file");
    }
    memcpy(&hdr, bytes.data(), sizeof(hdr));
    if (memcmp(hdr.magic, "SOVC", 4) != 0 || hdr.version != kVersion) {
      throw std::runtime_error("not a canopy map v2");
    }
    const size_t n = (size_t)hdr.w * hdr.h * 2;
    if (hdr.w == 0 || hdr.h == 0 || hdr.w > 8192 || hdr.h > 8192 ||
        bytes.size() != sizeof(hdr) + n * 2) {
      throw std::runtime_error("bad size");
    }
    if (!(hdr.texel > 0.f) || !(hdr.y1 > hdr.y0) || !std::isfinite(hdr.x0) ||
        !std::isfinite(hdr.z0)) {
      throw std::runtime_error("bad header values");
    }
    auto m = std::make_unique<CanopyMap>();
    m->width = hdr.w;
    m->height = hdr.h;
    m->x0 = hdr.x0;
    m->z0 = hdr.z0;
    m->texel = hdr.texel;
    m->y0 = hdr.y0;
    m->y1 = hdr.y1;
    m->data.resize(n);
    memcpy(m->data.data(), bytes.data() + sizeof(hdr), n * 2);
    m->serial = g_serial++;
    lg::info("canopy: {} {}x{} texels of {:.2f} m", level_name, hdr.w, hdr.h, hdr.texel);
    return m;
  } catch (const std::exception& e) {
    lg::warn("canopy: {} unreadable, {}", path.string(), e.what());
    return nullptr;
  }
}

void canopy_prepass(SharedRenderState* rs,
                    const float sun_dir[3],
                    const float c2w[16],
                    float day,
                    float sun_gain) {
  auto& f = g_frame;
  const auto& gs = Gfx::g_global_settings;
  memcpy(f.c2w, c2w, sizeof(f.c2w));
  float len = std::sqrt(sun_dir[0] * sun_dir[0] + sun_dir[1] * sun_dir[1] + sun_dir[2] * sun_dir[2]);
  len = std::max(len, 1e-6f);
  for (int i = 0; i < 3; i++) {
    f.sun[i] = sun_dir[i] / len;
  }
  // only by day (palette day weight), faded out with a low sun where the projection runs far
  f.sun[3] =
      gs.shadow_canopy * smoothstep(0.1f, 0.25f, f.sun[1]) * smoothstep(0.f, 0.5f, day) * sun_gain;
  std::vector<std::pair<std::pair<float, float>, const CanopyMap*>> cand;
  if (rs->loader && gs.shadow_canopy > 0.f) {
    const float cx = c2w[12] / kMeter, cz = c2w[14] / kMeter;
    for (auto* lev : rs->loader->get_in_use_levels()) {
      if (lev->canopy) {
        cand.push_back({camera_key(cx, cz, *lev->canopy), lev->canopy.get()});
      }
    }
  }
  std::sort(cand.begin(), cand.end(), [](const auto& a, const auto& b) {
    return a.first != b.first ? a.first < b.first : a.second->serial < b.second->serial;
  });
  const CanopyMap* want[kMaxMaps] = {nullptr, nullptr};
  for (size_t i = 0; i < cand.size() && (int)i < kMaxMaps; i++) {
    want[i] = cand[i].second;
  }
  // stable slots: a chosen map already in a slot keeps it
  const CanopyMap* next[kMaxMaps] = {nullptr, nullptr};
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
  int lw = 1, lh = 1;
  for (auto* m : next) {
    if (m) {
      lw = std::max(lw, (int)m->width);
      lh = std::max(lh, (int)m->height);
    }
  }
  const bool grow = (next[0] || next[1]) && (!f.tex || lw > f.layer_w || lh > f.layer_h);
  bool changed = false;
  for (int s = 0; s < kMaxMaps; s++) {
    changed |= next[s] != f.slot[s] || (next[s] ? next[s]->serial : 0) != f.serial[s];
  }
  if (grow || (changed && f.tex)) {
    if (!f.tex) {
      glGenTextures(1, &f.tex);
    }
    const GLint active = begin_unit();
    glBindTexture(GL_TEXTURE_2D_ARRAY, f.tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
    if (grow) {
      f.layer_w = std::max(lw, f.layer_w);
      f.layer_h = std::max(lh, f.layer_h);
      glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RG16, f.layer_w, f.layer_h, kMaxMaps, 0, GL_RG,
                   GL_UNSIGNED_SHORT, nullptr);
      for (int s = 0; s < kMaxMaps; s++) {
        write_layer(s, next[s], f.layer_w, f.layer_h);
      }
    } else {
      // only the layers whose map changed
      for (int s = 0; s < kMaxMaps; s++) {
        if (next[s] != f.slot[s] || (next[s] ? next[s]->serial : 0) != f.serial[s]) {
          write_layer(s, next[s], f.layer_w, f.layer_h);
        }
      }
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    glActiveTexture(active);
  }
  f.n = 0;
  for (int s = 0; s < kMaxMaps; s++) {
    f.slot[s] = next[s];
    f.serial[s] = next[s] ? next[s]->serial : 0;
    f.rect[s][0] = f.rect[s][1] = f.rect[s][2] = f.rect[s][3] = 0.f;
    f.hgt[s][0] = f.hgt[s][1] = 0.f;
    if (next[s]) {
      const auto* m = next[s];
      f.n = s + 1;
      f.rect[s][0] = m->x0 * kMeter;
      f.rect[s][1] = m->z0 * kMeter;
      f.rect[s][2] = 1.f / (m->texel * kMeter * f.layer_w);
      f.rect[s][3] = 1.f / (m->texel * kMeter * f.layer_h);
      f.hgt[s][0] = m->y0 * kMeter;
      f.hgt[s][1] = (m->y1 - m->y0) * kMeter;
    }
  }
}

void canopy_set_uniforms(GLuint program, bool camera_space) {
  const auto& f = g_frame;
  const auto& gs = Gfx::g_global_settings;
  const int n = gs.shadow_canopy > 0.f && f.sun[3] > 0.f && f.tex ? f.n : 0;
  glUniform1i(GLQ_LOC(kBit, program, "shadow_canopy_n"), n);
  if (n == 0) {
    return;
  }
  static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  glUniformMatrix4fv(GLQ_LOC(kBit, program, "shadow_canopy_mtx"), 1, GL_FALSE,
                     camera_space ? f.c2w : kIdentity);
  glUniform4fv(GLQ_LOC(kBit, program, "shadow_canopy_rect"), kMaxMaps, &f.rect[0][0]);
  glUniform2fv(GLQ_LOC(kBit, program, "shadow_canopy_h"), kMaxMaps, &f.hgt[0][0]);
  glUniform4fv(GLQ_LOC(kBit, program, "shadow_canopy_sun"), 1, f.sun);
  // sun patches: gap transmittance ramp from 0.15 over a width set by the edge sharpness
  const float width = 0.35f + (0.08f - 0.35f) * gs.shadow_canopy_sharp;
  glUniform4f(GLQ_LOC(kBit, program, "shadow_canopy_par"), 0.15f, width,
              gs.shadow_canopy_penumbra, gs.shadow_canopy_patch);
  // light kept in the shade under full cover, shifted toward blue (less red, a little more blue)
  const float t = gs.shadow_canopy_tint, keep = 1.f - gs.shadow_canopy_ambient;
  glUniform4f(GLQ_LOC(kBit, program, "shadow_canopy_amb"), keep * (1.f - 0.15f * t),
              keep * (1.f - 0.05f * t), keep * (1.f + 0.10f * t), 0.f);
  const GLint active = begin_unit();
  glBindTexture(GL_TEXTURE_2D_ARRAY, f.tex);
  glActiveTexture(active);
}

void canopy_disable(GLuint program) {
  glUniform1i(GLQ_LOC(kBit, program, "shadow_canopy_n"), 0);
}

void canopy_off() {
  g_frame.sun[3] = 0.f;
}

bool canopy_keeps_light(const DrawMode& mode, s64 combo) {
  if (mode.get_ab_enable()) {
    switch (mode.get_alpha_blend()) {
      case DrawMode::AlphaBlend::SRC_0_SRC_DST:
      case DrawMode::AlphaBlend::SRC_0_FIX_DST:
      case DrawMode::AlphaBlend::SRC_0_DST_DST:
      case DrawMode::AlphaBlend::ZERO_SRC_SRC_DST:
        return true;
      default:
        break;
    }
  }
  return combo >= 0 && emissive_table::find((u32)combo) != nullptr;
}

void canopy_set_emitter(GLuint program, bool on, int* last) {
  if ((int)on != *last) {
    *last = on ? 1 : 0;
    glUniform1i(GLQ_LOC(kBit, program, "canopy_emitter"), *last);
  }
}

void canopy_set_instance(GLuint program, const float world[16]) {
  glUniformMatrix4fv(GLQ_LOC(kBit, program, "canopy_inst"), 1, GL_FALSE, world);
}
