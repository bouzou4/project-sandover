#include "Swamp.h"

#include <algorithm>
#include <cmath>

#include "game/graphics/gfx.h"
#include "game/graphics/opengl_renderer/GlQueries.h"
#include "game/graphics/opengl_renderer/LevelHeightfield.h"
#include "game/graphics/opengl_renderer/Materials.h"
#include "game/graphics/opengl_renderer/Neighbours.h"
#include "game/graphics/opengl_renderer/SsaoRenderer.h"
#include "game/graphics/opengl_renderer/loader/Loader.h"

// perf-opt bit 25: GL queries off the per-frame path (GlQueries.h)
constexpr int kGlq = PERF_OPT_PRESENT2 + 1;

namespace {
constexpr float kMeter = 4096.f;
constexpr float kWaterY = 0.f;  // `swp-water` height (m)

/// One level of the mist pass: camera box (m, world xz: the level's tfrag and tie extent plus
/// 60 m, measured from the background like the swamp's) and water line. A sea row takes the
/// game's ocean height instead and stops rays at the sea plane.
struct MistRow {
  const char* level;
  float min_x, max_x, min_z, max_z;
  float water_y;
  bool sea;
};
constexpr MistRow kMistRows[] = {
    {"swamp", 253.f - 60.f, 724.f + 60.f, -2192.f - 60.f, -1811.f + 60.f, kWaterY, false},
    {"misty", -345.f, 252.f, 644.f, 1366.f, 0.f, true},
};

struct MistParams {
  bool on;
  float density, height_m, start_m, max, bright, sun;
};

MistParams mist_params(const MistRow& row) {
  const auto& gs = Gfx::g_global_settings;
  if (row.sea) {
    // strength scales the density and the cap's optical depth; ranges as pc-set-swamp clamps them
    const float k = std::clamp(gs.misty_mist_strength, 0.f, 4.f);
    return {gs.misty_mist && k > 0.f,
            std::max(gs.misty_mist_density, 0.f) * k,
            std::max(gs.misty_mist_height_m, 0.05f),
            std::max(gs.misty_mist_start_m, 0.f),
            1.f - std::pow(1.f - std::clamp(gs.misty_mist_max, 0.f, 0.999f), k),
            std::clamp(gs.misty_mist_bright, 0.f, 1.f),
            gs.swamp_mist_sun};
  }
  return {gs.swamp_mist,         gs.swamp_mist_density, gs.swamp_mist_height_m,
          gs.swamp_mist_start_m, gs.swamp_mist_max,     gs.swamp_mist_bright,
          gs.swamp_mist_sun};
}

bool g_bg_used = false;
u64 g_cover_frame = ~0ull;  // last frame a swamp tree drew rain effects
u64 g_mist_frame = ~0ull;   // last frame the swamp's mist pass drew

/// The mist colour: the fog colour lifted toward white by `b`.
void mist_colour(const SharedRenderState* rs, float b, float out[3]) {
  for (int i = 0; i < 3; i++) {
    const float f = rs->fog_color[i] / 255.f;
    out[i] = f + (0.85f - f) * b;
  }
}

bool level_loaded(SharedRenderState* rs, const char* name) {
  if (!rs->loader) {
    return false;
  }
  for (auto* lev : rs->loader->get_in_use_levels()) {
    if (lev && lev->level && lev->level->level_name == name) {
      return true;
    }
  }
  return false;
}

/// The mist row drawing this frame at this point of the frame, if any.
const MistRow* mist_row(SharedRenderState* rs, bool after_ocean) {
  if (rs->version != GameVersion::Jak1 || !rs->has_pc_data) {
    return nullptr;
  }
  const float x = rs->camera_pos.x() / kMeter, z = rs->camera_pos.z() / kMeter;
  for (const auto& row : kMistRows) {
    if (row.sea != after_ocean || !mist_params(row).on) {
      continue;
    }
    if (x < row.min_x || x > row.max_x || z < row.min_z || z > row.max_z) {
      continue;
    }
    // the player is in the level (the rain uses the same basis), not the level drawn as a neighbour
    if (neighbours::viewer() == row.level && level_loaded(rs, row.level)) {
      return &row;
    }
  }
  return nullptr;
}

/// The mist's ray uniforms (swamp_mist.glsl) for `row` on `id`.
void mist_ray_uniforms(SharedRenderState* rs, GLuint id, const MistRow& row, const MistParams& mp) {
  // SSAO camera space to world: axis signs of the game projection, then the camera rotation
  const auto* persp = rs->camera_persp;
  const float fog = rs->camera_fog.x();
  const float sg[3] = {fog * persp[0][0] >= 0.f ? -1.f : 1.f,
                       -fog * persp[1][1] >= 0.f ? -1.f : 1.f, persp[2][3] >= 0.f ? -1.f : 1.f};
  const auto& r = rs->camera_rot;
  float m[9];
  for (int k = 0; k < 3; k++) {
    for (int i = 0; i < 3; i++) {
      m[k * 3 + i] = r[i][k] * sg[k];  // column-major: column k is camera axis k in world
    }
  }
  glUniformMatrix3fv(GLQ_LOC(kGlq, id, "cam_to_world"), 1, GL_FALSE, m);
  const float water_y = row.sea ? water_ocean_height(rs) / kMeter : row.water_y;
  glUniform1f(GLQ_LOC(kGlq, id, "eye_h"), rs->camera_pos.y() / kMeter - water_y);
  glUniform4f(GLQ_LOC(kGlq, id, "mist"), mp.density, 1.f / std::max(mp.height_m, 0.05f),
              mp.start_m, mp.max);
  glUniform1i(GLQ_LOC(kGlq, id, "sea"), row.sea ? 1 : 0);
}
}  // namespace

