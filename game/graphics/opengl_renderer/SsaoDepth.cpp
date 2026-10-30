#include "SsaoDepth.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "common/log/log.h"

#include "game/graphics/gfx.h"
#include "game/graphics/opengl_renderer/PerfTimers.h"
#include "game/graphics/opengl_renderer/buckets.h"

#include "fmt/format.h"
#include "third-party/imgui/imgui.h"

GpuTimer::~GpuTimer() {
  if (m_init) {
    glDeleteQueries(kRing * 2, &m_queries[0][0]);
  }
}

void GpuTimer::begin() {
  if (!m_init) {
    glGenQueries(kRing * 2, &m_queries[0][0]);
    m_init = true;
  }
  collect(m_head);
  glQueryCounter(m_queries[m_head][0], GL_TIMESTAMP);
}

void GpuTimer::end() {
  glQueryCounter(m_queries[m_head][1], GL_TIMESTAMP);
  m_pending[m_head] = true;
  m_head = (m_head + 1) % kRing;
}

void GpuTimer::collect(int slot) {
  if (!m_pending[slot]) {
    return;
  }
  m_pending[slot] = false;
  GLuint64 t0 = 0, t1 = 0;
  glGetQueryObjectui64v(m_queries[slot][0], GL_QUERY_RESULT, &t0);
  glGetQueryObjectui64v(m_queries[slot][1], GL_QUERY_RESULT, &t1);
  double ms = double(t1 - t0) * 1e-6;
  m_min_ms = m_count ? std::min(m_min_ms, ms) : ms;
  m_max_ms = m_count ? std::max(m_max_ms, ms) : ms;
  m_sum_ms += ms;
  if (++m_count == kWindow) {
    m_last_avg_ms = float(m_sum_ms / m_count);
    lg::info("SSAO-TIMING name={} avg_ms={:.4f} min_ms={:.4f} max_ms={:.4f} n={}", m_name,
             m_last_avg_ms, m_min_ms, m_max_ms, m_count);
    m_sum_ms = 0;
    m_count = 0;
  }
}

SsaoDepth::~SsaoDepth() {
  free_target(m_resolved);
  free_target(m_ms);
  free_target(m_probe_out);
  if (m_vao) {
    glDeleteVertexArrays(1, &m_vao);
  }
}

int SsaoDepth::capture_bucket_jak1() {
  // everything up to here is opaque or alpha-tested, for both level slots.
  // Most foreground merc (Jak, actors) is in the pris buckets, after the alpha buckets.
  return Gfx::g_global_settings.ssao_capture_point == 1 ? (int)jak1::BucketId::GENERIC_PRIS
                                                        : (int)jak1::BucketId::SHRUB_GENERIC_LEVEL1;
}

void SsaoDepth::free_target(Target& t) {
  if (t.fbo) {
    glDeleteFramebuffers(1, &t.fbo);
  }
  if (t.tex) {
    glDeleteTextures(1, &t.tex);
  }
  t = Target();
}

void SsaoDepth::ensure_target(Target& t, int w, int h, int samples) {
  if (t.fbo && t.w == w && t.h == h && t.samples == samples) {
    return;
  }
  perf::AddMs hitch_t(perf::Counter::HITCH_TARGET_MS);
  free_target(t);
  t.w = w;
  t.h = h;
  t.samples = samples;
  glGenFramebuffers(1, &t.fbo);
  glGenTextures(1, &t.tex);
  glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
  const bool probe = &t == &m_probe_out;
  if (samples > 1) {
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, t.tex);
    glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, samples, GL_DEPTH24_STENCIL8, w, h, GL_TRUE);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D_MULTISAMPLE,
                           t.tex, 0);
  } else {
    glBindTexture(GL_TEXTURE_2D, t.tex);
    if (probe) {
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
    } else {
      glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, w, h, 0, GL_DEPTH_STENCIL,
                   GL_UNSIGNED_INT_24_8, nullptr);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER,
                           probe ? GL_COLOR_ATTACHMENT0 : GL_DEPTH_STENCIL_ATTACHMENT,
                           GL_TEXTURE_2D, t.tex, 0);
  }
  GLenum draw = probe ? GL_COLOR_ATTACHMENT0 : GL_NONE;
  glDrawBuffers(1, &draw);
  glReadBuffer(draw);
  auto status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (status != GL_FRAMEBUFFER_COMPLETE) {
    lg::error("SSAO depth: incomplete framebuffer 0x{:x} ({}x{}, {} samples)", status, w, h,
              samples);
  }
  lg::info("SSAO depth: allocated {}x{} target, {} samples", w, h, samples);
}

