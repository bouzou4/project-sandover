#pragma once

#include <memory>
#include <string>

#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/opengl_renderer/Fbo.h"
#include "game/graphics/opengl_renderer/UpscaleInputs.h"

namespace glq {
class SavedState;
}

/*!
 * NVIDIA DLSS super resolution (Jak 1, AA_DLSS): DLAA at render scale 1, the upscaler below it.
 * It takes the frame at the same point and from the same sources as FSR 3 (Fsr3Upscaler.h): the
 * scene renders single-sample at the render size with the shared projection jitter
 * (scene_jitter, Shader.h); after the sprite bucket, bloom and the velocity pass, run() hands the
 * colour, depth and velocity to DLSS and the debug, HUD text and subtitle buckets then draw at
 * display size into display(), which the final pass (tonemap, HDR output encode) reads.
 *
 * DLSS runs on a Direct3D 11 device on the GL context's adapter, through the NGX D3D11 API
 * (the SDK is fetched at build time, third-party/nvidia-dlss). The textures it reads and writes
 * are D3D11 textures shared with GL through WGL_NV_DX_interop2 and kept locked for GL except
 * around the evaluate call; unlocking hands them to D3D after GL's commands on them, locking hands
 * them back after D3D's. One GL pass copies the inputs into the shared textures (colour as is,
 * depth into R32F, velocity into RG16F) with the rows flipped, so DLSS sees the frame upright
 * in D3D's top-down convention; a flipped blit brings the output back into the GL display target.
 *
 * Inputs: velocity is p_prev - p_cur in render pixels with +y up (MotionVectors.h); after the row
 * flip that is DLSS's convention with a motion vector scale of (1, -1). The jitter is the
 * projection's content offset in render pixels, given with y negated for the same reason. Depth
 * is inverted (GEQUAL, cleared to 0); colour is HDR (exposure 1) when the scene target is FP16,
 * LDR otherwise. Reset on every invalid motion frame (cuts, loads, resizes).
 */
class DlssUpscaler {
 public:
  using Inputs = UpscaleInputs;

  DlssUpscaler();
  ~DlssUpscaler();
  DlssUpscaler(const DlssUpscaler&) = delete;
  DlssUpscaler& operator=(const DlssUpscaler&) = delete;

  /// Startup check, cheap (no device): WGL_NV_DX_interop2, an NVIDIA adapter behind the GL
  /// context and the NGX feature requirements. Logs why not, and whether nvngx_dlss.dll is beside
  /// gk; starts NGX's over-the-air update check on its own thread.
  static bool context_supported();
  /// After context_supported(): makes the device and initialises NGX, which resolves the DLSS
  /// runtime (nvngx_dlss.dll beside gk first, else the driver's NGX store). False, with one line
  /// naming what was tried, when NGX reports no super sampling.
  bool probe();
  /// The D3D11 device, the interop device and NGX, made on first use; false, logged once, on
  /// failure.
  bool ready();

  /// Frame start (DLSS active): this frame's jitter, loaded into the scene programs.
  void begin_frame(int rw, int rh, int dw);
  /// DLSS to dw x dh; the display target becomes the render framebuffer. False when the feature
  /// could not be created or evaluated (the caller then stops offering the mode).
  bool run(SharedRenderState* rs, ScopedProfilerNode& prof, const Inputs& in, int dw, int dh);

  /// Write the last run()'s output (before the text buckets) as `<stem>_dlss.exr`.
  void dump_output(const std::string& stem, u64 frame);
  /// The display-size target the last run() drew; the text buckets draw into it.
  Fbo* display();
  /// Free the feature and the targets (the device and NGX stay); cheap when none exist.
  void release();
  void draw_debug_window();

 private:
  /// The display target as the render framebuffer, with an empty depth and stencil for the text
  /// buckets (as at the frame's start), and the caller's GL state back.
  void finish(SharedRenderState* rs, const glq::SavedState& saved, int dw, int dh);

  struct Impl;
  std::unique_ptr<Impl> m;
};
