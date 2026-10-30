#include "LavaField.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <unordered_map>

#include "common/custom_data/Tfrag3Data.h"
#include "common/log/log.h"

#include "game/graphics/gfx.h"
#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/opengl_renderer/GlQueries.h"
#include "game/graphics/opengl_renderer/Materials.h"
#include "game/graphics/opengl_renderer/loader/Loader.h"

namespace lava_field {
namespace {

const Volume kVolumes[] = {
#include "LavaVolumes.inc"
};

constexpr float kMeter = 4096.f;
constexpr float kInf = std::numeric_limits<float>::infinity();
constexpr int kPatLava = 12;  // pat-material lava (Jak 1)
constexpr int kPatHotcoals = 11;
constexpr float kUpNy = 0.5f;
constexpr int kFieldUnit = 41;  // kb/facts/renderer.md texture units: 41-42
constexpr int kNearUnit = 42;
// the toon tonemap clips per channel from a max channel of 1.2 (tonemap_halo_start): lava light
// and molten bodies stay under this so they keep their hue
constexpr float kWhiteCap = 1.15f;
constexpr float kGlowCap = kWhiteCap;  // molten bodies too; cores pass it (lava_glow_mix)
constexpr float kOpenSky = 100.f;  // roof of an open texel, metres over its nearest lava (linear
                                    // filtering blends roof heights: no huge values)
constexpr float kTexelM = 1.f;
constexpr float kMarginM = 64.f;
constexpr int kMaxDim = 1024;

/// Texel range [lo, hi) whose centres can fall inside [a, b] (metres).
void span(float a, float b, float origin, float texel, int n, int* lo, int* hi) {
  *lo = std::max(0, (int)std::floor((a - origin) / texel - 0.5f));
  *hi = std::min(n, (int)std::ceil((b - origin) / texel + 0.5f));
}

/// Calls f(index, y at the texel centre) for every texel whose centre lies in the triangle's xz
/// projection, and for the texels under its vertices (thin triangles).
template <typename F>
void raster(const Tri& t, float x0, float z0, float texel, int w, int h, F&& f) {
  const float* a = t.v[0];
  const float* b = t.v[1];
  const float* c = t.v[2];
  const float det = (b[0] - a[0]) * (c[2] - a[2]) - (c[0] - a[0]) * (b[2] - a[2]);
  for (int k = 0; k < 3; k++) {
    const int i = (int)std::floor((t.v[k][0] - x0) / texel);
    const int j = (int)std::floor((t.v[k][2] - z0) / texel);
    if (i >= 0 && i < w && j >= 0 && j < h) {
      f((size_t)j * w + i, t.v[k][1]);
    }
  }
  if (std::abs(det) < 1e-9f) {
    return;
  }
  int i0, i1, j0, j1;
  span(std::min({a[0], b[0], c[0]}), std::max({a[0], b[0], c[0]}), x0, texel, w, &i0, &i1);
  span(std::min({a[2], b[2], c[2]}), std::max({a[2], b[2], c[2]}), z0, texel, h, &j0, &j1);
  for (int j = j0; j < j1; j++) {
    const float pz = z0 + (j + 0.5f) * texel;
    for (int i = i0; i < i1; i++) {
      const float px = x0 + (i + 0.5f) * texel;
      const float u = ((px - a[0]) * (c[2] - a[2]) - (c[0] - a[0]) * (pz - a[2])) / det;
      const float v = ((b[0] - a[0]) * (pz - a[2]) - (px - a[0]) * (b[2] - a[2])) / det;
      if (u < -1e-4f || v < -1e-4f || u + v > 1.0001f) {
        continue;
      }
      f((size_t)j * w + i, a[1] + u * (b[1] - a[1]) + v * (c[1] - a[1]));
    }
  }
}

struct Live {
  float level = 0.f;   // dir2 level (fades and clock), 0 while it does not point down
  float clock = 1.f;   // the lava clock's factor in it
  float rgb[3] = {0.f, 0.f, 0.f};
  bool valid = false;  // at least one report
  bool vanilla = false;  // vanilla lights the characters with it
};

std::mutex g_mutex;
std::unordered_map<std::string, Live> g_live;

struct Entry {
  const void* lev = nullptr;
  u64 load_id = UINT64_MAX;
  GLuint tex = 0;
  GLuint near_tex = 0;
  float x0 = 0.f, z0 = 0.f, ex = 1.f, ez = 1.f, texel_m = 1.f, y0 = 0.f;
  float share = 0.f;
  int mips = 1;
  u64 last_used = 0;  // renderer frame
};
std::unordered_map<std::string, Entry> g_fields;
bool g_used = false;

LevelData* find_level(SharedRenderState* rs, const std::string& name) {
  if (!rs->loader) {
    return nullptr;
  }
  for (auto* lev : rs->loader->get_in_use_levels()) {
    if (lev && lev->level && lev->level->level_name == name) {
      return lev;
    }
  }
  return nullptr;
}

void drop_unloaded(SharedRenderState* rs) {
  for (auto it = g_fields.begin(); it != g_fields.end();) {
    LevelData* lev = find_level(rs, it->first);
    const bool same = lev && lev == it->second.lev && lev->load_id == it->second.load_id;
    if (!keep_entry(lev != nullptr, same, it->second.last_used, rs->frame_idx)) {
      glDeleteTextures(1, &it->second.tex);
      glDeleteTextures(1, &it->second.near_tex);
      it = g_fields.erase(it);
    } else {
      ++it;
    }
  }
}

GLuint upload(GLenum ifmt, GLenum fmt, int w, int h, const float* data, bool mips) {
  GLuint t = 0;
  glGenTextures(1, &t);
  glBindTexture(GL_TEXTURE_2D, t);
  glTexImage2D(GL_TEXTURE_2D, 0, ifmt, w, h, 0, fmt, GL_FLOAT, data);
  if (mips) {
    glGenerateMipmap(GL_TEXTURE_2D);
  }
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mips ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  if (!mips) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
  }
  return t;
}

/// The field of a loaded level, uploaded from its loader-thread build on first use; null when
/// the level is not loaded or has no lava.
const Entry* ensure(SharedRenderState* rs, const std::string& name) {
  auto it = g_fields.find(name);
  LevelData* lev = find_level(rs, name);
  if (it != g_fields.end() && lev && it->second.lev == lev && it->second.load_id == lev->load_id) {
    it->second.last_used = rs->frame_idx;
    return it->second.tex ? &it->second : nullptr;
  }
  drop_unloaded(rs);
  if (!lev) {
    return nullptr;
  }
  Entry& e = g_fields[name];
  glDeleteTextures(1, &e.tex);
  glDeleteTextures(1, &e.near_tex);
  e.tex = e.near_tex = 0;
  e.lev = lev;
  e.load_id = lev->load_id;
  e.last_used = rs->frame_idx;
  const auto* p = lev->lava.get();
  if (!p || p->field.empty() || p->field.cov.empty()) {
    return nullptr;
  }
  const auto& f = p->field;
  const std::vector<float> rgb = near_texels(*p);
  e.tex = upload(GL_R16F, GL_RED, f.w, f.h, f.cov.data(), true);
  e.near_tex = upload(GL_RGB16F, GL_RGB, f.w, f.h, rgb.data(), false);
  glBindTexture(GL_TEXTURE_2D, 0);
  e.x0 = f.x0;
  e.z0 = f.z0;
  e.ex = f.w * f.texel_m;
  e.ez = f.h * f.texel_m;
  e.texel_m = f.texel_m;
  e.y0 = f.y0;
  e.share = p->share;
  e.mips = 1 + (int)std::floor(std::log2((float)std::max(f.w, f.h)));
  lg::info("lava field {}: {}x{} at {:.2f} m, collision lava {:.0f} m2, volumes {:.0f} m2, roofed "
           "{:.0f} m2, bake lava share {:.3f}",
           name, f.w, f.h, f.texel_m, f.collide_m2, f.volume_m2, f.roofed_m2, p->share);
  return &e;
}

/// The lava light of `level` for shading: the last report with a light (a report of 0 means the
/// mood lights nothing from below now: none); pinned, the clock's factor is held at its mean.
bool shading_light(const std::string& level, float rgb[3], float* clock, bool* vanilla = nullptr) {
  std::lock_guard<std::mutex> lk(g_mutex);
  auto it = g_live.find(level);
  if (it == g_live.end() || !it->second.valid || it->second.level <= 0.f) {
    return false;
  }
  const auto& l = it->second;
  if (vanilla) {
    *vanilla = l.vanilla;
  }
  const float k = Gfx::g_global_settings.lava_pin ? 0.875f / std::max(l.clock, 0.5f) : 1.f;
  for (int i = 0; i < 3; i++) {
    rgb[i] = l.rgb[i] * l.level * k;
  }
  *clock = Gfx::g_global_settings.lava_pin ? 1.f : std::clamp(l.clock / 0.875f, 0.85f, 1.15f);
  return true;
}

/// Field, light and glow uniforms of one lava level on an active program; returns lava_on.
int lava_uniforms(GLuint program, const Entry* e, const float rgb[3], float clock, bool glow,
                  bool under) {
  constexpr int kBit = PERF_OPT_GEOMETRY + 2;
  const auto& gs = Gfx::g_global_settings;
  int flags = glow ? 1 : 0;
  if (under && e) {
    flags |= 2 | 4;
    glActiveTexture(GL_TEXTURE0 + kFieldUnit);
    glBindTexture(GL_TEXTURE_2D, e->tex);
    glActiveTexture(GL_TEXTURE0 + kNearUnit);
    glBindTexture(GL_TEXTURE_2D, e->near_tex);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(GLQ_LOC(kBit, program, "lava_tex"), kFieldUnit);
    glUniform1i(GLQ_LOC(kBit, program, "lava_near"), kNearUnit);
    glUniform4f(GLQ_LOC(kBit, program, "lava_box"), e->x0, e->z0, 1.f / e->ex, 1.f / e->ez);
    glUniform4f(GLQ_LOC(kBit, program, "lava_fld"), e->texel_m, e->y0,
                std::max(gs.lava_under_reach_m, 1.f), (float)(e->mips - 1));
    const float k = gs.lava_under_gain;
    glUniform4f(GLQ_LOC(kBit, program, "lava_light"), rgb[0] * k, rgb[1] * k, rgb[2] * k,
                gs.lava_under_refl);
    glUniform3f(GLQ_LOC(kBit, program, "lava_under_k"),
                std::clamp(gs.lava_under_bake * e->share, 0.f, 1.f), kMarginM, kWhiteCap);
  }
  glUniform1i(GLQ_LOC(kBit, program, "lava_on"), flags);
  if (glow) {
    const float lo = gs.lava_glow_key_lo, hi = std::max(gs.lava_glow_key_hi, lo + 0.01f);
    glUniform4f(GLQ_LOC(kBit, program, "lava_glow"), lo, hi, gs.lava_glow_gain * clock,
                gs.lava_glow_mask);
    glUniform3f(GLQ_LOC(kBit, program, "lava_glow2"), gs.lava_glow_core * clock,
                gs.lava_glow_crust, kGlowCap);
  }
  glUniform1i(GLQ_LOC(kBit, program, "lava_view"), gs.lava_view);
  return flags;
}

}  // namespace

