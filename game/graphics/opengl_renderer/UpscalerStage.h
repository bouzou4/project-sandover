#pragma once

#include "common/util/Timer.h"

#include "game/graphics/opengl_renderer/DlssUpscaler.h"
#include "game/graphics/opengl_renderer/Fsr3Upscaler.h"
#include "game/graphics/opengl_renderer/Fsr4Upscaler.h"

struct RenderOptions;
namespace motion_vectors {
class Renderer;
}

/*!
 * The temporal upscale of the Jak 1 frame (FSR 3.1, DLSS or FSR 4): which one runs this frame,
 * the contexts per eye slot (SharedRenderState::eye_slot; mono uses slot 0), the jitter, the
 * reactive mask around the sprite bucket and the hand-off after it, whose display-size target
 * the text buckets and the final pass then use (display_src()).
 */
class UpscalerStage {
 public:
  /// Startup probes (Jak 1) and the second eye's shared programs and runtime.
  void init(bool jak1);
  /// Frame start, after setup_frame: this frame's upscaler from the settings, its fallbacks
  /// (no DLSS device, no FSR 4 provider) and the release of the contexts it does not use.
  void decide(const RenderOptions& settings, bool stereo);
  /// XR: eye 0's FSR 4 hand-back may wait for eye 1's (nothing draws into eye 0 after it).
  void arm_fsr4_defer(bool eligible);
  /// Per eye, before the buckets: the display size to reconstruct and this frame's jitter.
  void begin_frame(const SharedRenderState& rs, int display_w, int display_h);
  /// FSR 3.1's reactive mask: the scene before the sprite bucket, then the mask after it.
  void pre_sprite(const SharedRenderState& rs, bool hdr);
  void post_sprite(const SharedRenderState& rs, unsigned colour_tex);
  /// After the sprite bucket: the upscale; the display target becomes the render framebuffer.
  void run(SharedRenderState* rs,
           ScopedProfilerNode& prof,
           motion_vectors::Renderer& mv,
           unsigned colour_tex,
           bool hdr);

  bool temporal() const { return m_fsr3_frame || m_dlss_frame || m_fsr4_frame; }
  /// This eye's scene went to display_src().
  bool upscaled() const { return m_upscaled; }
  Fbo* display_src() const { return m_display_src; }
  /// Stereo, eye 1: no output yet.
  void clear_output() {
    m_upscaled = false;
    m_display_src = nullptr;
  }
  bool eye0_deferred() const { return m_fsr4_eye0_deferred; }
  /// Eye 0's deferred FSR 4 output, back with eye 1's hand-back.
  Fbo* finish_eye0_deferred();

  bool fsr3_ready(bool hdr) { return m_fsr3_eye[0].ready(hdr); }
  Fsr3Upscaler& fsr3(int slot) { return m_fsr3_eye[slot & 1]; }
  DlssUpscaler& dlss() { return m_dlss; }
  Fsr4Upscaler& fsr4(int slot) { return m_fsr4_eye[slot & 1]; }

 private:
  UpscaleInputs inputs(const SharedRenderState& rs,
                       const motion_vectors::Renderer& mv,
                       unsigned colour_tex,
                       bool hdr);

  bool m_upscaled = false;       // this frame's scene went to m_display_src
  Fbo* m_display_src = nullptr;  // that display-size target
  Fsr3Upscaler m_fsr3_eye[2];
  bool m_fsr3_frame = false;  // RenderOptions::temporal_aa of this frame
  Timer m_clock_eye[2];       // frame interval for the upscaler's constants, per eye slot
  DlssUpscaler m_dlss;
  bool m_dlss_frame = false;   // RenderOptions::dlss of this frame
  Fsr4Upscaler m_fsr4_eye[2];  // slot 1 borrows slot 0's runtime
  bool m_fsr4_frame = false;   // RenderOptions::fsr4 of this frame
  bool m_fsr4_serial = false;  // SANDOVER_XR_FSR4_SERIAL=1
  bool m_fsr4_defer = false;
  bool m_fsr4_eye0_deferred = false;
  int m_dw = 0, m_dh = 0;  // the display size this eye reconstructs
};
