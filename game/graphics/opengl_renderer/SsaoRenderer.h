#pragma once

#include "common/dma/gs.h"

#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/opengl_renderer/SsaoDepth.h"
#include "game/graphics/opengl_renderer/TemporalAccum.h"

/*!
 * Screen-space ambient occlusion for Jak 1: a multisampled depth copy after the
 * character buckets, XeGTAO (GTAO) at half or full resolution, XeGTAO's edge-aware denoiser,
 * and a stencil-masked multiply over opaque pixels. Stencil bit 7
 * excludes a pixel from AO, bit 6 marks characters.
 * Every pass runs inside one bucket dispatch, so each stereo eye gets its own AO.
 * Nothing here is called while both SSAO and the emissive tile lights are off; with SSAO off the
 * depth copy, normals and stencil still run for the tile lights.
 *
 * Normal target: while SSAO is on, the render framebuffer gets a second colour attachment
 * (RGB10_A2, same sample count). The opaque background buckets (5-30) and the character
 * buckets (48-56) draw with it enabled as draw buffer 1; each of their shaders writes the unit
 * normal of its surface in clip (x, y, w) space, encoded n * 0.5 + 0.5, with (0, 0, 0) for
 * "no normal". tfrag/tie use the .unbake sidecar's smooth normal, merc its skinned vertex
 * normal, everything else the face normal from screen derivatives. Blended draws other than
 * plain alpha blending leave it untouched. The AO pass averages the pixel's samples and falls
 * back to a normal reconstructed from depth where none was written.
 */
class SsaoRenderer {
 public:
  enum View {
    NONE = 0,
    RAW_AO = 1,
    BLURRED_AO = 2,
    NORMALS = 3,  // the normals AO uses: the target, depth where it has none
    DEPTH_56 = 4,
    STENCIL = 5,
    DEPTH_NORMALS = 6,  // normals reconstructed from depth only
    NORMAL_SOURCE = 7,  // green: normal target, red: depth fallback
    EMISSIVE = 8,
    FOG_T = 9,  // fog transmission carried by the normal target (ssao_normal_fog)
  };

  SsaoRenderer() = default;
  ~SsaoRenderer();
  SsaoRenderer(const SsaoRenderer&) = delete;
  SsaoRenderer& operator=(const SsaoRenderer&) = delete;

  /// Stencil writes for the draws of Jak 1 bucket `bucket`; call before the bucket renders.
  static void set_bucket_stencil(int bucket);
  /// Back to the vanilla stencil state (test off, full write mask).
  static void reset_stencil();
  /// Free eye slot `slot`'s AO history targets (slot 1 when stereo turns off).
  void release_history(int slot) { m_accum[slot & 1].release(); }

