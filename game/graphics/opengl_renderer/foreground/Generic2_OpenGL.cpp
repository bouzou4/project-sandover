#include "common/log/log.h"

#include "Generic2.h"
#include "game/graphics/gfx.h"
#include "game/graphics/opengl_renderer/InlandWater.h"
#include "game/graphics/opengl_renderer/InspectRenderer.h"
#include "game/graphics/opengl_renderer/Materials.h"
#include "game/graphics/opengl_renderer/SsaoRenderer.h"
#include "game/graphics/opengl_renderer/background/Canopy.h"

void Generic2::opengl_setup(ShaderLibrary& shaders) {
  // create OpenGL objects
  glGenBuffers(1, &m_ogl.vertex_buffer);
  glGenBuffers(1, &m_ogl.index_buffer);
  glGenVertexArrays(1, &m_ogl.vao);

  // set up the vertex array
  glBindVertexArray(m_ogl.vao);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ogl.index_buffer);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, m_indices.size() * sizeof(u32), nullptr, GL_STREAM_DRAW);
  glBindBuffer(GL_ARRAY_BUFFER, m_ogl.vertex_buffer);
  glBufferData(GL_ARRAY_BUFFER, m_verts.size() * sizeof(Vertex), nullptr, GL_STREAM_DRAW);

  // xyz
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0,                            // location 0 in the shader
                        3,                            // 3 floats per vert
                        GL_FLOAT,                     // floats
                        GL_TRUE,                      // normalized, ignored,
                        sizeof(Vertex),               //
                        (void*)offsetof(Vertex, xyz)  // offset in array
  );

  // rgba
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1,                             // location 1 in the shader
                        4,                             // 4 color components
                        GL_UNSIGNED_BYTE,              // u8
                        GL_TRUE,                       // normalized (255 becomes 1)
                        sizeof(Vertex),                //
                        (void*)offsetof(Vertex, rgba)  //
  );

  // stq
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2,                           // location 2 in the shader
                        2,                           // 2 floats per vert
                        GL_FLOAT,                    // floats
                        GL_FALSE,                    // normalized, ignored
                        sizeof(Vertex),              //
                        (void*)offsetof(Vertex, st)  // offset in array
  );

  // byte data
  glEnableVertexAttribArray(3);
  glVertexAttribIPointer(3,                                 // location 3 in the shader
                         4,                                 //
                         GL_UNSIGNED_BYTE,                  // u8's
                         sizeof(Vertex),                    //
                         (void*)offsetof(Vertex, tex_unit)  // offset in array
  );

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindVertexArray(0);

  const auto& shader = shaders[ShaderId::GENERIC];
  auto id = shader.id();

  shader.activate();
  m_ogl.alpha_reject = glGetUniformLocation(id, "alpha_reject");
  m_ogl.color_mult = glGetUniformLocation(id, "color_mult");
  m_ogl.fog_color = glGetUniformLocation(id, "fog_color");

  m_ogl.scale = glGetUniformLocation(id, "scale");
  m_ogl.mat_23 = glGetUniformLocation(id, "mat_23");
  m_ogl.mat_32 = glGetUniformLocation(id, "mat_32");
  m_ogl.mat_33 = glGetUniformLocation(id, "mat_33");
  m_ogl.fog_consts = glGetUniformLocation(id, "fog_constants");
  m_ogl.hvdf_offset = glGetUniformLocation(id, "hvdf_offset");
  m_ogl.gfx_hack_no_tex = glGetUniformLocation(id, "gfx_hack_no_tex");
  m_ogl.warp_sample_mode = glGetUniformLocation(id, "warp_sample_mode");
  m_ogl.use_full_matrix = glGetUniformLocation(id, "use_full_matrix");
  m_ogl.full_matrix = glGetUniformLocation(id, "full_matrix");
  m_ogl.stereo_eye_x = glGetUniformLocation(id, "stereo_eye_x");
  m_ogl.eye_on = glGetUniformLocation(id, "eye_on");
  m_ogl.eye_rot = glGetUniformLocation(id, "eye_rot");
  m_ogl.eye_t = glGetUniformLocation(id, "eye_t");
}

void Generic2::opengl_cleanup() {
  glDeleteBuffers(1, &m_ogl.vertex_buffer);
  glDeleteBuffers(1, &m_ogl.index_buffer);
  glDeleteVertexArrays(1, &m_ogl.vao);
}

