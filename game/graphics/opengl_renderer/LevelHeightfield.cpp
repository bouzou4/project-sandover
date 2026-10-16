#include "LevelHeightfield.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

#include "common/custom_data/Tfrag3Data.h"
#include "common/log/log.h"
#include "common/util/Timer.h"

#include "game/graphics/gfx.h"
#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/opengl_renderer/GlQueries.h"
#include "game/graphics/opengl_renderer/Swamp.h"
#include "game/graphics/opengl_renderer/loader/Loader.h"

#include "third-party/imgui/imgui.h"

namespace {
constexpr float kMeter = 4096.f;
constexpr int kMaxMaps = 2;
constexpr float kInf = std::numeric_limits<float>::infinity();
constexpr int kBit = PERF_OPT_GEOMETRY + 2;  // cached uniform lookups, as the shadow receivers
std::atomic<u64> g_serial{1};

struct SkyRow {
  const char* level;
  bool sky;
};
constexpr SkyRow kSky[] = {
#include "LevelSky.inc"
};

// render-thread state: two array layers with stable slots
struct Frame {
  GLuint tex = 0;
  int w = 0, h = 0, levels = 0;  // allocated layer size and mip count
  const LevelHeightfield* slot[kMaxMaps] = {nullptr, nullptr};
  u64 serial[kMaxMaps] = {0, 0};
  float rect[kMaxMaps][4] = {};  // x0, z0 (game units), uv per game unit in x, z
  float hgt[kMaxMaps][2] = {};   // y0, span (game units)
  std::string names[kMaxMaps];
  int n = 0;  // slots filled (0..2); an empty slot reads as open
  bool wanted = false;
  GLuint preview = 0;
  int preview_w = 0, preview_h = 0;
  u64 preview_serial = 0;
};
Frame g_frame;

GLint begin_unit() {
  GLint active = GL_TEXTURE0;  // the renderer's convention between passes
  if (!Gfx::perf_opt(kBit)) {
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
  }
  glActiveTexture(GL_TEXTURE0 + kHeightfieldUnit);
  return active;
}

// one layer: the map's own mips in the corner, marker texels around it
void upload_layer(const Frame& f, int layer, const LevelHeightfield* m) {
  std::vector<u16> pad;
  for (int k = 0; k < f.levels; k++) {
    const int W = f.w >> k, H = f.h >> k;
    pad.assign((size_t)W * H * 2, kHeightfieldNoTop);
    for (size_t i = 1; i < pad.size(); i += 2) {
      pad[i] = kHeightfieldNoBottom;
    }
    if (m && k < (int)m->gpu_mips.size()) {
      const auto& mip = m->gpu_mips[k];
      const int mw = heightfield_mip_dim(m->width, k), mh = heightfield_mip_dim(m->height, k);
      for (int y = 0; y < mh && y < H; y++) {
        memcpy(&pad[(size_t)y * W * 2], &mip[(size_t)y * mw * 2], std::min(mw, W) * 4);
      }
    }
    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, k, 0, 0, layer, W, H, 1, GL_RG, GL_UNSIGNED_SHORT,
                    pad.data());
  }
}

