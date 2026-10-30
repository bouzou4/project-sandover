// OpenGLRenderer: stereo and OpenXR frame path (Jak 1).

#include <algorithm>
#include <cmath>
#include <cstring>

#include "OpenGLRenderer.h"

#include "common/log/log.h"
#include "common/util/FileUtil.h"

#include "game/graphics/gfx.h"
#include "game/graphics/opengl_renderer/GlQueries.h"
#include "game/graphics/opengl_renderer/OpenGLRendererInternal.h"
#include "game/graphics/xr/OpenXr.h"

/*!
 * XR 2D layer: with a UI image to fill, the canvases are cleared and handed to the buckets
 * (ui_canvas_draw); without one they are freed and the 2D draws go into the eyes as before.
 */
void OpenGLRenderer::ui_layer_begin(xr::FrameTargets* xr) {
  auto& rs = m_render_state;
  if (!xr || !xr->ui.gl_texture) {
    rs.ui_canvas_fb[0] = rs.ui_canvas_fb[1] = 0;
    for (auto& c : m_ui_canvas) {
      if (c.valid) {
        c.clear();
      }
    }
    return;
  }
  GLint fb = 0;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
  for (int k = 0; k < 2; k++) {
    auto& c = m_ui_canvas[k];
    if (!c.matches(xr::kUiCanvasW, xr::kUiCanvasH, 1)) {
      c.clear();
      // FP16: an additive draw on the white canvas must not clip, or it reads as coverage
      c = make_fbo(xr::kUiCanvasW, xr::kUiCanvasH, 1, true, true);
      glBindTexture(GL_TEXTURE_2D, *c.tex_id);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glBindTexture(GL_TEXTURE_2D, 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, c.fbo_id);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glStencilMask(0xff);
    const float v = k == 0 ? 0.f : 1.f;
    glClearColor(v, v, v, 0.f);
    glClearDepth(0.0);
    glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    rs.ui_canvas_fb[k] = c.fbo_id;
  }
  glClearColor(0.f, 0.f, 0.f, 0.f);
  rs.ui_canvas_w = xr::kUiCanvasW;
  rs.ui_canvas_h = xr::kUiCanvasH;
  glBindFramebuffer(GL_FRAMEBUFFER, fb);
  glq::invalidate_state();
}

/*!
 * XR 2D layer: the canvases into the runtime's UI image (premultiplied, composited as a quad
 * layer), and the same quad drawn into the side-by-side frame for the window and screenshots.
 */
void OpenGLRenderer::ui_layer_finish(xr::FrameTargets& xr) {
  auto& rs = m_render_state;
  // the canvas spans the game's 2D screen (GS x +-256, y +-112) at the game's aspect: the chain's
  // projection with the head-view widening undone (game_tan, howto/xr.md), else as it is
  float aspect = 16.f / 9.f;
  const auto& p = rs.game_camera.persp;
  if (rs.game_camera.valid && p[0][0] != 0.f && p[1][1] != 0.f) {
    float sx = 1.f, sy = 1.f;
    if (xr.chain_head && xr.game_tan_x > 0.f && xr.game_tan_y > 0.f && xr.view_tan_x > 0.f &&
        xr.view_tan_y > 0.f) {
      sx = xr.view_tan_x / xr.game_tan_x;
      sy = xr.view_tan_y / xr.game_tan_y;
    }
    aspect = std::clamp(std::abs((256.f * p[1][1] * sy) / (112.f * p[0][0] * sx)), 1.f, 3.f);
  }
  static float logged_aspect = 0.f;
  if (std::abs(aspect - logged_aspect) > 1e-3f) {
    logged_aspect = aspect;
    lg::info("[xr] ui canvas aspect {:.4f} (chain head {}, game tan {:.3f} {:.3f})", aspect,
             xr.chain_head, xr.game_tan_x, xr.game_tan_y);
  }
  GLint fb = 0;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
  if (!m_xr_fbo) {
    glGenFramebuffers(1, &m_xr_fbo);
  }
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_FRAMEBUFFER_SRGB);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  auto& sh = rs.shaders[ShaderId::XR_UI];
  sh.activate();
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, *m_ui_canvas[0].tex_id);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, *m_ui_canvas[1].tex_id);
  glUniform1i(glGetUniformLocation(sh.id(), "on_black"), 0);
  glUniform1i(glGetUniformLocation(sh.id(), "on_white"), 1);
  const GLint rect = glGetUniformLocation(sh.id(), "rect");
  glBindVertexArray(screen_vao);

  glBindFramebuffer(GL_FRAMEBUFFER, m_xr_fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, xr.ui.gl_texture, 0);
  glViewport(0, 0, xr.ui.width, xr.ui.height);
  glDisable(GL_BLEND);
  set_tonemap_uniforms(sh, false);
  glUniform1i(glGetUniformLocation(sh.id(), "encode"), 1);
  glUniform4f(rect, -1.f, -1.f, 1.f, 1.f);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glUniform1i(glGetUniformLocation(sh.id(), "encode"), 0);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
  xr.ui.aspect = aspect;
  xr.ui.drawn = true;

  // preview: the head-locked quad as each eye of the side-by-side frame sees it
  if (xr.tracked && m_stereo_fbo.valid && xr.render_tan_x > 0.f && xr.render_tan_y > 0.f) {
    float hw = 0.f, hh = 0.f;
    xr::ui_half_size(aspect, &hw, &hh);
    const float d = xr::kUiDistanceM;
    const int w = m_stereo_fbo.width / 2, h = m_stereo_fbo.height;
    glBindFramebuffer(GL_FRAMEBUFFER, m_stereo_fbo.fbo_id);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glBlendEquation(GL_FUNC_ADD);
    for (int eye = 0; eye < 2; eye++) {
      const int dst_x = ((eye == 1) != Gfx::g_global_settings.stereo_swap_eyes) ? w : 0;
      const float hx = xr.eye[eye].head_x_m;  // OpenXR +x right
      glViewport(dst_x, 0, w, h);
      glUniform4f(rect, (-hw - hx) / (d * xr.render_tan_x), -hh / (d * xr.render_tan_y),
                  (hw - hx) / (d * xr.render_tan_x), hh / (d * xr.render_tan_y));
      glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  }
  glBindVertexArray(0);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, 0);
  glActiveTexture(GL_TEXTURE0);
  glEnable(GL_DEPTH_TEST);
  glBindFramebuffer(GL_FRAMEBUFFER, fb);
  glq::invalidate_state();
}

