#include "PaletteGlow.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <unordered_map>

#include "common/log/log.h"

#include "game/graphics/opengl_renderer/GlQueries.h"
#include "game/graphics/opengl_renderer/background/Unbake.h"
#include "game/graphics/opengl_renderer/background/background_common.h"

namespace {
constexpr int kMaxSlots = 8;
constexpr int kIters = 60;
constexpr float kRidge = 1e-3f;  // on e: only a tie-break toward ambient and sun
constexpr float kLuma[3] = {0.299f, 0.587f, 0.114f};
constexpr int kTexels = 8192;  // TIME_OF_DAY_COLOR_COUNT

std::unordered_map<GLuint, GLuint> g_glow_tex;  // tod texture -> glow texture (0: none)

// solves the k x k system g x = b (k <= 4) by Gaussian elimination; false if singular
bool solve_small(int k, double g[4][4], double b[4], double x[4]) {
  for (int c = 0; c < k; c++) {
    int p = c;
    for (int r = c + 1; r < k; r++) {
      if (std::abs(g[r][c]) > std::abs(g[p][c])) {
        p = r;
      }
    }
    if (std::abs(g[p][c]) < 1e-12) {
      return false;
    }
    std::swap(g[p], g[c]);
    std::swap(b[p], b[c]);
    for (int r = c + 1; r < k; r++) {
      const double f = g[r][c] / g[c][c];
      for (int j = c; j < k; j++) {
        g[r][j] -= f * g[c][j];
      }
      b[r] -= f * b[c];
    }
  }
  for (int c = k - 1; c >= 0; c--) {
    double s = b[c];
    for (int j = c + 1; j < k; j++) {
      s -= g[c][j] * x[j];
    }
    x[c] = s / g[c][c];
  }
  return true;
}

// per-slot sun gain for (a, e) over the samples with weight w (0: clipped)
void update_sigma(const float (*C)[3],
                  const float (*A)[3],
                  const float (*L)[3],
                  const double (*w)[3],
                  int S,
                  double a,
                  const double e[3],
                  double sig[kMaxSlots]) {
  for (int s = 0; s < S; s++) {
    double ll = 0, rl = 0;
    for (int c = 0; c < 3; c++) {
      ll += w[s][c] * L[s][c] * L[s][c];
      rl += w[s][c] * (C[s][c] - a * A[s][c] - e[c]) * L[s][c];
    }
    sig[s] = ll > 1e-6 ? std::max(rl / ll, 0.0) : 0.0;
  }
}

// rms residual of a A_s + sigma_s L_s with no emission
double fit_rms_without_glow(const float (*C)[3],
                            const float (*A)[3],
                            const float (*L)[3],
                            const double (*w)[3],
                            int S) {
  double aa = 0, n = 0;
  for (int s = 0; s < S; s++) {
    for (int c = 0; c < 3; c++) {
      aa += w[s][c] * A[s][c] * A[s][c];
      n += w[s][c];
    }
  }
  const double e0[3] = {0, 0, 0};
  double a = 0, sig[kMaxSlots] = {};
  for (int it = 0; it < kIters; it++) {
    double ra = 0;
    for (int s = 0; s < S; s++) {
      for (int c = 0; c < 3; c++) {
        ra += w[s][c] * (C[s][c] - sig[s] * L[s][c]) * A[s][c];
      }
    }
    a = aa > 1e-9 ? std::max(ra / aa, 0.0) : 0.0;
    update_sigma(C, A, L, w, S, a, e0, sig);
  }
  double sum = 0;
  for (int s = 0; s < S; s++) {
    for (int c = 0; c < 3; c++) {
      const double r = C[s][c] - a * A[s][c] - sig[s] * L[s][c];
      sum += w[s][c] * r * r;
    }
  }
  return std::sqrt(sum / std::max(n, 1.0));
}
}  // namespace