std::vector<Volume> volumes_of(const std::string& level) {
  std::vector<Volume> out;
  for (const auto& v : kVolumes) {
    if (level == v.level) {
      out.push_back(v);
    }
  }
  return out;
}

Field build(const std::vector<Tri>& lava,
            const std::vector<Tri>& ground,
            const std::vector<Tri>& cover,
            const std::vector<Volume>& volumes,
            float texel_m,
            float margin_m,
            int max_dim) {
  Field f;
  float lo[2] = {kInf, kInf}, hi[2] = {-kInf, -kInf};
  for (const auto& t : lava) {
    for (const auto& v : t.v) {
      lo[0] = std::min(lo[0], v[0]);
      hi[0] = std::max(hi[0], v[0]);
      lo[1] = std::min(lo[1], v[2]);
      hi[1] = std::max(hi[1], v[2]);
    }
  }
  for (const auto& v : volumes) {
    lo[0] = std::min(lo[0], v.x0);
    hi[0] = std::max(hi[0], v.x1);
    lo[1] = std::min(lo[1], v.z0);
    hi[1] = std::max(hi[1], v.z1);
  }
  if (!(hi[0] >= lo[0] && hi[1] >= lo[1])) {
    return f;
  }
  const float ex = hi[0] - lo[0] + 2 * margin_m, ez = hi[1] - lo[1] + 2 * margin_m;
  f.texel_m = std::max(texel_m, std::max(ex, ez) / (float)max_dim);
  f.x0 = lo[0] - margin_m;
  f.z0 = lo[1] - margin_m;
  f.w = std::max(1, (int)std::ceil(ex / f.texel_m));
  f.h = std::max(1, (int)std::ceil(ez / f.texel_m));
  const size_t n = (size_t)f.w * f.h;
  std::vector<float> top(n, -kInf), bot(n, kInf), wt(n, 0.f);
  for (const auto& t : lava) {
    raster(t, f.x0, f.z0, f.texel_m, f.w, f.h, [&](size_t k, float y) {
      top[k] = std::max(top[k], y);
      bot[k] = std::min(bot[k], y);
      wt[k] = std::max(wt[k], t.w);
    });
  }
  const float a = f.texel_m * f.texel_m;
  for (size_t k = 0; k < n; k++) {
    f.collide_m2 += top[k] > -kInf ? a : 0.f;
    f.collide_spread_m2 += top[k] - bot[k] > 2.f ? a : 0.f;
  }

  if (!volumes.empty()) {
    // highest upward collision per texel, inside the volumes' footprints
    std::vector<float> gtop(n, -kInf);
    for (const auto& t : ground) {
      float tx0 = kInf, tx1 = -kInf, tz0 = kInf, tz1 = -kInf;
      for (const auto& v : t.v) {
        tx0 = std::min(tx0, v[0]);
        tx1 = std::max(tx1, v[0]);
        tz0 = std::min(tz0, v[2]);
        tz1 = std::max(tz1, v[2]);
      }
      bool in_volume = false;
      for (const auto& v : volumes) {
        in_volume |= tx1 >= v.x0 && tx0 <= v.x1 && tz1 >= v.z0 && tz0 <= v.z1;
      }
      if (in_volume) {
        raster(t, f.x0, f.z0, f.texel_m, f.w, f.h,
               [&](size_t k, float y) { gtop[k] = std::max(gtop[k], y); });
      }
    }
    for (const auto& v : volumes) {
      int i0, i1, j0, j1;
      span(v.x0, v.x1, f.x0, f.texel_m, f.w, &i0, &i1);
      span(v.z0, v.z1, f.z0, f.texel_m, f.h, &j0, &j1);
      for (int j = j0; j < j1; j++) {
        const float pz = f.z0 + (j + 0.5f) * f.texel_m;
        for (int i = i0; i < i1; i++) {
          const float px = f.x0 + (i + 0.5f) * f.texel_m;
          if (px < v.x0 || px > v.x1 || pz < v.z0 || pz > v.z1) {
            continue;
          }
          const size_t k = (size_t)j * f.w + i;
          if (gtop[k] >= v.surface_m - 0.1f) {
            f.dry_texels++;
            continue;
          }
          if (top[k] == -kInf) {
            f.volume_m2 += a;
          }
          top[k] = std::max(top[k], v.surface_m);
          wt[k] = 1.f;
        }
      }
    }
  }

  f.y0 = kInf;
  for (float y : top) {
    if (y > -kInf) {
      f.y0 = std::min(f.y0, y);
    }
  }
  if (f.y0 == kInf) {
    return Field{};
  }
  f.cov.assign(n, 0.f);
  f.covh.assign(n, 0.f);
  for (size_t k = 0; k < n; k++) {
    if (top[k] > -kInf) {
      f.cov[k] = wt[k];
      f.covh[k] = top[k] - f.y0;
    }
  }

  // height of the nearest lava, spread breadth-first up to the margin
  std::vector<float> near_h(top);
  std::vector<int> dist(n, -1);
  std::vector<size_t> queue;
  for (size_t k = 0; k < n; k++) {
    if (top[k] > -kInf) {
      dist[k] = 0;
      queue.push_back(k);
    }
  }
  for (size_t q = 0; q < queue.size(); q++) {
    const size_t k = queue[q];
    const int i = (int)(k % f.w), j = (int)(k / f.w);
    const int di[4] = {1, -1, 0, 0}, dj[4] = {0, 0, 1, -1};
    for (int d = 0; d < 4; d++) {
      const int ii = i + di[d], jj = j + dj[d];
      if (ii < 0 || jj < 0 || ii >= f.w || jj >= f.h) {
        continue;
      }
      const size_t kk = (size_t)jj * f.w + ii;
      if (dist[kk] < 0) {
        dist[kk] = dist[k] + 1;
        near_h[kk] = near_h[k];
        queue.push_back(kk);
      }
    }
  }
  f.near_m.assign(n, -kInf);
  f.dist_m.assign(n, kInf);
  for (size_t k = 0; k < n; k++) {
    f.near_m[k] = dist[k] >= 0 ? near_h[k] : -kInf;
    f.dist_m[k] = dist[k] >= 0 ? dist[k] * f.texel_m : kInf;
  }
  f.roof.assign(n, kInf);
  for (const auto& t : cover) {
    raster(t, f.x0, f.z0, f.texel_m, f.w, f.h, [&](size_t k, float y) {
      if (dist[k] >= 0 && y > near_h[k] + 0.5f) {
        f.roof[k] = std::min(f.roof[k], y);
      }
    });
  }
  for (size_t k = 0; k < n; k++) {
    f.roofed_m2 += top[k] > -kInf && f.roof[k] < kInf ? a : 0.f;
  }
  return f;
}