/*!
 * Stereo debug path (Jak 1): replays the frame's DMA chain once per eye with the camera moved by
 * +/- IPD/2 along its x axis, and copies each eye into one half of a double-width buffer. The
 * second pass skips VIF interrupts and per-frame simulation so the game sees a single frame.
 */
void OpenGLRenderer::dispatch_buckets_stereo(DmaFollower dma,
                                             ScopedProfilerNode& prof,
                                             bool sync_after_buckets,
                                             xr::FrameTargets* xr) {
  const auto& gs = Gfx::g_global_settings;
  const float units_per_m = 4096.f / std::max(gs.stereo_world_scale, 0.001f);
  const float half_ipd = 0.5f * gs.stereo_ipd_m * units_per_m;
  // camera-space +x is screen left here (measured), so the left eye sits at +x
  float eye_x[2] = {half_ipd, -half_ipd};
  if (xr) {
    // OpenXR +x is right: negate the runtime's eye positions
    eye_x[0] = -xr->eye[0].head_x_m * units_per_m;
    eye_x[1] = -xr->eye[1].head_x_m * units_per_m;
  }
  // head tracking: the game's frustum edges (tangents) against the runtime's, per screen axis
  bool tracked = false;
  float fov_x = 1.f, fov_y = 1.f;
  // also without tracking: the eyes then only take the IPD offset, but keep the runtime's fov
  if (xr && m_render_state.game_camera.valid) {
    const auto& gc = m_render_state.game_camera;
    const float fog = gc.fog.x();
    const auto& p = gc.persp;
    if (fog != 0.f && p[0][0] != 0.f && p[1][1] != 0.f && xr->render_tan_x > 0.f &&
        xr->render_tan_y > 0.f) {
      // the renderers map GS x +-256 and y +-112 around the centre to the full target
      const float game_tan_x = std::abs(256.f * p[2][3] / (fog * p[0][0]));
      const float game_tan_y = std::abs(112.f * p[2][3] / (fog * p[1][1]));
      fov_x = game_tan_x / xr->render_tan_x;
      fov_y = game_tan_y / xr->render_tan_y;
      m_xr_game_tan[0] = game_tan_x;
      m_xr_game_tan[1] = game_tan_y;
      tracked = true;
      // the projection changes with a margin step or a cutscene zoom: log changes over 2 %
      if (std::abs(game_tan_x - m_xr_logged_tan[0]) > 0.02f * game_tan_x ||
          std::abs(game_tan_y - m_xr_logged_tan[1]) > 0.02f * game_tan_y) {
        m_xr_logged_tan[0] = game_tan_x;
        m_xr_logged_tan[1] = game_tan_y;
        lg::info(
            "[xr] game frustum half-tangents x {:.3f} y {:.3f} -> eye scale x {:.3f} y {:.3f} "
            "(game's own x {:.3f} y {:.3f})",
            game_tan_x, game_tan_y, fov_x, fov_y, xr->game_tan_x, xr->game_tan_y);
      }
    }
  }
  const Fbo* src = m_fbo_state.render_fbo;
  const int rw = src->width;
  const int rh = src->height;
  // per-eye temporal AA: each eye's image is its upscaler's output at the display size
  const bool temporal = m_upscaler.temporal();
  const int w = temporal && m_display_w > 0 ? m_display_w : rw;
  const int h = temporal && m_display_h > 0 ? m_display_h : rh;
  if (!m_stereo_fbo.matches(2 * w, h, 1) || m_stereo_hdr != m_fbo_state.hdr) {
    m_stereo_fbo.clear();
    m_stereo_fbo = make_fbo(2 * w, h, 1, false, m_fbo_state.hdr);
    m_stereo_hdr = m_fbo_state.hdr;
    glq::invalidate_state();
    glBindFramebuffer(GL_FRAMEBUFFER, src->fbo_id);
  }
  // an upscaler leaves the display target as the render target: eye 1 starts on the scene's
  const GLuint scene_fb = m_render_state.render_fb;
  const GLuint scene_depth = m_render_state.render_fb_depth_tex;
  const int scene_w = m_render_state.render_fb_w, scene_h = m_render_state.render_fb_h;

  for (int eye = 0; eye < 2; eye++) {
    m_probe_eye = eye;
    m_render_state.eye_slot = eye;
    m_render_state.stereo_eye_x = eye_x[eye];
    m_render_state.eye.active = false;
    if (tracked) {
      const auto& t = xr->eye[eye];
      auto& ev = m_render_state.eye;
      ev.active = true;
      std::memcpy(ev.rot, t.rot, sizeof(ev.rot));
      for (int k = 0; k < 3; k++) {
        ev.t[k] = t.pos_m[k] * units_per_m;
      }
      if (xr->chain_head) {
        // the chain's camera is the head view it was made from: only the rest of the way
        // (eye offset, head motion since) is left, rot * H^T and H * (t - head_t)
        const auto& H = xr->head_rot;
        float d[3];
        for (int k = 0; k < 3; k++) {
          d[k] = ev.t[k] - xr->head_t[k];
        }
        for (int r = 0; r < 3; r++) {
          ev.t[r] = H[r][0] * d[0] + H[r][1] * d[1] + H[r][2] * d[2];
          for (int c = 0; c < 3; c++) {
            ev.rot[r][c] = t.rot[r][0] * H[c][0] + t.rot[r][1] * H[c][1] + t.rot[r][2] * H[c][2];
          }
        }
      }
      ev.fov_x = fov_x;
      ev.fov_y = fov_y;
      ev.tan_x = xr->render_tan_x;
      ev.tan_y = xr->render_tan_y;
      ev.head_chain = xr->chain_head;
      m_render_state.stereo_eye_x = 0.f;
    }
    // a re-drawn chain repeats no per-frame work in either eye
    m_render_state.stereo_repeat_pass = eye == 1 || m_xr_repeat;
    if (eye == 1) {
      m_render_state.reset();
      m_render_state.render_fb = scene_fb;
      m_render_state.render_fb_depth_tex = scene_depth;
      m_render_state.render_fb_w = scene_w;
      m_render_state.render_fb_h = scene_h;
      m_upscaler.clear_output();
      glBindFramebuffer(GL_FRAMEBUFFER, src->fbo_id);
      glViewport(0, 0, rw, rh);
      glDisable(GL_SCISSOR_TEST);
      glClearColor(0.0, 0.0, 0.0, 0.0);
      glClearDepth(0.0);
      glClearStencil(0);
      glDepthMask(GL_TRUE);
      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
      glDisable(GL_BLEND);
      m_render_state.stencil_dirty = false;
    }
    auto p = prof.make_scoped_child(eye == 0 ? "eye-left" : "eye-right");
    dispatch_buckets(dma, p, sync_after_buckets);

    const int dst_x = ((eye == 1) != gs.stereo_swap_eyes) ? w : 0;
    // the upscaled eye with its text buckets; the scene target if the upscaler gave nothing
    const Fbo* eye_src =
        m_upscaler.upscaled() && m_upscaler.display_src() ? m_upscaler.display_src() : src;
    auto copy_eye = [&](const Fbo* from, int x) {
      const bool same = from->width == w && from->height == h;
      glBindFramebuffer(GL_READ_FRAMEBUFFER, from->fbo_id);
      glReadBuffer(GL_COLOR_ATTACHMENT0);
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_stereo_fbo.fbo_id);
      glDisable(GL_SCISSOR_TEST);
      glBlitFramebuffer(0, 0, from->width, from->height, x, 0, x + w, h, GL_COLOR_BUFFER_BIT,
                        same ? GL_NEAREST : GL_LINEAR);
      glBindFramebuffer(GL_FRAMEBUFFER, src->fbo_id);
    };
    if (!(eye == 0 && m_upscaler.eye0_deferred())) {
      copy_eye(eye_src, dst_x);
    }
    if (eye == 1 && m_upscaler.eye0_deferred()) {
      // eye 0's FSR 4 output, back with eye 1's hand-back
      copy_eye(m_upscaler.finish_eye0_deferred(), gs.stereo_swap_eyes ? w : 0);
    }
  }
  m_render_state.eye_slot = 0;
  if (xr) {
    // the FP16 target holds the scene before the tonemap; the swapchains take display values
    GLuint xr_src = m_stereo_fbo.fbo_id;
    if (m_fbo_state.hdr) {
      if (!m_xr_sdr_fbo.matches(2 * w, h, 1)) {
        m_xr_sdr_fbo.clear();
        m_xr_sdr_fbo = make_fbo(2 * w, h, 1, false);
      }
      glDisable(GL_SCISSOR_TEST);
      tonemap_to_sdr(m_stereo_fbo, m_xr_sdr_fbo);
      xr_src = m_xr_sdr_fbo.fbo_id;
    }
    for (int eye = 0; eye < 2; eye++) {
      const int dst_x = ((eye == 1) != gs.stereo_swap_eyes) ? w : 0;
      if (tracked) {
        copy_eye_to_xr_cropped(*xr, eye, xr_src, dst_x, w, h);
      } else {
        copy_eye_to_xr(xr->eye[eye], xr_src, dst_x, w, h);
      }
    }
    glq::invalidate_state();
    glBindFramebuffer(GL_FRAMEBUFFER, src->fbo_id);
  }
  m_render_state.stereo_eye_x = 0.f;
  m_render_state.eye.active = false;
  m_render_state.stereo_repeat_pass = false;
  if (xr) {
    xr->rendered = true;
  }
}