float palette_glow_entry(const float (*C)[3], const float (*A)[3], const float (*L)[3], int S) {
  if (S < 2 || S > kMaxSlots) {
    return 0.f;
  }
  // a clipped sample (254+) hides how its slot's light changed: left out
  double w[kMaxSlots][3];
  int seen[3] = {};
  for (int s = 0; s < S; s++) {
    for (int c = 0; c < 3; c++) {
      w[s][c] = C[s][c] < 254.f ? 1.0 : 0.0;
      seen[c] += (int)w[s][c];
    }
  }
  // a glow is the same in every slot: weight 1 while every channel's range is under
  // kGlowSteady of its brightest slot, 0 from twice that
  double range = -1.0;
  for (int c = 0; c < 3; c++) {
    if (seen[c] < 2) {
      continue;
    }
    double lo = 1e9, hi = 0;
    for (int s = 0; s < S; s++) {
      if (w[s][c] > 0) {
        lo = std::min(lo, (double)C[s][c]);
        hi = std::max(hi, (double)C[s][c]);
      }
    }
    range = std::max(range, hi > 1e-3 ? (hi - lo) / hi : 0.0);
  }
  const double steady =
      range < 0 ? 0.0 : std::clamp((2.0 * kGlowSteady - range) / kGlowSteady, 0.0, 1.0);
  if (steady <= 0.0 || fit_rms_without_glow(C, A, L, w, S) <= kGlowFitRms) {
    return 0.f;
  }
  // unknowns x = (a, e_r, e_g, e_b); rows (s, c) of the design: A_sc and the unit for e_c
  double gram[4][4] = {};
  for (int s = 0; s < S; s++) {
    for (int c = 0; c < 3; c++) {
      gram[0][0] += w[s][c] * A[s][c] * A[s][c];
      gram[0][1 + c] += w[s][c] * A[s][c];
      gram[1 + c][0] += w[s][c] * A[s][c];
      gram[1 + c][1 + c] += w[s][c];
    }
  }
  for (int c = 0; c < 3; c++) {
    gram[1 + c][1 + c] += kRidge;
  }
  double sig[kMaxSlots] = {}, x[4] = {};
  for (int it = 0; it < kIters; it++) {
    double R[kMaxSlots][3], b[4] = {};
    for (int s = 0; s < S; s++) {
      for (int c = 0; c < 3; c++) {
        R[s][c] = C[s][c] - sig[s] * L[s][c];
        b[0] += w[s][c] * R[s][c] * A[s][c];
        b[1 + c] += w[s][c] * R[s][c];
      }
    }
    // non-negative least squares by trying every support (15 subsets of 4)
    double best[4] = {}, best_cost = 1e300;
    for (int mask = 1; mask < 16; mask++) {
      int idx[4], k = 0;
      for (int j = 0; j < 4; j++) {
        if (mask & (1 << j)) {
          idx[k++] = j;
        }
      }
      double g[4][4], bb[4], xs[4];
      for (int r = 0; r < k; r++) {
        bb[r] = b[idx[r]];
        for (int c = 0; c < k; c++) {
          g[r][c] = gram[idx[r]][idx[c]];
        }
      }
      if (!solve_small(k, g, bb, xs)) {
        continue;
      }
      double cand[4] = {};
      bool ok = true;
      for (int r = 0; r < k; r++) {
        ok = ok && xs[r] >= -1e-9;
        cand[idx[r]] = xs[r];
      }
      if (!ok) {
        continue;
      }
      double cost = kRidge * (cand[1] * cand[1] + cand[2] * cand[2] + cand[3] * cand[3]);
      for (int s = 0; s < S; s++) {
        for (int c = 0; c < 3; c++) {
          const double r = R[s][c] - cand[0] * A[s][c] - cand[1 + c];
          cost += w[s][c] * r * r;
        }
      }
      if (cost < best_cost) {
        best_cost = cost;
        std::copy(cand, cand + 4, best);
      }
    }
    for (int j = 0; j < 4; j++) {
      x[j] = std::max(best[j], 0.0);
    }
    update_sigma(C, A, L, w, S, x[0], x + 1, sig);
  }
  // a channel clipped in every slot holds at least its dimmest slot above the ambient
  for (int c = 0; c < 3; c++) {
    if (seen[c] == 0) {
      double m = 1e9;
      for (int s = 0; s < S; s++) {
        m = std::min(m, C[s][c] - x[0] * A[s][c]);
      }
      x[1 + c] = std::max(m, 0.0);
    }
  }
  const double lum = kLuma[0] * x[1] + kLuma[1] * x[2] + kLuma[2] * x[3];
  return (float)(steady * std::max(lum - kGlowFloor, 0.0));
}

