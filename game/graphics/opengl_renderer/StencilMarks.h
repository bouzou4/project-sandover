#pragma once

#include "game/graphics/opengl_renderer/BucketRenderer.h"

/*!
 * The SSAO stencil marks of the sky and the opaque background (Jak 1), written after the
 * background instead of per bucket. A stencil write before the heavy depth-tested background
 * draws makes every later one of them about twice as slow on some GPUs
 * (kb/findings/2026-10-23-ao-prerequisite-cost.md), so buckets 0-30 write no stencil and the
 * marks they would leave are recovered from depth after bucket 30 (vista and sun disc included):
 *
 * - bit 7, sky and far ocean (buckets 0-4): depth still equal to its copy after bucket 4,
 *   only when buckets 0-4 drew a sample (conditional render on a query);
 * - bit 7, the deferred mid ocean: depth still equal to its copy after the ocean, where the
 *   ocean changed it;
 * - bit 5 (bit 7 clear), merc level props with local lights (buckets 10, 17): the same test
 *   around the bucket's draws.
 *
 * Every other pixel keeps the zero the background buckets wrote. Depth copies are taken only for
 * the sources drawn this dispatch. pc-set-perf-opt bit PERF_OPT_SSAO + 1 off restores the
 * per-bucket writes; probe bit 16384 (pc-set-ssao-option 4) keeps them and logs, per frame, the
 * samples where the late marks would differ (SSAO-MARKS lines).
 */
namespace stencil_marks {

/// Before bucket 0 of each dispatch. `on`: the SSAO stencil marks run this dispatch. Frees the
/// copies when off. Returns true when buckets 0-30 must not write stencil.
bool begin(SharedRenderState* rs, bool on);
/// True when buckets 0-30 write no stencil this dispatch.
bool deferred();
/// Before bucket 0: starts counting the samples buckets 0-4 draw.
void before_sky();
/// After bucket 4 (sky, far and vanilla mid ocean).
void after_sky(SharedRenderState* rs);
/// Around the deferred mid ocean; `water_depth` is the single-sample copy taken just before it.
void ocean(SharedRenderState* rs, bool after, GLuint water_depth);
/// Merc2, before the first draw of a merc level-prop bucket (idempotent) and after the bucket.
void props(SharedRenderState* rs, int bucket, bool after);
/// After bucket 30, vista and the sun disc: write the marks (or, probing, compare them).
void write(SharedRenderState* rs);
void release();

}  // namespace stencil_marks