/*!
 * Copies one eye from the side-by-side buffer into its OpenXR swapchain image. The game draws a
 * centred frustum, so the image is shifted to put its centre on the runtime's forward direction
 * for that eye (the runtime's field of view is usually asymmetric). The bytes are copied as they
 * are: with an sRGB swapchain format the display-encoded output is read back unchanged.
 */
void OpenGLRenderer::copy_eye_to_xr(const xr::EyeTarget& t,
                                    GLuint src_fbo,
                                    int src_x,
                                    int w,
                                    int h) {
  if (!m_xr_fbo) {
    glGenFramebuffers(1, &m_xr_fbo);
  }
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_xr_fbo);
  glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.gl_texture, 0);
  glDisable(GL_FRAMEBUFFER_SRGB);
  glDisable(GL_SCISSOR_TEST);
  // NDC position of the straight-ahead direction in the runtime's frustum
  const float fx = -(t.tan_right + t.tan_left) / (t.tan_right - t.tan_left);
  const float fy = -(t.tan_up + t.tan_down) / (t.tan_up - t.tan_down);
  const int dx = (int)std::lround(0.5f * fx * t.width);
  const int dy = (int)std::lround(0.5f * fy * t.height);
  if (dx != 0 || dy != 0) {
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
  }
  glBindFramebuffer(GL_READ_FRAMEBUFFER, src_fbo);
  const bool same_size = w == t.width && h == t.height;
  glBlitFramebuffer(src_x, 0, src_x + w, h, dx, dy, dx + t.width, dy + t.height,
                    GL_COLOR_BUFFER_BIT, same_size ? GL_NEAREST : GL_LINEAR);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_xr_fbo);
  glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
}