void Generic2::opengl_bind_and_setup_proj(SharedRenderState* render_state) {
  render_state->shaders[ShaderId::GENERIC].activate();
  glUniform4f(m_ogl.fog_color, render_state->fog_color[0] / 255.f,
              render_state->fog_color[1] / 255.f, render_state->fog_color[2] / 255.f,
              render_state->fog_intensity / 255);
  const auto& eye = render_state->eye;
  glUniform4f(m_ogl.scale, m_drawing_config.proj_scale[0] * (eye.active ? eye.fov_x : 1.f),
              m_drawing_config.proj_scale[1] * (eye.active ? eye.fov_y : 1.f),
              m_drawing_config.proj_scale[2], 0);
  glUniform1i(m_ogl.eye_on, eye.active ? 1 : 0);
  if (eye.active) {
    glUniformMatrix3fv(m_ogl.eye_rot, 1, GL_TRUE, &eye.rot[0][0]);
    glUniform3f(m_ogl.eye_t, eye.t[0], eye.t[1], eye.t[2]);
  }
  glUniform1f(m_ogl.mat_23, m_drawing_config.proj_mat_23);
  glUniform1f(m_ogl.mat_32, m_drawing_config.proj_mat_32);
  glUniform1f(m_ogl.mat_33, 0);
  glUniform3f(m_ogl.fog_consts, m_drawing_config.pfog0, m_drawing_config.fog_min,
              m_drawing_config.fog_max);
  glUniform4f(m_ogl.hvdf_offset, m_drawing_config.hvdf_offset[0], m_drawing_config.hvdf_offset[1],
              m_drawing_config.hvdf_offset[2], m_drawing_config.hvdf_offset[3]);
  glUniform1i(m_ogl.gfx_hack_no_tex, Gfx::g_global_settings.hack_no_tex);
  glUniform1f(m_ogl.stereo_eye_x, render_state->stereo_eye_x);
  SsaoRenderer::set_normal_mode(render_state->shaders[ShaderId::GENERIC].id(), 1);
}

void Generic2::setup_opengl_for_draw_mode(const DrawMode& draw_mode,
                                          u8 fix,
                                          SharedRenderState* render_state) {
  // compute alpha_reject:
  float alpha_reject = 0.f;
  if (draw_mode.get_at_enable()) {
    switch (draw_mode.get_alpha_test()) {
      case DrawMode::AlphaTest::ALWAYS:
        break;
      case DrawMode::AlphaTest::GEQUAL:
        alpha_reject = draw_mode.get_aref() / 128.f;
        break;
      case DrawMode::AlphaTest::NEVER:
        break;
      default:
        ASSERT_MSG(false, fmt::format("unknown alpha test: {}", (int)draw_mode.get_alpha_test()));
    }
  }

  // setup blending and color mult
  float color_mult = 1.f;
  if (!draw_mode.get_ab_enable()) {
    glDisable(GL_BLEND);
  } else {
    glEnable(GL_BLEND);
    glBlendColor(1, 1, 1, 1);
    if (draw_mode.get_alpha_blend() == DrawMode::AlphaBlend::SRC_DST_SRC_DST) {
      // (Cs - Cd) * As + Cd
      // Cs * As  + (1 - As) * Cd
      // s, d
      glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
      glBlendEquation(GL_FUNC_ADD);
    } else if (draw_mode.get_alpha_blend() == DrawMode::AlphaBlend::SRC_0_SRC_DST) {
      // (Cs - 0) * As + Cd
      // Cs * As + (1) * Cd
      // s, d
      // fix is ignored. it's usually 0, except for lightning, which sets it to 0x80.
      glBlendFunc(GL_SRC_ALPHA, GL_ONE);
      glBlendEquation(GL_FUNC_ADD);
    } else if (draw_mode.get_alpha_blend() == DrawMode::AlphaBlend::ZERO_SRC_SRC_DST) {
      // (0 - Cs) * As + Cd
      // Cd - Cs * As
      // s, d
      glBlendFunc(GL_SRC_ALPHA, GL_ONE);
      glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
    } else if (draw_mode.get_alpha_blend() == DrawMode::AlphaBlend::SRC_DST_FIX_DST) {
      // (Cs - Cd) * fix + Cd
      // Cs * fix + (1 - fx) * Cd
      glBlendFunc(GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA);
      glBlendColor(0, 0, 0, fix / 127.f);
      glBlendEquation(GL_FUNC_ADD);
    } else if (draw_mode.get_alpha_blend() == DrawMode::AlphaBlend::SRC_SRC_SRC_SRC) {
      // this is very weird...
      // Cs
      glBlendFunc(GL_ONE, GL_ZERO);
      glBlendEquation(GL_FUNC_ADD);
    } else if (draw_mode.get_alpha_blend() == DrawMode::AlphaBlend::SRC_0_DST_DST) {
      // (Cs - 0) * Ad + Cd
      glBlendFunc(GL_DST_ALPHA, GL_ONE);
      glBlendEquation(GL_FUNC_ADD);
      color_mult = 1.0f;
    } else if (draw_mode.get_alpha_blend() == DrawMode::AlphaBlend::SRC_0_FIX_DST) {
      glBlendEquation(GL_FUNC_ADD);
      glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ZERO);
    } else {
      ASSERT(false);
    }
  }
  SsaoRenderer::set_normal_write(
      !draw_mode.get_ab_enable() ||
      draw_mode.get_alpha_blend() == DrawMode::AlphaBlend::SRC_DST_SRC_DST ||
      draw_mode.get_alpha_blend() == DrawMode::AlphaBlend::SRC_SRC_SRC_SRC);

  // setup ztest
  if (draw_mode.get_zt_enable()) {
    glEnable(GL_DEPTH_TEST);
    switch (draw_mode.get_depth_test()) {
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
    // you aren't supposed to turn off z test enable, the GS had some bugs
    ASSERT(false);
  }

  if (draw_mode.get_depth_write_enable()) {
    glDepthMask(GL_TRUE);
  } else {
    glDepthMask(GL_FALSE);
  }

  glUniform1f(m_ogl.alpha_reject, alpha_reject);
  glUniform1f(m_ogl.color_mult, color_mult);
  glUniform4f(m_ogl.fog_color, render_state->fog_color[0] / 255.f,
              render_state->fog_color[1] / 255.f, render_state->fog_color[2] / 255.f,
              render_state->fog_intensity / 255);
}

