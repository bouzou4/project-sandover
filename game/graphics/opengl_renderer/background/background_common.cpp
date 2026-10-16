

#include "background_common.h"

#include <algorithm>
#include <cmath>

#include "common/util/os.h"
#include "common/util/simd_util.h"

#include "game/graphics/opengl_renderer/ActorShadows.h"
#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/opengl_renderer/GlQueries.h"
#include "game/graphics/opengl_renderer/Materials.h"
#include "game/graphics/opengl_renderer/SsaoRenderer.h"
#include "game/graphics/pipelines/opengl.h"

DoubleDraw setup_opengl_from_draw_mode(DrawMode mode, u32 tex_unit, bool mipmap) {
  glActiveTexture(tex_unit);

  if (mode.get_zt_enable()) {
    glEnable(GL_DEPTH_TEST);
    switch (mode.get_depth_test()) {
      case GsTest::ZTest::NEVER:
        glDepthFunc(GL_NEVER);
        break;
      case GsTest::ZTest::ALWAYS:
        glDepthFunc(GL_ALWAYS);
        break;
      case GsTest::ZTest::GEQUAL:
        glDepthFunc(GL_GEQUAL);
        break;
      case GsTest::ZTest::GREATER:
        glDepthFunc(GL_GREATER);
        break;
      default:
        ASSERT(false);
    }
  } else {
    glDisable(GL_DEPTH_TEST);
  }

  DoubleDraw double_draw;

  bool should_enable_blend = false;
  if (mode.get_ab_enable() && mode.get_alpha_blend() != DrawMode::AlphaBlend::DISABLED) {
    should_enable_blend = true;
    switch (mode.get_alpha_blend()) {
      case DrawMode::AlphaBlend::SRC_SRC_SRC_SRC:
        should_enable_blend = false;
        // (SRC - SRC) * alpha + SRC = SRC, no blend.
        break;
      case DrawMode::AlphaBlend::SRC_DST_SRC_DST:
        glBlendEquation(GL_FUNC_ADD);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
        break;
      case DrawMode::AlphaBlend::SRC_0_SRC_DST:
        glBlendEquation(GL_FUNC_ADD);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ZERO);
        break;
      case DrawMode::AlphaBlend::SRC_0_FIX_DST:
        glBlendEquation(GL_FUNC_ADD);
        glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ZERO);
        break;
      case DrawMode::AlphaBlend::SRC_DST_FIX_DST:
        // Cv = (Cs - Cd) * FIX + Cd
        // Cs * FIX * 0.5
        // Cd * FIX * 0.5
        glBlendEquation(GL_FUNC_ADD);
        glBlendFuncSeparate(GL_CONSTANT_COLOR, GL_CONSTANT_COLOR, GL_ONE, GL_ZERO);
        glBlendColor(0.5, 0.5, 0.5, 0.5);
        break;
      case DrawMode::AlphaBlend::ZERO_SRC_SRC_DST:
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ZERO);
        glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
        break;
      case DrawMode::AlphaBlend::SRC_0_DST_DST:
        glBlendFunc(GL_DST_ALPHA, GL_ONE);
        glBlendEquation(GL_FUNC_ADD);
        double_draw.color_mult = 0.5f;
        break;
      default:
        ASSERT(false);
    }
  } else {
    should_enable_blend = false;
  }

  if (should_enable_blend) {
    glEnable(GL_BLEND);
  } else {
    glDisable(GL_BLEND);
  }
  // the normal target survives only replacing or plain alpha blending
  SsaoRenderer::set_normal_write(!should_enable_blend ||
                                 mode.get_alpha_blend() == DrawMode::AlphaBlend::SRC_DST_SRC_DST);

  if (mode.get_clamp_s_enable()) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  } else {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
  }

  if (mode.get_clamp_t_enable()) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  } else {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
  }

  if (mode.get_filt_enable()) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    mipmap ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  } else {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  }

  // for some reason, they set atest NEVER + FB_ONLY to disable depth writes
  bool alpha_hack_to_disable_z_write = false;

  float alpha_min = 0.;
  if (mode.get_at_enable()) {
    switch (mode.get_alpha_test()) {
      case DrawMode::AlphaTest::ALWAYS:
        break;
      case DrawMode::AlphaTest::GEQUAL:
        alpha_min = mode.get_aref() / 127.f;
        switch (mode.get_alpha_fail()) {
          case GsTest::AlphaFail::KEEP:
            // ok, no need for double draw
            break;
          case GsTest::AlphaFail::FB_ONLY:
            if (mode.get_depth_write_enable()) {
              // darn, we need to draw twice
              double_draw.kind = DoubleDrawKind::AFAIL_NO_DEPTH_WRITE;
              double_draw.aref_second = alpha_min;
            } else {
              alpha_min = 0.f;
            }
            break;
          default:
            ASSERT(false);
        }
        break;
      case DrawMode::AlphaTest::NEVER:
        if (mode.get_alpha_fail() == GsTest::AlphaFail::FB_ONLY) {
          alpha_hack_to_disable_z_write = true;
        } else {
          ASSERT(false);
        }
        break;
      default:
        ASSERT(false);
    }
  }

  if (mode.get_depth_write_enable() && !alpha_hack_to_disable_z_write) {
    glDepthMask(GL_TRUE);
  } else {
    glDepthMask(GL_FALSE);
  }
  double_draw.aref_first = alpha_min;
  return double_draw;
}

DoubleDraw setup_tfrag_shader(SharedRenderState* render_state, DrawMode mode, ShaderId shader) {
  auto draw_settings = setup_opengl_from_draw_mode(mode, GL_TEXTURE0, true);
  auto sh_id = render_state->shaders[shader].id();
  if (auto u_id = GLQ_LOC(PERF_OPT_GEOMETRY + 2, sh_id, "alpha_min"); u_id != -1) {
    glUniform1f(u_id, draw_settings.aref_first);
  }
  if (auto u_id = GLQ_LOC(PERF_OPT_GEOMETRY + 2, sh_id, "alpha_max"); u_id != -1) {
    glUniform1f(u_id, 10.f);
  }
  return draw_settings;
}