namespace {
/// Pixel rect (x0, y0, x1, y1) of one eye's frustum inside the symmetric w x h frame.
void eye_crop(const xr::FrameTargets& frame, int eye, int w, int h, int r[4]) {
  const auto& t = frame.eye[eye];
  const float tx = frame.render_tan_x, ty = frame.render_tan_y;
  auto px = [&](float tan) { return (int)std::lround((tan + tx) / (2.f * tx) * w); };
  auto py = [&](float tan) { return (int)std::lround((tan + ty) / (2.f * ty) * h); };
  r[0] = px(t.tan_left);
  r[1] = py(t.tan_down);
  r[2] = px(t.tan_right);
  r[3] = py(t.tan_up);
}
}  // namespace

/// XR frame: each eye's swapchain image, as given to the runtime, to <stem>-left/-right.png.
void OpenGLRenderer::save_xr_eyes(const xr::FrameTargets& frame, const std::string& path) {
  const std::string stem = path.size() > 4 && path.substr(path.size() - 4) == ".png"
                               ? path.substr(0, path.size() - 4)
                               : path;
  if (!m_xr_fbo) {
    glGenFramebuffers(1, &m_xr_fbo);
  }
  // the poses of the dumped frame: eye drawn vs the head the chain was built from
  const auto& r = frame.eye[0].rot;
  const auto& h = frame.head_rot;
  lg::info(
      "[xr] eye dump: eye0 rot {:.6f} {:.6f} {:.6f} / {:.6f} {:.6f} {:.6f} / {:.6f} {:.6f} {:.6f} "
      "chain {} head {:.6f} {:.6f} {:.6f} / {:.6f} {:.6f} {:.6f} / {:.6f} {:.6f} {:.6f}",
      r[0][0], r[0][1], r[0][2], r[1][0], r[1][1], r[1][2], r[2][0], r[2][1], r[2][2],
      frame.chain_head ? 1 : 0, h[0][0], h[0][1], h[0][2], h[1][0], h[1][1], h[1][2], h[2][0],
      h[2][1], h[2][2]);
  GLint old_read = 0;
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read);
  const GLboolean old_srgb = glIsEnabled(GL_FRAMEBUFFER_SRGB);
  glDisable(GL_FRAMEBUFFER_SRGB);
  for (int eye = 0; eye < 2; eye++) {
    const auto& t = frame.eye[eye];
    if (!t.gl_texture) {
      continue;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_xr_fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.gl_texture,
                           0);
    finish_screenshot(stem + (eye == 0 ? "-left.png" : "-right.png"), t.width, t.height, 0, 0,
                      m_xr_fbo, GL_COLOR_ATTACHMENT0, false);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_xr_fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
  }
  // the 2D layer's image as the runtime gets it (premultiplied; alpha dropped)
  if (frame.ui.drawn && frame.ui.gl_texture) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_xr_fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           frame.ui.gl_texture, 0);
    finish_screenshot(stem + "-ui.png", frame.ui.width, frame.ui.height, 0, 0, m_xr_fbo,
                      GL_COLOR_ATTACHMENT0, false);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_xr_fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
  }
  glBindFramebuffer(GL_READ_FRAMEBUFFER, old_read);
  if (old_srgb) {
    glEnable(GL_FRAMEBUFFER_SRGB);
  }
}

