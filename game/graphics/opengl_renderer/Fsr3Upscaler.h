#pragma once

#include <array>
#include <string>

#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/opengl_renderer/Fbo.h"
#include "game/graphics/opengl_renderer/UpscaleInputs.h"

/*!
 * AMD FidelityFX FSR 3.1 temporal upscaler (Jak 1, AA_FSR3), a thin OpenGL 4.3 backend over the
 * SDK's GPU headers (third-party/fidelityfx, unmodified). The scene renders single-sample at the
 * render size with a sub-pixel projection jitter (scene_jitter, Shader.h); once it is complete
 * (after the sprite bucket, bloom and the velocity pass) run() reconstructs it at the display size
 * from the frame history, which serves as the antialiasing at render scale 1 and as the upscaler
 * below it. The debug, HUD text and subtitle buckets then draw at display size into display(),
 * and the final pass (tonemap, HDR output encode) reads it in place of the scene target.
 *
 * Passes, in the SDK's order: prepare inputs, luma pyramid, shading change pyramid, shading
 * change, prepare reactivity, luma instability, accumulate (with sharpening), RCAS, and the debug
 * view when asked. Each is a compute program built from shaders/fsr3/fsr3_<pass>.comp, the OpenGL
 * callbacks (shaders/fsr3/ffx_fsr3upscaler_callbacks_gl.h) and the SDK headers, with the bindings
 * numbered per kind (textures, images, uniform blocks, storage buffers) from the pass's
 * `fsr3-bindings` line. The reactive mask comes from the SDK's autogen pass, comparing the scene
 * before and after the sprite bucket (particles and HUD sprites).
 *
 * Inputs: colour as the scene buffer holds it (display-encoded, FP16 headroom above 1 with the
 * HDR flag), the depth texture (GEQUAL, cleared to 0: inverted, view depth from the frame camera),
 * the velocity buffer (MotionVectors.h: render-target pixels, p_prev - p_cur, unjittered, which is
 * the SDK's convention with a motion vector scale of 1 / render size), exposure 1, the frame time
 * and a reset on every invalid motion frame (cuts, loads, resizes).
 */
class Fsr3Upscaler {
 public:
  using Inputs = UpscaleInputs;

  Fsr3Upscaler() = default;
  ~Fsr3Upscaler();
  Fsr3Upscaler(const Fsr3Upscaler&) = delete;
  Fsr3Upscaler& operator=(const Fsr3Upscaler&) = delete;

  /// The context can run the passes: GL 4.3 compute and the vendored headers. Cheap; no compile.
  static bool context_supported();
  /// Build the programs of one HDR variant on its first use; false, logged once, if they fail.
  bool ready(bool hdr);

  /// Use `owner`'s programs (a second eye's context: separate history, one compile).
  void share_programs(Fsr3Upscaler* owner) { m_programs = owner; }
  /// Frame start (FSR active): this frame's jitter from the sequence for the render and display
  /// widths, loaded into the scene programs. `index` >= 0 picks the phase (both eyes of a
  /// display frame take the same), else this context's own counter advances.
  void begin_frame(int rw, int rh, int dw, s64 index = -1);
  /// No jitter (the text buckets after run(), frames without FSR). Cheap when already none.
  static void clear_jitter();
  /// Before the sprite bucket: a copy of the scene, the reactive pass's "opaque only" input.
  void capture_opaque(unsigned src_fbo, int rw, int rh, int dw, int dh, bool hdr);
  /// After the sprite bucket: the reactive mask from the scene against the copy.
  void build_reactive(unsigned colour_tex, int rw, int rh);
  /// The temporal upscale to dw x dh; the display target becomes the render framebuffer.
  void run(SharedRenderState* rs, ScopedProfilerNode& prof, const Inputs& in, int dw, int dh);

  /// Write the last run()'s output (before the text buckets) as `<stem>_fsr3.exr`.
  void dump_output(const std::string& stem, u64 frame);
  /// The display-size target the last run() drew; the text buckets draw into it.
  Fbo* display() { return &m_display; }
  /// Free every target; the next run() starts with no history.
  void release();
  void draw_debug_window();

 private:
  struct Program {
    unsigned id = 0;
    std::array<int, 16> srv{};  // resource per texture unit (-1 none)
    std::array<int, 8> uav{};   // resource per image unit
    std::array<int, 4> cb{};    // uniform block per binding
    bool atomic = false;        // the SPD counter at storage buffer binding 0
    int nsrv = 0, nuav = 0, ncb = 0;
  };
  struct Tex {
    unsigned id = 0;
    unsigned fmt = 0;
    int w = 0, h = 0, levels = 1;
  };

  bool build(int variant);
  const Fsr3Upscaler& programs() const { return m_programs ? *m_programs : *this; }
  void alloc(int rw, int rh, int dw, int dh, bool hdr);
  void free_targets();
  void clear(const Tex& t, float v0, float v1 = 0.f, float v2 = 0.f, float v3 = 0.f);
  void dispatch(const Program& p, int gx, int gy);
  unsigned srv_texture(int res) const;
  void uav_image(int res, unsigned* tex, int* level, unsigned* fmt) const;

  Fsr3Upscaler* m_programs = nullptr;  // owner of the programs below, when shared
  int m_state[2] = {0, 0};  // per HDR variant: 0 not tried, 1 ready, -1 failed
  Program m_prog[2][11];
  Program m_reactive_prog[2];

  int m_rw = 0, m_rh = 0, m_dw = 0, m_dh = 0;
  bool m_hdr = false;
  unsigned m_scratch_fbo = 0;
  unsigned m_samplers[3] = {};  // linear clamp, linear mip clamp, nearest clamp
  unsigned m_ubo[4] = {};       // FSR3UPSCALER, SPD, RCAS, REACTIVE
  unsigned m_atomic = 0;        // SSBO

  // resources by the ids in Fsr3Upscaler.cpp
  Tex m_tex[24];
  Fbo m_display;

  // per-frame state, as the SDK's context keeps it
  bool m_first = true;
  int m_frame = 0;  // resource frame index (ping-pong parity)
  unsigned m_jitter_index = 0;
  float m_jitter[2] = {0.f, 0.f}, m_prev_jitter[2] = {0.f, 0.f};
  int m_prev_render[2] = {0, 0}, m_prev_upscale[2] = {0, 0};
  float m_phase_count = 0.f;
  float m_frame_index = 0.f;
  bool m_reactive_built = false;
  bool m_opaque_captured = false;
  int m_resets = 0;
  std::string m_status = "not run";

  // inputs bound to the current pass (render frame)
  unsigned m_in_colour = 0, m_in_depth = 0, m_in_velocity = 0;
};