std::array<math::Vector4f, 4> make_new_cam_mat(const math::Vector4f cam_T_w[4],
                                               const math::Vector4f persp[4],
                                               float fog_constant,
                                               float hvdf_z) {
  // renderers may eventually have tricks to do things in local coordinates - so use the convention
  // that the shader has already subtracted off the camera translation from the vertex position.
  // (I think this could help with accuracy too, since you aren't rotating and subtracting two large
  // vectors that are very close to each other)

  // this is the perspective x-scaling. This is used to map to a 256-pixel buffer.
  const float game_pxx = persp[0][0];
  // on PC, OpenGL uses normalized coordinates for drawing, so divide by the pixel width.
  // in the game, the perspective divide includes a multiplication by the fog constant, for PC,
  // just include this multiply here so we can let OpenGL do the perspective multiply.
  const float pc_pxx = fog_constant * game_pxx / 256.f;

  // this is the perspective y-scaling.
  const float game_pyy = persp[1][1];
  // same logic as y - there's a later SCISSOR scaling in the shader that expects this ratio.
  const float pc_pyy = -fog_constant * game_pyy / 128.f;

  // the depth is considered twice. Once, as the value to write into the depth buffer, which is
  // scaled for PC here:
  const float depth_scale = fog_constant * persp[2][2] / 8388608;

  // and once as the value used for perspective divide
  const float game_pzw = persp[2][3];
  const float game_depth_offset = persp[3][2];

  // set up PC scaling values
  math::Vector3f persp_scale(pc_pxx, pc_pyy, depth_scale);

  // it turns out that shifting the depth buffer to line up with OpenGL is equivalent to adding
  // transformed.w * (hvdf_z / 8388608.f - 1.f) to the depth value. We know that w is just depth *
  // pzw, so we can include the effect here:
  const float pc_z_offset = (hvdf_z / 8388608.f - 1.f);
  persp_scale.z() += pc_z_offset * game_pzw;

  std::array<math::Vector4f, 4> result;
  for (auto& x : result) {
    x.set_zero();
  }

  // fill out the upper 3x3 - simply scale the rotation matrix by the perspective scale.
  for (int row = 0; row < 3; row++) {
    for (int col = 0; col < 3; col++) {
      result[row][col] = cam_T_w[row][col] * persp_scale[col];
    }
  }

  // fill out the right most column. This converts world-space points to depth for divide, scaled by
  // pzw. for now, copy the game.
  for (int row = 0; row < 3; row++) {
    result[row][3] = cam_T_w[row][2] * game_pzw;
  }

  // depth buffer offset - now needs to be scaled by the PC depth buffer scaling too
  result[3][2] = fog_constant * game_depth_offset / 8388608;

  return result;
}