void SsaoDepth::capture(SharedRenderState* render_state,
                        ScopedProfilerNode& prof,
                        bool resolved_only) {
  auto& settings = Gfx::g_global_settings;
  const int w = render_state->render_fb_w;
  const int h = render_state->render_fb_h;
  GLint samples = 1;
  glBindFramebuffer(GL_FRAMEBUFFER, render_state->render_fb);
  glGetIntegerv(GL_SAMPLES, &samples);
  samples = std::max(samples, 1);

  const bool probe = !resolved_only && settings.ssao_msaa_probe_request;
  const bool want_ms = !resolved_only && settings.ssao_msaa_mode == 1 && samples > 1;
  if (settings.ssao_gpu_timing && !resolved_only) {
    m_capture_timer.begin();
  }
  const bool scissor = glIsEnabled(GL_SCISSOR_TEST);
  if (scissor) {
    glDisable(GL_SCISSOR_TEST);
  }
  if (probe) {
    while (glGetError() != GL_NO_ERROR) {
    }
  }

  if (!want_ms || probe) {
    ensure_target(m_resolved, w, h, 1);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, render_state->render_fb);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_resolved.fbo);
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
  }
  GLenum resolve_err = probe ? glGetError() : GL_NO_ERROR;
  if (want_ms || (probe && samples > 1)) {
    ensure_target(m_ms, w, h, samples);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, render_state->render_fb);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_ms.fbo);
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
  }
  GLenum copy_err = probe ? glGetError() : GL_NO_ERROR;
  prof.add_draw_call();

  m_captured = true;
  m_captured_ms = want_ms;
  glBindFramebuffer(GL_FRAMEBUFFER, render_state->render_fb);
  if (scissor) {
    glEnable(GL_SCISSOR_TEST);
  }
  if (settings.ssao_gpu_timing && !resolved_only) {
    m_capture_timer.end();
  }

  if (probe) {
    settings.ssao_msaa_probe_request = false;
    if (samples <= 1) {
      m_probe_result = fmt::format("no MSAA on the render target ({}x{})", w, h);
      lg::info("SSAO-MSAA samples=1 w={} h={} resolve_err=0x{:x}", w, h, resolve_err);
    } else {
      lg::info("SSAO-MSAA blit errors: resolve=0x{:x} ms_copy=0x{:x}", resolve_err, copy_err);
      run_msaa_probe(render_state);
    }
  }
}

void SsaoDepth::run_msaa_probe(SharedRenderState* render_state) {
  const int w = m_resolved.w;
  const int h = m_resolved.h;
  if (!m_vao) {
    glGenVertexArrays(1, &m_vao);
  }
  ensure_target(m_probe_out, w, h, 1);
  glBindFramebuffer(GL_FRAMEBUFFER, m_probe_out.fbo);
  glViewport(0, 0, w, h);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_STENCIL_TEST);
  auto& sh = render_state->shaders[ShaderId::SSAO_MSAA_PROBE];
  sh.activate();
  glUniform1i(glGetUniformLocation(sh.id(), "resolved"), 0);
  glUniform1i(glGetUniformLocation(sh.id(), "ms"), 1);
  glUniform1i(glGetUniformLocation(sh.id(), "samples"), m_ms.samples);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, m_resolved.tex);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, m_ms.tex);
  glBindVertexArray(m_vao);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);
  glActiveTexture(GL_TEXTURE0);

  std::vector<float> px(size_t(w) * h * 4);
  glReadBuffer(GL_COLOR_ATTACHMENT0);
  glReadPixels(0, 0, w, h, GL_RGBA, GL_FLOAT, px.data());

  // 24-bit depth: one step is ~6e-8
  constexpr float eps = 1e-7f;
  size_t edge = 0, s0 = 0, lo = 0, hi = 0, mean = 0, none = 0, all_s0 = 0;
  double max_err_s0 = 0;
  for (size_t i = 0; i < px.size(); i += 4) {
    const float d_s0 = std::abs(px[i]), d_lo = std::abs(px[i + 1]), d_hi = std::abs(px[i + 2]),
                d_mean = std::abs(px[i + 3]);
    all_s0 += d_s0 < eps;
    max_err_s0 = std::max(max_err_s0, double(d_s0));
    // (r - min) - (r - max) = -(max - min)
    if (px[i + 1] - px[i + 2] > eps) {
      edge++;
      const bool m_s0 = d_s0 < eps, m_lo = d_lo < eps, m_hi = d_hi < eps, m_mean = d_mean < eps;
      s0 += m_s0;
      lo += m_lo;
      hi += m_hi;
      mean += m_mean;
      none += !(m_s0 || m_lo || m_hi || m_mean);
    }
  }
  m_probe_result = fmt::format(
      "{} samples, {}x{}: resolved==sample0 on {}/{} px (max diff {:.3g}); edge px {}: "
      "==s0 {}, ==min {}, ==max {}, ==mean {}, none {}",
      m_ms.samples, w, h, all_s0, size_t(w) * h, max_err_s0, edge, s0, lo, hi, mean, none);
  lg::info(
      "SSAO-MSAA samples={} w={} h={} px={} match_s0_all={} max_diff_s0={:.3g} edge_px={} "
      "edge_s0={} edge_min={} edge_max={} edge_mean={} edge_none={}",
      m_ms.samples, w, h, size_t(w) * h, all_s0, max_err_s0, edge, s0, lo, hi, mean, none);

  glBindFramebuffer(GL_FRAMEBUFFER, render_state->render_fb);
  glViewport(0, 0, render_state->render_fb_w, render_state->render_fb_h);
}