void swamp_setup_background(SharedRenderState* render_state,
                            GLuint program,
                            const std::string& level_name) {
  const auto& gs = Gfx::g_global_settings;
  const bool any = render_state->version == GameVersion::Jak1 &&
                   (gs.swamp_water || gs.swamp_wet || gs.shore_wet);
  if (!any && !g_bg_used) {
    return;
  }
  g_bg_used = true;
  const bool swamp = any && level_name == "swamp";
  const bool shore = any && gs.shore_wet && !swamp && water_level_has_sea(level_name);
  int flags = swamp ? (gs.swamp_water ? 1 : 0) | (gs.swamp_wet ? 2 : 0) : shore ? 4 : 0;
  if (gs.wet_pbr && (flags & ~1)) {
    flags |= 16;
  }
  glUniform1i(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "sw_flags"), flags);
  if (!flags) {
    return;
  }
  if (flags & 16) {
    // the darkening is in linear light, like the shoreline's
    glUniform4f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "sw_gloss"), gs.wet_pbr_gloss,
                gs.wet_pbr_rough, gs.wet_pbr_f0, std::pow(gs.wet_pbr_dark, 1.f / 2.2f));
    glUniform2f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "sw_splash"), gs.wet_pbr_splash_full_m,
                std::max(gs.wet_pbr_splash_top_m, gs.wet_pbr_splash_full_m + 0.1f));
  }
  if (shore) {
    // shore darkening is in linear light; the shaded colour is display-encoded. No puddles.
    glUniform4f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "sw_wet"), std::pow(gs.shore_wet_dark, 1.f / 2.2f),
                gs.shore_wet_film, 0.f, 0.f);
    glUniform4f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "sw_shore"), water_ocean_height(render_state) / kMeter,
                gs.shore_wet_band_m, gs.shore_wet_cell_m, 0.f);
    return;
  }
  // rain needs open sky: the wet block reads the level heightfield for cover
  g_cover_frame = render_state->frame_idx;
  heightfield_set_uniforms(program);
  glUniform4f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "sw_water"), gs.swamp_water_dark,
              gs.swamp_water_refl, gs.swamp_water_ripple, gs.swamp_water_glint);
  glUniform4f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "sw_wet"), gs.swamp_wet_dark,
              gs.swamp_wet_coat, gs.swamp_wet_puddle, 0.f);
  // renderer clock, like water_time; water option 90 pins both
  glUniform1f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "sw_time"),
              gs.water_clock_s >= 0.f ? gs.water_clock_s
                                      : (float)(render_state->frame_idx % 216000) / 60.f);
}

bool swamp_cover_wanted(u64 frame_idx) {
  return g_cover_frame != ~0ull && frame_idx - g_cover_frame <= 2;
}

