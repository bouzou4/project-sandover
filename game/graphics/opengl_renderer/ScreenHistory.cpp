#include "ScreenHistory.h"

#include <algorithm>

#include "game/graphics/opengl_renderer/GlQueries.h"
#include "game/graphics/opengl_renderer/MotionVectors.h"

namespace {
// perf-opt bit 25: GL queries off the per-frame path (GlQueries.h)
constexpr int kGlq = PERF_OPT_PRESENT2 + 1;
}  // namespace

ScreenHistory::~ScreenHistory() {
  release();
}

void ScreenHistory::release() {
  for (auto& t : m_t) {
    if (t.fbo) {
      glDeleteFramebuffers(1, &t.fbo);
      glDeleteTextures(1, &t.tex);
    }
    t = {};
  }
  m_w = m_h = 0;
  m_have_prev = false;
}

void ScreenHistory::begin(int w, int h, const float k[9], const math::Vector3f& cam, u64 frame_idx) {
  if (w != m_w || h != m_h) {
    release();
    for (auto& t : m_t) {
      glGenFramebuffers(1, &t.fbo);
      glGenTextures(1, &t.tex);
      glBindTexture(GL_TEXTURE_2D, t.tex);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
      glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.tex, 0);
      const GLenum draw = GL_COLOR_ATTACHMENT0;
      glDrawBuffers(1, &draw);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    m_w = w;
    m_h = h;
  }
  if (m_have_prev) {
    std::copy(m_cur_k, m_cur_k + 9, m_prev_k);
    m_prev_cam = m_cur_cam;
  }
  const auto& mv = motion_vectors::frame();
  m_valid = m_have_prev && frame_idx == m_prev_frame + 1 && (!mv.active || mv.valid);
  m_cur = 1 - m_cur;
  std::copy(k, k + 9, m_cur_k);
  m_cur_cam = cam;
  m_have_prev = true;
  m_prev_frame = frame_idx;
}

void ScreenHistory::set_uniforms(GLuint prog, int unit) const {
  glUniformMatrix3fv(GLQ_LOC(kGlq, prog, "sh_prev_k"), 1, GL_TRUE, m_prev_k);
  glUniform3f(GLQ_LOC(kGlq, prog, "sh_prev_cam"), m_prev_cam.x(), m_prev_cam.y(), m_prev_cam.z());
  glUniform1i(GLQ_LOC(kGlq, prog, "sh_valid"), m_valid ? 1 : 0);
  glUniform1i(GLQ_LOC(kGlq, prog, "sh_prev"), unit);
  glActiveTexture(GL_TEXTURE0 + unit);
  glBindTexture(GL_TEXTURE_2D, m_t[1 - m_cur].tex);
  glActiveTexture(GL_TEXTURE0);
}