bool palette_glow_compute(const tfrag3::PackedTimeOfDay& colors,
                          const UnbakeSidecar& sidecar,
                          std::vector<u8>* out) {
  int slots[kMaxSlots], S = 0;
  const u32 use = sidecar.slot_mask & (sidecar.day_mask | sidecar.night_mask);
  for (int s = 0; s < kMaxSlots; s++) {
    if (use & (1u << s)) {
      slots[S++] = s;
    }
  }
  if (S < 2) {
    return false;
  }
  float A[kMaxSlots][3], L[kMaxSlots][3], C[kMaxSlots][3];
  float sun_max = 0.f, sun_min = 1e30f;
  for (int i = 0; i < S; i++) {
    for (int c = 0; c < 3; c++) {
      A[i][c] = sidecar.slots[slots[i]].amb[c];
      L[i][c] = sidecar.slots[slots[i]].lgt[c];
    }
    const float lum = kLuma[0] * L[i][0] + kLuma[1] * L[i][1] + kLuma[2] * L[i][2];
    sun_max = std::max(sun_max, lum);
    sun_min = std::min(sun_min, lum);
  }
  if (!(sun_min < kGlowSunless * sun_max)) {
    return false;  // a sun in every slot can stand in for a constant light: no split
  }
  out->assign(colors.color_count, 0);
  for (u32 e = 0; e < colors.color_count; e++) {
    for (int i = 0; i < S; i++) {
      for (int c = 0; c < 3; c++) {
        C[i][c] = colors.read(e, slots[i], c);
      }
    }
    (*out)[e] = (u8)std::lround(std::min(palette_glow_entry(C, A, L, S), 255.f));
  }
  return true;
}

void palette_glow_prepare(const tfrag3::Level& level, UnbakeSidecar* sidecar) {
  const auto t0 = std::chrono::steady_clock::now();
  size_t entries = 0, lit = 0;
  auto add = [&](const tfrag3::PackedTimeOfDay& colors) {
    std::vector<u8> glow;
    if (colors.color_count <= (u32)kTexels && palette_glow_compute(colors, *sidecar, &glow)) {
      entries += glow.size();
      const auto n = std::count_if(glow.begin(), glow.end(), [](u8 v) { return v != 0; });
      lit += n;
      if (!n) {
        glow.clear();
      }
    }
    sidecar->glow[&colors] = std::move(glow);
  };
  for (const auto& trees : level.tfrag_trees) {
    for (const auto& tree : trees) {
      add(tree.colors);
    }
  }
  for (const auto& trees : level.tie_trees) {
    for (const auto& tree : trees) {
      add(tree.colors);
    }
  }
  for (const auto& tree : level.shrub_trees) {
    add(tree.time_of_day_colors);
  }
  lg::info("palette glow: {} of {} entries, {:.1f} ms (loader)", lit, entries,
           std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
}

void palette_glow_setup_tree(const SharedRenderState* state,
                             GLuint program,
                             const UnbakeSidecar* sidecar,
                             const tfrag3::PackedTimeOfDay& colors,
                             GLuint tod_texture) {
  if (!ws_world_on(state)) {
    return;  // the shaders read ws_glow only under the map
  }
  auto it = g_glow_tex.find(tod_texture);
  if (it == g_glow_tex.end()) {
    GLuint tex = 0;
    const std::vector<u8>* glow = nullptr;
    if (sidecar) {
      const auto found = sidecar->glow.find(&colors);
      glow = found != sidecar->glow.end() ? &found->second : nullptr;
    }
    if (glow && !glow->empty()) {
      std::vector<u8> texels(kTexels, 0);
      std::copy(glow->begin(), glow->end(), texels.begin());  // zero past the palette
      GLint active = GL_TEXTURE0;
      glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
      glActiveTexture(GL_TEXTURE0 + kPaletteGlowUnit);
      glGenTextures(1, &tex);
      glBindTexture(GL_TEXTURE_1D, tex);
      glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
      glTexImage1D(GL_TEXTURE_1D, 0, GL_R8, kTexels, 0, GL_RED, GL_UNSIGNED_BYTE, texels.data());
      glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
      glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glActiveTexture(active);
    }
    it = g_glow_tex.emplace(tod_texture, tex).first;
  }
  glUniform1i(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "ws_glow_on"), it->second ? 1 : 0);
  if (it->second) {
    GLint active = GL_TEXTURE0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    glActiveTexture(GL_TEXTURE0 + kPaletteGlowUnit);
    glBindTexture(GL_TEXTURE_1D, it->second);
    glActiveTexture(active);
  }
}

void palette_glow_forget(GLuint tod_texture) {
  auto it = g_glow_tex.find(tod_texture);
  if (it == g_glow_tex.end()) {
    return;
  }
  if (it->second) {
    glDeleteTextures(1, &it->second);
  }
  g_glow_tex.erase(it);
}