/// XR frame sequence: the eye images of N consecutive drawn display frames, copied on the GPU
/// (no stall that would move the timing) and saved as xrseqSS-fKK-left/-right.png afterwards.
void OpenGLRenderer::xr_seq_capture(const xr::FrameTargets& frame) {
  const auto& gs = Gfx::g_global_settings;
  if (gs.xr_seq_serial != m_xr_seq_serial) {
    m_xr_seq_serial = gs.xr_seq_serial;
    if (!m_xr_seq_tex.empty()) {
      glDeleteTextures((GLsizei)m_xr_seq_tex.size(), m_xr_seq_tex.data());
    }
    m_xr_seq_n = gs.xr_seq_frames;
    m_xr_seq_done = 0;
    m_xr_seq_tex.assign(2 * m_xr_seq_n, 0);
    for (int i = 0; i < 2 * m_xr_seq_n; i++) {
      const auto& t = frame.eye[i % 2];
      GLint fmt = GL_SRGB8_ALPHA8;
      glBindTexture(GL_TEXTURE_2D, t.gl_texture);
      glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &fmt);
      glGenTextures(1, &m_xr_seq_tex[i]);
      glBindTexture(GL_TEXTURE_2D, m_xr_seq_tex[i]);
      glTexStorage2D(GL_TEXTURE_2D, 1, (GLenum)fmt, t.width, t.height);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    glq::invalidate_state();
    return;  // allocation frame: the sequence starts with the next one
  }
  if (m_xr_seq_done >= m_xr_seq_n) {
    return;
  }
  const int k = m_xr_seq_done++;
  for (int eye = 0; eye < 2; eye++) {
    const auto& t = frame.eye[eye];
    glCopyImageSubData(t.gl_texture, GL_TEXTURE_2D, 0, 0, 0, 0, m_xr_seq_tex[2 * k + eye],
                       GL_TEXTURE_2D, 0, 0, 0, 0, t.width, t.height, 1);
  }
  const auto deg = [](float y, float x) { return std::atan2(y, x) * 57.29578f; };
  const auto& r = frame.eye[0].rot;
  const auto& h = frame.head_rot;
  const auto& e0 = frame.eye[0];
  lg::info(
      "[xr] seq {} frame {}: fresh {} game tan x {:.6f} y {:.6f} view tan x {:.6f} y {:.6f} "
      "margin {:.4f} deg render tan x {:.6f} y {:.6f} eye0 fov tan {:.6f} {:.6f} {:.6f} {:.6f} "
      "head_x {:.6f} {:.6f} eye0 yaw {:.4f} chain yaw {:.4f}",
      m_xr_seq_serial, k, m_xr_repeat ? 0 : 1, m_xr_game_tan[0], m_xr_game_tan[1], frame.view_tan_x,
      frame.view_tan_y, deg(frame.view_tan_y, 1.f) - deg(frame.render_tan_y, 1.f),
      frame.render_tan_x, frame.render_tan_y, e0.tan_left, e0.tan_right, e0.tan_up, e0.tan_down,
      frame.eye[0].head_x_m, frame.eye[1].head_x_m, deg(r[0][2], r[0][0]), deg(h[0][2], h[0][0]));
  if (m_xr_seq_done < m_xr_seq_n) {
    return;
  }
  if (!m_xr_fbo) {
    glGenFramebuffers(1, &m_xr_fbo);
  }
  GLint old_read = 0;
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read);
  const GLboolean old_srgb = glIsEnabled(GL_FRAMEBUFFER_SRGB);
  glDisable(GL_FRAMEBUFFER_SRGB);
  for (int i = 0; i < 2 * m_xr_seq_n; i++) {
    const auto& t = frame.eye[i % 2];
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_xr_fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           m_xr_seq_tex[i], 0);
    finish_screenshot(file_util::make_screenshot_filepath(
                          m_version, fmt::format("xrseq{:02d}-f{:02d}-{}", m_xr_seq_serial, i / 2,
                                                 i % 2 ? "right" : "left")),
                      t.width, t.height, 0, 0, m_xr_fbo, GL_COLOR_ATTACHMENT0, false);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_xr_fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
  }
  glBindFramebuffer(GL_READ_FRAMEBUFFER, old_read);
  if (old_srgb) {
    glEnable(GL_FRAMEBUFFER_SRGB);
  }
  glDeleteTextures((GLsizei)m_xr_seq_tex.size(), m_xr_seq_tex.data());
  m_xr_seq_tex.clear();
  glq::invalidate_state();
  lg::info("[xr] seq {} saved {} frames", m_xr_seq_serial, m_xr_seq_n);
}

