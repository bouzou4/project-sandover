#include "MotionBlur.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

#include "common/log/log.h"

#include "game/graphics/gfx.h"
#include "game/graphics/opengl_renderer/GlQueries.h"
#include "game/graphics/opengl_renderer/PerfTimers.h"
#include "game/graphics/screenshot.h"

#include "third-party/imgui/imgui.h"

namespace motion_blur {
namespace {

std::atomic<float> g_dt_pending{0.f};
std::atomic<float> g_dt_chain{0.f};  // latched with the chain the render thread draws next
std::atomic<int> g_log_frames{0};
std::atomic<bool> g_vehicle_pending{false};
std::atomic<bool> g_vehicle_chain{false};

constexpr int kGlq = PERF_OPT_PRESENT2 + 1;  // GL queries off the per-frame path, as SMAA
// taps per strength (weak, medium, strong): longer trails need more to stay clean
constexpr int kTaps[3] = {16, 16, 24};
constexpr float kSoftZ = 0.05f;
constexpr float kMinDt = 1.f / 240.f, kMaxDt = 1.f / 30.f;

void make_target(unsigned& fbo,
                 unsigned& tex,
                 int w,
                 int h,
                 GLint ifmt,
                 GLenum fmt,
                 GLenum type,
                 GLint filter) {
  glGenFramebuffers(1, &fbo);
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexImage2D(GL_TEXTURE_2D, 0, ifmt, w, h, 0, fmt, type, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
  const GLenum draw = GL_COLOR_ATTACHMENT0;
  glDrawBuffers(1, &draw);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    lg::error("motion blur: incomplete framebuffer ({}x{})", w, h);
  }
}

}  // namespace

void set_frame_dt(float seconds) {
  g_dt_pending = seconds;
}

void set_vehicle(bool riding) {
  g_vehicle_pending = riding;
}

void latch_for_chain() {
  g_dt_chain = g_dt_pending.load();
  g_vehicle_chain = g_vehicle_pending.load();
}

void log_frames(int frames) {
  g_log_frames = frames;
}

Renderer::~Renderer() {
  release();
  if (m_vao) {
    glDeleteVertexArrays(1, &m_vao);
  }
}

void Renderer::release() {
  for (Target* t : {&m_scene, &m_tile_max, &m_neighbour}) {
    if (t->fbo) {
      glDeleteFramebuffers(1, &t->fbo);
      glDeleteTextures(1, &t->tex);
    }
    *t = Target();
  }
  m_w = m_h = m_tile = 0;
}

void Renderer::ensure(int w, int h, int tile, bool hdr) {
  if (!m_vao) {
    glGenVertexArrays(1, &m_vao);
  }
  if (w == m_w && h == m_h && tile == m_tile && hdr == m_hdr && m_scene.fbo) {
    return;
  }
  release();
  make_target(m_scene.fbo, m_scene.tex, w, h, hdr ? GL_RGBA16F : GL_RGBA8, GL_RGBA,
              hdr ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE, GL_LINEAR);
  const int tw = (w + tile - 1) / tile, th = (h + tile - 1) / tile;
  make_target(m_tile_max.fbo, m_tile_max.tex, tw, th, GL_RG16F, GL_RG, GL_HALF_FLOAT, GL_NEAREST);
  make_target(m_neighbour.fbo, m_neighbour.tex, tw, th, GL_RG16F, GL_RG, GL_HALF_FLOAT, GL_NEAREST);
  glBindTexture(GL_TEXTURE_2D, 0);
  m_w = w;
  m_h = h;
  m_tile = tile;
  m_hdr = hdr;
  lg::info("motion blur: allocated targets {}x{}, {} px tiles", w, h, tile);
}

bool Renderer::begin_frame(SharedRenderState* rs, motion_vectors::Renderer& mv) {
  const auto& gs = Gfx::g_global_settings;
  m_level = gs.mb_burst_cycle && g_burst_frame >= 0 ? g_burst_frame % 4 : gs.mb_level;
  const bool on = gs.mb_level > 0 && m_level > 0 && rs->version == GameVersion::Jak1 &&
                  !rs->stereo_active;
  const auto& f = motion_vectors::frame();
  if (on && !f.active && !m_logged_no_input) {
    lg::warn("motion blur: no motion vectors this frame (velocity target unavailable), skipped");
    m_logged_no_input = true;
  }
  m_active = on && f.active && f.valid;
  if (!on && m_scene.fbo) {
    release();
    lg::info("motion blur: off, targets freed");
  }
  if (!on) {
    // kept across cut frames while on; FSR keeps the velocity buffer itself alive
    mv.release_relative();
  }
  m_camera = gs.mb_camera == 2 || (gs.mb_camera == 1 && g_vehicle_chain.load());
  mv.want_relative(m_active, m_camera);
  rs->defer_hud_sprites = m_active;
  return m_active;
}

void Renderer::render(SharedRenderState* rs,
                      ScopedProfilerNode& prof,
                      const motion_vectors::Renderer& mv) {
  if (!m_active) {
    return;
  }
  const unsigned rel = mv.rel_texture();
  if (!rel) {
    if (!m_logged_no_input) {
      lg::warn("motion blur: the velocity pass wrote no relative velocity, skipped");
      m_logged_no_input = true;
    }
    return;
  }
  const auto& gs = Gfx::g_global_settings;
  const int w = rs->render_fb_w, h = rs->render_fb_h;
  const float radius = std::max(2.f, gs.mb_radius_px * h / 2160.f);
  const int tile = std::max(4, (int)std::ceil(radius));

  // exposure = shutter x the frame's step, the step clamped to a sane window; v covers one step
  const int level = std::clamp(m_level, 1, 3) - 1;
  const float shutter = gs.mb_shutter[level];
  const float dt = g_dt_chain.load();
  const float step_scale = dt > 0.f ? std::clamp(dt, kMinDt, kMaxDt) / dt : 1.f;
  const float vscale = 0.5f * shutter * step_scale;
  m_last_dt = dt;
  m_last_scale = vscale;
  m_last_radius = radius;
  m_frames++;

  static glq::SavedState saved("motion-blur", kGlq, 0b111, 0);
  saved.capture();
  // after the capture: a (re)allocation binds its own framebuffer and texture
  ensure(w, h, tile, rs->hdr_target);
  glBindVertexArray(m_vao);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_BLEND);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