void Generic2::setup_opengl_tex(u16 unit,
                                u16 tbp,
                                bool filter,
                                bool clamp_s,
                                bool clamp_t,
                                SharedRenderState* render_state) {
  // look up the texture
  std::optional<u64> tex;
  u32 tbp_to_lookup = tbp & 0x7fff;
  bool use_mt4hh = tbp & 0x8000;

  if (use_mt4hh) {
    tex = render_state->texture_pool->lookup_mt4hh(tbp_to_lookup);
  } else {
    tex = render_state->texture_pool->lookup(tbp_to_lookup);
  }

  if (!tex) {
    lg::warn("Failed to find texture at {}, using random (generic2)", tbp_to_lookup);
    tex = render_state->texture_pool->get_placeholder_texture();
  }

  glActiveTexture(GL_TEXTURE0 + unit);
  glBindTexture(GL_TEXTURE_2D, *tex);
  if (clamp_s) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  } else {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
  }

  if (clamp_t) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  } else {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
  }

  if (filter) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    true ? GL_LINEAR : GL_LINEAR_MIPMAP_LINEAR);  // todo
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  } else {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  }

  if (render_state->version >= GameVersion::Jak2 && tbp_to_lookup == 1216) {
    glUniform1ui(m_ogl.warp_sample_mode, 1);
    // warp shader uses region clamp, which isn't supported by DrawMode.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  } else {
    glUniform1ui(m_ogl.warp_sample_mode, 0);
  }
}

