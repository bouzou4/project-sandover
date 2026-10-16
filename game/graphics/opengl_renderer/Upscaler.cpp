#include "Upscaler.h"

#include <cmath>
#include <cstdint>

#include "common/log/log.h"

#include "game/graphics/gfx.h"
#include "game/graphics/opengl_renderer/GlQueries.h"
#include "game/graphics/opengl_renderer/PerfTimers.h"

// AMD's own constant setup (FsrEasuCon, FsrRcasCon), compiled for the CPU as the headers document
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wunused-variable"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#endif
#define A_CPU 1
#include "game/graphics/opengl_renderer/shaders/ffx/ffx_a.h"
#include "game/graphics/opengl_renderer/shaders/ffx/ffx_fsr1.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

// perf-opt bit 25: GL queries off the per-frame path (GlQueries.h)
constexpr int kGlq = PERF_OPT_PRESENT2 + 1;

namespace {
Fbo make_target(int w, int h, bool hdr, bool depth) {
  Fbo f;
  glGenFramebuffers(1, &f.fbo_id);
  glBindFramebuffer(GL_FRAMEBUFFER, f.fbo_id);
  GLuint tex;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexImage2D(GL_TEXTURE_2D, 0, hdr ? GL_RGBA16F : GL_RGBA8, w, h, 0, GL_RGBA,
               hdr ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
  f.tex_id = tex;
  if (depth) {
    GLuint z;
    glGenTextures(1, &z);
    glBindTexture(GL_TEXTURE_2D, z);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, w, h, 0, GL_DEPTH_STENCIL,
                 GL_UNSIGNED_INT_24_8, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, z, 0);
    f.zbuf_stencil_id = z;
    f.zbuf_is_texture = true;
  }
  GLenum draw = GL_COLOR_ATTACHMENT0;
  glDrawBuffers(1, &draw);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    lg::error("upscale: incomplete framebuffer ({}x{})", w, h);
  }
  glBindTexture(GL_TEXTURE_2D, 0);
  f.valid = true;
  f.multisample_count = 1;
  f.width = w;
  f.height = h;
  return f;
}

void set_con(GLuint prog, const char* name, const AU1* c) {
  glUniform4ui(glq::uniform_loc(kGlq, prog, name), c[0], c[1], c[2], c[3]);
}
}  // namespace

Upscaler::~Upscaler() {
  free_targets();
  if (m_vao) {
    glDeleteVertexArrays(1, &m_vao);
  }
}

void Upscaler::free_targets() {
  m_src.clear();
  m_easu.clear();
  m_display.clear();
  m_rw = m_rh = m_dw = m_dh = 0;
}

void Upscaler::ensure(int rw, int rh, int dw, int dh, bool hdr, bool fsr) {
  if (!m_vao) {
    glGenVertexArrays(1, &m_vao);
  }
  if (rw == m_rw && rh == m_rh && dw == m_dw && dh == m_dh && hdr == m_hdr) {
    if (fsr && !m_easu.valid) {
      m_easu = make_target(dw, dh, hdr, false);
    } else if (!fsr) {
      m_easu.clear();
    }
    return;
  }
  free_targets();
  m_src = make_target(rw, rh, hdr, false);
  if (fsr) {
    m_easu = make_target(dw, dh, hdr, false);
  }
  m_display = make_target(dw, dh, hdr, true);
  m_rw = rw;
  m_rh = rh;
  m_dw = dw;
  m_dh = dh;
  m_hdr = hdr;
  lg::info("upscale: targets {}x{} -> {}x{} {}", rw, rh, dw, dh, hdr ? "RGBA16F" : "RGBA8");
}

void Upscaler::run(SharedRenderState* rs, ScopedProfilerNode& prof, int dw, int dh) {
  const auto& gs = Gfx::g_global_settings;
  static glq::SavedState saved("upscale", kGlq, 0b1, 0);
  saved.capture();
  const int rw = rs->render_fb_w, rh = rs->render_fb_h;
  const bool fsr = gs.upscaler == UPSCALE_FSR1 && rs->shaders.fsr_ready();
  ensure(rw, rh, dw, dh, rs->hdr_target, fsr);
  auto& easu = rs->shaders[ShaderId::FSR_EASU];
  auto& rcas = rs->shaders[ShaderId::FSR_RCAS];
  glBindVertexArray(m_vao);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_BLEND);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

  {
    // copy, or resolve, the scene to a single-sample texture
    perf::Scope ps(perf::Group::UPSCALE_COPY);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, rs->render_fb);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_src.fbo_id);
    glBlitFramebuffer(0, 0, rw, rh, 0, 0, rw, rh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  }

  if (fsr) {
    {
      perf::Scope ps(perf::Group::UPSCALE_EASU);
      AU1 c0[4], c1[4], c2[4], c3[4];
      FsrEasuCon(c0, c1, c2, c3, (AF1)rw, (AF1)rh, (AF1)rw, (AF1)rh, (AF1)dw, (AF1)dh);
      glBindFramebuffer(GL_FRAMEBUFFER, m_easu.fbo_id);
      glViewport(0, 0, dw, dh);
      easu.activate();
      glUniform1i(GLQ_LOC(kGlq, easu.id(), "src"), 0);
      glUniform1i(GLQ_LOC(kGlq, easu.id(), "headroom"), m_hdr ? 1 : 0);
      glUniform2f(GLQ_LOC(kGlq, easu.id(), "out_size"), (float)dw, (float)dh);
      set_con(easu.id(), "con0", c0);
      set_con(easu.id(), "con1", c1);
      set_con(easu.id(), "con2", c2);
      set_con(easu.id(), "con3", c3);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, *m_src.tex_id);
      glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    {
      perf::Scope ps(perf::Group::UPSCALE_RCAS);
      AU1 r[4];
      FsrRcasCon(r, (AF1)gs.fsr_sharpness);
      glBindFramebuffer(GL_FRAMEBUFFER, m_display.fbo_id);
      rcas.activate();
      glUniform1i(GLQ_LOC(kGlq, rcas.id(), "src"), 0);
      glUniform1i(GLQ_LOC(kGlq, rcas.id(), "headroom"), m_hdr ? 1 : 0);
      set_con(rcas.id(), "con0", r);
      glBindTexture(GL_TEXTURE_2D, *m_easu.tex_id);
      glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    prof.add_draw_call(2);
  } else {
    perf::Scope ps(perf::Group::UPSCALE_BILINEAR);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_src.fbo_id);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_display.fbo_id);
    glBlitFramebuffer(0, 0, rw, rh, 0, 0, dw, dh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
  }
  // the text buckets draw at display size: an empty depth and stencil, as at the frame's start
  glBindFramebuffer(GL_FRAMEBUFFER, m_display.fbo_id);
  glDepthMask(GL_TRUE);
  glStencilMask(0xFF);
  glClearBufferfi(GL_DEPTH_STENCIL, 0, 0.f, 0);
  saved.restore();
  glBindFramebuffer(GL_FRAMEBUFFER, m_display.fbo_id);
  glViewport(0, 0, dw, dh);
  rs->render_fb = m_display.fbo_id;
  rs->render_fb_w = dw;
  rs->render_fb_h = dh;
  rs->render_fb_depth_tex = *m_display.zbuf_stencil_id;
}
