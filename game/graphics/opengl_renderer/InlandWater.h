#pragma once

#include "common/common_types.h"

#include "game/graphics/opengl_renderer/BucketRenderer.h"

/*!
 * Inland water (Jak 1): the `water-anim` actors that are not the ocean (ponds, lakes, the jungle
 * river, the village fountain and rice paddies, the sunken pools, the purple cave pools) read as
 * clear water, and Misty's quicksand as wet mud. One rule for every such surface; which draws it
 * applies to comes from `InlandWater.inc`, generated from the game's own data by
 * `python -m sandover.inland_water` (texture combo -> clear or mud, and the level's sky flag).
 * Dark eco pools and lava are not in the table.
 *
 * Clear water (Merc2 base draws in MERC_WATER, Generic2 in GENERIC_WATER, both after SSAO):
 * - Depth: the view ray's length through the water, from SSAO's depth copy (the bed) and the
 *   fragment's own depth, sets the alpha by Beer-Lambert: 0 at the shore, the game's own alpha
 *   (its `color-mult` tint, lit by the mood) where the water is deep.
 * - Merc2 on a level with a sky: a fresnel reflection of the mood sky (at the fog colour's
 *   brightness, as the ocean's), capped, and a sun glint on the game's ripple normals; the
 *   game's envmap layer of those draws is not drawn, so the shine is replaced, not doubled.
 *   Without a sky (caves, sunken) the envmap layer stays and nothing is reflected.
 * - Generic2 (sunken pools, which keep a ripple query for their sparks): depth only.
 * Mud (Merc2, opaque): the game's colour darkened, a rough capped sky reflection and a broad sun
 * lobe on the ripple normals; no transparency.
 *
 * Needs PBR on (the boot state stays vanilla); the depth part also needs SSAO on.
 * `pc-set-inland-water` (InlandWaterOption) turns the parts on and off and sets the parameters.
 */
namespace inland_water {

enum Kind : int { NONE = 0, CLEAR = 1, MUD = 2, SKY = 4 };

/// Kind of the draw with this texture combo (CLEAR or MUD, | SKY on a level with a sky), or NONE.
int kind_for_combo(u32 combo);

/// True when the rule applies to merc (`merc`) or generic draws: a part on and the class shading
/// on for that path (pbr_merc / pbr_background), as the shaders' `pbr_on` gate. No GL call.
bool active(const SharedRenderState* rs, bool merc);

/// True when `setup` must run for this path: the rule is on, or it was on and is not reset yet.
bool needs_setup(const SharedRenderState* rs, bool merc);

/// Kind of a Merc2 draw (level texture index), with the parts switched off this frame removed.
int merc_kind(const SharedRenderState* rs, const LevelData* lev, s32 texture);

/// True if a Merc2 effect with this base texture draws its own reflection instead of the
/// game's envmap layer (also in the `inland_water_picked` mode, which shades nothing).
bool replaces_envmap(const SharedRenderState* rs, const LevelData* lev, s32 texture);

/// This frame's SSAO depth copy (`csz`, metres) and projection, or 0 before it exists.
void set_depth(GLuint csz, const float* proj_info);

/// Uniforms of `program` (merc2 or generic) for the draws that follow: depth texture on unit 19,
/// parameters, and for merc the sun of `lev` (the draws' level). Returns the location of
/// `iw_kind` (set 0 here), or -1 when the rule is off for this path; then `iw_kind` is reset once
/// after it was last on. Locations are cached.
GLint setup(SharedRenderState* rs, GLuint program, bool merc, const LevelData* lev = nullptr);

}  // namespace inland_water