void first_tfrag_draw_setup(const GoalBackgroundCameraData& in_settings,
                            SharedRenderState* render_state,
                            ShaderId shader) {
  const auto& gs = Gfx::g_global_settings;
  const int fog_mode = render_state->vista_frame_fog;
  GoalBackgroundCameraData pushed;
  const GoalBackgroundCameraData* sp = &in_settings;
  if (fog_mode == VISTA_FOG_PUSH_OUT) {
    pushed = vista_fog_camera(in_settings, gs.vista_fog_end_m);
    sp = &pushed;
  }
  const GoalBackgroundCameraData& settings = *sp;
  const auto& sh = render_state->shaders[shader];
  sh.activate();
  auto id = sh.id();
  glUniform1i(GLQ_LOC(PERF_OPT_GEOMETRY + 2, id, "gfx_hack_no_tex"),
              Gfx::g_global_settings.hack_no_tex);
  glUniform1i(GLQ_LOC(PERF_OPT_GEOMETRY + 2, id, "decal"), false);
  glUniform1i(GLQ_LOC(PERF_OPT_GEOMETRY + 2, id, "tex_T0"), 0);
  glUniformMatrix4fv(GLQ_LOC(PERF_OPT_GEOMETRY + 2, id, "camera"), 1, GL_FALSE,
                     settings.camera[0].data());
  SsaoRenderer::set_normal_mode(id, 1);
  if (pbr_was_used()) {
    glUniform1i(GLQ_LOC(PERF_OPT_GEOMETRY + 2, id, "pbr_on"), 0);
  }

  auto newcam =
      make_new_cam_mat(settings.rot, settings.perspective, settings.fog.x(), settings.hvdf_off.z());

  /*
  fmt::print("camera:\n{}\n{}\n{}\n{}\n", settings.camera[0].to_string_aligned(),
             settings.camera[1].to_string_aligned(), settings.camera[2].to_string_aligned(),
             settings.camera[3].to_string_aligned());

  fmt::print("camera2:\n{}\n{}\n{}\n{}\n", newcam[0].to_string_aligned(),
             newcam[1].to_string_aligned(), newcam[2].to_string_aligned(),
             newcam[3].to_string_aligned());

  fmt::print("persp:\n{}\n{}\n{}\n{}\n", settings.perspective[0].to_string_aligned(),
             settings.perspective[1].to_string_aligned(),
             settings.perspective[2].to_string_aligned(),
             settings.perspective[3].to_string_aligned());
  fmt::print("rot:\n{}\n{}\n{}\n{}\n", settings.rot[0].to_string_aligned(),
             settings.rot[1].to_string_aligned(), settings.rot[2].to_string_aligned(),
             settings.rot[3].to_string_aligned());
  fmt::print("ctrans: {}\n", settings.trans.to_string_aligned());
  fmt::print("hvdf: {}\n", settings.hvdf_off.to_string_aligned());
  */

  glUniformMatrix4fv(GLQ_LOC(PERF_OPT_GEOMETRY + 2, id, "pc_camera"), 1, GL_FALSE,
                     newcam[0].data());
  actor_shadows_set_uniforms(render_state, id, false);

  glUniform4f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, id, "hvdf_offset"), settings.hvdf_off[0],
              settings.hvdf_off[1], settings.hvdf_off[2], settings.hvdf_off[3]);
  glUniform4f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, id, "cam_trans"), settings.trans[0], settings.trans[1],
              settings.trans[2], settings.trans[3]);
  glUniform1f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, id, "fog_constant"), settings.fog.x());
  glUniform1f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, id, "fog_min"), settings.fog.y());
  glUniform1f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, id, "fog_max"), settings.fog.z());
  const VistaNeighbourFog* nb =
      fog_mode == VISTA_FOG_UNIFIED ? render_state->vista_nb_fog : nullptr;
  const float* tint = nb ? nb->tint : nullptr;
  glUniform4f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, id, "fog_color"), render_state->fog_color[0] / 255.f,
              render_state->fog_color[1] / 255.f, render_state->fog_color[2] / 255.f,
              render_state->fog_intensity / 255);
  // the shaders apply the tint by the share of the fog it owns (atmo_combine)
  glUniform3f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, id, "fog_tint"), tint ? tint[0] - 1.f : 0.f,
              tint ? tint[1] - 1.f : 0.f, tint ? tint[2] - 1.f : 0.f);
  // x: 1 on, 2 on and at least the game's fog; y: start (m), z: density (1/m), w: 1 / scale height
  if (fog_mode == VISTA_FOG_UNIFIED) {
    const float h = gs.vista_atmo_height_m;
    const float density = gs.vista_atmo_density_per_km * (nb ? nb->density_scale : 1.f);
    glUniform4f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, id, "atmo"), gs.vista_fog_floor ? 2.f : 1.f,
                gs.vista_atmo_start_m, density / 1000.f, h > 0.f ? 1.f / h : 0.f);
    if (nb && nb->max_fog >= 0.f) {
      // atmo_fog() tops out at 255 - fog_min
      glUniform1f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, id, "fog_min"),
                  255.f * (1.f - std::min(nb->max_fog, 1.f)));
    }
    const float pzw = std::abs(settings.perspective[2][3]);
    glUniform1f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, id, "atmo_wscale"),
                pzw > 0.f ? 1.f / (pzw * 4096.f) : 0.f);
    vista_haze_uniforms(render_state, id, nb ? nb->haze_scale : 1.f);
    render_state->atmo_set_shaders |= 1ull << (int)shader;
  } else if (render_state->atmo_set_shaders & (1ull << (int)shader)) {
    glUniform4f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, id, "atmo"), 0.f, 0.f, 0.f, 0.f);
    render_state->atmo_set_shaders &= ~(1ull << (int)shader);
  }
}

void interp_time_of_day_slow(const math::Vector<s32, 4> itimes[4],
                             const tfrag3::PackedTimeOfDay& in,
                             math::Vector<u8, 4>* out) {
  math::Vector<u16, 4> weights[8];
  for (int component = 0; component < 8; component++) {
    int quad_idx = component / 2;
    int word_off = (component % 2 * 2);
    for (int channel = 0; channel < 4; channel++) {
      int word = word_off + (channel / 2);
      int hw_off = channel % 2;

      u32 word_val = itimes[quad_idx][word];
      u32 hw_val = hw_off ? (word_val >> 16) : word_val;
      hw_val = hw_val & 0xff;
      weights[component][channel] = hw_val;
    }
  }

  math::Vector<u16, 4> temp[4];

  for (u32 color_quad = 0; color_quad < (in.color_count + 3) / 4; color_quad++) {
    for (auto& x : temp) {
      x.set_zero();
    }

    const u8* input_ptr = in.data.data() + color_quad * 128;
    for (u32 component = 0; component < 8; component++) {
      for (u32 color = 0; color < 4; color++) {
        for (u32 channel = 0; channel < 4; channel++) {
          temp[color][channel] += weights[component][channel] * (*input_ptr);
          input_ptr++;
        }
      }
    }

    for (u32 color = 0; color < 4; color++) {
      auto& o = out[color_quad * 4 + color];
      for (u32 channel = 0; channel < 3; channel++) {
        o[channel] = std::min(255, temp[color][channel] >> 6);
      }
      o[3] = std::min(128, temp[color][3] >> 6);
    }
  }
}