void Generic2::do_draws_for_alpha(SharedRenderState* render_state,
                                  ScopedProfilerNode& prof,
                                  DrawMode::AlphaBlend alpha,
                                  bool hud) {
  int clear_mat = -1;
  for (u32 i = 0; i < m_next_free_bucket; i++) {
    auto& bucket = m_buckets[i];
    auto& first = m_adgifs[bucket.start];
    if (first.mode.get_alpha_blend() == alpha && first.uses_hud == hud) {
      setup_opengl_for_draw_mode(first.mode, first.fix, render_state);
      if (render_state->light_material) {
        // depth-writing draws whose blend lands a = 1 clear the material behind them (no
        // highlight on generic); other modes leave output 2 off (reverse subtract gives a = 0)
        const auto ab = first.mode.get_alpha_blend();
        const bool lands_one = ab == DrawMode::AlphaBlend::SRC_SRC_SRC_SRC ||
                               SsaoRenderer::blend_replaces_alpha(first.mode);
        const int c = first.mode.get_depth_write_enable() && lands_one ? 1 : 0;
        if (c != clear_mat) {
          clear_mat = c;
          SsaoRenderer::emissive_draw_buffers(c == 1, false, true);
        }
      }
      setup_opengl_tex(0, first.tbp, first.mode.get_filt_enable(), first.mode.get_clamp_s_enable(),
                       first.mode.get_clamp_t_enable(), render_state);
      if (inspect::active()) {
        inspect::DrawRecord rec;
        rec.kind = inspect::GENERIC;
        rec.draw = (int)i;
        rec.tbp = first.tbp;
        rec.mode = first.mode;
        inspect::set(render_state->shaders[ShaderId::GENERIC].id(), inspect::add(std::move(rec)));
      }
      if (m_canopy) {
        auto it = m_canopy_tbp.find(first.tbp);
        if (it == m_canopy_tbp.end()) {
          s64 combo = -1;
          if (!(first.tbp & 0x8000)) {
            if (auto* gt = render_state->texture_pool->lookup_gpu_texture(first.tbp & 0x7fff)) {
              combo = ((s64)gt->tex_id.page << 16) | gt->tex_id.tex;
            }
          }
          it = m_canopy_tbp.emplace(first.tbp, combo).first;
        }
        canopy_set_emitter(render_state->shaders[ShaderId::GENERIC].id(),
                           canopy_keeps_light(first.mode, it->second), &m_canopy_emitter);
      }
      if (m_pbr) {
        auto it = m_pbr_tbp.find(first.tbp);
        if (it == m_pbr_tbp.end()) {
          std::pair<int, std::pair<GLuint, float>> e = {PBR_SPECIAL, {0, 0.f}};
          if (!(first.tbp & 0x8000)) {
            if (auto* gt = render_state->texture_pool->lookup_gpu_texture(first.tbp & 0x7fff)) {
              const u32 combo = ((u32)gt->tex_id.page << 16) | gt->tex_id.tex;
              e.first = pbr_mat_for_combo(combo);
              pbr_maps_for_combo(combo, &e.second.first, &e.second.second);
            }
          }
          it = m_pbr_tbp.emplace(first.tbp, e).first;
        }
        m_pbr_draw.set_direct(it->second.first, it->second.second.first, it->second.second.second);
      }
      if (m_iw_loc >= 0) {
        auto it = m_iw_tbp.find(first.tbp);
        if (it == m_iw_tbp.end()) {
          int k = inland_water::NONE;
          if (!(first.tbp & 0x8000)) {
            if (auto* gt = render_state->texture_pool->lookup_gpu_texture(first.tbp & 0x7fff)) {
              k = inland_water::kind_for_combo(((u32)gt->tex_id.page << 16) | gt->tex_id.tex);
              k = Gfx::g_global_settings.inland_water ? (k & inland_water::CLEAR) : 0;
            }
          }
          it = m_iw_tbp.emplace(first.tbp, k).first;
        }
        if (it->second != m_iw_kind) {
          m_iw_kind = it->second;
          glUniform1i(m_iw_loc, m_iw_kind);
        }
      }
      glDrawElements(GL_TRIANGLE_STRIP, bucket.idx_count, GL_UNSIGNED_INT,
                     (void*)(sizeof(u32) * bucket.idx_idx));
      prof.add_draw_call();
      prof.add_tri(bucket.tri_count);
    }
  }
}

void Generic2::do_hud_draws(SharedRenderState* render_state, ScopedProfilerNode& prof) {
  for (u32 i = 0; i < m_next_free_bucket; i++) {
    auto& bucket = m_buckets[i];
    auto& first = m_adgifs[bucket.start];
    if (first.uses_hud) {
      setup_opengl_for_draw_mode(first.mode, first.fix, render_state);
      setup_opengl_tex(0, first.tbp, first.mode.get_filt_enable(), first.mode.get_clamp_s_enable(),
                       first.mode.get_clamp_t_enable(), render_state);
      glDrawElements(GL_TRIANGLE_STRIP, bucket.idx_count, GL_UNSIGNED_INT,
                     (void*)(sizeof(u32) * bucket.idx_idx));
      prof.add_draw_call();
      prof.add_tri(bucket.tri_count);
    }
  }
}

