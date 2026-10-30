#pragma once

#include "game/graphics/opengl_renderer/BucketRenderer.h"

/*!
 * Temporal accumulation of a scalar screen-space signal whose noise pattern rotates per frame
 * (GTAO today; a dithered contact-shadow ray can reuse it). Jak 1, motion vectors required.
 *
 * Each run reprojects the previous result to this frame: the frame camera's reprojection
 * (motion_vectors::frame().cam_reproj) from the render target's depth, or the merc object velocity
 * where a character's surface is the visible one. A history texel is used only while its stored
 * clip w matches the depth expected there (disocclusion rejects it), the history is clipped to this
 * frame's signal around the texel (3x3 mean +- `gamma` std devs, inside the 3x3 min/max), so an
 * occluder that moved away leaves no trail on the surface it darkened, and the history restarts on a
 * motion-vector cut (frame not valid), a resize, a frame without motion vectors or a frame gap (a
 * frame on which the consumer did not run). The blend keeps
 * `blend` of the history on static texels and `blend_dynamic` on characters.
 *
 * Signal and result are at the accumulation size: the render target's, or half of it with each
 * texel standing for the full-res pixel the SSAO z mip 1 kept (`half_res`). Negative signal values
 * mark excluded texels; they pass through and never enter the history.
 *
 * Cost: one full-screen pass at the accumulation size, reading nine signal texels, the depth (all
 * samples), the object velocity and four history texels, writing one RG16F texel.
 */
class TemporalAccum {
 public:
  struct Inputs {
    GLuint signal = 0;  ///< R16F at the accumulation size
    int full_w = 0;     ///< render target size
    int full_h = 0;
    bool half_res = true;  ///< the SSAO rotated-grid half-resolution layout
    GLuint depth = 0;      ///< render target depth (multisample when samples > 1)
    int samples = 1;
    GLuint dyn_mask = 0;  ///< optional R8 mask, the depth's sample count: 0.5 marks characters
    float blend = 0.9f;
    float blend_dynamic = 0.6f;
    float w_tol = 0.05f;  ///< share of the expected clip w a history texel may differ by
    bool clamp = true;    ///< clamp the history to this frame's 3x3 signal range
    float gamma = 1.5f;   ///< > 0: narrow that range to mean +- gamma std devs
  };

  TemporalAccum() = default;
  ~TemporalAccum();
  TemporalAccum(const TemporalAccum&) = delete;
  TemporalAccum& operator=(const TemporalAccum&) = delete;

  /// True when this frame can accumulate (motion vectors produced; in stereo, per eye).
  static bool available(const SharedRenderState* rs);
  /// Accumulate; returns the RG16F result (r value, g clip w), 0 when not available. The result
  /// stays valid until the next run. Leaves its framebuffer, program and texture units 0-3 bound:
  /// callers run it inside their own saved GL state.
  GLuint run(SharedRenderState* rs, const Inputs& in);
  /// Forget the history (the next run starts from the signal).
  void reset() { m_have = false; }
  /// Free the history targets (allocated again on the next run).
  void release();

 private:
  void ensure(int w, int h);
  struct Target {
    GLuint fbo = 0;
    GLuint tex = 0;
  };
  Target m_hist[2];
  int m_cur = 0;
  int m_w = 0, m_h = 0;
  bool m_have = false;
  u64 m_last_frame = 0;
  float m_prev_jitter[2] = {0.f, 0.f};
  GLuint m_vao = 0;
};
