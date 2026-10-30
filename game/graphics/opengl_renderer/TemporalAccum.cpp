#include "TemporalAccum.h"

#include "common/log/log.h"

#include "game/graphics/gfx.h"
#include "game/graphics/opengl_renderer/GlQueries.h"
#include "game/graphics/opengl_renderer/MotionVectors.h"
#include "game/graphics/opengl_renderer/Shader.h"

namespace {
constexpr int kOptNoQueries = PERF_OPT_SSAO + 3;  // cached uniform locations
}

TemporalAccum::~TemporalAccum() {
  release();
  if (m_vao) {
    glDeleteVertexArrays(1, &m_vao);
  }
}

void TemporalAccum::release() {
  for (auto& t : m_hist) {
    if (t.fbo) {
      glDeleteFramebuffers(1, &t.fbo);
    }
    if (t.tex) {
      glDeleteTextures(1, &t.tex);
    }
    t = Target();
  }
  m_w = m_h = 0;
  m_have = false;
}

void TemporalAccum::ensure(int w, int h) {
  if (w == m_w && h == m_h) {
    return;
  }
  release();
  for (auto& t : m_hist) {
    glGenTextures(1, &t.tex);
    glBindTexture(GL_TEXTURE_2D, t.tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG16F, w, h, 0, GL_RG, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &t.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.tex, 0);
    GLenum draw = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &draw);
  }
  glBindTexture(GL_TEXTURE_2D, 0);
  m_w = w;
  m_h = h;
  m_have = false;
}

bool TemporalAccum::available(const SharedRenderState* rs) {
  return motion_vectors::frame().active;
}

GLuint TemporalAccum::run(SharedRenderState* rs, const Inputs& in) {
  if (!available(rs) || !in.signal || !in.depth) {
    m_have = false;
    return 0;
  }
  const auto& mv = motion_vectors::frame();
  const int w = in.half_res ? std::max(1, in.full_w >> 1) : in.full_w;
  const int h = in.half_res ? std::max(1, in.full_h >> 1) : in.full_h;
  ensure(w, h);
  if (!m_vao) {
    glGenVertexArrays(1, &m_vao);
  }
  // a frame without a run (the consumer was off) leaves the history stale
  const bool have = m_have && mv.valid && rs->display_tick == m_last_frame + 1;
  m_last_frame = rs->display_tick;
  const int src = m_cur, dst = 1 - m_cur;
  const bool ms = in.samples > 1;
  const bool obj = mv.obj_tex != 0 && mv.obj_samples == in.samples;

  auto& sh = rs->shaders[ShaderId::TEMPORAL_ACCUM];
  sh.activate();
  const GLuint id = sh.id();
  glUniform1i(GLQ_LOC(kOptNoQueries, id, "signal"), 0);
  glUniform1i(GLQ_LOC(kOptNoQueries, id, "history"), 1);
  glUniform1i(GLQ_LOC(kOptNoQueries, id, "have_history"), have ? 1 : 0);
  glUniform1i(GLQ_LOC(kOptNoQueries, id, "depth_ss"), 2);
  glUniform1i(GLQ_LOC(kOptNoQueries, id, "depth_ms"), 3);
  glUniform1i(GLQ_LOC(kOptNoQueries, id, "samples"), in.samples);
  glUniform1i(GLQ_LOC(kOptNoQueries, id, "obj_ss"), 4);
  glUniform1i(GLQ_LOC(kOptNoQueries, id, "obj_ms"), 5);
  glUniform1i(GLQ_LOC(kOptNoQueries, id, "obj_on"), obj ? 1 : 0);
  glUniform1i(GLQ_LOC(kOptNoQueries, id, "dyn_ss"), 6);
  glUniform1i(GLQ_LOC(kOptNoQueries, id, "dyn_ms"), 7);
  glUniform1i(GLQ_LOC(kOptNoQueries, id, "dyn_on"), in.dyn_mask ? 1 : 0);
  glUniformMatrix4fv(GLQ_LOC(kOptNoQueries, id, "reproj"), 1, GL_FALSE, mv.cam_reproj);
  glUniform2f(GLQ_LOC(kOptNoQueries, id, "wk"), mv.w_k0, mv.w_k1);
  glUniform2f(GLQ_LOC(kOptNoQueries, id, "full_size"), (float)in.full_w, (float)in.full_h);
  glUniform1i(GLQ_LOC(kOptNoQueries, id, "half_res"), in.half_res ? 1 : 0);
  glUniform1f(GLQ_LOC(kOptNoQueries, id, "blend"), in.blend);
  glUniform1f(GLQ_LOC(kOptNoQueries, id, "blend_dyn"), in.blend_dynamic);
  glUniform1f(GLQ_LOC(kOptNoQueries, id, "w_tol"), in.w_tol);
  glUniform1i(GLQ_LOC(kOptNoQueries, id, "nb_clamp"), in.clamp ? 1 : 0);
  glUniform1f(GLQ_LOC(kOptNoQueries, id, "nb_gamma"), in.gamma);
  // the projection jitter of this frame and the history's, so texels register
  float jx = 0.f, jy = 0.f;
  scene_jitter::get(&jx, &jy);
  glUniform4f(GLQ_LOC(kOptNoQueries, id, "jitter"), jx, jy, m_prev_jitter[0], m_prev_jitter[1]);
  m_prev_jitter[0] = jx;
  m_prev_jitter[1] = jy;

  auto bind = [](int unit, GLenum target, GLuint tex) {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(target, tex);
  };
  bind(0, GL_TEXTURE_2D, in.signal);
  bind(1, GL_TEXTURE_2D, m_hist[src].tex);
  bind(2, GL_TEXTURE_2D, ms ? 0 : in.depth);
  bind(3, GL_TEXTURE_2D_MULTISAMPLE, ms ? in.depth : 0);
  bind(4, GL_TEXTURE_2D, obj && !ms ? mv.obj_tex : 0);
  bind(5, GL_TEXTURE_2D_MULTISAMPLE, obj && ms ? mv.obj_tex : 0);
  bind(6, GL_TEXTURE_2D, in.dyn_mask && !ms ? in.dyn_mask : 0);
  bind(7, GL_TEXTURE_2D_MULTISAMPLE, in.dyn_mask && ms ? in.dyn_mask : 0);

  glBindFramebuffer(GL_FRAMEBUFFER, m_hist[dst].fbo);
  glViewport(0, 0, w, h);
  glBindVertexArray(m_vao);
  glDrawArrays(GL_TRIANGLES, 0, 3);

  for (int unit = 7; unit >= 3; unit--) {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(unit % 2 ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D, 0);
  }
  glActiveTexture(GL_TEXTURE0);
  m_cur = dst;
  m_have = true;
  return m_hist[dst].tex;
}