void split_collision(const tfrag3::Level& level,
                     std::vector<Tri>* lava,
                     std::vector<Tri>* ground,
                     std::vector<Tri>* cover) {
  const auto& verts = level.collision.vertices;
  for (size_t i = 0; i + 2 < verts.size(); i += 3) {
    const auto& v0 = verts[i];
    const bool up = v0.ny >= kUpNy * 32767.f;
    if (!up && v0.ny > -kUpNy * 32767.f) {
      continue;
    }
    Tri t;
    for (int k = 0; k < 3; k++) {
      t.v[k][0] = verts[i + k].x / kMeter;
      t.v[k][1] = verts[i + k].y / kMeter;
      t.v[k][2] = verts[i + k].z / kMeter;
    }
    const int mat = (v0.pat >> 6) & 0x3f;
    if (mat == kPatLava) {
      if (up) {
        lava->push_back(t);
      }
      continue;
    }
    if (mat == kPatHotcoals && up) {
      t.w = kHotcoalsWeight;
      lava->push_back(t);
    }
    if (up) {
      ground->push_back(t);
    }
    cover->push_back(t);
  }
}

Field build_level(const tfrag3::Level& level, const std::string& name) {
  std::vector<Tri> lava, ground, cover;
  split_collision(level, &lava, &ground, &cover);
  return build(lava, ground, cover, volumes_of(name), kTexelM, kMarginM, kMaxDim);
}