void interp_time_of_day(const math::Vector<s32, 4> itimes[4],
                        const tfrag3::PackedTimeOfDay& packed_colors,
                        math::Vector<u8, 4>* out) {
  math::Vector<u16, 4> weights[8];
  for (int component = 0; component < 8; component++) {
    int quad_idx = component / 2;
    int word_off = (component % 2 * 2);
    for (int channel = 0; channel < 4; channel++) {
      int word = word_off + (channel / 2);
      int hw_off = channel % 2;

      u32 word_val = itimes[quad_idx][word];
      u32 hw_val = hw_off ? (word_val >> 16) : word_val;
      hw_val = hw_val & 0xff;
      weights[component][channel] = hw_val;
    }
  }

  // weight multipliers
  __m128i weights0 = _mm_setr_epi16(weights[0][0], weights[0][1], weights[0][2], weights[0][3],
                                    weights[0][0], weights[0][1], weights[0][2], weights[0][3]);
  __m128i weights1 = _mm_setr_epi16(weights[1][0], weights[1][1], weights[1][2], weights[1][3],
                                    weights[1][0], weights[1][1], weights[1][2], weights[1][3]);
  __m128i weights2 = _mm_setr_epi16(weights[2][0], weights[2][1], weights[2][2], weights[2][3],
                                    weights[2][0], weights[2][1], weights[2][2], weights[2][3]);
  __m128i weights3 = _mm_setr_epi16(weights[3][0], weights[3][1], weights[3][2], weights[3][3],
                                    weights[3][0], weights[3][1], weights[3][2], weights[3][3]);
  __m128i weights4 = _mm_setr_epi16(weights[4][0], weights[4][1], weights[4][2], weights[4][3],
                                    weights[4][0], weights[4][1], weights[4][2], weights[4][3]);
  __m128i weights5 = _mm_setr_epi16(weights[5][0], weights[5][1], weights[5][2], weights[5][3],
                                    weights[5][0], weights[5][1], weights[5][2], weights[5][3]);
  __m128i weights6 = _mm_setr_epi16(weights[6][0], weights[6][1], weights[6][2], weights[6][3],
                                    weights[6][0], weights[6][1], weights[6][2], weights[6][3]);
  __m128i weights7 = _mm_setr_epi16(weights[7][0], weights[7][1], weights[7][2], weights[7][3],
                                    weights[7][0], weights[7][1], weights[7][2], weights[7][3]);

  // saturation: note that alpha is saturated to 128 but the rest are 255.
  // TODO: maybe we should saturate to 255 for everybody (can do this using a single packus) and
  // change the shader to deal with this.
  __m128i sat = _mm_set_epi16(128, 255, 255, 255, 128, 255, 255, 255);

  for (u32 color_quad = 0; color_quad < packed_colors.color_count / 4; color_quad++) {
    // first, load colors. We put 16 bytes / register and don't touch the upper half because we
    // convert u8s to u16s.
    {
      const u8* base = packed_colors.data.data() + color_quad * 128;
      __m128i color0_p = _mm_loadu_si64((const __m128i*)(base + 0));
      __m128i color1_p = _mm_loadu_si64((const __m128i*)(base + 16));
      __m128i color2_p = _mm_loadu_si64((const __m128i*)(base + 32));
      __m128i color3_p = _mm_loadu_si64((const __m128i*)(base + 48));
      __m128i color4_p = _mm_loadu_si64((const __m128i*)(base + 64));
      __m128i color5_p = _mm_loadu_si64((const __m128i*)(base + 80));
      __m128i color6_p = _mm_loadu_si64((const __m128i*)(base + 96));
      __m128i color7_p = _mm_loadu_si64((const __m128i*)(base + 112));

      // unpack to 16-bits. each has 16x 16 bit colors.
      __m128i color0 = _mm_cvtepu8_epi16(color0_p);
      __m128i color1 = _mm_cvtepu8_epi16(color1_p);
      __m128i color2 = _mm_cvtepu8_epi16(color2_p);
      __m128i color3 = _mm_cvtepu8_epi16(color3_p);
      __m128i color4 = _mm_cvtepu8_epi16(color4_p);
      __m128i color5 = _mm_cvtepu8_epi16(color5_p);
      __m128i color6 = _mm_cvtepu8_epi16(color6_p);
      __m128i color7 = _mm_cvtepu8_epi16(color7_p);

      // multiply by weights
      color0 = _mm_mullo_epi16(color0, weights0);
      color1 = _mm_mullo_epi16(color1, weights1);
      color2 = _mm_mullo_epi16(color2, weights2);
      color3 = _mm_mullo_epi16(color3, weights3);
      color4 = _mm_mullo_epi16(color4, weights4);
      color5 = _mm_mullo_epi16(color5, weights5);
      color6 = _mm_mullo_epi16(color6, weights6);
      color7 = _mm_mullo_epi16(color7, weights7);

      // add. This order minimizes dependencies.
      color0 = _mm_adds_epi16(color0, color1);
      color2 = _mm_adds_epi16(color2, color3);
      color4 = _mm_adds_epi16(color4, color5);
      color6 = _mm_adds_epi16(color6, color7);

      color0 = _mm_adds_epi16(color0, color2);
      color4 = _mm_adds_epi16(color4, color6);

      color0 = _mm_adds_epi16(color0, color4);

      // divide, because we multiplied our weights by 2^7.
      color0 = _mm_srli_epi16(color0, 6);

      // saturate
      color0 = _mm_min_epu16(sat, color0);

      // back to u8s.
      auto result = _mm_packus_epi16(color0, color0);

      // store result
      _mm_storel_epi64((__m128i*)(&out[color_quad * 4]), result);
    }

    {
      const u8* base = packed_colors.data.data() + color_quad * 128 + 8;
      __m128i color0_p = _mm_loadu_si64((const __m128i*)(base + 0));
      __m128i color1_p = _mm_loadu_si64((const __m128i*)(base + 16));
      __m128i color2_p = _mm_loadu_si64((const __m128i*)(base + 32));
      __m128i color3_p = _mm_loadu_si64((const __m128i*)(base + 48));
      __m128i color4_p = _mm_loadu_si64((const __m128i*)(base + 64));
      __m128i color5_p = _mm_loadu_si64((const __m128i*)(base + 80));
      __m128i color6_p = _mm_loadu_si64((const __m128i*)(base + 96));
      __m128i color7_p = _mm_loadu_si64((const __m128i*)(base + 112));

      // unpack to 16-bits. each has 16x 16 bit colors.
      __m128i color0 = _mm_cvtepu8_epi16(color0_p);
      __m128i color1 = _mm_cvtepu8_epi16(color1_p);
      __m128i color2 = _mm_cvtepu8_epi16(color2_p);
      __m128i color3 = _mm_cvtepu8_epi16(color3_p);
      __m128i color4 = _mm_cvtepu8_epi16(color4_p);
      __m128i color5 = _mm_cvtepu8_epi16(color5_p);
      __m128i color6 = _mm_cvtepu8_epi16(color6_p);
      __m128i color7 = _mm_cvtepu8_epi16(color7_p);

      // multiply by weights
      color0 = _mm_mullo_epi16(color0, weights0);
      color1 = _mm_mullo_epi16(color1, weights1);
      color2 = _mm_mullo_epi16(color2, weights2);
      color3 = _mm_mullo_epi16(color3, weights3);
      color4 = _mm_mullo_epi16(color4, weights4);
      color5 = _mm_mullo_epi16(color5, weights5);
      color6 = _mm_mullo_epi16(color6, weights6);
      color7 = _mm_mullo_epi16(color7, weights7);

      // add. This order minimizes dependencies.
      color0 = _mm_adds_epi16(color0, color1);
      color2 = _mm_adds_epi16(color2, color3);
      color4 = _mm_adds_epi16(color4, color5);
      color6 = _mm_adds_epi16(color6, color7);

      color0 = _mm_adds_epi16(color0, color2);
      color4 = _mm_adds_epi16(color4, color6);

      color0 = _mm_adds_epi16(color0, color4);

      // divide, because we multiplied our weights by 2^7.
      color0 = _mm_srli_epi16(color0, 6);

      // saturate
      color0 = _mm_min_epu16(sat, color0);

      // back to u8s.
      auto result = _mm_packus_epi16(color0, color0);

      // store result
      _mm_storel_epi64((__m128i*)(&out[color_quad * 4 + 2]), result);
    }
  }
}

