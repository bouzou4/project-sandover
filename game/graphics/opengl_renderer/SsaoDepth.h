#pragma once

#include <string>

#include "game/graphics/opengl_renderer/BucketRenderer.h"

/*!
 * Averages GPU time between begin() and end() with GL_TIMESTAMP queries. Results are read a
 * few frames late so the CPU never waits on the GPU.
 */
class GpuTimer {
 public:
  explicit GpuTimer(const std::string& name) : m_name(name) {}
  ~GpuTimer();
  void begin();
  void end();
  /// Average of the last completed window, in milliseconds (negative if none yet).
  float last_avg_ms() const { return m_last_avg_ms; }

 private:
  void collect(int slot);
  static constexpr int kRing = 4;
  static constexpr int kWindow = 240;
  std::string m_name;
  bool m_init = false;
  GLuint m_queries[kRing][2] = {};
  bool m_pending[kRing] = {};
  int m_head = 0;
  double m_sum_ms = 0;
  double m_min_ms = 0;
  double m_max_ms = 0;
  int m_count = 0;
  float m_last_avg_ms = -1.f;
};

/*!
 * Copies the main framebuffer's depth into a sampleable texture partway through the frame, for
 * screen-space effects (SSAO) that need the depth of the opaque scene. Also provides a debug view
 * of that depth and a one-shot probe of how the driver resolves multisampled depth.
 * Nothing here allocates or draws unless one of the ssao_* settings in GfxGlobalSettings is on.
 */
class SsaoDepth {
 public:
  SsaoDepth() = default;
  ~SsaoDepth();
  SsaoDepth(const SsaoDepth&) = delete;
  SsaoDepth& operator=(const SsaoDepth&) = delete;

  /// Jak 1 bucket after which the capture runs, from the current settings.
  static int capture_bucket_jak1();

  /// Copy the depth of render_state->render_fb into this object's texture. `resolved_only`
  /// always writes the single-sample copy and skips the MSAA copy, the probe and the timer.
  void capture(SharedRenderState* render_state,
               ScopedProfilerNode& prof,
               bool resolved_only = false);

  /// Draw the captured depth, linearized, over the whole render framebuffer.
  void draw_debug(SharedRenderState* render_state, ScopedProfilerNode& prof);

  void draw_debug_window();

  GpuTimer& frame_timer() { return m_frame_timer; }

  /// Single-sample depth texture (GL_DEPTH24_STENCIL8), or 0 if nothing was captured.
  GLuint depth_tex() const { return m_resolved.tex; }

 private:
  struct Target {
    GLuint fbo = 0;
    GLuint tex = 0;
    int w = 0;
    int h = 0;
    int samples = 0;
  };
  void ensure_target(Target& t, int w, int h, int samples);
  void free_target(Target& t);
  void run_msaa_probe(SharedRenderState* render_state);

  Target m_resolved;
  Target m_ms;
  Target m_probe_out;
  GLuint m_vao = 0;
  bool m_captured_ms = false;
  bool m_captured = false;
  float m_far_dist = 400.f * 4096.f;
  bool m_raw = false;
  std::string m_probe_result;
  GpuTimer m_capture_timer{"ssao-depth-capture"};
  GpuTimer m_frame_timer{"buckets"};
};