/*!
 * Head-tracked copy: the frame was drawn with a symmetric frustum of half-tangents
 * render_tan_x/y; the eye's own (asymmetric) frustum is cut out of it and scaled to its image.
 */
void OpenGLRenderer::copy_eye_to_xr_cropped(const xr::FrameTargets& frame,
                                            int eye,
                                            GLuint src_fbo,
                                            int src_x,
                                            int w,
                                            int h) {
  const auto& t = frame.eye[eye];
  if (!m_xr_fbo) {
    glGenFramebuffers(1, &m_xr_fbo);
  }
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_xr_fbo);
  glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.gl_texture, 0);
  glDisable(GL_FRAMEBUFFER_SRGB);
  glDisable(GL_SCISSOR_TEST);
  int r[4];
  eye_crop(frame, eye, w, h, r);
  const int x0 = src_x + r[0], x1 = src_x + r[2];
  const int y0 = r[1], y1 = r[3];
  glBindFramebuffer(GL_READ_FRAMEBUFFER, src_fbo);
  const bool same_size = x1 - x0 == t.width && y1 - y0 == t.height;
  glBlitFramebuffer(x0, y0, x1, y1, 0, 0, t.width, t.height, GL_COLOR_BUFFER_BIT,
                    same_size ? GL_NEAREST : GL_LINEAR);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_xr_fbo);
  glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
}