void SsaoDepth::draw_debug(SharedRenderState* render_state, ScopedProfilerNode& prof) {
  if (!m_captured) {
    return;
  }
  if (!m_vao) {
    glGenVertexArrays(1, &m_vao);
  }
  const int w = render_state->render_fb_w;
  const int h = render_state->render_fb_h;
  glBindFramebuffer(GL_FRAMEBUFFER, render_state->render_fb);
  glViewport(0, 0, w, h);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_SCISSOR_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

  // same depth mapping as make_new_cam_mat in background_common.cpp
  const bool raw = m_raw || !render_state->has_pc_data;
  float lin[3] = {0, 1, 0};
  if (!raw) {
    const auto* persp = render_state->camera_persp;
    const float fog = render_state->camera_fog.x();
    const float pzw = persp[2][3];
    lin[0] = fog * persp[3][2] / 8388608.f;
    lin[1] = pzw;
    lin[2] =
        fog * persp[2][2] / 8388608.f + (render_state->camera_hvdf_off.z() / 8388608.f - 1.f) * pzw;
  }

  auto& sh = render_state->shaders[ShaderId::SSAO_DEPTH_DEBUG];
  sh.activate();
  glUniform1i(glGetUniformLocation(sh.id(), "depth_tex"), 0);
  glUniform1i(glGetUniformLocation(sh.id(), "depth_tex_ms"), 1);
  glUniform1i(glGetUniformLocation(sh.id(), "use_ms"), m_captured_ms ? 1 : 0);
  glUniform1i(glGetUniformLocation(sh.id(), "raw"), raw ? 1 : 0);
  glUniform3f(glGetUniformLocation(sh.id(), "lin"), lin[0], lin[1], lin[2]);
  glUniform1f(glGetUniformLocation(sh.id(), "far_dist"), m_far_dist);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, m_captured_ms ? 0 : m_resolved.tex);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, m_captured_ms ? m_ms.tex : 0);
  glBindVertexArray(m_vao);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);
  glActiveTexture(GL_TEXTURE0);
  prof.add_draw_call();
  prof.add_tri(1);
}

void SsaoDepth::draw_debug_window() {
  auto& s = Gfx::g_global_settings;
  ImGui::Checkbox("Capture depth", &s.ssao_depth_capture);
  ImGui::Checkbox("Show depth", &s.ssao_depth_debug);
  ImGui::RadioButton("After opaque background (30)", &s.ssao_capture_point, 0);
  ImGui::SameLine();
  ImGui::RadioButton("After pris (56)", &s.ssao_capture_point, 1);
  ImGui::RadioButton("Resolve blit", &s.ssao_msaa_mode, 0);
  ImGui::SameLine();
  ImGui::RadioButton("MSAA copy, sample 0", &s.ssao_msaa_mode, 1);
  ImGui::Checkbox("Raw depth", &m_raw);
  float far_m = m_far_dist / 4096.f;
  if (ImGui::SliderFloat("Far (m)", &far_m, 10.f, 2000.f)) {
    m_far_dist = far_m * 4096.f;
  }
  ImGui::Checkbox("GPU timers", &s.ssao_gpu_timing);
  ImGui::Text("capture: %.4f ms  buckets: %.4f ms", m_capture_timer.last_avg_ms(),
              m_frame_timer.last_avg_ms());
  if (ImGui::Button("Probe MSAA resolve")) {
    s.ssao_msaa_probe_request = true;
    s.ssao_depth_capture = true;
  }
  ImGui::TextWrapped("%s", m_probe_result.c_str());
}
