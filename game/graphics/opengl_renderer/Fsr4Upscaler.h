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
 * AMD FSR 4 (Jak 1, AA_FSR4): native AA at render scale 1, the upscaler below it. It takes the
 * frame at the same point, from the same sources and with the same jitter sequence as FSR 3.1 and
 * DLSS (Fsr3Upscaler.h, DlssUpscaler.h); the debug, HUD text and subtitle buckets then draw at
 * display size into display().
 *
 * FSR 4 runs through the ffx-api (amd_fidelityfx_loader_dx12.dll, loaded at run time) on the
 * process's D3D12 device (D3d12Bridge.h). One GL pass copies colour, depth (to R32F) and velocity
 * into shared textures with the rows flipped, the bridge hands them to the D3D12 queue, FSR 4
 * dispatches there, and a flipped blit brings the output back into the GL display target.
 *
 * Inputs: velocity is p_prev - p_cur in render pixels, +y up; after the row flip that is a motion
 * vector scale of (1, -1). The jitter is the projection's content offset in render pixels, y
 * negated for the flip. Depth inverted (GEQUAL, cleared to 0). Colour HDR (exposure 1) when the
 * scene target is FP16. Reset on every invalid motion frame (cuts, loads, resizes).
 */
class Fsr4Upscaler {
 public:
  using Inputs = UpscaleInputs;

  Fsr4Upscaler();
  ~Fsr4Upscaler();
  Fsr4Upscaler(const Fsr4Upscaler&) = delete;
  Fsr4Upscaler& operator=(const Fsr4Upscaler&) = delete;

  /// Startup check on the D3D12 bridge: the runtime beside gk and a provider of version 4 or
  /// later for this device; a test context of it follows on a worker thread (poll()). Holds the
  /// bridge when true; logs one line naming what failed when false.
  bool probe();

  /// A second eye's context: `owner`'s runtime and provider (owner probed first and outlives
  /// this one), its own context, history and shared targets.
  void share_runtime(Fsr4Upscaler* owner, int eye);
  /// Frame start (FSR 4 active): this frame's jitter, loaded into the scene programs. `index`
  /// >= 0 picks the phase (both eyes of a display frame take the same).
  void begin_frame(int rw, int rh, int dw, s64 index = -1);
  /// FSR 4 to dw x dh; the display target becomes the render framebuffer. False when the context
  /// could not be created or the dispatch failed (the caller then stops offering the mode).
  bool run(SharedRenderState* rs, ScopedProfilerNode& prof, const Inputs& in, int dw, int dh);
  /// Stereo: `defer` leaves this eye's hand-back and output blit pending (finish_deferred), so
  /// its D3D12 dispatch runs while GL draws the other eye; `pending` (another eye's deferred
  /// targets) comes back to GL with this run's hand-back, in one lock.
  bool run(SharedRenderState* rs,
           ScopedProfilerNode& prof,
           const Inputs& in,
           int dw,
           int dh,
           bool defer,
           Fsr4Upscaler* pending);
  /// A deferred run: hand back (unless another run took it) and blit the output into display().
  void finish_deferred();
  bool deferred() const;

  /// Write the last run()'s output (before the text buckets) as `<stem>_fsr4.exr`.
  void dump_output(const std::string& stem, u64 frame);
  Fbo* display();
  /// Free the context and the targets (the runtime and the device stay).
  void release();
  /// Each frame: false once the warm-up context (made off the render thread after probe())
  /// found no FSR 4; the mode is then withdrawn and the D3D12 device released.
  bool poll();
  void draw_debug_window();

 private:
  void finish(SharedRenderState* rs, const glq::SavedState& saved, int dw, int dh);
  void output_blit();

  struct Impl;
  std::unique_ptr<Impl> m;
};