  // scene copy (a resolve when multisampled), as SMAA takes it
  glBindFramebuffer(GL_READ_FRAMEBUFFER, rs->render_fb);
  glReadBuffer(GL_COLOR_ATTACHMENT0);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_scene.fbo);
  glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);

  const int tw = (w + tile - 1) / tile, th = (h + tile - 1) / tile;
  auto& tsh = rs->shaders[ShaderId::MOTION_BLUR_TILES];
  tsh.activate();
  const GLuint tp = tsh.id();
  glUniform1i(GLQ_LOC(kGlq, tp, "src"), 0);
  glUniform1i(GLQ_LOC(kGlq, tp, "tile"), tile);
  glUniform1f(GLQ_LOC(kGlq, tp, "vscale"), vscale);
  glUniform1f(GLQ_LOC(kGlq, tp, "rmax"), radius);
  glViewport(0, 0, tw, th);
  glActiveTexture(GL_TEXTURE0);
  // 1. tile max
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_tile_max.fbo);
  glUniform1i(GLQ_LOC(kGlq, tp, "mode"), 0);
  glBindTexture(GL_TEXTURE_2D, rel);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  // 2. neighbour max
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_neighbour.fbo);
  glUniform1i(GLQ_LOC(kGlq, tp, "mode"), 1);
  glBindTexture(GL_TEXTURE_2D, m_tile_max.tex);
  glDrawArrays(GL_TRIANGLES, 0, 3);

  if (g_log_frames > 0) {
    // debug readback: tiles with a blur and the longest half-length
    g_log_frames--;
    std::vector<float> tv((size_t)tw * th * 2);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_tile_max.fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glReadPixels(0, 0, tw, th, GL_RG, GL_FLOAT, tv.data());
    int moving = 0;
    float longest = 0.f;
    for (size_t i = 0; i < tv.size(); i += 2) {
      const float l = std::sqrt(tv[i] * tv[i] + tv[i + 1] * tv[i + 1]);
      moving += l >= 0.5f;
      longest = std::max(longest, l);
    }
    lg::info("MB f={} step={:.5f}s scale={:.4f} cap={:.1f}px tiles={}/{} longest={:.1f}px",
             rs->frame_idx, dt, vscale, radius, moving, tw * th, longest);
  }

  // 3. reconstruction over the scene; RGB only (PS2 blending reads destination alpha)
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, rs->render_fb);
  glViewport(0, 0, w, h);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
  auto& sh = rs->shaders[ShaderId::MOTION_BLUR];
  sh.activate();
  const GLuint p = sh.id();
  glUniform1i(GLQ_LOC(kGlq, p, "scene"), 0);
  glUniform1i(GLQ_LOC(kGlq, p, "rel"), 1);
  glUniform1i(GLQ_LOC(kGlq, p, "nmax"), 2);
  glUniform2f(GLQ_LOC(kGlq, p, "size"), (float)w, (float)h);
  glUniform1i(GLQ_LOC(kGlq, p, "tile"), tile);
  glUniform1f(GLQ_LOC(kGlq, p, "vscale"), vscale);
  glUniform1f(GLQ_LOC(kGlq, p, "rmax"), radius);
  glUniform1i(GLQ_LOC(kGlq, p, "taps"), kTaps[level]);
  glUniform1f(GLQ_LOC(kGlq, p, "frame"), (float)(m_frames % 64));
  glUniform1f(GLQ_LOC(kGlq, p, "soft_z"), kSoftZ);
  glUniform1i(GLQ_LOC(kGlq, p, "view"), std::clamp(gs.mb_view, 0, 2));
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, m_scene.tex);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, rel);
  glActiveTexture(GL_TEXTURE2);
  glBindTexture(GL_TEXTURE_2D, m_neighbour.tex);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  prof.add_draw_call(3);
  saved.restore();
}

void Renderer::draw_debug_window() {
  auto& gs = Gfx::g_global_settings;
  ImGui::SliderInt("Level (0 off, 1-3)", &gs.mb_level, 0, 3);
  ImGui::SliderFloat3("Shutter (weak, medium, strong)", gs.mb_shutter, 0.f, 2.f);
  ImGui::SliderFloat("Radius cap at 2160p (px)", &gs.mb_radius_px, 4.f, 128.f);
  ImGui::SliderInt("Camera (0 off, 1 vehicles, 2 always)", &gs.mb_camera, 0, 2);
  ImGui::SliderInt("View (1 velocity, 2 tiles)", &gs.mb_view, 0, 2);
  ImGui::Text("vehicle %d  camera blur %d", (int)g_vehicle_chain.load(), (int)m_camera);
  ImGui::Text("active %d  step %.4f s  half-length %.3f x v  cap %.1f px", m_active, m_last_dt,
              m_last_scale, m_last_radius);
}

}  // namespace motion_blur
