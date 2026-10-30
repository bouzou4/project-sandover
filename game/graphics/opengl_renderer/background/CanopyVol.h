#pragma once

#include <memory>
#include <string>
#include <vector>

#include "common/common_types.h"

#include "third-party/glad/include/glad/glad.h"

struct SharedRenderState;

/*!
 * Canopy volume (Jak 1): the leaf volume of each level's tree crowns for the backlit leaves' sun
 * transmittance (pbr_bg_frag.glsl). Read from `$SANDOVER_CANOPY_VOL`, else
 * `$SANDOVER_DATA/lighting/canopy`, else `<project>/sandover/data/lighting/canopy`: map v3 written
 * by `sandover-unbake canopy-vol` (sandover/tools/sandover/unbake/canopy_vol.py, which documents
 * the format), sky levels only. The canopy shade keeps its own v2 maps (Canopy.h).
 *
 * The loader thread reduces each map to 2x2-texel cells (the average of lowest leaf, highest
 * leaf and volume cover over the texels i >> 1 maps to, edge cells over the texels they have).
 * The GPU holds those cells for the two loaded levels nearest the camera in a two-layer RGBA16
 * array on unit 43 (sampler `canopy_vol`), read with texelFetch.
 */
struct CanopyVolMap {
  u32 width = 0, height = 0;                        // texels of the file
  float x0 = 0, z0 = 0, texel = 0, y0 = 0, y1 = 0;  // metres
  bool sky = false;
  u32 cw = 0, ch = 0;      // cells (2x2 texels, rounded up)
  std::vector<u16> cells;  // per cell: lowest leaf, highest leaf, volume cover, 0
  u64 serial = 0;
};

constexpr int kCanopyVolUnit = 43;  // kb/facts/renderer.md texture units: 43

/// Load the level's v3 map (loader thread). Null if missing or unreadable.
std::unique_ptr<CanopyVolMap> canopy_vol_load(const std::string& level_name);

/// Reduce a v3 map's texels (4 channels each, row-major) to the 2x2 cells (tested on the CPU).
void canopy_vol_cells(const u16* texels, u32 w, u32 h, CanopyVolMap* out);

/// Per frame, after the camera is known (with heightfield_prepare): pick the maps of the two
/// loaded levels nearest the camera into stable layer slots, uploading only a changed layer;
/// frees the array while the backlight sun occlusion is off.
void canopy_vol_prepare(SharedRenderState* render_state);

/// Backlight sun transmittance uniforms (pbr_bl_*), the volume array (unit 43) and the level
/// heightfield for a background program with PBR_BG_FRAG (active). Off unless PbrParam
/// SUN_OCCLUSION is set.
void canopy_set_backlight_uniforms(GLuint program);