// order: the map the camera is over first (distance to its extent), then centre distance
std::pair<float, float> camera_key(float x, float z, const LevelHeightfield& m) {
  const float x1 = m.x0 + m.texel * m.width, z1 = m.z0 + m.texel * m.height;
  const float dx = std::max({m.x0 - x, 0.f, x - x1});
  const float dz = std::max({m.z0 - z, 0.f, z - z1});
  const float cx = 0.5f * (m.x0 + x1) - x, cz = 0.5f * (m.z0 + z1) - z;
  return {std::sqrt(dx * dx + dz * dz), std::sqrt(cx * cx + cz * cz)};
}
// Open spans per column: triangles sampled half a texel across, 0.5 m up, into 64 height bins;
// a second pass moves the span's ends onto the nearest samples.
template <typename V, typename R>
void build_gaps(LevelHeightfield& m, const V& v, const R& inside_range) {
  constexpr float kMinGap = 2.f;
  const size_t n_tex = (size_t)m.width * m.height;
  const float t = m.texel;
  auto each_sample = [&](auto&& fn) {
    for (size_t k = 0; k + 2 < v.size(); k += 3) {
      float p[3][3];
      for (int a = 0; a < 3; a++) {
        p[a][0] = v[k + a].x / kMeter;
        p[a][1] = v[k + a].y / kMeter;
        p[a][2] = v[k + a].z / kMeter;
      }
      float lxz = 0.f, ly = 0.f;
      for (int e = 0; e < 3; e++) {
        const float* a = p[e];
        const float* b = p[(e + 1) % 3];
        lxz = std::max({lxz, std::abs(b[0] - a[0]), std::abs(b[2] - a[2])});
        ly = std::max(ly, std::abs(b[1] - a[1]));
      }
      const int n = std::clamp((int)std::ceil(std::max(2.f * lxz / t, 2.f * ly)), 1, 512);
      for (int a = 0; a <= n; a++) {
        for (int b = 0; a + b <= n; b++) {
          const float u = (float)a / n, w = (float)b / n;
          float q[3];
          for (int c = 0; c < 3; c++) {
            q[c] = p[0][c] + u * (p[1][c] - p[0][c]) + w * (p[2][c] - p[0][c]);
          }
          const int i = (int)std::floor((q[0] - m.x0) / t), j = (int)std::floor((q[2] - m.z0) / t);
          if (inside_range(q[1]) && i >= 0 && j >= 0 && i < (int)m.width && j < (int)m.height) {
            fn((size_t)j * m.width + i, q[1]);
          }
        }
      }
    }
  };
  std::vector<u64> bins(n_tex, 0);
  each_sample([&](size_t k, float y) {
    const float span = m.top[k] - m.bottom[k];
    if (span > kMinGap) {
      bins[k] |= 1ull << std::clamp((int)((y - m.bottom[k]) / span * 64.f), 0, 63);
    }
  });
  m.gap_lo.assign(n_tex, 0.f);
  m.gap_hi.assign(n_tex, 0.f);
  std::vector<float> mid(n_tex, kInf);
  for (size_t k = 0; k < n_tex; k++) {
    const float span = m.top[k] - m.bottom[k];
    if (!(span > kMinGap)) {
      continue;
    }
    const float bin = span / 64.f;
    int best = 0, best_at = 0, run = 0;
    for (int b = 0; b < 64; b++) {
      run = (bins[k] >> b) & 1 ? 0 : run + 1;
      if (run > best) {
        best = run;
        best_at = b + 1 - run;
      }
    }
    if (best * bin >= kMinGap) {
      mid[k] = m.bottom[k] + (best_at + 0.5f * best) * bin;
      m.gap_lo[k] = m.bottom[k];
      m.gap_hi[k] = m.top[k];
    }
  }
  each_sample([&](size_t k, float y) {
    if (mid[k] < kInf) {
      if (y < mid[k]) {
        m.gap_lo[k] = std::max(m.gap_lo[k], y);
      } else {
        m.gap_hi[k] = std::min(m.gap_hi[k], y);
      }
    }
  });
}
}  // namespace

u16 heightfield_encode_top(float y, float y0, float y1) {
  if (!(y > -kInf)) {
    return kHeightfieldNoTop;
  }
  return (u16)(1 + std::lround(std::clamp((y - y0) / (y1 - y0), 0.f, 1.f) * 65533.f));
}

u16 heightfield_encode_bottom(float y, float y0, float y1) {
  if (!(y < kInf)) {
    return kHeightfieldNoBottom;
  }
  return (u16)(1 + std::lround(std::clamp((y - y0) / (y1 - y0), 0.f, 1.f) * 65533.f));
}

float heightfield_decode(u16 v, float y0, float y1) {
  return y0 + (y1 - y0) * (float)(v - 1) / 65533.f;
}

int heightfield_mip_dim(u32 n, int k) {
  return (int)((n + (1u << k) - 1) >> k);
}

int heightfield_alloc_dim(u32 n) {
  const u32 cell = 1u << (kHeightfieldLevels - 1);
  return (int)(((std::max(n, 1u) + cell - 1) / cell) * cell);
}

