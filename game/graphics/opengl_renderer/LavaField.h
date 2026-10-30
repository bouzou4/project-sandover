#pragma once

#include <memory>
#include <string>
#include <vector>

#include "common/common_types.h"

#include "third-party/glad/include/glad/glad.h"

namespace tfrag3 {
struct Level;
}
struct SharedRenderState;

/*!
 * Lava rules (Jak 1), riding on the class shading (`pbr_enable`), so the boot state is vanilla.
 * `pc-set-lava` turns each part and its parameters.
 *
 * Glow (`lava_glow`): texels of the `lava` material class split into molten and crust by
 * a blackbody chroma key on their own colour, `R - max(0.6 G, B)` between two thresholds. Molten
 * texels keep their colour with HDR headroom, the brightest (R + G) turning yellow-white, and
 * write the emissive mask with red = green (bloom, AO exclusion, never a tile light); the crust
 * shades as stone, darkened for contrast. Background draws (tfrag, tie, shrub) and merc (the lava
 * lakes drawn as `water-anim` actors).
 *
 * Field (`lava_under`): a top-down map per level, built when the level is first drawn,
 * from the collision mesh's `lava` triangles that face up plus the lava `water-anim` volumes
 * (`LavaVolumes.inc`, generated from the entities), each volume's footprint minus the ground that
 * reaches its surface. Built on the loader thread with the level (`prepare`), uploaded on its
 * first draw; the CPU copy stays with the level, so an upload can be repeated. The lava is an
 * area light: coverage with a mip chain gives, for a point h above the lava, how much of the discs
 * of radius h/2 .. 4h below it (toward where it faces) is lava, which approximates its view
 * factor. A second, linearly filtered texture holds per texel the slab top (the first horizontal
 * collision above the nearest lava: a ceiling, a walkway, the floor of a tunnel stacked above;
 * none under open sky), the nearest lava's height and the horizontal distance to it, so stacked
 * geometry never takes light through solid rock and the light fades out smoothly upward and
 * sideways. The light is the game's own lava light (the mood's `light-group 0 dir2` while it
 * points down, sent each frame by `pc-lava-light`). Background class shading adds it on
 * lava-facing and downward sides, less the share the level's bake already holds (lava-hued
 * palette slots 4-7); merc adds it on characters (world position from the frame camera), less
 * vanilla's own dir2 under-light.
 */