float bake_lava_share(const tfrag3::Level& level) {
  double sum[8][3] = {};
  auto add = [&](const tfrag3::PackedTimeOfDay& c) {
    const size_t quads = (c.color_count + 3) / 4;
    if (c.data.size() < quads * 128) {
      return;
    }
    const u8* p = c.data.data();
    for (size_t q = 0; q < quads; q++) {
      for (int pal = 0; pal < 8; pal++) {
        for (int col = 0; col < 4; col++) {
          for (int ch = 0; ch < 3; ch++) {
            sum[pal][ch] += p[q * 128 + pal * 16 + col * 4 + ch];
          }
        }
      }
    }
  };
  for (const auto& geom : level.tfrag_trees) {
    for (const auto& t : geom) {
      add(t.colors);
    }
  }
  for (const auto& geom : level.tie_trees) {
    for (const auto& t : geom) {
      add(t.colors);
    }
  }
  double sun = 0.0, lava = 0.0;
  for (int s = 0; s < 4; s++) {
    sun += (sum[s][0] + sum[s][1] + sum[s][2]) / 4.0;
  }
  for (int s = 4; s < 8; s++) {
    const double r = sum[s][0], g = sum[s][1], b = sum[s][2];
    if (r > 0.0 && b < 0.25 * r && g < 0.75 * r) {
      lava += r + g + b;
    }
  }
  return lava + sun > 0.0 ? (float)(lava / (lava + sun)) : 0.f;
}