bool sphere_in_view_ref(const math::Vector4f& sphere, const math::Vector4f* planes) {
  math::Vector4f acc =
      planes[0] * sphere.x() + planes[1] * sphere.y() + planes[2] * sphere.z() - planes[3];

  return acc.x() > -sphere.w() && acc.y() > -sphere.w() && acc.z() > -sphere.w() &&
         acc.w() > -sphere.w();
}

// this isn't super efficient, but we spend so little time here it's not worth it to go faster.
void cull_check_all_slow(const math::Vector4f* planes,
                         const std::vector<tfrag3::VisNode>& nodes,
                         const u8* level_occlusion_string,
                         u8* out) {
  if (level_occlusion_string) {
    for (size_t i = 0; i < nodes.size(); i++) {
      u16 my_id = nodes[i].my_id;
      bool not_occluded =
          my_id != 0xffff && level_occlusion_string[my_id / 8] & (1 << (7 - (my_id & 7)));
      out[i] = not_occluded && sphere_in_view_ref(nodes[i].bsphere, planes);
    }
  } else {
    for (size_t i = 0; i < nodes.size(); i++) {
      out[i] = sphere_in_view_ref(nodes[i].bsphere, planes);
    }
  }
}

void make_all_visible_multidraws(std::pair<int, int>* draw_ptrs_out,
                                 GLsizei* counts_out,
                                 void** index_offsets_out,
                                 const std::vector<tfrag3::ShrubDraw>& draws) {
  u64 md_idx = 0;
  for (size_t i = 0; i < draws.size(); i++) {
    const auto& draw = draws[i];
    u64 iidx = draw.first_index_index;
    std::pair<int, int> ds;
    ds.first = md_idx;
    ds.second = 1;
    counts_out[md_idx] = draw.num_indices;
    index_offsets_out[md_idx] = (void*)(iidx * sizeof(u32));
    md_idx++;
    draw_ptrs_out[i] = ds;
  }
}

u32 make_all_visible_multidraws(std::pair<int, int>* draw_ptrs_out,
                                GLsizei* counts_out,
                                void** index_offsets_out,
                                const std::vector<tfrag3::StripDraw>& draws) {
  u64 md_idx = 0;
  u32 num_tris = 0;
  for (size_t i = 0; i < draws.size(); i++) {
    const auto& draw = draws[i];
    u64 iidx = draw.unpacked.idx_of_first_idx_in_full_buffer;
    std::pair<int, int> ds;
    ds.first = md_idx;
    ds.second = 1;
    int num_inds = 0;
    for (auto& grp : draw.vis_groups) {
      num_tris += grp.num_tris;
      num_inds += grp.num_inds;
    }
    counts_out[md_idx] = num_inds;
    index_offsets_out[md_idx] = (void*)(iidx * sizeof(u32));
    draw_ptrs_out[i] = ds;
    md_idx++;
  }
  return num_tris;
}

u32 make_all_visible_index_list(std::pair<int, int>* group_out,
                                u32* idx_out,
                                const std::vector<tfrag3::ShrubDraw>& draws,
                                const u32* idx_in) {
  int idx_buffer_ptr = 0;
  for (size_t i = 0; i < draws.size(); i++) {
    const auto& draw = draws[i];
    std::pair<int, int> ds;
    ds.first = idx_buffer_ptr;
    memcpy(&idx_out[idx_buffer_ptr], idx_in + draw.first_index_index,
           draw.num_indices * sizeof(u32));
    idx_buffer_ptr += draw.num_indices;
    ds.second = idx_buffer_ptr - ds.first;
    group_out[i] = ds;
  }
  return idx_buffer_ptr;
}

