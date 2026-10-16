#include "InlandWater.h"

#include <algorithm>
#include <unordered_map>

#include "common/log/log.h"

#include "game/graphics/gfx.h"
#include "game/graphics/opengl_renderer/GlQueries.h"
#include "game/graphics/opengl_renderer/Materials.h"
#include "game/graphics/opengl_renderer/loader/Loader.h"

namespace inland_water {
namespace {

struct Row {
  u32 combo;
  int kind;  // 1 clear, 2 mud
  bool sky;
};

constexpr Row kRows[] = {
#include "InlandWater.inc"
};

const std::unordered_map<u32, int>& table() {
  static const std::unordered_map<u32, int> t = [] {
    std::unordered_map<u32, int> m;
    for (const auto& r : kRows) {
      m[r.combo] = r.kind | (r.sky ? SKY : 0);
    }
    return m;
  }();
  return t;
}

GLuint g_csz = 0;
float g_proj[4] = {};
bool g_used[2] = {};  // generic, merc: the program got the rule once; reset when it turns off

int with_settings(int k) {
  const auto& gs = Gfx::g_global_settings;
  if ((k & CLEAR) && !gs.inland_water) {
    return NONE;
  }
  if ((k & MUD) && !gs.inland_mud) {
    return NONE;
  }
  return k;
}
}  // namespace

int kind_for_combo(u32 combo) {
  const auto& t = table();
  auto it = t.find(combo);
  return it == t.end() ? NONE : it->second;
}

bool active(const SharedRenderState* rs, bool merc) {
  const auto& gs = Gfx::g_global_settings;
  return pbr_active(rs) && (merc ? gs.pbr_merc : gs.pbr_background) &&
         (merc ? (gs.inland_water || gs.inland_mud) : gs.inland_water);
}

bool needs_setup(const SharedRenderState* rs, bool merc) {
  return g_used[merc ? 1 : 0] || active(rs, merc);
}

int merc_kind(const SharedRenderState* rs, const LevelData* lev, s32 texture) {
  if (!active(rs, true) || texture < 0 || !lev || !lev->level ||
      texture >= (s32)lev->level->textures.size()) {
    return NONE;
  }
  return with_settings(kind_for_combo(lev->level->textures[texture].combo_id));
}

bool replaces_envmap(const SharedRenderState* rs, const LevelData* lev, s32 texture) {
  const int k = merc_kind(rs, lev, texture);
  return (k & CLEAR) && (k & SKY);
}

void set_depth(GLuint csz, const float* proj_info) {
  g_csz = csz;
  if (proj_info) {
    std::copy(proj_info, proj_info + 4, g_proj);
  }
}

GLint setup(SharedRenderState* rs, GLuint program, bool merc, const LevelData* lev) {
  constexpr int kBit = PERF_OPT_GEOMETRY + 2;
  bool& used = g_used[merc ? 1 : 0];
  if (!active(rs, merc) || Gfx::g_global_settings.inland_water_picked) {
    if (used) {
      glUniform1i(GLQ_LOC(kBit, program, "iw_kind"), 0);
      used = false;
    }
    return -1;
  }
  used = true;
  const auto& gs = Gfx::g_global_settings;
  const GLint kind = GLQ_LOC(kBit, program, "iw_kind");
  glUniform1i(kind, 0);
  // fragment depth to camera z (m), as the SSAO depth copy was linearized
  const auto* persp = rs->camera_persp;
  const float fog = rs->camera_fog.x();
  glUniform3f(GLQ_LOC(kBit, program, "iw_lin"), fog * persp[3][2] / 8388608.f, persp[2][3],
              fog * persp[2][2] / 8388608.f +
                  (rs->camera_hvdf_off.z() / 8388608.f - 1.f) * persp[2][3]);
  glUniform4f(GLQ_LOC(kBit, program, "iw_proj"), g_proj[0], g_proj[1], g_proj[2], g_proj[3]);
  glUniform1i(GLQ_LOC(kBit, program, "iw_depth_ok"), g_csz ? 1 : 0);
  glUniform1i(GLQ_LOC(kBit, program, "iw_depth"), 19);
  glUniform1i(GLQ_LOC(kBit, program, "iw_view"), gs.inland_water_view);
  glActiveTexture(GL_TEXTURE19);
  glBindTexture(GL_TEXTURE_2D, g_csz);
  glActiveTexture(GL_TEXTURE0);
  glUniform4f(GLQ_LOC(kBit, program, "iw_water"), std::max(gs.inland_water_depth_m, 0.01f),
              gs.inland_water_refl, gs.inland_water_refl_cap, gs.inland_water_glint);
  if (merc) {
    glUniform4f(GLQ_LOC(kBit, program, "iw_mud"), gs.inland_mud_dark, gs.inland_mud_refl_cap,
                std::clamp(gs.inland_mud_rough, 0.05f, 1.f), gs.inland_mud_glint);
    // the sun of the water's own level (the frame's is the last background drawn, which can be
    // an enclosed neighbour with a fake overhead sun), to the merc camera frame as world up
    math::Vector3f dir, col;
    float day = 0;
    const bool sun =
        water_level_sun(lev ? lev->unbake.get() : nullptr, rs->frame_idx, &dir, &col, &day);
    math::Vector3f c(0.f, 0.f, 0.f);
    if (sun && rs->has_pc_data) {
      const auto* r = frame_camera(*rs).rot;
      for (int j = 0; j < 3; j++) {
        c[j] = (r[0][j] * dir[0] + r[1][j] * dir[1] + r[2][j] * dir[2]) * gs.pbr_merc_up;
      }
    }
    const bool ok = sun && c.length() > 1e-3f;
    if (ok) {
      c = c.normalized();
    }
    glUniform4f(GLQ_LOC(kBit, program, "iw_sun"), c.x(), c.y(), c.z(), ok ? 1.f : 0.f);
    glUniform3f(GLQ_LOC(kBit, program, "iw_sun_col"), col.x(), col.y(), col.z());
    glUniform4f(GLQ_LOC(kBit, program, "sun_glint"), water_sun_glint_strength(day),
                gs.water_sun_glint_rough, gs.water_sun_glint_peak, gs.water_sun_glint ? 1.f : 0.f);
    glUniform1f(GLQ_LOC(kBit, program, "tm_toon_knee"), gs.tonemap_toon_knee);
    if (gs.water_sun_glint && rs->has_pc_data) {
      // merc camera space to world, as LavaField's lava_c2w, for the glint's ripple field
      const auto& r = frame_camera(*rs).rot;
      float c2w[16] = {};
      for (int i = 0; i < 3; i++) {
        for (int k = 0; k < 3; k++) {
          c2w[k * 4 + i] = r[i][k];
        }
        c2w[12 + i] = -(r[i][0] * r[3][0] + r[i][1] * r[3][1] + r[i][2] * r[3][2]);
      }
      c2w[15] = 1.f;
      glUniformMatrix4fv(GLQ_LOC(kBit, program, "iw_c2w"), 1, GL_FALSE, c2w);
      glUniform1f(GLQ_LOC(kBit, program, "iw_time"),
                  gs.water_clock_s >= 0.f ? gs.water_clock_s
                                          : (float)(rs->frame_idx % 216000) / 60.f);
      if (gs.inland_water_view >= 2 && rs->frame_idx % 120 == 0) {
        lg::info("inland glint: sun {} world ({:.3f} {:.3f} {:.3f}) cam ({:.3f} {:.3f} {:.3f}) "
                 "rot0 ({:.3f} {:.3f} {:.3f}) rot1 ({:.3f} {:.3f} {:.3f}) rot2 ({:.3f} {:.3f} {:.3f})",
                 sun, dir.x(), dir.y(), dir.z(), c.x(), c.y(), c.z(), r[0][0], r[0][1], r[0][2],
                 r[1][0], r[1][1], r[1][2], r[2][0], r[2][1], r[2][2]);
      }
    }
  }
  return kind;
}

}  // namespace inland_water
