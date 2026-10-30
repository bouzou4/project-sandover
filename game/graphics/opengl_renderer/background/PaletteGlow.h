#pragma once

#include <vector>

#include "common/common_types.h"
#include "common/custom_data/Tfrag3Data.h"

#include "third-party/glad/include/glad/glad.h"

struct SharedRenderState;
struct UnbakeSidecar;

/*!
 * Baked glow per palette entry, for the world sun map's removed share (world_shadow_vert.glsl).
 *
 * The map removes a receiver's direct sun share where the sun is blocked. Without sidecar data
 * (shrubs, low fit confidence) that share is estimated as the colour above the level's ambient
 * floor, so light the bake put there from a glow prop (Mountain Pass crystals) was read as sun
 * and removed. A palette entry holds the vertex's light in every slot; the sun's part changes
 * with the slot's sun (direction, colour, none at night) and the ambient's with the slot's
 * ambient, while a glow is the same in every slot. Per entry, over the fitted day and night
 * slots s, with the level's fitted ambient A_s and sun L_s (sidecar header):
 *
 *   C_s = a A_s + sigma_s L_s (+ e),  a >= 0, sigma_s >= 0 free per slot, e >= 0 rgb constant
 *
 * With sigma free per slot the sun may explain any sun-coloured light in any slot, so e takes only
 * what neither the ambient nor a sun of that slot's colour can: light held at night where the
 * level has no sun, or of a colour the sun does not have. e counts only where the model without
 * it misses by more than kGlowFitRms, only above kGlowFloor, and only on entries whose light
 * barely changes between slots (each channel's range under 2 x kGlowSteady of its brightest
 * slot, full weight under kGlowSteady): a lit vertex that the fit's global colours explain
 * poorly varies with the sun and keeps its share. Clipped samples (254+) are left out. Stored:
 * weight x max(lum(e) - floor, 0) in palette u8 steps (128 = 1.0), one R8 texel per entry; the
 * shaders turn it into the glow's share of the live colour. Levels with fewer than two such slots
 * get none, and so do levels without a sunless slot (fitted sun under kGlowSunless of the
 * brightest): where every slot has a sun, a constant light is not separable from it.
 */
constexpr int kPaletteGlowUnit = 59;  // kb/facts/renderer.md texture units: 59, `ws_glow`, bound at link
constexpr float kGlowFitRms = 12.f;   // u8 rms over slots and channels
constexpr float kGlowFloor = 16.f;    // u8 luminance
constexpr float kGlowSteady = 0.1f;   // luminance range over slots / brightest slot
constexpr float kGlowSunless = 0.05f;  // a slot's sun luminance / the brightest slot's

/// e (u8 luminance above the floor) of one entry: C, A, L are S rows of rgb (u8 units).
float palette_glow_entry(const float (*C)[3], const float (*A)[3], const float (*L)[3], int S);

/// One value per entry of `colors` (0..255); false if the sidecar has fewer than two fitted
/// day/night slots or none of them is sunless.
bool palette_glow_compute(const tfrag3::PackedTimeOfDay& colors,
                          const UnbakeSidecar& sidecar,
                          std::vector<u8>* out);

/// Solves every tree palette of `level` into `sidecar->glow` (loader thread, any option state).
void palette_glow_prepare(const tfrag3::Level& level, UnbakeSidecar* sidecar);

/*!
 * Per tree draw, on the tree's active program, while the world map is on (no GL call otherwise):
 * binds the tree's glow texture to unit 59 (uploaded on first use from `sidecar->glow`) and
 * sets `ws_glow_on`. Leaves the active texture unit as it found it.
 */
void palette_glow_setup_tree(const SharedRenderState* state,
                             GLuint program,
                             const UnbakeSidecar* sidecar,
                             const tfrag3::PackedTimeOfDay& colors,
                             GLuint tod_texture);

/// Drops the glow texture built for `tod_texture` (tree unload).
void palette_glow_forget(GLuint tod_texture);