  /// Attach and clear the normal target; call before bucket 0 of each dispatch while SSAO is on.
  /// Detaches it instead when Gfx::g_global_settings.ssao_normal_target is off.
  void begin_normals(SharedRenderState* render_state);
  /// Detach the normal target and zero the passes' normal switches (SSAO switched off).
  void end_normals(SharedRenderState* render_state);
  /// True while the normal target is attached (then end_normals must run once SSAO is off).
  bool normals_attached() const { return m_nrm_attached; }
  /// Draw buffer 1 on for the buckets that write normals; call before bucket `bucket` renders.
  static void set_bucket_normals(SharedRenderState* render_state, int bucket);
  /// Set `ssao_nrm` on the active program (0 off, 1 face normal, 2 vertex normal) if the normal
  /// target is in use this frame.
  static void set_normal_mode(GLuint program, int mode);
  /// Enable or mask draw buffer 1 for the next draws (blend modes that cannot carry a normal).
  static void set_normal_write(bool write);
  /// Background depth prepass: every colour mask off and kept off (normal and emissive mask
  /// calls are ignored) until unlocked, which puts back all-on masks plus the tracked ones.
  static void color_lock(bool on);
  /// True if the normal target is in use this frame.
  static bool normals_active();
  /*!
   * Detail normal target (RG8, colour attachment 6, with the normal target while the tile lights
   * and the background material shading are on): the material's mapped normal around the
   * normal target's (ssao_normal_frag.glsl). Sets `ssao_dn` and `ssao_dn_rot` (world to the tile
   * pass's view space, `view_sign` x camera_rot^T) on a background program.
   */
  static void set_detail_uniforms(SharedRenderState* render_state, GLuint program);
  /// Bind the detail target to `prog` (`dn_ss`, `dn_ms`, `dn_on`) for a full-screen pass.
  void bind_detail(GLuint prog, int ss_unit, int ms_unit);
  /*!
   * Emissive mask target (RG8, the frame's sample count) at colour attachment 2 of the render
   * framebuffer: attached and cleared to 0 when `on`, detached once when not. With
   * `light_material` it is RGBA8 and b, a carry the highlight material (pbr_hl_pack), cleared to
   * (0, 1) = none. Call before bucket 0 of each dispatch.
   */
  void begin_emissive(SharedRenderState* render_state, bool on);
  /// Route fragment output 2 of the following draws to the emissive target, or stop.
  static void emissive_draw_buffers(bool on);
  /// The same with a colour mask on output 2: `em` its E channels (r, g), `material` the
  /// highlight material (b, a). Turning the output off restores the full mask.
  static void emissive_draw_buffers(bool on, bool em, bool material);
  /// Sprites (Sprite3) on: draw buffer 4 joins the emissive output and takes the sprite depth.
  /// False when the target is not live (emissive lights off); off again with `on` false.
  static bool sprite_depth_drawing(bool on);
  /// lin of ssao_linearize.frag for this dispatch: device depth to camera z (x 4096).
  static const float* depth_lin();
  /// Highlight material on: per background draw, a blended draw (`blend`) writes a = 1 and masks
  /// b, a; `em`: the draw writes E (r, g) too. `state` starts at -1 per tree.
  static void material_blend_guard(GLuint program, bool blend, bool em, int& state);
  /// True if `mode` really blends (setup_opengl_from_draw_mode: SRC_SRC_SRC_SRC is a replace).
  static bool draw_blends(const DrawMode& mode);
  /// True if `mode` blends with alpha factors (ONE, ZERO) under a forward add, so an output
  /// a = 1 lands as 1 ("no highlight") whatever b becomes.
  static bool blend_replaces_alpha(const DrawMode& mode);
  /// Draw buffer 5 (motion vectors object target, MotionVectors.h) on or off for the following
  /// draws, keeping buffers 0-4 of the bound framebuffer; on only around merc2 draws.
  static void set_velocity_drawing(bool on);
  static bool velocity_drawing();
  /// Route fragment output 3 to the inspect ID target (InspectRenderer), or stop.
  static void set_inspect_drawing(SharedRenderState* render_state, bool on);
  /// The emissive mask of this dispatch (0 if not written) and its sample count.
  GLuint emissive_texture() const { return m_em_live ? m_em_tex : 0; }
  int emissive_samples() const { return m_em_samples; }
  /// Nearest emissive sprite per pixel (R16F 1 / z in metres, 0: none; emissive samples), or 0.
  GLuint sprite_depth_texture() const;

  /// Depth copy, then (with `ao`) AO, blur and composite. Call right after the last character
  /// bucket (56).
  void render(SharedRenderState* render_state, ScopedProfilerNode& prof, bool ao);
  /// True once render() has produced this dispatch's depth copy (camera-space z and projection).
  bool have_frame() const { return m_have_frame; }
  /*!
   * Bind this dispatch's view inputs to `prog` for a later full-screen pass: `csz` (camera-space
   * z in metres, mips 0..4) on `csz_unit`, the `proj_info` and `far_m` uniforms, and the normal
   * target as in ssao_ao.frag (`nrm_ss`, `nrm_ms`, `nrm_samples`, `nrm_scale`).
   */
  void bind_view_inputs(GLuint prog, int csz_unit, int nrm_ss_unit, int nrm_ms_unit);
  /// Full-res pixel to camera-space xy per metre of z, as `proj_info` in the SSAO shaders.
  const float* proj_info() const { return m_proj_info; }
  /// This dispatch's camera-space z (metres, R32F, farthest sample), valid when have_frame().
  GLuint csz_texture() const { return m_csz; }