namespace lava_field {

/// A triangle in metres.
struct Tri {
  float v[3][3];
  float w = 1.f;  // emitter weight: 1 lava, kHotcoalsWeight hot coals
};

/// Hot coals (firecanyon's plates over molten cracks, hot walkways) light as this share of lava.
constexpr float kHotcoalsWeight = 0.35f;

/// A lava `water-anim` volume: footprint and surface, metres.
struct Volume {
  const char* level;
  u32 aid;
  const char* name;
  float x0, x1, z0, z1;
  float surface_m;
};

/// Volumes of `level` from the generated table.
std::vector<Volume> volumes_of(const std::string& level);

struct Field {
  int w = 0, h = 0;
  float x0 = 0.f, z0 = 0.f, texel_m = 1.f;
  float y0 = 0.f;            // height the second channel is relative to (lowest lava)
  std::vector<float> cov;    // 0..1
  std::vector<float> covh;   // cov x (lava height - y0)
  std::vector<float> near_m; // height of the nearest lava (m), -inf where none in the field
  std::vector<float> dist_m; // horizontal distance to the nearest lava (m, 4-neighbour steps)
  std::vector<float> roof;   // slab top (m): first horizontal collision over the nearest lava
                             // within the margin; +inf under open sky or out of reach
  float collide_spread_m2 = 0.f;  // collision lava with more than 2 m between its surfaces
  float roofed_m2 = 0.f;          // lava with a roof over it
  float collide_m2 = 0.f;    // lava from the collision mesh
  float volume_m2 = 0.f;     // lava added by the volumes
  int dry_texels = 0;        // volume texels dropped because ground reaches the surface
  bool empty() const { return w == 0 || h == 0; }
};

/*!
 * Rasterises `lava` (collision lava triangles facing up) into a grid of `texel_m` over their
 * footprint and the volumes' (plus `margin_m`), at most `max_dim` texels a side (the texel grows
 * to fit). A volume covers its footprint at its surface height except where `ground` (upward
 * collision of any material) reaches within 0.1 m of the surface or above. Where both cover a
 * texel, the higher surface wins. The roof of a texel is the lowest of `cover` (horizontal
 * collision facing up or down) more than 0.5 m above the nearest lava's height, which is spread
 * `margin_m` out from the lava.
 */
Field build(const std::vector<Tri>& lava,
            const std::vector<Tri>& ground,
            const std::vector<Tri>& cover,
            const std::vector<Volume>& volumes,
            float texel_m,
            float margin_m,
            int max_dim);

/// Collision triangles: `lava` (pat-material lava facing up, and hotcoals at kHotcoalsWeight),
/// `ground` (the rest facing up) and `cover` (every non-lava triangle facing up or down).
void split_collision(const tfrag3::Level& level,
                     std::vector<Tri>* lava,
                     std::vector<Tri>* ground,
                     std::vector<Tri>* cover);

/// The field of a loaded level (its collision and the table's volumes), as the renderer builds it.
Field build_level(const tfrag3::Level& level, const std::string& name);

/*!
 * Share of the level's baked vertex light (tfrag and tie palettes) held by the lava-hued slots
 * among 4-7: a slot counts when its summed colour has blue under a quarter and green under 0.75
 * of red. Lightning (white) and lamp slots do not count.
 */
float bake_lava_share(const tfrag3::Level& level);


/// A level's field as built on the loader thread (`prepare`), uploaded on first draw.
struct Prepared {
  Field field;
  float share = 0.f;  // bake_lava_share
};

/// Loader thread: the field of `level` and its bake share; null when the level has no lava.
std::shared_ptr<Prepared> prepare(const tfrag3::Level& level, const std::string& name);

/// The game's lava light of a level this frame (`pc-lava-light`): dir2 level (with the mood's
/// fades and the clock), the lava clock's factor, colour, and whether vanilla lights the
/// characters with it (lavatube computes it but zeroes it).
void goal_light(const std::string& level,
                float level_x,
                float clock,
                float r,
                float g,
                float b,
                bool vanilla);
/// Texture data of a prepared field, per texel: slab top over the nearest lava and that lava's
/// height (both from y0), horizontal distance to it. Leaves `p` unchanged (re-uploads need it).
std::vector<float> near_texels(const Prepared& p);

/// Frames an uploaded field stays while its level is not drawn (the loader keeps a level loaded
/// at least 180 frames after its last use).
constexpr u64 kKeepFrames = 600;

/*!
 * Whether the field cached for a level stays: yes while the level is drawn with the same load,
 * and for kKeepFrames after it was last drawn; no once the level is drawn with another load.
 */
bool keep_entry(bool in_use, bool same_load, u64 last_used, u64 now);

/*!
 * Per background tree (tfrag, tie, shrub), after `pbr_setup_background` on an active program:
 * the glow and field uniforms for `level_name`, uploading the level's field on first use. No GL
 * call until a part was first on.
 */
void setup_background(SharedRenderState* render_state, GLuint program, const std::string& level_name);

/// Merc program setup: the glow uniforms (lakes drawn as actors) and the field's light on the
/// characters. The program must be active. No GL call while PBR is off.
void setup_merc(SharedRenderState* render_state, GLuint program);

/// True if a shrub draw of class `cls` writes the emissive mask (the glow is on for lava).
bool shrub_writes_emissive(const SharedRenderState* render_state, int cls);

/// True if a merc draw of level texture `tex` is lava-class with the glow on (writes the mask).
bool merc_writes_emissive(const SharedRenderState* render_state,
                          const std::vector<u8>* classes,
                          s32 tex);

}  // namespace lava_field
