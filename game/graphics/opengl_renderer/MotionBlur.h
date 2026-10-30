#pragma once

#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/opengl_renderer/MotionVectors.h"

/*!
 * Per-object motion blur (Jak 1), the "Motion blur" setting: Off, Weak, Medium, Strong
 * (Gfx::g_global_settings.mb_level 0-3). Nothing here runs while it is off; when it is on the
 * motion vectors are produced whatever their own switch says (motion_vectors::wanted()).
 *
 * Per object: the blur follows motion_vectors::Renderer::rel_texture(). An object never blurs more
 * than it moves on screen and the camera's motion never counts: a merc pixel takes its motion
 * against the world (v_full - v_cam) scaled down to at most |v_full|; the world is zero. Jak held
 * by the follow camera (v_full ~ 0) stays sharp, a still enemy under a pan (v_rel 0) stays sharp
 * like the world, a spin or a running enemy blurs fully.
 *
 * Camera motion blur (mb_camera: 0 off, 1 vehicles, 2 always): merc pixels take v_full and the
 * rest of the frame its camera-only velocity. Vehicles: only while the game reports the player mounted
 * (collide-action racer or flut on the target, sent on change as pc-set-motion-blur 11). It uses
 * the strength row's shutter and does nothing while that row is off.
 *
 * Shutter: the blur spans the distance the surface covers while the shutter is open, centred on
 * the frame: half-length = 0.5 x shutter x v, v the buffer's per-frame velocity, shutter the
 * strength's fraction of the frame interval (mb_shutter, Medium 0.5 = a 180 degree shutter). The
 * interval is the game's own step for that frame (pc-set-motion-blur 1, latched with the chain);
 * it is clamped to 1/240-1/30 s for the exposure, so a hitch or a lagging frame (one step covering
 * several) does not smear further than 1/30 s would. At a higher frame rate v is shorter per frame,
 * so the blur is shorter: the same motion blurs 2.4x less at 144 Hz than at 60 Hz, as a camera with
 * the same shutter angle would. The half-length is capped at mb_radius_px x render height / 2160.
 *
 * Filter (McGuire et al. 2012, sample directions after Jimenez 2014), on the scene colour at render
 * size after the tile lights and before the HUD sprites, bloom, SMAA and the upscale:
 *  1. tile max: the longest blur vector per tile (tile = the cap, so a blur reaches one tile);
 *  2. neighbour max: the longest of the 3x3 tiles around;
 *  3. reconstruction: `taps` samples in mirrored pairs along the neighbourhood's and the pixel's
 *     own direction, weighted by depth (a sample in front spreads over the pixel by its own reach,
 *     one behind shows only where the pixel itself moves) with an interleaved-gradient dither that
 *     changes each frame. Pixels with no blur within reach are discarded and keep their samples.
 * HUD: the 2D HUD sprites are held back by Sprite3 (defer_hud_sprites) and drawn after this pass;
 * merc HUD models carry a flag through the object target (w negative) and are neither blurred nor
 * gathered. The text buckets draw later anyway. A cut frame (motion vectors invalid) is not
 * blurred.
 */
namespace motion_blur {

/// Game thread: the game time step (s) of the frame being built.
void set_frame_dt(float seconds);
/// Game thread: the player rides a vehicle (zoomer, flut flut) in the frame being built.
void set_vehicle(bool riding);
/// Game thread, with the chain (gl_send_chain): take the step and vehicle flag for the chain.
void latch_for_chain();
/// Log the step and the half-length scale of the next `frames` blurred frames.
void log_frames(int frames);

class Renderer {
 public:
  Renderer() = default;
  ~Renderer();
  Renderer(const Renderer&) = delete;
  Renderer& operator=(const Renderer&) = delete;

  /// After motion_vectors::Renderer::begin_frame, before bucket 0: decides whether this frame is
  /// blurred, asks the velocity pass for its input and holds back the HUD sprites if so.
  bool begin_frame(SharedRenderState* rs, motion_vectors::Renderer& mv);
  /// After the velocity pass (resolve) of a frame begin_frame said yes to.
  void render(SharedRenderState* rs, ScopedProfilerNode& prof, const motion_vectors::Renderer& mv);
  bool active() const { return m_active; }
  void draw_debug_window();

 private:
  struct Target {
    unsigned fbo = 0;
    unsigned tex = 0;
  };
  void ensure(int w, int h, int tile, bool hdr);
  void release();

  bool m_active = false;
  int m_level = 0;  // this frame's strength, 1-3
  bool m_camera = false;  // this frame blurs the world by the camera's motion
  int m_w = 0, m_h = 0, m_tile = 0;
  bool m_hdr = false;
  Target m_scene;      // single-sample copy of the scene colour
  Target m_tile_max;   // RG16F per tile
  Target m_neighbour;  // RG16F per tile
  unsigned m_vao = 0;
  u64 m_frames = 0;
  bool m_logged_no_input = false;
  // last frame's numbers (debug window)
  float m_last_dt = 0.f, m_last_scale = 0.f, m_last_radius = 0.f;
};

}  // namespace motion_blur
