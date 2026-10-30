#pragma once

#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/opengl_renderer/Neighbours.h"

struct TfragPcPortData;
struct UnbakeSidecar;

/*!
 * The sun in the sky (Jak 1). A disc at the direction of the fitted sun model (the .unbake
 * sidecar's day slots), drawn after the opaque background and before the transparent passes,
 * only on sky pixels.
 *
 * - Position: each day slot's fitted direction sits at the hour its palette weight peaks (the
 *   mood table); the sun slerps between them by the game's hour (`vista_tod_addr`) and continues
 *   along the arc past the first and last slot, so it rises and sets smoothly while the lighting
 *   stays slot based. Without the hour it falls back to the palette-weighted slot blend.
 * - Occlusion: the disc draws with a depth test that passes only where the depth is still the
 *   clear value (the sky), so geometry in front cuts it per sample. A 1x1 pass counts sky taps
 *   over the disc in the depth buffer and eases toward that share over a few frames; the whole
 *   sun (colour and bloom mask) scales by it, so a sun behind leaves fades instead of flickering.
 * - Haze: the vanilla fog of a distant object at the fog end (and the unified fog and haze when
 *   they are on) gives the horizontal transmittance; along the sun ray the fog layer is thinner,
 *   by the fog scale height over the sine of the elevation. The disc dims by that transmittance
 *   and is added over the sky, which already carries the haze colour, so the frame gets
 *   sun * T + haze * (1 - T) as the fog pass gives a bright distant object.
 * - Brightness: in paper whites (linear). With HDR output the core is far above the peak, so
 *   hdr_map's shoulder lands it at the display peak and the toon clip turns it white; the limb
 *   keeps the sun model's colour. In SDR the toon tonemap shows a white core and coloured rim.
 *   The glow is the bloom pass: the disc writes the emissive mask so the bright pass takes
 *   `sun_disc_glare` of its colour.
 * - Hidden when the palette's day weight goes to night, below the horizon, in levels without a
 *   sky (LevelSky.inc) and in stereo or XR views.
 *
 * Style 1 (pc-set-sun-option 20) replaces the disc with Jak 3 style bodies: a soft sun glow with
 * a white-hot core, a starburst and a wide wash; a textured moon with a blue halo and wash at the
 * night slots' fitted light; stars at night. All occluded like the disc and faded by the haze.
 * The starburst and the sun wash are a camera effect: `render_flare` draws them over the finished
 * scene (after bloom, before the HUD), scaled by the sun's visibility, not cut per pixel.
 * The sprites are Jak 3's own sky textures, read from `$SANDOVER_DATA/sky/jak3` (data, not repo);
 * without them style 1 draws the disc.
 *
 * pc-set-sun-option 10-29 (gfx.h SunOption); off at boot, on with the play look's vista switch.
 */
/*!
 * The sun the disc draws, for other passes (the world sun map): direction toward the sun (world,
 * unit) on the smooth arc by the game hour, or the palette blend of the day slots without the
 * hour, and the palette's day weight. Uses the viewer level's sidecar, else the first port's with
 * one. False without a sidecar, a live day slot or in a level without a sky.
 */
bool sun_disc_direction(const SharedRenderState* rs,
                        const TfragPcPortData* const ports[2],
                        neighbours::MoodTable* mood,
                        math::Vector3f* dir,
                        float* day,
                        float* hour_out = nullptr);

/*!
 * The moon, the same way: the sidecar's night slots (header night_mask) blended by the palette,
 * direction toward the moon (world, unit, the slots' fitted light), the palette's night weight
 * and the blend's direct share of its light (lgt over lgt + amb, luminance). False without a
 * sidecar, a live night slot or in a level without a sky. The moon disc and the shadows read it.
 */
bool moon_disc_direction(const SharedRenderState* rs,
                         const TfragPcPortData* const ports[2],
                         math::Vector3f* dir,
                         float* weight,
                         float* share);

class SunDisc {
 public:
  SunDisc() = default;
  ~SunDisc();
  SunDisc(const SunDisc&) = delete;
  SunDisc& operator=(const SunDisc&) = delete;

  /// Draw into the render framebuffer; call after the opaque background buckets (and the vista)
  /// with the frame's two tfrag camera ports (either may be null).
  void render(SharedRenderState* rs, ScopedProfilerNode& prof, const TfragPcPortData* ports[2]);
  /// Style 1's starburst and sun wash over everything but the HUD; call after the scene's last
  /// world bucket (and bloom). Draws only in a frame whose `render` placed the sun.
  void render_flare(SharedRenderState* rs, ScopedProfilerNode& prof);

 private:
  /// Style 1 (sky_style): Jak 3 style sun glow, starburst and wash, moon, halo and stars.
  void render_bodies(SharedRenderState* rs,
                     ScopedProfilerNode& prof,
                     const UnbakeSidecar& sc,
                     const TfragPcPortData* port,
                     const std::string& viewer);
  bool make_body_assets();
  /// Loads the sprites once (GL state kept); false without them, and style 1 draws the disc.
  bool bodies_ready();

  GLuint m_vao = 0;
  GLuint m_body_fbo = 0;
  GLuint m_body_vis = 0;  // 2x1: sun, moon
  GLuint m_moon_tex = 0;
  float m_moon_pad = 1.f;  // moon texture size with its soft margin over the disc's
  GLuint m_glow_tex = 0;
  GLuint m_burst_tex = 0;
  GLuint m_soft_tex = 0;
  GLuint m_dot_tex = 0;
  bool m_assets_tried = false;
  bool m_assets_ok = false;
  GLuint m_star_vao = 0;
  GLuint m_star_vbo = 0;
  int m_star_count = 0;
  u64 m_body_last[2] = {~0ull, ~0ull};
  u64 m_body_log = 0;
  struct Flare {
    u64 frame = ~0ull;
    float info[4] = {};
    float up_cs[3] = {}, sun_cs[3] = {};
    float burst[3] = {}, wash[3] = {};
    float rot = 0.f, gain = 0.f;
  } m_flare;
  GLuint m_vis_fbo = 0;
  GLuint m_vis_tex = 0;
  u64 m_last_frame = ~0ull;  // last frame that updated the visibility
  u64 m_log_frame = 0;
  neighbours::MoodTable m_mood;
};
