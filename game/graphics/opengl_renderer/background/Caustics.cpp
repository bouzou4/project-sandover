#include "Caustics.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "game/graphics/gfx.h"
#include "game/graphics/opengl_renderer/GlQueries.h"
#include "game/graphics/opengl_renderer/Materials.h"

namespace {
constexpr double kPi = 3.14159265358979323846;

struct CausticMood {
  const char* level;
  int first_slot;
  float cell_m[3];  // tfrag, tie, shrub
};
constexpr float kStrengthRange = 2.f;  // strength byte = a / range x 255 (caustics.py)
constexpr CausticMood kCausticMoods[] = {
#include "CausticMoods.inc"
};

// weight halfword of `slot`, channel `ch` (r g b a), low byte = weight x 64 (update-mood-itimes)
s32& weight_word(math::Vector<s32, 4> itimes[4], int slot, int ch) {
  return itimes[slot / 2][(slot % 2) * 2 + ch / 2];
}

u32 get_weight(math::Vector<s32, 4> itimes[4], int slot, int ch) {
  const u32 w = (u32)weight_word(itimes, slot, ch);
  return ((ch % 2) ? w >> 16 : w) & 0xffu;
}

void set_weight(math::Vector<s32, 4> itimes[4], int slot, int ch, u32 v) {
  s32& w = weight_word(itimes, slot, ch);
  const int sh = (ch % 2) * 16;
  w = (s32)(((u32)w & ~(0xffu << sh)) | ((v & 0xffu) << sh));
}

struct Clock {
  double t = 0.;
  u64 advanced_frame = ~0ull;
  u64 dt_frame = ~0ull;
  double dt = 0.;  // real time since the previous rendered frame, s (at most kMaxStep)
  std::chrono::steady_clock::time_point last{};
  std::vector<std::pair<std::string, u64>> sig;  // raw caustic weights last seen, per level
} g_clock;
constexpr double kMaxStep = 0.1;  // a hitch or a load does not jump the pattern

bool g_used = false;
std::vector<math::Vector<u8, 4>> g_env;
std::vector<GLuint> g_env_uploaded;  // time-of-day textures holding their envelope

/// Covariance of the four loop weights over update-mood-caustics' 32 frames.
struct WeightCov {
  float m[4][4] = {};
  WeightCov() {
    float w[4] = {}, ws[32][4], mean[4] = {};
    for (int s = 0; s < 64; s++) {
      const int v = (s >> 3) & 3;
      const float f = 0.125f * (s & 7);
      w[(v + 3) & 3] = 1.f - f;
      w[v] = f;
      if (s >= 32) {
        for (int i = 0; i < 4; i++) {
          ws[s - 32][i] = w[i];
          mean[i] += w[i] / 32.f;
        }
      }
    }
    for (auto& row : ws) {
      for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
          m[i][j] += (row[i] - mean[i]) * (row[j] - mean[j]) / 32.f;
        }
      }
    }
  }
};

/// Per palette entry: rgb the mean of the four caustic slots, alpha the original's temporal RMS
/// over its loop relative to its mean light (luminance), / kStrengthRange. The loop's weights sum
/// to 1.25 (the slots that are not fading hold 0.125), so its mean light is 1.25 x the slot mean.
void upload_envelope(int base, const tfrag3::PackedTimeOfDay& colors, GLuint tod_texture) {
  static const WeightCov cov;
  if (g_env.size() < colors.color_count) {
    g_env.resize(colors.color_count);
  }
  for (u32 c = 0; c < colors.color_count; c++) {
    float lum[4], mean = 0.f;
    int sum[3] = {};
    for (int s = 0; s < 4; s++) {
      const int r = colors.read(c, base + s, 0), g = colors.read(c, base + s, 1),
                b = colors.read(c, base + s, 2);
      sum[0] += r;
      sum[1] += g;
      sum[2] += b;
      lum[s] = 0.299f * r + 0.587f * g + 0.114f * b;
      mean += 0.25f * lum[s];
    }
    float var = 0.f;
    for (int i = 0; i < 4; i++) {
      for (int j = 0; j < 4; j++) {
        var += (lum[i] - mean) * cov.m[i][j] * (lum[j] - mean);
      }
    }
    const float a = mean > 0.f ? std::sqrt(std::max(var, 0.f)) / (1.25f * mean) : 0.f;
    auto& o = g_env[c];
    for (int ch = 0; ch < 3; ch++) {
      o[ch] = (u8)((sum[ch] + 2) / 4);
    }
    o[3] = (u8)std::lround(std::clamp(a / kStrengthRange, 0.f, 1.f) * 255.f);
  }
  GLint active = GL_TEXTURE0;
  glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
  glActiveTexture(GL_TEXTURE10);
  glBindTexture(GL_TEXTURE_1D, tod_texture);
  glTexSubImage1D(GL_TEXTURE_1D, 0, kCausticEnvOffset, colors.color_count, GL_RGBA,
                  GL_UNSIGNED_INT_8_8_8_8_REV, g_env.data());
  glActiveTexture(active);
}
}  // namespace