u32 make_multidraws_from_vis_string(std::pair<int, int>* draw_ptrs_out,
                                    GLsizei* counts_out,
                                    void** index_offsets_out,
                                    const std::vector<tfrag3::StripDraw>& draws,
                                    const std::vector<u8>& vis_data) {
  u64 md_idx = 0;
  u32 num_tris = 0;
  u32 sanity_check = 0;
  for (size_t i = 0; i < draws.size(); i++) {
    const auto& draw = draws[i];
    u64 iidx = draw.unpacked.idx_of_first_idx_in_full_buffer;
    ASSERT(sanity_check == iidx);
    std::pair<int, int> ds;
    ds.first = md_idx;
    ds.second = 0;
    bool building_run = false;
    u64 run_start = 0;
    for (auto& grp : draw.vis_groups) {
      sanity_check += grp.num_inds;
      bool vis = grp.vis_idx_in_pc_bvh == UINT16_MAX || vis_data[grp.vis_idx_in_pc_bvh];
      if (vis) {
        num_tris += grp.num_tris;
      }

      if (building_run) {
        if (!vis) {
          building_run = false;
          counts_out[md_idx] = iidx - run_start;
          index_offsets_out[md_idx] = (void*)(run_start * sizeof(u32));
          ds.second++;
          md_idx++;
        }
      } else {
        if (vis) {
          building_run = true;
          run_start = iidx;
        }
      }

      iidx += grp.num_inds;
    }

    if (building_run) {
      building_run = false;
      counts_out[md_idx] = iidx - run_start;
      index_offsets_out[md_idx] = (void*)(run_start * sizeof(u32));
      ds.second++;
      md_idx++;
    }

    draw_ptrs_out[i] = ds;
  }
  return num_tris;
}

u32 make_multidraws_from_vis_and_proto_string(std::pair<int, int>* draw_ptrs_out,
                                              GLsizei* counts_out,
                                              void** index_offsets_out,
                                              const std::vector<tfrag3::StripDraw>& draws,
                                              const std::vector<u8>& vis_data,
                                              const std::vector<u8>& proto_vis_data) {
  u64 md_idx = 0;
  u32 num_tris = 0;
  u32 sanity_check = 0;
  for (size_t i = 0; i < draws.size(); i++) {
    const auto& draw = draws[i];
    u64 iidx = draw.unpacked.idx_of_first_idx_in_full_buffer;
    ASSERT(sanity_check == iidx);
    std::pair<int, int> ds;
    ds.first = md_idx;
    ds.second = 0;
    bool building_run = false;
    u64 run_start = 0;
    for (auto& grp : draw.vis_groups) {
      sanity_check += grp.num_inds;
      bool vis = (grp.vis_idx_in_pc_bvh == UINT16_MAX || vis_data[grp.vis_idx_in_pc_bvh]) &&
                 proto_vis_data[grp.tie_proto_idx];
      if (vis) {
        num_tris += grp.num_tris;
      }

      if (building_run) {
        if (!vis) {
          building_run = false;
          counts_out[md_idx] = iidx - run_start;
          index_offsets_out[md_idx] = (void*)(run_start * sizeof(u32));
          ds.second++;
          md_idx++;
        }
      } else {
        if (vis) {
          building_run = true;
          run_start = iidx;
        }
      }

      iidx += grp.num_inds;
    }

    if (building_run) {
      building_run = false;
      counts_out[md_idx] = iidx - run_start;
      index_offsets_out[md_idx] = (void*)(run_start * sizeof(u32));
      ds.second++;
      md_idx++;
    }

    draw_ptrs_out[i] = ds;
  }
  return num_tris;
}

u32 make_index_list_from_vis_string(std::pair<int, int>* group_out,
                                    u32* idx_out,
                                    const std::vector<tfrag3::StripDraw>& draws,
                                    const std::vector<u8>& vis_data,
                                    const u32* idx_in,
                                    u32* num_tris_out) {
  int idx_buffer_ptr = 0;
  u32 num_tris = 0;
  for (size_t i = 0; i < draws.size(); i++) {
    const auto& draw = draws[i];
    int vtx_idx = 0;
    std::pair<int, int> ds;
    ds.first = idx_buffer_ptr;
    bool building_run = false;
    int run_start_out = 0;
    int run_start_in = 0;
    for (auto& grp : draw.vis_groups) {
      bool vis = grp.vis_idx_in_pc_bvh == UINT16_MAX || vis_data[grp.vis_idx_in_pc_bvh];
      if (vis) {
        num_tris += grp.num_tris;
      }

      if (building_run) {
        if (vis) {
          idx_buffer_ptr += grp.num_inds;
        } else {
          building_run = false;
          memcpy(&idx_out[run_start_out],
                 idx_in + draw.unpacked.idx_of_first_idx_in_full_buffer + run_start_in,
                 (idx_buffer_ptr - run_start_out) * sizeof(u32));
        }
      } else {
        if (vis) {
          building_run = true;
          run_start_out = idx_buffer_ptr;
          run_start_in = vtx_idx;
          idx_buffer_ptr += grp.num_inds;
        }
      }
      vtx_idx += grp.num_inds;
    }

    if (building_run) {
      memcpy(&idx_out[run_start_out],
             idx_in + draw.unpacked.idx_of_first_idx_in_full_buffer + run_start_in,
             (idx_buffer_ptr - run_start_out) * sizeof(u32));
    }

    ds.second = idx_buffer_ptr - ds.first;
    group_out[i] = ds;
  }
  *num_tris_out = num_tris;
  return idx_buffer_ptr;
}

