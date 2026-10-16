#pragma once

#include <string>

#include "game/graphics/opengl_renderer/BucketRenderer.h"

class SsaoRenderer;

/*!
 * The swamp look (Jak 1): murky water, wet surfaces and low mist together, applied
 * only in the swamp level (where the game rains: `update-rain` from `target-powerup-process`
 * while the current level is `swamp`). On by default; the parts ride on the Materials switch
 * (water, wet) and on ambient occlusion (mist), so the vanilla boot state is unchanged.
 * `pc-set-swamp` turns each part and its parameters for tuning and debugging.
 *
 * - Water (`swamp_water`): the swamp water is not the ocean renderer but one tfrag texture,
 *   `swp-water`, flat at y = 0 and opaque (alpha-tested, no blend). Its draws carry
 *   `kPbrSwampWaterBit` in `pbr_mat`; the class shading replaces their colour with a darker
 *   body, a mood-environment reflection with Schlick fresnel, procedural rain-ripple rings and a
 *   sun glint on them. Rides on the class shading (needs `pbr_enable`).
 * - Wet (`swamp_wet`): every shaded background class darkens by its porosity and gets a water
 *   film (sharp environment reflection and glint on the geometric normal); flat ground gets
 *   noise-masked puddles with mirror reflection and the same ripples. Needs `pbr_enable`.
 * - Mist (`swamp_mist`): a full-screen pass after SSAO that blends an exponential height fog
 *   (water line y = 0, scale height `swamp_mist_height_m`) along each pixel's ray, with a
 *   forward-scatter glow toward the sun. Reads SSAO's depth copy, so it needs SSAO on; runs only
 *   while the swamp is loaded and the camera is over its geometry.
 *
 * The wet shoreline (`shore_wet`) is the wet part's rule outside the swamp: in every other level
 * with a sea (`water_level_has_sea`), shaded porous classes are wet in a band above the game's
 * ocean height (`water_ocean_height`), full at the water line and fading upward over
 * `shore_wet_band_m` (half on steep faces) with a world-noise edge; no puddles, no rain rings.
 * Needs `pbr_enable`. `pc-set-swamp` options 16-20.
 *
 * The mist pass is a per-level table (`kMistRows`): the swamp, and Misty Island's sea mist
 * (`misty_mist`, options 25-30 and 33), a low layer from 10 m out. A sea row draws after the near
 * ocean, stops each ray at the sea plane and mists the sky only just over the horizon. Sprites
 * take the swamp's mist only.
 */

/// Per background tree (tfrag, tie, shrub), after `pbr_setup_background` on an active program:
/// the swamp or shoreline flags and parameters for `level_name`. No GL call until a part was
/// first on.
void swamp_setup_background(SharedRenderState* render_state,
                            GLuint program,
                            const std::string& level_name);

/// True while the swamp's rain effects drew within the last two frames, so the level
/// heightfield (their sky cover) is wanted; heightfield_prepare ORs it in.
bool swamp_cover_wanted(u64 frame_idx);

/// Sprite3 frame setup on its active program: the mist the pass drew this frame (`sw_mist`
/// density 0 when it did not), so blended world sprites drawn after it take the same mist.
void swamp_mist_sprite_uniforms(const SharedRenderState* rs, GLuint program);

/// True if the mist pass would draw this frame at this point: `after_ocean` false right after
/// SSAO (the swamp), true after the near ocean (sea rows: Misty Island). Toggle on, the player in
/// the row's level, camera over it.
bool swamp_mist_wanted(SharedRenderState* render_state, bool after_ocean = false);

/// The shared uniforms (swamp_mist.glsl: ray to world, eye height, density, sea) of the mist
/// that draws over this frame's AO, swamp or sea row, on `id`; false (none set) if no mist draws.
bool swamp_mist_uniforms(SharedRenderState* render_state, GLuint id);

class SwampMist {
 public:
  SwampMist() = default;
  ~SwampMist();
  SwampMist(const SwampMist&) = delete;
  SwampMist& operator=(const SwampMist&) = delete;

  /// Blend the mist over the render framebuffer; call right after `SsaoRenderer::render`
  /// (`after_ocean` false) or after the near ocean bucket (true).
  void render(SharedRenderState* rs,
              ScopedProfilerNode& prof,
              SsaoRenderer& ssao,
              bool after_ocean = false);

 private:
  GLuint m_vao = 0;
};
