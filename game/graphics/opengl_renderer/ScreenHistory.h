#pragma once

#include "common/math/Vector.h"

#include "game/graphics/opengl_renderer/BucketRenderer.h"

/*!
 * History of a screen-space term for passes that converge a dithered signal on their own before
 * the upscaler (FSR and DLSS clamp high-variance history away, so they do not denoise it).
 * RGBA16F ping-pong targets at the render size; the shader side is screen_history.glsl
 * (SCREEN_HISTORY_GLSL): reproject a world position into the previous frame with that frame's
 * camera and read the previous history there, its depth channel for disocclusion.
 *
 * Camera-only reprojection from the frame's own camera (world axes, game units, the 3x3 clip
 * matrix of ContactShadows): usable at any bucket, before the motion-vector pass. Moving actors
 * rely on the consumer's neighbourhood clamp. Invalid (history reset) on the first frame, a
 * resize, a frame gap and every frame the motion-vector cut flag marks.
 */
class ScreenHistory {
 public:
  ScreenHistory() = default;
  ~ScreenHistory();
  ScreenHistory(const ScreenHistory&) = delete;
  ScreenHistory& operator=(const ScreenHistory&) = delete;

  /// Start a frame: swap the targets, resize, decide validity. `k` is this frame's camera offset to
  /// clip (x, y, w = view depth), row-major; `cam` the camera position (world, game units).
  void begin(int w, int h, const float k[9], const math::Vector3f& cam, u64 frame_idx);
  /// Bind the previous history to `unit` and set the sh_* uniforms on the active `prog`.
  void set_uniforms(GLuint prog, int unit) const;
  /// This frame's target (written by the consumer's temporal pass).
  GLuint cur_fbo() const { return m_t[m_cur].fbo; }
  GLuint cur_tex() const { return m_t[m_cur].tex; }
  bool valid() const { return m_valid; }
  void release();

 private:
  struct Target {
    GLuint fbo = 0, tex = 0;
  };
  Target m_t[2];
  int m_cur = 0;
  int m_w = 0, m_h = 0;
  bool m_have_prev = false;
  bool m_valid = false;
  u64 m_prev_frame = 0;
  float m_prev_k[9] = {};
  math::Vector3f m_prev_cam;
  float m_cur_k[9] = {};
  math::Vector3f m_cur_cam;
};