void heightfield_build_gpu_mips(LevelHeightfield& m) {
  m.gpu_mips.clear();
  int w = (int)m.width, h = (int)m.height;
  std::vector<u16> cur((size_t)w * h * 2);
  for (size_t k = 0; k < (size_t)w * h; k++) {
    cur[2 * k] = heightfield_encode_top(m.top[k], m.y0, m.y1);
    cur[2 * k + 1] = heightfield_encode_bottom(m.bottom[k], m.y0, m.y1);
  }
  m.gpu_mips.push_back(cur);
  // halving rounds up: cell x of the next level covers 2x and 2x + 1, so texel i sits in cell
  // i >> k at level k and an odd last row or column is kept
  while ((int)m.gpu_mips.size() < kHeightfieldLevels) {
    const int nw = (w + 1) / 2, nh = (h + 1) / 2;
    std::vector<u16> next((size_t)nw * nh * 2);
    for (int y = 0; y < nh; y++) {
      for (int x = 0; x < nw; x++) {
        u16 t = kHeightfieldNoTop, b = kHeightfieldNoBottom;
        for (int k = 0; k < 4; k++) {
          const int sx = std::min(2 * x + (k & 1), w - 1), sy = std::min(2 * y + (k >> 1), h - 1);
          const size_t s = ((size_t)sy * w + sx) * 2;
          t = std::max(t, cur[s]);
          b = std::min(b, cur[s + 1]);
        }
        next[((size_t)y * nw + x) * 2] = t;
        next[((size_t)y * nw + x) * 2 + 1] = b;
      }
    }
    m.gpu_mips.push_back(next);
    cur = std::move(next);
    w = nw;
    h = nh;
  }
}

float LevelHeightfield::height_top(float x, float z) const {
  const int i = (int)std::floor((x - x0) / texel), j = (int)std::floor((z - z0) / texel);
  if (i < 0 || j < 0 || i >= (int)width || j >= (int)height) {
    return -kInf;
  }
  return top[(size_t)j * width + i];
}

float LevelHeightfield::height_bottom(float x, float z) const {
  const int i = (int)std::floor((x - x0) / texel), j = (int)std::floor((z - z0) / texel);
  if (i < 0 || j < 0 || i >= (int)width || j >= (int)height) {
    return kInf;
  }
  return bottom[(size_t)j * width + i];
}

float LevelHeightfield::solid_at(float x, float y, float z, float margin) const {
  const int i = (int)std::floor((x - x0) / texel), j = (int)std::floor((z - z0) / texel);
  if (i < 0 || j < 0 || i >= (int)width || j >= (int)height) {
    return 0.f;
  }
  const size_t k = (size_t)j * width + i;
  // the soft edge lies in the open, so a thin roof slab still reads fully solid
  auto ramp = [&](float v) { return std::clamp(v / margin, 0.f, 1.f); };
  const float below_top = ramp(top[k] + margin - y);
  if (below_top <= 0.f || gap_hi.empty()) {
    return below_top;
  }
  const float in_gap = std::min(ramp(y - gap_lo[k]), ramp(gap_hi[k] - y));
  return below_top * (1.f - in_gap);
}

float LevelHeightfield::solid_span(float x, float z, float ya, float yb, float margin) const {
  const int i = (int)std::floor((x - x0) / texel), j = (int)std::floor((z - z0) / texel);
  if (i < 0 || j < 0 || i >= (int)width || j >= (int)height) {
    return 0.f;
  }
  const size_t k = (size_t)j * width + i;
  if (ya > top[k] + margin) {
    return 0.f;
  }
  // piecewise linear in y: the maximum is at an end or where a ramp ends
  float best = 0.f;
  const bool gaps = !gap_hi.empty();
  for (float y : {ya, yb, top[k], gaps ? gap_lo[k] : ya, gaps ? gap_hi[k] : ya}) {
    best = std::max(best, solid_at(x, std::clamp(y, ya, yb), z, margin));
  }
  return best;
}

bool heightfield_level_has_sky(const std::string& level) {
  for (const auto& r : kSky) {
    if (level == r.level) {
      return r.sky;
    }
  }
  return false;
}

bool heightfield_wanted() {
  // consumers add their switch here: the backlit leaves' sun occlusion (CanopyVol.h)
  const auto& gs = Gfx::g_global_settings;
  return gs.pbr_transmit_occl && gs.pbr_enable && gs.pbr_background;
}

