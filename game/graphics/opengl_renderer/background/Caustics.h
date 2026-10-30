#pragma once

#include "common/custom_data/Tfrag3Data.h"

#include "game/graphics/opengl_renderer/background/background_common.h"

/*!
 * Sunken caustics (Jak 1). The original lights caustics through four palette slots that
 * update-mood-caustics cross-fades in a 32-frame loop: four phases of one coarse per-vertex
 * pattern sharing one envelope. With the pattern on (`caustic_mode`, PBR on, a level whose mood
 * runs that loop, CausticMoods.inc), the four weights are frozen at their time mean, so the mean
 * light stays exactly the original's, and the background shaders add a zero-mean procedural web
 * (`pbr_caustic`, pbr_bg_frag.glsl) scaled by the envelope: the mean of the four slots, uploaded
 * per tree after the frame's colours in the time-of-day texture, with the original's own temporal
 * swing per palette entry in alpha (a mask: no web where the original barely animates). The noise cell is the level's own per geometry kind, so the
 * pattern decorrelates over the distance the original's phases do.
 */

/// Geometry kind of a tree; the noise cell is per level and kind (CausticMoods.inc).
enum class CausticKind { TFRAG = 0, TIE = 1, SHRUB = 2 };

/// Texel offset of the caustic envelope in a tree's time-of-day texture (2x the colour count wide).
constexpr int kCausticEnvOffset = 8192;

/// First palette slot of the caustic loop for `level`, or -1 if its mood has none.
int caustic_first_slot(const char* level);

/// True if the procedural pattern replaces the loop in `level` this frame.
bool caustic_active(const SharedRenderState* state, const char* level);

/*!
 * Per level and frame, next to cave_dark_itimes: freezes the four caustic weights of an active
 * level at their mean (sum / 4 per channel, the sum is constant over the loop) and advances the
 * pattern clock by the real frame time (at most 0.1 s) when the level's raw weights moved, i.e.
 * the game is not paused; held at `caustic_clock_hold_s` while that is >= 0.
 */
void caustic_itimes(const SharedRenderState* state, TfragPcPortData* data);

/*!
 * Per tree, on the tree's active program (tfrag3, shrub, etie_base), after the frame's colours
 * were uploaded: the envelope upload and the `cau_*` uniforms; `cau_w` = 0 turns the term off.
 * No GL call until a caustic tree was first drawn. The envelope is uploaded on a texture's first
 * use; call caustic_forget_texture before deleting a tree's time-of-day texture.
 */
void caustic_setup_tree(SharedRenderState* state,
                        GLuint program,
                        const std::string& level,
                        const math::Vector<s32, 4> itimes[4],
                        const tfrag3::PackedTimeOfDay& colors,
                        GLuint tod_texture,
                        CausticKind kind);

/// The tree texture `tod_texture` is about to be deleted: its name may come back for another tree.
void caustic_forget_texture(GLuint tod_texture);
