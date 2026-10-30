#pragma once

#include <string>

#include "game/graphics/opengl_renderer/BucketRenderer.h"

/*!
 * Per-pixel screen-space motion vectors (a velocity buffer) for later temporal passes (Jak 1).
 * Nothing here runs unless wanted() (mv_enable, or motion blur on). In stereo and XR each eye has
 * its own Renderer (its previous eye camera, so head motion is in the camera velocity) and runs
 * only for a per-eye temporal pass; frames chain on SharedRenderState::display_tick.
 *
 * Output (velocity_texture()): RG16F, single-sample, the render target's size. Each texel holds
 * v = p_prev - p_cur in render-target pixels, in the texture's own texel grid (+x right, +y up, as
 * GL stores it): the pixel's surface was at texel (x, y) + v in the previous frame. A consumer
 * that samples in GL texel or UV space adds v (UV: v / size) with no conversion; one that flips the
 * rows (a D3D interop copy) negates v.y along with the flip. No sub-pixel jitter is in v: a jittered
 * projection would subtract the jitter delta in the camera pass and the merc vertex shader, the two
 * places that build the previous clip position.
 *
 * Method: the merc vertex shader writes object velocity (previous-frame bone matrices, matched by
 * actor identity, Merc2) into an RGBA16F target at colour attachment 5 of the render framebuffer
 * (x, y velocity, z the clip w, a = 1); everything else gets camera-only velocity in a full-screen
 * pass from depth and the previous frame's camera. The pass takes each pixel's nearest sample and
 * keeps the object velocity only where the merc surface is still the visible one (its w matches the
 * depth). Pixels with no depth (sky) reproject as directions (rotation only).
 *
 * frame_valid() is false on a camera cut: a game-reported cut (camera teleport, instant camera
 * switch), a jump the size of a cut, a load blackout, a resize, stereo turning on or off, the
 * first frame after the toggle or after a frame with no camera. The buffer is then all zero.
 */
namespace motion_vectors {

/// The buffer is wanted: the motion-vector switch, or motion blur (which turns it on by itself).
bool wanted();

enum View : int {
  VIEW_NONE = 0,
  VIEW_VELOCITY = 1,  // hue = direction, brightness = magnitude (mv_view_scale px = full)
  VIEW_ERROR = 2,     // |previous colour warped by the velocity - current colour| x gain
  VIEW_SOURCE = 3,    // green: object velocity, blue: camera-only, red: sky, magenta: invalid frame
  VIEW_ERROR_CAMERA = 4,  // as VIEW_ERROR with camera-only velocity everywhere
};

/// Game thread: a camera cut is in the frame being built (pc-set-motion-vectors 4).
void game_cut();
/// Game thread, with the chain: take the pending cut for the chain sent now.
void latch_cut_for_chain();
/// Game or REPL thread: dump the next `frames` rendered frames into `dir` (EXR, see dump()).
void request_dump(const std::string& dir, int frames);

/// The frame's camera, for the merc vertex shader's camera-only fallback.
struct FrameState {
  bool active = false;  // the buffer is produced this frame
  bool valid = false;   // false on a cut: consumers must not treat the frame as motion
  float cam_reproj[16];  // current clip -> previous clip (column-major)
  int fb_w = 0, fb_h = 0;
  float w_k0 = 0.f, w_k1 = 0.f;  // clip w = w_k0 / (ndc z - w_k1), game units
  float tan_half_x = 1.f, tan_half_y = 1.f;  // the frame camera's half field of view
  unsigned obj_tex = 0;  // merc object velocity (RGBA16F: xy px, z clip w), the scene's samples
  int obj_samples = 0;   // valid from begin_frame; written by the character buckets
};
const FrameState& frame();

/// Merc models of a frame by previous-bone lookup result (debug window, dump).
struct MercStats {
  int matched = 0;  // previous bones found: object velocity
  int missed = 0;   // keyed, no usable previous record: camera-only
  int unkeyed = 0;  // no identity from the game: camera-only
};
/// Merc2, per model drawn into the frame while the buffer is active.
void count_merc(bool matched, bool keyed);

class Renderer {
 public:
  Renderer() = default;
  ~Renderer();
  Renderer(const Renderer&) = delete;
  Renderer& operator=(const Renderer&) = delete;