u32 make_index_list_from_vis_and_proto_string(std::pair<int, int>* group_out,
                                              u32* idx_out,
                                              const std::vector<tfrag3::StripDraw>& draws,
                                              const std::vector<u8>& vis_data,
                                              const std::vector<u8>& proto_vis_data,
                                              const u32* idx_in,
                                              u32* num_tris_out) {
  int idx_buffer_ptr = 0;
  u32 num_tris = 0;
  for (size_t i = 0; i < draws.size(); i++) {
    const auto& draw = draws[i];
    int vtx_idx = 0;
    std::pair<int, int> ds;
    ds.first = idx_buffer_ptr;
    bool building_run = false;
    int run_start_out = 0;
    int run_start_in = 0;
    for (auto& grp : draw.vis_groups) {
      bool vis = (grp.vis_idx_in_pc_bvh == UINT16_MAX || vis_data[grp.vis_idx_in_pc_bvh]) &&
                 proto_vis_data[grp.tie_proto_idx];
      if (vis) {
        num_tris += grp.num_tris;
      }

      if (building_run) {
        if (vis) {
          idx_buffer_ptr += grp.num_inds;
        } else {
          building_run = false;
          memcpy(&idx_out[run_start_out],
                 idx_in + draw.unpacked.idx_of_first_idx_in_full_buffer + run_start_in,
                 (idx_buffer_ptr - run_start_out) * sizeof(u32));
        }
      } else {
        if (vis) {
          building_run = true;
          run_start_out = idx_buffer_ptr;
          run_start_in = vtx_idx;
          idx_buffer_ptr += grp.num_inds;
        }
      }
      vtx_idx += grp.num_inds;
    }

    if (building_run) {
      memcpy(&idx_out[run_start_out],
             idx_in + draw.unpacked.idx_of_first_idx_in_full_buffer + run_start_in,
             (idx_buffer_ptr - run_start_out) * sizeof(u32));
    }

    ds.second = idx_buffer_ptr - ds.first;
    group_out[i] = ds;
  }
  *num_tris_out = num_tris;
  return idx_buffer_ptr;
}

u32 make_all_visible_index_list(std::pair<int, int>* group_out,
                                u32* idx_out,
                                const std::vector<tfrag3::StripDraw>& draws,
                                const u32* idx_in,
                                u32* num_tris_out) {
  int idx_buffer_ptr = 0;
  u32 num_tris = 0;
  for (size_t i = 0; i < draws.size(); i++) {
    const auto& draw = draws[i];
    std::pair<int, int> ds;
    ds.first = idx_buffer_ptr;
    u32 num_inds = 0;
    for (auto& grp : draw.vis_groups) {
      num_inds += grp.num_inds;
      num_tris += grp.num_tris;
    }
    memcpy(&idx_out[idx_buffer_ptr], idx_in + draw.unpacked.idx_of_first_idx_in_full_buffer,
           num_inds * sizeof(u32));
    idx_buffer_ptr += num_inds;
    ds.second = idx_buffer_ptr - ds.first;
    group_out[i] = ds;
  }
  *num_tris_out = num_tris;
  return idx_buffer_ptr;
}

namespace {
void apply_xr_eye(GoalBackgroundCameraData* cam, const EyeView& e) {
  // rot[i][j]: camera coordinate j of world axis i; eye rot' = rot * R^T, trans' = trans + rot t
  float m[3][3], mn[3][3];
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) {
      m[i][j] = cam->rot[i][j];
    }
  }
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) {
      mn[i][j] = m[i][0] * e.rot[j][0] + m[i][1] * e.rot[j][1] + m[i][2] * e.rot[j][2];
    }
  }
  const math::Vector4f old_trans = cam->trans;
  for (int i = 0; i < 3; i++) {
    cam->trans[i] += m[i][0] * e.t[0] + m[i][1] * e.t[1] + m[i][2] * e.t[2];
  }

  // side planes (inside: n.p - d > -r): widen to the eye's fov in camera space, then move
  for (int k = 0; k < 4; k++) {
    const float n[3] = {cam->planes[0][k], cam->planes[1][k], cam->planes[2][k]};
    const float at_cam = n[0] * old_trans[0] + n[1] * old_trans[1] + n[2] * old_trans[2] -
                         cam->planes[3][k];
    float nc[3];
    for (int j = 0; j < 3; j++) {
      nc[j] = m[0][j] * n[0] + m[1][j] * n[1] + m[2][j] * n[2];
    }
    const float len0 = std::sqrt(nc[0] * nc[0] + nc[1] * nc[1] + nc[2] * nc[2]);
    nc[2] /= std::abs(nc[0]) >= std::abs(nc[1]) ? e.fov_x : e.fov_y;
    const float len1 = std::sqrt(nc[0] * nc[0] + nc[1] * nc[1] + nc[2] * nc[2]);
    if (len1 > 0.f) {
      for (auto& x : nc) {
        x *= len0 / len1;
      }
    }
    float d = -at_cam;
    for (int i = 0; i < 3; i++) {
      const float nw = mn[i][0] * nc[0] + mn[i][1] * nc[1] + mn[i][2] * nc[2];
      cam->planes[i][k] = nw;
      d += nw * cam->trans[i];
    }
    cam->planes[3][k] = d;
  }

  eye_world_to_clip(cam->camera, cam->perspective, e);
  for (auto& row : cam->perspective) {
    row.x() *= e.fov_x;
    row.y() *= e.fov_y;
  }
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) {
      cam->rot[i][j] = mn[i][j];
    }
  }
}
}  // namespace

void apply_stereo_eye(GoalBackgroundCameraData* cam, SharedRenderState* rs) {
  auto& g = rs->game_camera;
  for (int i = 0; i < 4; i++) {
    g.planes[i] = cam->planes[i];
    g.matrix[i] = cam->camera[i];
    g.persp[i] = cam->perspective[i];
    g.rot[i] = cam->rot[i];
  }
  g.pos = cam->trans;
  g.hvdf_off = cam->hvdf_off;
  g.fog = cam->fog;
  g.valid = true;
  if (rs->eye.active) {
    apply_xr_eye(cam, rs->eye);
    return;
  }
  const float eye_x = rs->stereo_eye_x;
  if (eye_x == 0.f) {
    return;
  }
  // camera x axis in world space is column 0 of the (row-vector) rotation
  math::Vector3f right(cam->rot[0][0], cam->rot[1][0], cam->rot[2][0]);
  math::Vector3f delta = right * eye_x;
  for (int i = 0; i < 3; i++) {
    cam->trans[i] += delta[i];
  }
  cam->camera[3] -= cam->perspective[0] * eye_x;
  cam->planes[3] +=
      cam->planes[0] * delta[0] + cam->planes[1] * delta[1] + cam->planes[2] * delta[2];
}