std::shared_ptr<Prepared> prepare(const tfrag3::Level& level, const std::string& name) {
  auto p = std::make_shared<Prepared>();
  p->field = build_level(level, name);
  if (p->field.empty()) {
    return nullptr;
  }
  p->share = bake_lava_share(level);
  p->field.covh = {};
  return p;
}

void goal_light(const std::string& level,
                float level_x,
                float clock,
                float r,
                float g,
                float b,
                bool vanilla) {
  std::lock_guard<std::mutex> lk(g_mutex);
  auto& l = g_live[level];
  l.vanilla = vanilla;
  l.level = level_x;
  l.clock = clock;
  l.rgb[0] = r;
  l.rgb[1] = g;
  l.rgb[2] = b;
  l.valid = true;
}

std::vector<float> near_texels(const Prepared& p) {
  const auto& f = p.field;
  std::vector<float> rgb(f.roof.size() * 3);
  for (size_t k = 0; k < f.roof.size(); k++) {
    const float nh = f.near_m[k] > -kInf ? f.near_m[k] - f.y0 : 0.f;
    rgb[3 * k] = std::min(f.roof[k] - f.y0, nh + kOpenSky);
    rgb[3 * k + 1] = nh;
    rgb[3 * k + 2] = std::min(f.dist_m[k], 1000.f);
  }
  return rgb;
}