  /// After the frame camera is primed, before bucket 0: cut detection, matrices, object target.
  /// `wanted`: a consumer needs the buffer this frame (FSR 3) whatever mv_enable says.
  /// `force_cut`: the frame is a cut whatever this renderer sees (the other eye's cut).
  void begin_frame(SharedRenderState* rs,
                   bool camera_primed,
                   bool blackout,
                   bool wanted = false,
                   bool force_cut = false);
  /// After the last scene bucket (SPRITE), before FSR 3: the velocity pass.
  void resolve(SharedRenderState* rs, ScopedProfilerNode& prof);
  /// After the last bucket: the selected debug view over the frame.
  void draw_view(SharedRenderState* rs, ScopedProfilerNode& prof);
  void draw_debug_window();

  /// RG16F velocity of this frame (valid after resolve()), 0 when off.
  unsigned velocity_texture() const { return m_resolved ? m_vel.tex : 0; }
  /// Ask the next resolve() for the motion blur input (rel_texture()); `camera`: the rest of the
  /// frame carries its camera-only velocity instead of zero.
  void want_relative(bool on, bool camera) {
    m_rel_wanted = on;
    m_rel_camera = camera;
  }
  /// RGBA16F, written by resolve() when asked on a valid frame, else 0. xy (px, as
  /// velocity_texture()): object pixels move against the world (object minus camera-only velocity),
  /// scaled down to no longer than their on-screen velocity, elsewhere zero; asked with `camera`,
  /// object pixels take their on-screen velocity and the rest the camera-only one. z the view
  /// distance (m, sky 60000); w 1 on merc HUD models (no blur).
  unsigned rel_texture() const { return m_rel_written ? m_rel.tex : 0; }
  /// Free everything and forget the previous camera (a second eye's renderer after stereo).
  void shutdown(SharedRenderState* rs) {
    detach_object_target(rs);
    release();
    m_have_prev = false;
  }
  /// Free the motion blur input (motion blur off; the velocity buffer may stay on for FSR).
  void release_relative();
  bool frame_valid() const;
  /// Targets exist (begin_frame must run once more to free them after the toggle goes off).
  bool allocated() const { return m_obj_fb != 0 || m_vel.fbo != 0; }
  /// This frame's dump path stem (`<dir>/mv_NNN`) when resolve() dumped it, else empty: a later
  /// pass of the frame (FSR 3) writes its own image next to it.
  const std::string& dumped_stem() const { return m_dumped_stem; }

 private:
  struct Target {
    unsigned fbo = 0;
    unsigned tex = 0;
  };
  void release();
  void ensure_targets(int w, int h, bool hdr);
  void ensure_object_target(SharedRenderState* rs, int samples);
  void detach_object_target(SharedRenderState* rs);
  void copy_colour(SharedRenderState* rs, Target& dst);
  void dump(SharedRenderState* rs);

  int m_w = 0, m_h = 0;
  bool m_hdr = false;
  Target m_vel;       // RG16F result
  Target m_aux;       // RGBA16F: camera-only velocity xy, view distance (m), source code
  Target m_rel;       // RGBA16F: relative velocity xy, view distance (m), HUD flag
  Target m_colour;    // single-sample copy of this frame's scene (error view, dump)
  Target m_history;   // the previous frame's copy
  bool m_rel_wanted = false;
  bool m_rel_camera = false;
  bool m_rel_written = false;
  bool m_have_history = false;
  unsigned m_obj = 0;  // RGBA16F object velocity, the frame's sample count
  int m_obj_w = 0, m_obj_h = 0, m_obj_samples = 0;
  unsigned m_obj_fb = 0;  // render framebuffer it is attached to
  unsigned m_vao = 0;
  bool m_resolved = false;
  bool m_aux_written = false;

  // previous frame's camera (frame camera fields, as the background packets carried them)
  bool m_have_prev = false;
  u64 m_prev_frame = 0;
  math::Vector4f m_prev_rot[4], m_prev_persp[4], m_prev_hvdf, m_prev_fog, m_prev_trans;
  int m_prev_w = 0, m_prev_h = 0;
  bool m_prev_blackout = false;
  bool m_prev_stereo = false;

  // this frame's pass inputs
  float m_reproj[16] = {};
  float m_sky[9] = {};
  float m_w_k0 = 0.f, m_w_k1 = 0.f;  // clip w from device depth: k0 / (ndc_z - k1)
  double m_w_per_unit = 0.0;          // clip w per game unit of view depth (dump)
  const char* m_cut_reason = "first";
  const char* m_run_reason = "";
  int m_invalid_run = 0;  // consecutive invalid frames (the log names each run once)
  u64 m_cuts = 0;
  float m_last_move_m = 0.f, m_last_turn_deg = 0.f;

  // dump state (render thread)
  int m_dump_left = 0;
  u64 m_dump_serial = 0;
  int m_dump_index = 0;
  std::string m_dump_dir;
  std::string m_dumped_stem;
};

}  // namespace motion_vectors
