#include "GlQueries.h"

#include <cstdlib>
#include <cstring>
#include <unordered_map>

#include "common/log/log.h"

#include "fmt/format.h"

namespace glq {

namespace {
u64 g_generation = 1;
}

void invalidate_state() {
  g_generation++;
}

GLint uniform_loc(int bit, GLuint program, const std::string& name) {
  if (!Gfx::perf_opt(bit)) {
    return glGetUniformLocation(program, name.c_str());
  }
  static std::unordered_map<GLuint, std::unordered_map<std::string, GLint>> cache;
  auto& per = cache[program];
  const auto it = per.find(name);
  if (it != per.end()) {
    return it->second;
  }
  const GLint l = glGetUniformLocation(program, name.c_str());
  per.emplace(name, l);
  return l;
}

void SavedState::capture() {
  static const bool check = std::getenv("SANDOVER_GL_STATE_CHECK") != nullptr;
  const bool on = m_bit != kLive && Gfx::perf_opt(m_bit);
  const u32 mask = Gfx::g_global_settings.perf_opt_mask;
  const bool stale = !m_valid || m_gen != g_generation || m_mask != mask;
  const bool refresh = !on || stale || ++m_frames % 256 == 0;
  if (!refresh && !check) {
    return;
  }
  State s;
  std::memset(&s, 0, sizeof(State));  // padding too, for the memcmp
  query(s);
  if (check && on && !stale && std::memcmp(&s, &m_s, sizeof(State)) != 0 && m_logs < 20) {
    m_logs++;
    std::string what;
    auto diff = [&](const char* name, GLint a, GLint b) {
      if (a != b) {
        what += fmt::format(" {} {}->{}", name, b, a);
      }
    };
    diff("program", s.program, m_s.program);
    diff("vao", s.vao, m_s.vao);
    diff("active", s.active_tex - GL_TEXTURE0, m_s.active_tex - GL_TEXTURE0);
    diff("draw-fbo", s.draw_fbo, m_s.draw_fbo);
    diff("read-fbo", s.read_fbo, m_s.read_fbo);
    for (int u = 0; u < kUnits; u++) {
      diff(fmt::format("tex{}", u).c_str(), s.tex_2d[u], m_s.tex_2d[u]);
      diff(fmt::format("ms{}", u).c_str(), s.tex_ms[u], m_s.tex_ms[u]);
    }
    lg::warn("glq: {} state differs from the cached copy (frame {}, generation {}):{}", m_site,
             m_frames, g_generation, what.empty() ? " other" : what);
  }
  std::memcpy(&m_s, &s, sizeof(State));
  m_valid = true;
  m_gen = g_generation;
  m_mask = mask;
}

void SavedState::restore() const {
  auto set = [](GLenum cap, GLboolean v) { v ? glEnable(cap) : glDisable(cap); };
  const State& s = m_s;
  set(GL_DEPTH_TEST, s.depth_test);
  set(GL_BLEND, s.blend);
  set(GL_SCISSOR_TEST, s.scissor);
  set(GL_CULL_FACE, s.cull);
  set(GL_STENCIL_TEST, s.stencil);
  glDepthMask(s.depth_mask);
  glColorMask(s.color_mask[0], s.color_mask[1], s.color_mask[2], s.color_mask[3]);
  glBlendFuncSeparate(s.blend_src_rgb, s.blend_dst_rgb, s.blend_src_a, s.blend_dst_a);
  glBlendEquationSeparate(s.blend_eq_rgb, s.blend_eq_a);
  glStencilFunc(s.stencil_func, s.stencil_ref, (GLuint)s.stencil_value_mask);
  glStencilOp(s.stencil_fail, s.stencil_zfail, s.stencil_zpass);
  glStencilMask((GLuint)s.stencil_write_mask);
  glViewport(s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3]);
  glUseProgram(s.program);
  glBindVertexArray(s.vao);
  for (int u = 0; u < kUnits; u++) {
    if (((m_units_2d | m_units_ms) >> u) & 1) {
      glActiveTexture(GL_TEXTURE0 + u);
      if ((m_units_2d >> u) & 1) {
        glBindTexture(GL_TEXTURE_2D, s.tex_2d[u]);
      }
      if ((m_units_ms >> u) & 1) {
        glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, s.tex_ms[u]);
      }
    }
  }
  glActiveTexture(s.active_tex);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s.draw_fbo);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, s.read_fbo);
  glReadBuffer(s.read_buffer);
}

void SavedState::query(State& s) const {
  s.depth_test = glIsEnabled(GL_DEPTH_TEST);
  s.blend = glIsEnabled(GL_BLEND);
  s.scissor = glIsEnabled(GL_SCISSOR_TEST);
  s.cull = glIsEnabled(GL_CULL_FACE);
  s.stencil = glIsEnabled(GL_STENCIL_TEST);
  glGetBooleanv(GL_DEPTH_WRITEMASK, &s.depth_mask);
  glGetBooleanv(GL_COLOR_WRITEMASK, s.color_mask);
  glGetIntegerv(GL_BLEND_SRC_RGB, &s.blend_src_rgb);
  glGetIntegerv(GL_BLEND_DST_RGB, &s.blend_dst_rgb);
  glGetIntegerv(GL_BLEND_SRC_ALPHA, &s.blend_src_a);
  glGetIntegerv(GL_BLEND_DST_ALPHA, &s.blend_dst_a);
  glGetIntegerv(GL_BLEND_EQUATION_RGB, &s.blend_eq_rgb);
  glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &s.blend_eq_a);
  glGetIntegerv(GL_STENCIL_FUNC, &s.stencil_func);
  glGetIntegerv(GL_STENCIL_REF, &s.stencil_ref);
  glGetIntegerv(GL_STENCIL_VALUE_MASK, &s.stencil_value_mask);
  glGetIntegerv(GL_STENCIL_WRITEMASK, &s.stencil_write_mask);
  glGetIntegerv(GL_STENCIL_FAIL, &s.stencil_fail);
  glGetIntegerv(GL_STENCIL_PASS_DEPTH_FAIL, &s.stencil_zfail);
  glGetIntegerv(GL_STENCIL_PASS_DEPTH_PASS, &s.stencil_zpass);
  glGetIntegerv(GL_READ_BUFFER, &s.read_buffer);
  glGetIntegerv(GL_VIEWPORT, s.viewport);
  glGetIntegerv(GL_CURRENT_PROGRAM, &s.program);
  glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s.vao);
  glGetIntegerv(GL_ACTIVE_TEXTURE, &s.active_tex);
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &s.draw_fbo);
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &s.read_fbo);
  for (int u = 0; u < kUnits; u++) {
    if (((m_units_2d | m_units_ms) >> u) & 1) {
      glActiveTexture(GL_TEXTURE0 + u);
      if ((m_units_2d >> u) & 1) {
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &s.tex_2d[u]);
      }
      if ((m_units_ms >> u) & 1) {
        glGetIntegerv(GL_TEXTURE_BINDING_2D_MULTISAMPLE, &s.tex_ms[u]);
      }
    }
  }
  glActiveTexture(s.active_tex);
}

}  // namespace glq
