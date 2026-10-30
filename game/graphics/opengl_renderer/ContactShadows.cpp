#include "ContactShadows.h"

#include <algorithm>
#include <cmath>

#include "common/log/log.h"

#include "game/graphics/gfx.h"
#include "game/graphics/opengl_renderer/ActorShadows.h"
#include "game/graphics/opengl_renderer/GlQueries.h"
#include "game/graphics/opengl_renderer/SsaoRenderer.h"
#include "game/graphics/opengl_renderer/Swamp.h"
#include "game/graphics/opengl_renderer/WorldShadows.h"

namespace {
// perf-opt bit 25: GL queries off the per-frame path (GlQueries.h)
constexpr int kGlq = PERF_OPT_PRESENT2 + 1;
constexpr float kScissorAdjust = 512.f / 448.f;  // Jak 1 SCISSOR_ADJUST * HEIGHT_SCALE
constexpr float kSsaoFarM = 10000.f;  // SsaoRenderer kFarM: the depth copy of sky pixels

/// Camera offset (world axes, game units) to clip x, y and w of the background draws, row-major
/// (make_new_cam_mat, as MotionVectors' cam_to_clip; x, y and w carry no translation), divided
/// by |pzw| so w is the view depth in game units (the SSAO depth copy x 4096).
bool cam_to_clip3(const SharedRenderState* rs, float k[9]) {
  const auto* rot = rs->camera_rot;
  const auto* persp = rs->camera_persp;
  const float fog = rs->camera_fog.x();
  const float pzw = std::abs(persp[2][3]);
  if (pzw <= 0.f) {
    return false;
  }
  const float sc[3] = {fog * persp[0][0] / 256.f / pzw,
                       -fog * persp[1][1] / 128.f * kScissorAdjust / pzw, persp[2][3] / pzw};
  for (int r = 0; r < 3; r++) {
    for (int j = 0; j < 3; j++) {
      k[r * 3 + j] = -rot[j][r] * sc[r];
    }
  }
  return sc[0] != 0.f && sc[1] != 0.f && sc[2] != 0.f;
}

bool invert3(const float m[9], float out[9]) {
  const double a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5], g = m[6], h = m[7],
               i = m[8];
  const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
  if (std::abs(det) < 1e-30) {
    return false;
  }
  const double id = 1.0 / det;
  const double r[9] = {e * i - f * h, c * h - b * i, b * f - c * e,
                       f * g - d * i, a * i - c * g, c * d - a * f,
                       d * h - e * g, b * g - a * h, a * e - b * d};
  for (int n = 0; n < 9; n++) {
    out[n] = (float)(r[n] * id);
  }
  return true;
}
}  // namespace

ContactShadows::~ContactShadows() {
  if (m_vao) {
    glDeleteVertexArrays(1, &m_vao);
  }
  release_raw();
}

void ContactShadows::release() {
  release_raw();
  m_history.release();
}

void ContactShadows::release_raw() {
  if (m_raw_fbo) {
    glDeleteFramebuffers(1, &m_raw_fbo);
    glDeleteTextures(1, &m_raw_tex);
  }
  m_raw_fbo = m_raw_tex = 0;
  m_raw_w = m_raw_h = 0;
}

void ContactShadows::ensure_raw(int w, int h) {
  if (w == m_raw_w && h == m_raw_h) {
    return;
  }
  release_raw();
  glGenFramebuffers(1, &m_raw_fbo);
  glGenTextures(1, &m_raw_tex);
  glBindTexture(GL_TEXTURE_2D, m_raw_tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, w, h, 0, GL_RED, GL_HALF_FLOAT, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, m_raw_fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_raw_tex, 0);
  const GLenum draw = GL_COLOR_ATTACHMENT0;
  glDrawBuffers(1, &draw);
  glBindTexture(GL_TEXTURE_2D, 0);
  m_raw_w = w;
  m_raw_h = h;
}

ContactShadows::Params ContactShadows::params(int level, int quality) {
  Params p = level >= 2 ? Params{16, 1.5f, 0.5f, 1.0f, 0.12f, 16}
                         : Params{16, 0.8f, 0.35f, 0.8f, 0.08f, 16};
  if (quality <= 0) {
    p.steps = 8;
  }
  return p;
}