namespace {
const CausticMood* find_mood(const char* level) {
  for (const auto& m : kCausticMoods) {
    if (strcmp(level, m.level) == 0) {
      return &m;
    }
  }
  return nullptr;
}
}  // namespace

int caustic_first_slot(const char* level) {
  const auto* m = find_mood(level);
  return m ? m->first_slot : -1;
}

bool caustic_active(const SharedRenderState* state, const char* level) {
  return state->version == GameVersion::Jak1 && Gfx::g_global_settings.caustic_mode != 0 &&
         pbr_active(state) && caustic_first_slot(level) >= 0;
}

void caustic_itimes(const SharedRenderState* state, TfragPcPortData* data) {
  if (!caustic_active(state, data->level_name)) {
    return;
  }
  const int base = caustic_first_slot(data->level_name);
  auto* it = data->camera.itimes;
  const auto& gs = Gfx::g_global_settings;
  if (g_clock.dt_frame != state->frame_idx) {
    const auto now = std::chrono::steady_clock::now();
    g_clock.dt = g_clock.dt_frame == ~0ull
                     ? 0.
                     : std::min(std::chrono::duration<double>(now - g_clock.last).count(), kMaxStep);
    g_clock.last = now;
    g_clock.dt_frame = state->frame_idx;
  }
  if (gs.caustic_clock_hold_s >= 0.f) {
    g_clock.t = gs.caustic_clock_hold_s;
    g_clock.advanced_frame = state->frame_idx;
  }
  u64 sig = 0;
  for (int s = 0; s < 4; s++) {
    for (int ch = 0; ch < 4; ch++) {
      sig = sig * 131 + get_weight(it, base + s, ch);
    }
  }
  auto e = std::find_if(g_clock.sig.begin(), g_clock.sig.end(),
                        [&](const auto& p) { return p.first == data->level_name; });
  if (e == g_clock.sig.end()) {
    g_clock.sig.emplace_back(data->level_name, sig);
  } else if (e->second != sig) {
    e->second = sig;
    if (g_clock.advanced_frame != state->frame_idx) {
      g_clock.advanced_frame = state->frame_idx;
      g_clock.t += g_clock.dt;
    }
  }
  for (int ch = 0; ch < 4; ch++) {
    u32 sum = 0;
    for (int s = 0; s < 4; s++) {
      sum += get_weight(it, base + s, ch);
    }
    for (int s = 0; s < 4; s++) {
      set_weight(it, base + s, ch, sum / 4);
    }
  }
}

void caustic_setup_tree(SharedRenderState* state,
                        GLuint program,
                        const std::string& level,
                        const math::Vector<s32, 4> itimes[4],
                        const tfrag3::PackedTimeOfDay& colors,
                        GLuint tod_texture,
                        CausticKind kind) {
  const bool on = caustic_active(state, level.c_str());
  if (!on && !g_used) {
    return;
  }
  g_used = true;
  const GLint w_loc = GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "cau_w");
  if (!on) {
    glUniform3f(w_loc, 0.f, 0.f, 0.f);
    return;
  }
  const int base = caustic_first_slot(level.c_str());
  if (colors.color_count > (u32)kCausticEnvOffset) {
    glUniform3f(w_loc, 0.f, 0.f, 0.f);
    return;
  }

  // envelope: static palette data in texels the colour upload never reaches, once per texture
  if (std::find(g_env_uploaded.begin(), g_env_uploaded.end(), tod_texture) ==
      g_env_uploaded.end()) {
    g_env_uploaded.push_back(tod_texture);
    upload_envelope(base, colors, tod_texture);
  }

  // live weight sum per channel: four frozen slots of sum / 4 each
  math::Vector<s32, 4> it[4];
  for (int i = 0; i < 4; i++) {
    it[i] = itimes[i];
  }
  float w[3];
  for (int ch = 0; ch < 3; ch++) {
    w[ch] = 4.f * get_weight(it, base, ch) / 64.f;
  }
  glUniform3f(w_loc, w[0], w[1], w[2]);

  const auto& gs = Gfx::g_global_settings;
  const double cell =
      std::max(find_mood(level.c_str())->cell_m[(int)kind] * gs.caustic_cell_scale, 0.05f);
  const double loop = (gs.caustic_web > 0.f ? kPi : 2. * kPi) * gs.caustic_loop_hz * g_clock.t;
  const double d = gs.caustic_drift_mps * g_clock.t / cell;
  glUniform4f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "cau_a"), (float)(1. / cell),
              (float)std::fmod(loop, 2. * kPi), gs.caustic_amp, gs.caustic_web);
  glUniform4f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "cau_b"), (float)(0.6 * d), (float)(0.3 * d),
              (float)(0.75 * d), gs.caustic_web_mean);
  glUniform1i(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "cau_ldr"), state->hdr_target ? 0 : 1);
}

void caustic_forget_texture(GLuint tod_texture) {
  g_env_uploaded.erase(std::remove(g_env_uploaded.begin(), g_env_uploaded.end(), tod_texture),
                       g_env_uploaded.end());
}
