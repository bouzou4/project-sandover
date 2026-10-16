#pragma once

#include <array>
#include <string>
#include <vector>

#include "common/common_types.h"

/*!
 * World light list (Jak 1): point lights at known emitter positions, so a
 * vent, power cell, lamp or crystal keeps lighting nearby surfaces when it is off screen or small
 * on screen. The second producer of EmissiveLights, next to the screen-space tiles.
 *
 * Table: `WorldLights.inc`, generated from the light census by `sandover-lights bake` and
 * compiled in (a test checks it against the census): `{level, x, y, z, r, g, b, radius_m, gain,
 * class, gate, param, aid}`, metres, game world space. Grouped by level on first use.
 *
 * State: each light has a gate. `ALWAYS`, `HOUR` (the part spawner's hour mask, with the hour GOAL
 * sends) and `SLOT` (the static E weight the background renderers use this frame) are resolved
 * here. The entity gates are answered by GOAL once per frame (`pc-world-lights-begin`, `-query`,
 * `-set`, called from the display loop): the renderer lists the nearest entity-gated lights, GOAL
 * looks each up by actor id and returns how strongly it glows (0..1000). Unknown = off.
 *
 * The tile lights use select(); the actor lights (ActorShadows) use select_actor(). Both queue
 * their entity-gated rows for GOAL.
 */