void ContactShadows::render(SharedRenderState* rs,
                            ScopedProfilerNode& prof,
                            SsaoRenderer& ssao,
                            bool temporal) {
  const auto& gs = Gfx::g_global_settings;
  if (gs.shadow_contact <= 0) {
    m_warned = false;
    release();
    return;
  }
  if (!temporal) {
    if (!m_warned) {
      lg::info("contact-shadows: needs FSR 3 or DLSS, off under this antialiasing mode");
      m_warned = true;
    }
    release();
    return;
  }
  m_warned = false;
  const auto& ws = rs->world_shadow;
  if (!ssao.have_frame() || !rs->shadow.active || !ws.active || ws.strength <= 0.f ||
      rs->stereo_active || rs->eye.active) {
    return;
  }
  float k[9], kinv[9];
  if (!cam_to_clip3(rs, k) || !invert3(k, kinv)) {
    return;
  }

  const int w = rs->render_fb_w, h = rs->render_fb_h;
  // bind_view_inputs uses units 1-3, the emissive mask 5 and 6, the history 7, raw 8
  static glq::SavedState saved("contact-shadows", kGlq, 0b111101110, 0b1001000);
  saved.capture();
  if (!m_vao) {
    glGenVertexArrays(1, &m_vao);
  }
  ensure_raw(w, h);
  const math::Vector3f cam(rs->camera_pos.x(), rs->camera_pos.y(), rs->camera_pos.z());
  m_history.begin(w, h, k, cam, rs->frame_idx);
  glBindVertexArray(m_vao);
  glViewport(0, 0, w, h);
  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_BLEND);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

  // 1. raw term
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_raw_fbo);
  auto& sh = rs->shaders[ShaderId::CONTACT_SHADOW];
  sh.activate();
  GLuint id = sh.id();
  ssao.bind_view_inputs(id, 1, 2, 3);
  world_shadows_set_uniforms(rs, id, false);
  actor_shadows_set_uniforms(rs, id, false);

  const GLuint em = ssao.emissive_texture();
  const int em_n = ssao.emissive_samples();
  glUniform1i(GLQ_LOC(kGlq, id, "em_ss"), 5);
  glUniform1i(GLQ_LOC(kGlq, id, "em_ms"), 6);
  glUniform1i(GLQ_LOC(kGlq, id, "em_samples"), em_n);
  glUniform1i(GLQ_LOC(kGlq, id, "em_on"), em && gs.ssao_emissive ? 1 : 0);
  glUniform2f(GLQ_LOC(kGlq, id, "em_ramp"), gs.ssao_emissive_lo,
              std::max(gs.ssao_emissive_hi, gs.ssao_emissive_lo + 1e-3f));
  glActiveTexture(GL_TEXTURE5);
  glBindTexture(GL_TEXTURE_2D, em && em_n <= 1 ? em : 0);
  glActiveTexture(GL_TEXTURE6);
  glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, em && em_n > 1 ? em : 0);
  glActiveTexture(GL_TEXTURE0);

  const bool fog_w = gs.ssao_fog_weight > 0.f;
  const bool mist = fog_w && swamp_mist_uniforms(rs, id);
  glUniform1i(GLQ_LOC(kGlq, id, "fog_w"), fog_w ? 1 : 0);
  glUniform1f(GLQ_LOC(kGlq, id, "fog_k"), gs.ssao_fog_weight);
  glUniform1i(GLQ_LOC(kGlq, id, "mist_on"), mist ? 1 : 0);

  glUniformMatrix3fv(GLQ_LOC(kGlq, id, "cs_k"), 1, GL_TRUE, k);
  glUniformMatrix3fv(GLQ_LOC(kGlq, id, "cs_kinv"), 1, GL_TRUE, kinv);
  glUniform3f(GLQ_LOC(kGlq, id, "cs_cam"), cam.x(), cam.y(), cam.z());
  math::Vector3f L(ws.sun_world.x(), ws.sun_world.y(), ws.sun_world.z());
  const float ll = std::sqrt(L.dot(L));
  L = ll > 0.f ? L / ll : math::Vector3f(0.f, 1.f, 0.f);
  glUniform3f(GLQ_LOC(kGlq, id, "cs_light"), L.x(), L.y(), L.z());
  const Params p = params(gs.shadow_contact, gs.shadow_quality);
  glUniform4f(GLQ_LOC(kGlq, id, "cs_par"), (float)p.steps, p.length_m, p.thickness_m,
              p.strength * std::clamp(ws.strength, 0.f, 1.f) *
                  std::clamp(gs.shadow_strength, 0.f, 1.f));
  glUniform1f(GLQ_LOC(kGlq, id, "cs_cap"), p.screen_cap);
  glUniform1f(GLQ_LOC(kGlq, id, "cs_share"), std::clamp(rs->shadow.share_global, 0.f, 0.95f));
  glUniform1f(GLQ_LOC(kGlq, id, "cs_frame"), (float)(rs->frame_idx % 64));
  glUniform1i(GLQ_LOC(kGlq, id, "cs_view"), gs.shadow_contact_view);
  glUniform2f(GLQ_LOC(kGlq, id, "cs_size"), (float)w, (float)h);
  glDrawArrays(GL_TRIANGLES, 0, 3);

  // 2. temporal: blend into the reprojected history
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_history.cur_fbo());
  auto& st = rs->shaders[ShaderId::CONTACT_SHADOW_TEMPORAL];
  st.activate();
  id = st.id();
  m_history.set_uniforms(id, 7);
  glUniform1i(GLQ_LOC(kGlq, id, "cs_raw"), 8);
  glUniform1i(GLQ_LOC(kGlq, id, "csz"), 1);
  glUniformMatrix3fv(GLQ_LOC(kGlq, id, "cs_kinv"), 1, GL_TRUE, kinv);
  glUniform3f(GLQ_LOC(kGlq, id, "cs_cam"), cam.x(), cam.y(), cam.z());
  glUniform2f(GLQ_LOC(kGlq, id, "cs_size"), (float)w, (float)h);
  glUniform1f(GLQ_LOC(kGlq, id, "cs_far"), kSsaoFarM);
  glUniform1f(GLQ_LOC(kGlq, id, "cs_max_n"), (float)p.history);
  glActiveTexture(GL_TEXTURE8);
  glBindTexture(GL_TEXTURE_2D, m_raw_tex);
  glActiveTexture(GL_TEXTURE0);
  glDrawArrays(GL_TRIANGLES, 0, 3);

  // 3. composite: depth-aware 3x3, multiplied into the frame
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, rs->render_fb);
  const bool view = gs.shadow_contact_view > 0;
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
  if (!view) {
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    // PS2 blending reads destination alpha later: keep it
    glBlendFuncSeparate(GL_DST_COLOR, GL_ZERO, GL_ZERO, GL_ONE);
    // stencil bit 7: sky, far ocean and translucent actors
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glStencilFunc(GL_EQUAL, 0x00, 0x80);
  }
  auto& sc = rs->shaders[ShaderId::CONTACT_SHADOW_COMPOSITE];
  sc.activate();
  id = sc.id();
  glUniform1i(GLQ_LOC(kGlq, id, "cs_hist"), 7);
  glUniform1i(GLQ_LOC(kGlq, id, "cs_raw"), 8);
  glUniform1i(GLQ_LOC(kGlq, id, "cs_view"), gs.shadow_contact_view);
  glUniform2f(GLQ_LOC(kGlq, id, "cs_size"), (float)w, (float)h);
  glActiveTexture(GL_TEXTURE7);
  glBindTexture(GL_TEXTURE_2D, m_history.cur_tex());
  glActiveTexture(GL_TEXTURE0);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  prof.add_draw_call(3);

  for (int u : {5, 7, 8}) {
    glActiveTexture(GL_TEXTURE0 + u);
    glBindTexture(GL_TEXTURE_2D, 0);
  }
  glActiveTexture(GL_TEXTURE6);
  glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);
  glActiveTexture(GL_TEXTURE0);
  saved.restore();
}