std::unique_ptr<LevelHeightfield> heightfield_build(const tfrag3::CollisionMesh& mesh,
                                                    const std::string& level,
                                                    const HeightfieldSpec& spec) {
  const auto& v = mesh.vertices;
  if (v.size() < 3) {
    return nullptr;
  }
  Timer timer;
  auto P = [&](size_t k, int c) { return (c == 0 ? v[k].x : c == 1 ? v[k].y : v[k].z) / kMeter; };
  auto inside_range = [&](float y) { return y >= spec.y_min && y <= spec.y_max; };
  float x0 = kInf, z0 = kInf, y0 = kInf, x1 = -kInf, z1 = -kInf, y1 = -kInf;
  for (size_t k = 0; k < v.size(); k++) {
    if (!inside_range(P(k, 1))) {
      continue;
    }
    x0 = std::min(x0, P(k, 0));
    x1 = std::max(x1, P(k, 0));
    y0 = std::min(y0, P(k, 1));
    y1 = std::max(y1, P(k, 1));
    z0 = std::min(z0, P(k, 2));
    z1 = std::max(z1, P(k, 2));
  }
  {
    // a few stray far vertices must not coarsen the whole map: clamp to the 0.2-99.8% extent
    std::vector<float> xs, zs;
    for (size_t k = 0; k < v.size(); k++) {
      if (inside_range(P(k, 1))) {
        xs.push_back(P(k, 0));
        zs.push_back(P(k, 2));
      }
    }
    if (xs.empty()) {
      return nullptr;
    }
    auto pct = [](std::vector<float>& a, double q) {
      const size_t i = std::min(a.size() - 1, (size_t)(q * (a.size() - 1)));
      std::nth_element(a.begin(), a.begin() + i, a.end());
      return a[i];
    };
    x0 = std::max(x0, pct(xs, 0.002) - 50.f);
    x1 = std::min(x1, pct(xs, 0.998) + 50.f);
    z0 = std::max(z0, pct(zs, 0.002) - 50.f);
    z1 = std::min(z1, pct(zs, 0.998) + 50.f);
  }
  if (spec.has_region) {
    x0 = std::max(x0, spec.region[0]);
    z0 = std::max(z0, spec.region[1]);
    x1 = std::min(x1, spec.region[2]);
    z1 = std::min(z1, spec.region[3]);
  }
  if (!(x1 > x0 && z1 > z0)) {
    return nullptr;
  }
  auto m = std::make_unique<LevelHeightfield>();
  m->level = level;
  const float ext = std::max(x1 - x0, z1 - z0) + 2.f;
  m->texel = std::max(spec.texel_m, std::ceil(ext / HeightfieldSpec::kMaxSize));
  m->x0 = std::floor(x0) - 1.f;
  m->z0 = std::floor(z0) - 1.f;
  m->width = (u32)std::ceil((x1 + 1.f - m->x0) / m->texel);
  m->height = (u32)std::ceil((z1 + 1.f - m->z0) / m->texel);
  m->y0 = y0;
  m->y1 = std::max(y1, y0 + 1.f);
  m->top.assign((size_t)m->width * m->height, -kInf);
  m->bottom.assign((size_t)m->width * m->height, kInf);
  const float t = m->texel;
  auto put = [&](float x, float z, float y) {
    if (!inside_range(y)) {
      return;
    }
    const int i = (int)std::floor((x - m->x0) / t), j = (int)std::floor((z - m->z0) / t);
    if (i >= 0 && j >= 0 && i < (int)m->width && j < (int)m->height) {
      const size_t k = (size_t)j * m->width + i;
      m->top[k] = std::max(m->top[k], y);
      m->bottom[k] = std::min(m->bottom[k], y);
    }
  };
  for (size_t k = 0; k + 2 < v.size(); k += 3) {
    float p[3][3];
    for (int a = 0; a < 3; a++) {
      for (int c = 0; c < 3; c++) {
        p[a][c] = P(k + a, c);
      }
    }
    // edges at half-texel steps (walls have no footprint: their edges carry them)
    for (int e = 0; e < 3; e++) {
      const float* a = p[e];
      const float* b = p[(e + 1) % 3];
      const float len = std::max(std::abs(b[0] - a[0]), std::abs(b[2] - a[2]));
      const int n = std::max(1, (int)std::ceil(2.f * len / t));
      for (int s = 0; s <= n; s++) {
        const float f = (float)s / n;
        put(a[0] + f * (b[0] - a[0]), a[2] + f * (b[2] - a[2]), a[1] + f * (b[1] - a[1]));
      }
    }
    // texel centres inside the footprint
    const float ax = p[1][0] - p[0][0], az = p[1][2] - p[0][2];
    const float bx = p[2][0] - p[0][0], bz = p[2][2] - p[0][2];
    const float d = ax * bz - bx * az;
    if (std::abs(d) < 1e-6f) {
      continue;
    }
    // clamped to the map: a stray far vertex must not walk its whole bounding box
    const int W = (int)m->width - 1, H = (int)m->height - 1;
    auto cell = [&](float a, int hi) {
      return (int)std::clamp(std::floor(a / t), -1.f, (float)hi + 1);
    };
    const int i0 = std::max(cell(std::min({p[0][0], p[1][0], p[2][0]}) - m->x0, W), 0);
    const int i1 = std::min(cell(std::max({p[0][0], p[1][0], p[2][0]}) - m->x0, W), W);
    const int j0 = std::max(cell(std::min({p[0][2], p[1][2], p[2][2]}) - m->z0, H), 0);
    const int j1 = std::min(cell(std::max({p[0][2], p[1][2], p[2][2]}) - m->z0, H), H);
    for (int j = j0; j <= j1; j++) {
      const float cz = m->z0 + (j + 0.5f) * t;
      for (int i = i0; i <= i1; i++) {
        const float cx = m->x0 + (i + 0.5f) * t;
        const float s = ((cx - p[0][0]) * bz - bx * (cz - p[0][2])) / d;
        const float r = (ax * (cz - p[0][2]) - (cx - p[0][0]) * az) / d;
        if (s >= 0.f && r >= 0.f && s + r <= 1.f) {
          put(cx, cz, p[0][1] + s * (p[1][1] - p[0][1]) + r * (p[2][1] - p[0][1]));
        }
      }
    }
  }
  build_gaps(*m, v, inside_range);
  heightfield_build_gpu_mips(*m);
  m->serial = g_serial++;
  m->build_ms = (float)timer.getMs();
  lg::info("heightfield: {} {}x{} texels of {:.0f} m, {:.1f} MB, {:.1f} ms", level, m->width,
           m->height, m->texel, m->bytes() / 1e6, m->build_ms);
  return m;
}