bool keep_entry(bool in_use, bool same_load, u64 last_used, u64 now) {
  if (in_use) {
    return same_load;
  }
  return now - last_used <= kKeepFrames;
}

void setup_background(SharedRenderState* render_state,
                      GLuint program,
                      const std::string& level_name) {
  const auto& gs = Gfx::g_global_settings;
  const bool jak1 = render_state->version == GameVersion::Jak1;
  const bool glow = jak1 && gs.lava_glow;
  const bool under = jak1 && gs.lava_under;
  if (!glow && !under) {
    if (g_used) {
      glUniform1i(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "lava_on"), 0);
    }
    return;
  }
  g_used = true;
  const Entry* e = under ? ensure(render_state, level_name) : nullptr;
  float rgb[3] = {0.f, 0.f, 0.f}, clock = 1.f;
  const bool lit = shading_light(level_name, rgb, &clock);
  lava_uniforms(program, e, rgb, clock, glow, under && lit);
}

void setup_merc(SharedRenderState* render_state, GLuint program) {
  const auto& gs = Gfx::g_global_settings;
  const bool on = render_state->version == GameVersion::Jak1 && pbr_active(render_state);
  const bool glow = on && gs.lava_glow;
  const bool under = on && gs.lava_under;
  if (!glow && !under) {
    if (g_used && pbr_active(render_state)) {
      glUniform1i(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "lava_on"), 0);
    }
    return;
  }
  g_used = true;
  // the loaded lava field whose box holds the camera, else the nearest one's box
  const float cx = render_state->camera_pos.x() / kMeter, cz = render_state->camera_pos.z() / kMeter;
  const Entry* e = nullptr;
  std::string name;
  float best = kInf;
  for (const auto& [n, f] : g_fields) {
    LevelData* lev = find_level(render_state, n);
    if (!f.tex || !lev || lev != f.lev || lev->load_id != f.load_id) {
      continue;
    }
    const float dx = std::max({f.x0 - cx, 0.f, cx - (f.x0 + f.ex)});
    const float dz = std::max({f.z0 - cz, 0.f, cz - (f.z0 + f.ez)});
    if (dx * dx + dz * dz < best) {
      best = dx * dx + dz * dz;
      e = &f;
      name = n;
    }
  }
  float rgb[3] = {0.f, 0.f, 0.f}, clock = 1.f;
  bool vanilla = false;
  const bool lit = e && shading_light(name, rgb, &clock, &vanilla);
  const int flags = lava_uniforms(program, e, rgb, clock, glow, under && lit);
  if (flags & 2) {
    // merc camera space to world: camera_rot is world to camera (row vectors, rows 0-2 a rotation)
    const auto& r = render_state->camera_rot;
    float c2w[16] = {};
    for (int i = 0; i < 3; i++) {
      for (int k = 0; k < 3; k++) {
        c2w[k * 4 + i] = r[i][k];
      }
      c2w[12 + i] = -(r[i][0] * r[3][0] + r[i][1] * r[3][1] + r[i][2] * r[3][2]);
    }
    c2w[15] = 1.f;
    glUniformMatrix4fv(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "lava_c2w"), 1, GL_FALSE, c2w);
    glUniform1f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "lava_char"), gs.lava_under_char);
    glUniform1f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "lava_vanilla"), vanilla ? 1.f : 0.f);
    glUniform1f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "lava_gain"), gs.lava_under_gain);
  }
}

bool merc_writes_emissive(const SharedRenderState* render_state,
                          const std::vector<u8>* classes,
                          s32 tex) {
  return render_state->emissive_mask && Gfx::g_global_settings.lava_glow &&
         pbr_active(render_state) && pbr_texture_class(classes, tex) == PBR_LAVA;
}

bool shrub_writes_emissive(const SharedRenderState* render_state, int cls) {
  return cls == PBR_LAVA && render_state->emissive_mask && Gfx::g_global_settings.lava_glow &&
         pbr_active(render_state);
}

}  // namespace lava_field