void swamp_mist_sprite_uniforms(const SharedRenderState* rs, GLuint program) {
  const auto& gs = Gfx::g_global_settings;
  // only while the pass drew this frame, so a sprite never takes mist the world did not
  const bool on = g_mist_frame == rs->frame_idx;
  glUniform4f(GLQ_LOC(kGlq, program, "sw_mist"), on ? gs.swamp_mist_density : 0.f,
              1.f / std::max(gs.swamp_mist_height_m, 0.05f), gs.swamp_mist_start_m,
              gs.swamp_mist_max);
  glUniform1f(GLQ_LOC(kGlq, program, "sw_eye_h"), rs->camera_pos.y() / kMeter - kWaterY);
  float c[3];
  mist_colour(rs, gs.swamp_mist_bright, c);
  glUniform3f(GLQ_LOC(kGlq, program, "sw_mist_col"), c[0], c[1], c[2]);
  math::Vector3f sdir, scol;
  const bool sun = pbr_frame_sun(&sdir, &scol);
  const float smax = std::max({scol.x(), scol.y(), scol.z(), 1e-3f});
  glUniform4f(GLQ_LOC(kGlq, program, "sw_sun"), sdir.x(), sdir.y(), sdir.z(),
              on && sun ? gs.swamp_mist_sun : 0.f);
  glUniform3f(GLQ_LOC(kGlq, program, "sw_sun_col"), scol.x() / smax, scol.y() / smax,
              scol.z() / smax);
}

bool swamp_mist_wanted(SharedRenderState* rs, bool after_ocean) {
  return mist_row(rs, after_ocean) != nullptr;
}

bool swamp_mist_uniforms(SharedRenderState* rs, GLuint id) {
  // the composite runs before both mist passes, so either row's mist lies over its pixels
  const MistRow* row = mist_row(rs, false);
  row = row ? row : mist_row(rs, true);
  if (!row) {
    return false;
  }
  mist_ray_uniforms(rs, id, *row, mist_params(*row));
  return true;
}

SwampMist::~SwampMist() {
  if (m_vao) {
    glDeleteVertexArrays(1, &m_vao);
  }
}

void SwampMist::render(SharedRenderState* rs,
                       ScopedProfilerNode& /*prof*/,
                       SsaoRenderer& ssao,
                       bool after_ocean) {
  const MistRow* row = ssao.have_frame() ? mist_row(rs, after_ocean) : nullptr;
  if (!row) {
    return;
  }
  const MistParams mp = mist_params(*row);
  if (!row->sea) {
    // sprites take the swamp's mist only (swamp_mist_sprite_uniforms)
    g_mist_frame = rs->frame_idx;
  }

  // bind_view_inputs uses units 1-3
  static glq::SavedState saved("swamp-mist", kGlq, 0b1110, 0b1110);
  saved.capture();

  if (!m_vao) {
    glGenVertexArrays(1, &m_vao);
  }
  glBindVertexArray(m_vao);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, rs->render_fb);
  glViewport(0, 0, rs->render_fb_w, rs->render_fb_h);
  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_STENCIL_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glEnable(GL_BLEND);
  glBlendEquation(GL_FUNC_ADD);
  // destination alpha is the PS2 blend source of later draws: keep it
  glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);

  auto& sh = rs->shaders[ShaderId::SWAMP_MIST];
  sh.activate();
  const GLuint id = sh.id();
  ssao.bind_view_inputs(id, 1, 2, 3);

  mist_ray_uniforms(rs, id, *row, mp);
  float mc[3];
  mist_colour(rs, mp.bright, mc);
  glUniform3f(GLQ_LOC(kGlq, id, "mist_col"), mc[0], mc[1], mc[2]);
  math::Vector3f sdir, scol;
  const bool sun = pbr_frame_sun(&sdir, &scol);
  const float smax = std::max({scol.x(), scol.y(), scol.z(), 1e-3f});
  glUniform3f(GLQ_LOC(kGlq, id, "sun_dir"), sdir.x(), sdir.y(), sdir.z());
  glUniform3f(GLQ_LOC(kGlq, id, "sun_col"), scol.x() / smax, scol.y() / smax,
              scol.z() / smax);
  glUniform1f(GLQ_LOC(kGlq, id, "sun_k"), sun ? mp.sun : 0.f);
  glDrawArrays(GL_TRIANGLES, 0, 3);

  saved.restore();
}