const LevelHeightfield* heightfield_of(const LevelData* level) {
  return level ? level->heightfield.get() : nullptr;
}

void heightfield_prepare(SharedRenderState* rs) {
  auto& f = g_frame;
  f.wanted = heightfield_wanted() || swamp_cover_wanted(rs->frame_idx);
  std::vector<std::pair<std::pair<float, float>, const LevelHeightfield*>> cand;
  if (f.wanted && rs->loader && rs->version == GameVersion::Jak1) {
    const float cx = rs->camera_pos.x() / kMeter, cz = rs->camera_pos.z() / kMeter;
    for (auto* lev : rs->loader->get_in_use_levels()) {
      if (lev->heightfield) {
        cand.push_back({camera_key(cx, cz, *lev->heightfield), lev->heightfield.get()});
      }
    }
  }
  std::sort(cand.begin(), cand.end(), [](const auto& a, const auto& b) {
    return a.first != b.first ? a.first < b.first : a.second->level < b.second->level;
  });
  const LevelHeightfield* want[kMaxMaps] = {nullptr, nullptr};
  for (size_t i = 0; i < cand.size() && (int)i < kMaxMaps; i++) {
    want[i] = cand[i].second;
  }
  // stable slots: a chosen map already in a slot keeps it
  const LevelHeightfield* next[kMaxMaps] = {nullptr, nullptr};
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
      need_w = std::max(need_w, (int)m->width);
      need_h = std::max(need_h, (int)m->height);
    }
  }
  const bool any = next[0] || next[1];
  need_w = heightfield_alloc_dim(need_w);
  need_h = heightfield_alloc_dim(need_h);
  if (!any && f.tex) {
    // no consumer or no map: free the array
    glDeleteTextures(1, &f.tex);
    f.tex = 0;
    f.w = f.h = f.levels = 0;
  } else if (any && (!f.tex || need_w > f.w || need_h > f.h)) {
    // (re)allocate every mip of both layers, then fill both
    if (!f.tex) {
      glGenTextures(1, &f.tex);
    }
    f.w = std::max(need_w, f.w);
    f.h = std::max(need_h, f.h);
    f.levels = kHeightfieldLevels;
    const GLint active = begin_unit();
    glBindTexture(GL_TEXTURE_2D_ARRAY, f.tex);
    for (int k = 0; k < f.levels; k++) {
      glTexImage3D(GL_TEXTURE_2D_ARRAY, k, GL_RG16, f.w >> k, f.h >> k, kMaxMaps, 0, GL_RG,
                   GL_UNSIGNED_SHORT, nullptr);
    }
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, f.levels - 1);
    // nearest only: blending a marker with a height, or a max cell with its neighbour, would
    // invent surfaces (the shader reads with texelFetch anyway)
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
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
    // only the layers whose map changed
    bool bound = false;
    GLint active = GL_TEXTURE0;
    for (int s = 0; s < kMaxMaps; s++) {
      const u64 ser = next[s] ? next[s]->serial : 0;
      if (next[s] == f.slot[s] && ser == f.serial[s]) {
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
    f.names[s] = next[s] ? next[s]->level : "";
    f.rect[s][0] = f.rect[s][1] = f.rect[s][2] = f.rect[s][3] = 0.f;
    f.hgt[s][0] = f.hgt[s][1] = 0.f;
    if (next[s]) {
      const auto* m = next[s];
      f.n = s + 1;
      f.rect[s][0] = m->x0 * kMeter;
      f.rect[s][1] = m->z0 * kMeter;
      f.rect[s][2] = 1.f / (m->texel * kMeter);
      f.rect[s][3] = 1.f / (m->texel * kMeter);
      f.hgt[s][0] = m->y0 * kMeter;
      f.hgt[s][1] = (m->y1 - m->y0) * kMeter;
    }
  }
}