void Generic2::do_draws(SharedRenderState* render_state, ScopedProfilerNode& prof) {
  glBindVertexArray(m_ogl.vao);
  glBindBuffer(GL_ARRAY_BUFFER, m_ogl.vertex_buffer);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ogl.index_buffer);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, m_next_free_idx * sizeof(u32), m_indices.data(),
               GL_STREAM_DRAW);
  glBufferData(GL_ARRAY_BUFFER, m_next_free_vert * sizeof(Vertex), m_verts.data(), GL_STREAM_DRAW);

  glEnable(GL_PRIMITIVE_RESTART);
  glPrimitiveRestartIndex(UINT32_MAX);

  opengl_bind_and_setup_proj(render_state);
  if (m_drawing_config.uses_full_matrix) {
    glUniform1i(m_ogl.use_full_matrix, 1);
    glUniformMatrix4fv(m_ogl.full_matrix, 1, GL_FALSE, m_drawing_config.full_matrix[0].data());
  } else {
    glUniform1i(m_ogl.use_full_matrix, 0);
  }
  const GLuint gid = render_state->shaders[ShaderId::GENERIC].id();
  m_pbr = !m_drawing_config.uses_full_matrix && pbr_active(render_state);
  pbr_setup_generic(render_state, gid, m_pbr);
  // the canopy follows the actor shadow prepass: off with shadows, as wind TIE
  m_canopy = !m_drawing_config.uses_full_matrix && render_state->shadow.active &&
             render_state->version == GameVersion::Jak1;
  if (m_canopy) {
    canopy_set_uniforms(gid, true);
    m_canopy_emitter = -1;
    m_canopy_tbp.clear();
  } else {
    canopy_disable(gid);
  }
  m_iw_loc = -1;
  if (inland_water::needs_setup(render_state, false)) {
    m_iw_loc = inland_water::setup(render_state, gid, false);
    m_iw_kind = 0;
    m_iw_tbp.clear();
  }
  if (m_pbr) {
    m_pbr_draw.begin(gid, nullptr, nullptr);
    m_pbr_tbp.clear();
  }
  constexpr DrawMode::AlphaBlend alpha_order[ALPHA_MODE_COUNT] = {
      DrawMode::AlphaBlend::SRC_0_FIX_DST,    DrawMode::AlphaBlend::SRC_SRC_SRC_SRC,
      DrawMode::AlphaBlend::SRC_DST_SRC_DST,  DrawMode::AlphaBlend::SRC_0_SRC_DST,
      DrawMode::AlphaBlend::ZERO_SRC_SRC_DST, DrawMode::AlphaBlend::SRC_DST_FIX_DST,
      DrawMode::AlphaBlend::SRC_0_DST_DST,
  };

  for (int i = 0; i < ALPHA_MODE_COUNT; i++) {
    if (m_alpha_draw_enable[i]) {
      do_draws_for_alpha(render_state, prof, alpha_order[i], false);
    }
  }
  if (render_state->light_material) {
    SsaoRenderer::emissive_draw_buffers(false);
  }

  if (m_drawing_config.uses_hud) {
    glUniform4f(m_ogl.scale, m_drawing_config.hud_scale[0], m_drawing_config.hud_scale[1],
                m_drawing_config.hud_scale[2], 0);
    glUniform1f(m_ogl.mat_23, m_drawing_config.hud_mat_23);
    glUniform1f(m_ogl.mat_32, m_drawing_config.hud_mat_32);
    glUniform1f(m_ogl.mat_33, m_drawing_config.hud_mat_33);
    glUniform1i(m_ogl.gfx_hack_no_tex, false);
    glUniform1f(m_ogl.stereo_eye_x, 0.f);
    glUniform1i(m_ogl.eye_on, 0);
    if (m_pbr) {
      pbr_setup_generic(render_state, render_state->shaders[ShaderId::GENERIC].id(), false);
      m_pbr = false;
    }
    canopy_disable(render_state->shaders[ShaderId::GENERIC].id());

    if (!ui_canvas_draw(*render_state, [&](int) { do_hud_draws(render_state, prof); })) {
      do_hud_draws(render_state, prof);
    }
  }
}