namespace world_lights {

/// Must match GATES in sandover/tools/sandover/lights/bake.py.
enum Gate : u8 {
  ALWAYS = 0,
  HOUR = 1,  // param: bit h set = off at hour h
  SLOT = 2,  // param: E source (0-7 palette slot, 8 static, 9 night), weight from the background
  ALIVE = 10,
  CRATE = 11,
  CRYSTAL = 12,
  TASK_OPEN = 13,
  TASK_DONE = 14,
  DARK_CRYSTAL = 15,  // maincave/darkcave dark-crystal: follows its countdown flash
  ECO = 16,           // eco shot or blast (transient, not in the table): follows GOAL's report
};

/// Preset classes (`cls`): strong (vents, cells, crystals, lava, lit trim), weak (orbs, eco,
/// crates), arc (lightning, fixed low), particle (part spawners, times the group's strength),
/// lamp (lamps and torches: static E on night and flame slots), glow (decorative props lit by
/// design, e.g. snow ice crystals: they take their own tile slots, kAddedSlots).
enum Class : u8 {
  STRONG = 0,
  WEAK = 1,
  ARC = 2,
  PARTICLE = 3,
  LAMP = 4,
  GLOW = 5,
  NUM_CLASSES = 6
};

constexpr int kMaxLights = 32;   // upper bound of the per-frame count preset
constexpr int kMaxQueries = 64;  // entity-gated lights GOAL is asked about per frame
constexpr u32 kPropCrystal = 1;  // glow row param bit: a crystal (not capped with the other props)
// falloff of the lights the bake lacks, on the ground (emissive_light.frag kAddedR0) and on actors:
// 1 / (1 + d^2 / r0^2), windowed by (1 - d^2 / radius^2)^2
constexpr float kAddedR0M = 1.8f;
constexpr float kWideR0M = 3.0f;  // the same for a dark eco crystal: a 4 m tall emitter
constexpr int kAddedSlots = 8;   // tile slots for glow props, beyond `count` (GlowView fade_m 0)
constexpr int kDarkSlots = 2;    // tile slots for dark eco crystals, beyond those
constexpr int kEcoSlots = 4;     // tile slots for eco shots and blasts, beyond those
constexpr int kEcoRowBase = 1 << 24;  // row ids of eco lights: base + GOAL process id

/// E source weights of one level for this frame (Unbake.h `unbake_emissive_weights` layout).
struct LevelWeights {
  std::string name;
  bool have = false;
  float w[10] = {};
};

struct Selected {
  float pos_m[3];    // world, metres
  float power[3];    // colour x class intensity x gain x state
  float radius_m;
  bool arc;
  bool added;  // glow prop or dark eco crystal: light the bake lacks, it replaces no screen tile
  bool wide;   // dark eco crystal: falloff r0 kWideR0M
  bool snap;   // dark eco crystal: listed at zero power while dark, no fade (follows the flash)
  bool launched = false;  // lit and placed by this frame's particle launch (launch_push)
  int row;                // table row, stable while the table is loaded
};

/// How the glow props are chosen (pc-set-emissive-lights 35-37). `fade_m` <= 0: the kAddedSlots
/// nearest to the camera at full power (before 35 existed).
struct GlowView {
  float fwd[3] = {0.f, 0.f, 1.f};  // camera forward, world, unit
  float tan_half = 1.f;            // tangent of the half-angle of a cone holding the view
  float fade_m = 0.f;              // power fades to 0 over [0.55, 1] x this from the camera
  int slots = 0;                   // glow prop slots (0: every slot the others leave)
  int reserved = 0;                // slots kept free (eco shots, room for rows fading out)
  // screen tile lights of the last readback (world, metres): a row within reach of one ranks
  // before rows that are nearer but cover none
  const std::vector<std::array<float, 3>>* tiles = nullptr;
};

/*!
 * This frame's lights: the `count` nearest to `cam_m` (world, metres) that are on, from the
 * levels in `levels`, plus glow props and dark eco crystals in slots of their own (`glow`). Rows
 * that take over a screen tile light (`glow.tiles` within their radius for kClaimInFrames, held
 * kClaimFrames after) come before nearer rows that take over none, the nearest count/2 of them.
 * Also refreshes the list GOAL is asked about next frame.
 */
std::vector<Selected> select(const std::vector<LevelWeights>& levels,
                             const float cam_m[3],
                             int count,
                             const float class_intensity[NUM_CLASSES],
                             float radius_scale,
                             float radius_max_m,
                             const GlowView& glow = GlowView());

/// Glow prop weight for a light at `pos_m`, radius `radius_m`, from the camera: the distance
/// fade times a soft in-view term (1 inside the cone, 0 beyond its margin). Ranking only.
float glow_view_score(const GlowView& g, const float cam_m[3], const float pos_m[3],
                      float radius_m, float* dist_fade);

/// A light for the characters (ActorShadows): world metres, power in merc vertex-colour units.
struct ActorLight {
  bool added = false;  // light not in the bake (glow props, dark eco crystals)
  float pos_m[3];
  float power[3];
  float influence;  // luminance reaching the query point
  float dist_m;     // from the query point
  float r0_m = 1.f, radius_m = 1.f;  // falloff (kAddedR0M or kWideR0M and the light's radius
                                     // for lights the bake lacks)
  float state;      // gate state 0..1
  float fade = 1.f;  // ActorShadows' fade in/out weight, already in power
  int row;
  u8 cls, gate;
  u32 aid;
  const char* level;
};

/*!
 * Actor lights: rows that are on within `reach_m` of `at_m` (Jak), ranked by the luminance they
 * bring there, lum(power) / (1 + d^2 / r0^2) windowed to 0 at the reach; rows in `keep` rank
 * `keep_bonus` times higher. Lamps, fire part spawners, strong emitters and (weakly) glow
 * props only: no eco, arcs, butterflies or waterfalls. A near-white lamp is lit as
 * incandescent. Queues the nearest entity-gated rows for GOAL.
 */
std::vector<ActorLight> select_actor(const std::vector<LevelWeights>& levels,
                                     const float at_m[3],
                                     int count,
                                     float reach_m,
                                     float r0_m,
                                     const std::vector<int>& keep,
                                     float keep_bonus);

/*!
 * Darkcave: the lit crystals of `levels`, at most `max`, as {x, y, z (world metres), glow}. The
 * glow is the crystal's palette fade (0..2, cavecrystal glow-u); unlit crystals are not listed.
 * Call after select_actor(); queues every crystal of `levels` for GOAL.
 */
int cave_crystals(const std::vector<LevelWeights>& levels, float out[][4], int max);

/// GOAL: start this frame's state update. `hour` 0..23 (-1 unknown). Returns the query count.
int goal_begin(int hour);
/// GOAL: `aid | gate << 32` of query `i`, 0 past the end.
u64 goal_query(int i);
/// GOAL: glow of query `i`, 0..1000 (crystals up to 2000).
void goal_set(int i, int level_milli);

/*!
 * Eco shots and blasts: GOAL lists the live ones once per frame (main.gc
 * pc-eco-lights-update) by the particle group they launch; EcoLights.inc (`sandover-lights eco`)
 * gives each group its colour. The list holds while GOAL sends none (game paused), so a paused
 * frame keeps the light of the frame before. select() and select_actor() add them as lights the
 * bake lacks, with no fade (snap), in slots of their own.
 */
/// GOAL: start this frame's eco list; false while the eco lights are off (GOAL sends nothing).
/// begin, known and push run on the GOAL thread only and take no lock; end takes it once.
bool eco_begin();
/// GOAL: true when `group` has a row in EcoLights.inc (GOAL caches the answer per group).
bool eco_known(const char* group);
/// GOAL: one shot (`size_m` its collision radius) or blast (`size_m` 0) at `pos_m`, glowing
/// `level` 0..1, by its particle group; unknown groups are ignored.
void eco_push(const char* group, const float pos_m[3], float size_m, float level, u32 id);
/// GOAL: the list is complete; it replaces the last one. The Dark Cave's half reach follows the
/// level Jak is in (Jak 1 keeps no level per process, and the game's own level-by-sphere test is
/// unreliable, level.gc level-get-target-inside), so a shot across that border takes Jak's side.
void eco_end();

/*!
 * Particle launches (the game's own emitter state): every sparticle-launch-control spawn call
 * reports its group, origin and the launching entity (sparticle-launcher.gc), whether particles
 * are drawn or not. Only the glow groups of LaunchLights.inc count. An entity-gated row whose
 * entity launched one this frame is on, at the origin plus the group's offset (launcher's frame);
 * for 30 frames after its last launch it is dark, then the GOAL query answers again (a process
 * that stopped running, e.g. actor-pause past 80-220 m). Off: `pc-set-emissive-lights 38 0`.
 */
/// GOAL thread: one spawn call. `quat` (x y z w) is the launcher's orientation, or null.
void launch_push(const char* group, const float pos_m[3], u32 aid, const float* quat);
/// GOAL thread, display loop: the frame's launches replace the last ones (kept while paused).
void launch_end(bool paused);
/// A launch-placed row that got a slot in the last select() and is in view: the screen tiles in
/// its direction and depth band stand for it (LightTracker::Params::emitters).
struct LaunchLit {
  float pos_m[3];
  float radius_m;
  float colour[3];
};
std::vector<LaunchLit> launch_lit();
/// Entities in the current launch list; `calls`: every spawn call of that frame (burst log).
int launch_count(int* calls = nullptr);
/// Eco lights listed now, for the debug window.
int eco_count();

/// Rows in the table (built on first use).
int size();
/// Rebuild the per-level index and clear the cached state (debug window button).
void reload();
/// Lights on / queried in the last select(), for the debug window.
int last_lit();
int last_queries();
/// Forget the tile claims (camera cut): the new view's rows rank nearest-first until claimed.
void reset_claims();
/// Last select(): rows claimed, claimed rows ranked first, unclaimed rows within 8 m left out.
void last_claims(int* claimed, int* first, int* dropped_near);

}  // namespace world_lights