void cave_dark_itimes(const SharedRenderState* state, TfragPcPortData* data) {
  const float f = state->shadow.bg_dark;
  if (!state->shadow.active || f >= 1.f || strncmp(data->level_name, "darkcave", 8) != 0) {
    return;
  }
  // slot 0: itimes[0] words 0 (r | g << 16) and 1 (b | a << 16), weight in each halfword's low byte
  auto scale = [&](u32 hw) {
    const u32 w = (u32)std::lround((hw & 0xff) * f);
    return (hw & 0xff00) | std::min(w, 0xffu);
  };
  u32 w0 = (u32)data->camera.itimes[0][0], w1 = (u32)data->camera.itimes[0][1];
  w0 = scale(w0 & 0xffff) | (scale(w0 >> 16) << 16);
  w1 = scale(w1 & 0xffff) | (w1 & 0xffff0000u);  // alpha unscaled
  data->camera.itimes[0][0] = (s32)w0;
  data->camera.itimes[0][1] = (s32)w1;
}

void update_render_state_from_pc_settings(SharedRenderState* state, const TfragPcPortData& data) {
  if (!state->has_pc_data) {
    for (int i = 0; i < 4; i++) {
      state->camera_planes[i] = data.camera.planes[i];
      state->camera_matrix[i] = data.camera.camera[i];
      state->camera_persp[i] = data.camera.perspective[i];
      state->camera_rot[i] = data.camera.rot[i];
    }
    state->camera_pos = data.camera.trans;
    state->camera_hvdf_off = data.camera.hvdf_off;
    state->camera_fog = data.camera.fog;
    state->has_pc_data = true;
  }
}

bool vista_hide_active(const SharedRenderState* render_state, int level_id) {
  const auto& gs = Gfx::g_global_settings;
  return level_id >= 0 && render_state->version == GameVersion::Jak1 && gs.vista_enable &&
         render_state->vista_drawn && gs.vista_hide_standins &&
         active_vista_filter(render_state)[0];
}

const char* active_vista_filter(const SharedRenderState* render_state) {
  const auto& gs = Gfx::g_global_settings;
  return gs.vista_neighbours ? render_state->vista_hide_filter.c_str() : gs.vista_hide;
}

bool vista_name_matches(const std::string& name, const char* filter) {
  std::string_view f(filter);
  const auto slash = name.rfind('/');
  const std::string_view base = slash == std::string::npos
                                    ? std::string_view(name)
                                    : std::string_view(name).substr(slash + 1);
  while (!f.empty()) {
    auto comma = f.find(',');
    auto tok = f.substr(0, comma);
    if (!tok.empty() && tok[0] == '=') {
      tok.remove_prefix(1);
      if (!tok.empty() && (tok == name || tok == base)) {
        return true;
      }
    } else if (!tok.empty() && name.find(tok) != std::string::npos) {
      return true;
    }
    if (comma == std::string_view::npos) {
      break;
    }
    f.remove_prefix(comma + 1);
  }
  return false;
}

void vista_haze_uniforms(const SharedRenderState* render_state, GLuint program, float scale) {
  const auto& gs = Gfx::g_global_settings;
  const float haze = gs.vista_neighbours ? gs.vista_haze_density_per_km * scale : 0.f;
  glUniform4f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "haze"), haze / 1000.f, gs.vista_haze_start_m,
              gs.vista_haze_max, 0.f);
  float hc[3];
  for (int i = 0; i < 3; i++) {
    hc[i] = std::min(render_state->fog_color[i] / 255.f * gs.vista_haze_tint[i], 1.f);
  }
  glUniform3f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "haze_color"), hc[0], hc[1], hc[2]);
}

void vista_atmo_foreground_uniforms(const SharedRenderState* render_state, GLuint program) {
  const auto& gs = Gfx::g_global_settings;
  const bool on = render_state->vista_frame_fog == VISTA_FOG_UNIFIED;
  const float h = gs.vista_atmo_height_m;
  const float mode = on ? (gs.vista_fog_floor ? 2.f : 1.f) : 0.f;
  glUniform4f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "atmo"), mode, gs.vista_atmo_start_m,
              gs.vista_atmo_density_per_km / 1000.f, h > 0.f ? 1.f / h : 0.f);
  vista_haze_uniforms(render_state, program, 1.f);
  const auto& r = render_state->camera_rot;
  glUniform3f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "atmo_up"), r[1][0], r[1][1], r[1][2]);
  const auto& t = render_state->camera_pos;
  glUniform3f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "atmo_cam"), t[0], t[1], t[2]);
}

GoalBackgroundCameraData vista_fog_camera(const GoalBackgroundCameraData& in, float end_m) {
  GoalBackgroundCameraData out = in;
  const float pzw = in.perspective[2][3];
  const float fog_min = in.fog.y();
  const float fog_max = in.fog.z();
  if (pzw == 0.f || fog_min == fog_max) {
    return out;
  }
  // fog value is hvdf.w + pzw * z: fog_max at the start distance, fog_min at the end
  const float start = (fog_max - in.hvdf_off.w()) / pzw;
  const float end = end_m * 4096.f;
  if (end <= start) {
    return out;
  }
  const float new_pzw = (fog_min - fog_max) / (end - start);
  const float r = new_pzw / pzw;
  out.perspective[2][3] = new_pzw;
  out.fog.x() *= r;
  for (auto& row : out.camera) {
    row.w() *= r;
  }
  out.hvdf_off.w() = fog_max - new_pzw * start;
  return out;
}
