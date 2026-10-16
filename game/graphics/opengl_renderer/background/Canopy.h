#pragma once

#include <memory>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "common/dma/gs.h"

#include "third-party/glad/include/glad/glad.h"

/*!
 * Canopy shade (Jak 1): a per-level top-down map of overhead foliage, `<level>.canopy`, written
 * by `sandover-unbake canopy` (sandover/tools/sandover/unbake/canopy.py, which documents the
 * format). Read from `$SANDOVER_CANOPY`, else `$SANDOVER_DATA/lighting`, else
 * `<project>/sandover/data/lighting`. A missing file means no canopy.
 *
 * Receivers of the actor shadow term take the canopy's light (`shadow_canopy_light` in
 * actor_shadow_frag.glsl): the point is projected toward the sun up to the canopy height, the
 * cover there (filtered over about a metre, more with distance to the leaves) gives sun patches
 * through the leaf gaps that replace the bake's own sun visibility, and the mean cover above the
 * point cuts and cools the sky light. Under the canopy the floor reads darker with brighter
 * patches that move with the sun; outside it nothing changes.
 */
struct CanopyMap {
  u32 width = 0, height = 0;
  float x0 = 0, z0 = 0, texel = 0, y0 = 0, y1 = 0;  // metres
  std::vector<u16> data;                           // coverage, height per texel
  u64 serial = 0;                                  // unique per load
};

/// Load `level_name`'s map; null if absent or unreadable (one log line). Thread-safe.
std::unique_ptr<CanopyMap> canopy_load(const std::string& level_name);

struct SharedRenderState;

/*!
 * Per frame, from the actor shadow pre-pass: the maps of the two levels in use nearest the
 * camera (the one it is over first) into stable layer slots, a layer re-uploaded only when its
 * map changes; `day` is the palette's day weight (the effect runs only by day), `sun_gain` the
 * sun shadows' shared fade (world_shadows_sun_strength). Also notes
 * and note the sun direction (world, toward the sun) and the camera-to-world matrix
 * (column-major) for receivers drawn in camera space.
 */
void canopy_prepass(SharedRenderState* render_state,
                    const float sun_dir[3],
                    const float c2w[16],
                    float day,
                    float sun_gain);

/// Canopy uniforms (cached lookups, sampler `canopy_tex`) and the map array on unit 30 for
/// `program` (active); the active unit is left as found. camera_space as in
/// actor_shadows_set_uniforms.
void canopy_set_uniforms(GLuint program, bool camera_space);

/// No canopy for the next draws of `program` (HUD, full-matrix generic).
void canopy_disable(GLuint program);

/// The actor shadow prepass stopped early (shadows off, no camera): no canopy this frame.
void canopy_off();

/// Draws that keep their own light under the canopy: additive or subtractive blends, and
/// textures in the actor emissive table (`combo` = tpage << 16 | index, or -1 when unknown).
bool canopy_keeps_light(const DrawMode& mode, s64 combo);

/// Set `canopy_emitter` of `program` (active) when it differs from `*last` (-1 to force).
void canopy_set_emitter(GLuint program, bool on, int* last);

/// World matrix of a wind TIE instance after the wind (column-major, game units) for the
/// tie_wind program's canopy position.
void canopy_set_instance(GLuint program, const float world[16]);