int heightfield_bound() {
  return g_frame.wanted && g_frame.tex ? g_frame.n : 0;
}

void heightfield_set_uniforms(GLuint program) {
  const auto& f = g_frame;
  const int n = f.wanted && f.tex ? f.n : 0;
  glUniform1i(GLQ_LOC(kBit, program, "lh_n"), n);
  if (n == 0) {
    return;
  }
  glUniform4fv(GLQ_LOC(kBit, program, "lh_rect"), kMaxMaps, &f.rect[0][0]);
  glUniform2fv(GLQ_LOC(kBit, program, "lh_h"), kMaxMaps, &f.hgt[0][0]);
  glUniform1i(GLQ_LOC(kBit, program, "lh_levels"), f.levels);
  const GLint active = begin_unit();
  glBindTexture(GL_TEXTURE_2D_ARRAY, f.tex);
  glActiveTexture(active);
}

void heightfield_draw_debug_window() {
  auto& f = g_frame;
  ImGui::Text("wanted by a consumer: %s, maps in use: %d", f.wanted ? "yes" : "no", f.n);
  for (int i = 0; i < kMaxMaps; i++) {
    if (f.slot[i]) {
      ImGui::Text("  slot %d: %s %ux%u texels of %.0f m, %.1f MB, %.1f ms", i, f.names[i].c_str(),
                  f.slot[i]->width, f.slot[i]->height, f.slot[i]->texel, f.slot[i]->bytes() / 1e6,
                  f.slot[i]->build_ms);
    }
  }
  static int which = 0;
  static bool show = false;
  ImGui::Checkbox("Preview (top: grey; bottom below top: blue)", &show);
  ImGui::SliderInt("Slot", &which, 0, kMaxMaps - 1);
  const LevelHeightfield* m = f.slot[which];
  if (!show || !m || m->gpu_mips.empty()) {
    return;
  }
  if (f.preview_serial != m->serial) {
    const auto& d = m->gpu_mips[0];
    const int step = std::max(1, (int)std::max(m->width, m->height) / 512);
    const int pw = (int)m->width / step, ph = (int)m->height / step;
    std::vector<u8> rgba((size_t)pw * ph * 4);
    for (int y = 0; y < ph; y++) {
      for (int x = 0; x < pw; x++) {
        const size_t s = ((size_t)(ph - 1 - y) * step * m->width + (size_t)x * step) * 2;
        const u8 t = (u8)(d[s] >> 8), b = (u8)(d[s + 1] >> 8);
        const bool under = d[s + 1] != kHeightfieldNoBottom && b + 8 < t;
        u8* o = &rgba[((size_t)y * pw + x) * 4];
        o[0] = under ? t / 2 : t;
        o[1] = under ? t / 2 : t;
        o[2] = t;
        o[3] = 255;
      }
    }
    if (!f.preview) {
      glGenTextures(1, &f.preview);
    }
    glBindTexture(GL_TEXTURE_2D, f.preview);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, pw, ph, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
    f.preview_w = pw;
    f.preview_h = ph;
    f.preview_serial = m->serial;
  }
  ImGui::Image((ImTextureID)(intptr_t)f.preview, ImVec2((float)f.preview_w, (float)f.preview_h));
}