  /// Selected debug view over the whole frame; call after the last bucket.
  void draw_view(SharedRenderState* render_state, ScopedProfilerNode& prof);
  void draw_debug_window();

 private:
  struct Tex {
    GLuint fbo = 0;
    GLuint tex = 0;
  };
  struct SavedState;
  void ensure_targets(int w, int h, int samples);
  void free_targets();
  void ensure_depth_copy(int w, int h, int samples);
  void free_depth_copy();
  void free_normals();
  void bind_normals(GLuint prog, int ss_unit, int ms_unit);
  void fullscreen();
  void set_proj_uniforms(GLuint prog);
  void bind_emissive(GLuint prog);
  bool cave_level_loaded(SharedRenderState* rs);
  /// Stencil bit 7 per sample into m_ex; false when the depth-stencil is not a texture.
  bool make_exclude_mask(SharedRenderState* rs, int w, int h, int samples);

  int m_w = 0, m_h = 0, m_samples = 0;
  static constexpr int kMips = 5;
  Tex m_depth;       // depth copy after bucket 56 when direct depth is off
  GLuint m_csz = 0;  // R32F camera-space z in metres, mips 0..4
  GLuint m_csz_fbo[kMips] = {};
  Tex m_ao[3];          // raw, then the blur or denoise ping-pong (m_ao_w x m_ao_h)
  Tex m_color;          // resolved colour for the cave luminance floor
  Tex m_ex;             // R8, 1 where stencil bit 7 excludes the sample from AO
  GLuint m_ex_depth = 0;  // depth-stencil texture attached to m_ex.fbo
  int m_ao_result = 0;  // index into m_ao of the final AO this frame
  GLuint m_vao = 0;
  GLuint m_em_tex = 0;
  int m_em_w = 0, m_em_h = 0, m_em_samples = 0;
  bool m_em_live = false;  // cleared and attached this frame
  void free_sprite_depth();
  GLuint m_sz_tex = 0;
  int m_sz_w = 0, m_sz_h = 0, m_sz_samples = 0;
  bool m_em_mat = false;   // RGBA8 with the highlight material (SharedRenderState::light_material)
  bool m_have_frame = false;
  bool m_ao_frame = false;  // this dispatch drew AO
  float m_view_far_m = 200.f;
  float m_proj_info[4] = {};
  float m_nrm_scale[2] = {};  // clip-space normal to view space: x and y factors
  GLuint m_nrm = 0;           // normal target, attached as colour 1 of the render framebuffer
  int m_nrm_w = 0, m_nrm_h = 0, m_nrm_samples = 0;
  bool m_nrm_attached = false;
  GLuint m_dn = 0;  // detail normal target, colour 6
  int m_dn_w = 0, m_dn_h = 0, m_dn_samples = 0;
  bool m_dn_attached = false;
  int m_ao_w = 0, m_ao_h = 0;  // AO targets: half the frame, or all of it for full-res GTAO
  bool m_ao_full = false;
  bool m_fixed_noise_logged = false;
  TemporalAccum m_accum[2];  // AO history (motion vectors), per eye slot
  bool m_accum_full[2] = {false, false};
  bool m_accum_on = false;  // this frame accumulated (debug window)
  unsigned m_noise_frame = 0;

  GpuTimer m_t_total{"ssao-total"};
  GpuTimer m_t_copy{"ssao-copy56"};
  GpuTimer m_t_linearize{"ssao-linearize-mips"};
  GpuTimer m_t_ao{"ssao-ao"};
  GpuTimer m_t_blur{"ssao-blur"};  // the denoise passes
  GpuTimer m_t_composite{"ssao-composite"};
};
