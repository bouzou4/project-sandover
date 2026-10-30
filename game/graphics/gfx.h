#pragma once

/*!
 * @file gfx.h
 * Graphics component for the runtime. Abstraction layer for the main graphics routines.
 */

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <vector>

#include "common/common_types.h"
#include "common/util/FileUtil.h"
#include "common/versions/versions.h"

#include "game/kernel/common/kboot.h"
#include "game/settings/settings.h"
#include "game/system/hid/display_manager.h"
#include "game/system/hid/input_manager.h"

// forward declarations
struct GfxGlobalSettings;
class GfxDisplay;

// enum for rendering pipeline
enum class GfxPipeline { Invalid = 0, OpenGL };

// module for the different rendering pipelines
struct GfxRendererModule {
  std::function<int(GfxGlobalSettings&)> init;
  std::function<std::shared_ptr<GfxDisplay>(int width,
                                            int height,
                                            const char* title,
                                            GfxGlobalSettings& settings,
                                            GameVersion version,
                                            bool is_main)>
      make_display;
  std::function<void()> exit;
  std::function<u32()> vsync;
  std::function<u32()> sync_path;
  std::function<void(const void*, u32)> send_chain;
  std::function<void(const u8*, int, u32)> texture_upload_now;
  std::function<void(u32, u32, u32)> texture_relocate;
  std::function<void(const std::vector<std::string>&)> set_levels;
  std::function<void(const std::vector<std::string>&)> set_active_levels;
  std::function<void()> force_reload_all;
  std::function<void(const std::string&)> force_reload_level;
  std::function<void()> force_reload_common;
  std::function<void(float)> set_pmode_alp;
  GfxPipeline pipeline;
  const char* name;
};

// runtime settings
static constexpr int PAT_MOD_COUNT = 4;
static constexpr int PAT_EVT_COUNT = 20;
static constexpr int PAT_MAT_COUNT = 34;
/// SSAO parameters (Jak 1). Field order matches sandover/presets/ssao.toml and SsaoPresets.inc.
struct SsaoParams {
  float radius_m = 0.8f;     // XeGTAO effect radius in metres, before its 1.457 multiplier
  float intensity = 0.9f;    // composite strength on background pixels
  float merc_weight = 1.5f;  // strength multiplier on character pixels
  float luma_floor = 4.f;    // u8 luminance below which cave pixels get no AO
  float cave_scale = 0.5f;   // strength multiplier while a fixed-slot cave level is loaded
  int gtao_slices = 2;       // XeGTAO slices per pixel
  int gtao_steps = 2;        // XeGTAO steps per slice side
  int gtao_full_res = 0;     // 1: AO and its denoise at full resolution, else half
  float gtao_thin = 0.f;     // XeGTAO thin occluder compensation (its thickness heuristic)
};

/// pc-set-ssao-param indices; values are sent in millionths. 3-5, 9, 12 and 13 were SAO's.
enum class SsaoParam {
  ENABLE = 0,
  RADIUS = 1,
  INTENSITY = 2,
  MERC_WEIGHT = 6,
  LUMA_FLOOR = 7,
  CAVE_SCALE = 8,
  VIEW = 10,
  GPU_TIMING = 11,
  NORMAL_TARGET = 14,
  EMISSIVE = 15,     // 0: ignore the sidecar's emissive mask E, 1: scale AO by 1 - ramp(E)
  EMISSIVE_LO = 16,  // E at which AO starts to fade
  EMISSIVE_HI = 17,  // E at and above which a pixel gets no AO
  FOG_WEIGHT = 18,   // 0: AO over fog as before; else AO fades by the fog and mist, this exponent
  GTAO_SLICES = 19,
  GTAO_STEPS = 20,
  GTAO_FULL_RES = 21,
  GTAO_THIN = 22,
  GTAO_DENOISE = 23,  // XeGTAO denoise passes 1-3 after the AO history (dev; not a preset field)
  GTAO_HISTORY = 24,  // 0: no AO history, the temporal AA alone resolves the rotating noise (A/B)
  GTAO_HISTORY_CLAMP = 25,  // 0: AO history without its neighbourhood clamp (dev A/B)
  GTAO_HISTORY_GAMMA = 26,  // clamp box: 3x3 mean +- this many std devs inside min/max; 0: min/max
};

/// XeGTAO constants (its defaults) that no preset sets.
struct GtaoSettings {
  int denoise = 2;             // denoise passes after the AO history (3 without one)
  bool history = true;                 // AO history on frames with motion vectors
  bool history_clamp = true;           // history clamped to this frame's 3x3 AO range
  float history_gamma = 1.5f;          // clamp box: 3x3 mean +- gamma sigma, inside min/max (0: min/max)
  float history_blend = 0.9f;          // AO history weight on static texels
  float history_blend_dynamic = 0.6f;  // on characters
  float radius_mul = 1.457f;   // effect radius = SsaoParams::radius_m x this
  float falloff_range = 0.615f;
  float sample_power = 2.f;
  float response = 0.5f;       // composite darkening = intensity x occlusion^response
  float max_darkening = 0.8f;  // cap on that intensity, characters included
  float mip_offset = 3.3f;
  float blur_beta = 1.2f;      // denoise centre weight
};

/// pc-set-sun-option indices; values are sent in millionths.
enum class SunOption {
  ENABLE = 0,
  MODE = 1,        // SunMode
  OFFSET_DEG = 2,  // along the fitted arc, + toward later hours
  NIGHT = 3,       // SunNight
  STRENGTH = 4,    // scales the whole delta, 1 = model strength
  VIEW = 5,        // SunView
  ARC_POS = 6,     // SunMode::ARC_POS position, 0..4 = day slots 0..4
  DISC = 10,           // 0/1: the sun drawn in the sky (sky levels with a sidecar)
  DISC_SIZE_DEG = 11,  // angular diameter of the disc, degrees
  DISC_GLARE = 12,     // share of the disc's colour the bloom bright pass takes, 0..1
  DISC_CORE = 13,      // HDR: core luminance in paper whites (hdr_map clips it to the peak)
  DISC_RIM = 14,       // HDR: limb luminance in paper whites
  DISC_SDR_CORE = 15,  // SDR: core luminance in paper whites (before the toon tonemap)
  DISC_SDR_RIM = 16,   // SDR: limb luminance in paper whites
  DISC_FADE = 17,      // per-frame step of the visibility toward its new value, 0..1
  DISC_HDR = 18,       // 0 HDR values while HDR output presents, 1 always, 2 never (captures)
  DISC_RESET = 19,     // options 11-18 back to their defaults (sandover-settings-reapply)
  SKY_STYLE = 20,      // 0: the disc (11-18); 1: Jak 3 style sun glow, moon and stars
  SKY_BURST = 21,      // starburst strength, 1 = Jak 3's
  SKY_WASH = 22,       // wide sun and moon wash strength, 1 = Jak 3's
  SKY_GLOW = 23,       // HDR: sun glow luminance in paper whites around the hot core
  SKY_MOON = 24,       // HDR: moon disc luminance in paper whites
  SKY_MOON_SDR = 25,   // SDR: the same
  SKY_STARS = 26,      // star brightness, 1 = default, 0 = no stars
  SKY_MOON_DEG = 27,   // moon angular diameter, degrees
  SKY_GLOW_SDR = 28,   // SDR: sun glow luminance in paper whites
  SKY_RESET = 29,      // options 20-28 back to their defaults (sandover-settings-reapply)
};

/// pc-set-tonemap indices; values are sent in millionths.
enum class TonemapOption {
  MODE = 0,         // TonemapMode
  EXPOSURE = 1,     // scale before the curve, 1 = neutral
  KNEE = 2,         // filmic: identity below, shoulder to 1.0 above
  PAPER_WHITE = 3,  // nits, reserved for HDR output
  TOON_KNEE = 4,    // toon: luminance identity below
  TOON_RANGE = 5,   // toon: luminance 1 + range maps to 1.0
  WHITE_START = 6,  // toon and bloom hue clip: max channel where the core starts to whiten
  WHITE_FULL = 7,   // max channel where the core is the per-channel clip (vanilla)
  HALO_START = 8,   // toon: max channel where a halo starts toward the per-channel clip
  HALO_FULL = 9,    // max channel where that halo is the per-channel clip
};

enum class TonemapMode { CLAMP = 0, FILMIC = 1, TOON = 2 };

/// pc-set-perf-opt: first bit of each optimization area's four-bit range. A set bit turns that
/// optimization on (default all on); a bit no optimization uses does nothing.
enum PerfOptArea : int {
  PERF_OPT_TILE_LIGHTS = 0,  // bits 0-3
  PERF_OPT_SSAO = 4,         // 4-7
  PERF_OPT_VISTA = 8,        // 8-11
  PERF_OPT_WATER = 12,       // 12-15
  PERF_OPT_POST = 16,        // 16-19: present, post, bloom
  PERF_OPT_GEOMETRY = 20,    // 20-23: geometry and CPU
  PERF_OPT_PRESENT2 = 24,    // 24-27: present extras, the mid ocean depth pass (26), water LOD (27)
  PERF_OPT_CORE = 28,        // 28-31: renderer core, the world shadow map (28, 30, 31)
};
/// World map: the outer cascades take a cheaper soft-shadow filter and, on a 4096 map, the first
/// cascade is drawn at 2048 (opt/shadow-kernel).
constexpr int PERF_OPT_WORLD_KERNEL = PERF_OPT_CORE + 0;
/// World map receivers skip lookups that cannot change the colour (opt/shadow-receivers).
constexpr int PERF_OPT_WORLD_GATE = PERF_OPT_CORE + 3;
/// Mid ocean depth first, then its shading at depth EQUAL: once per pixel (opt/ocean-deferred).
constexpr int PERF_OPT_OCEAN_ONCE = PERF_OPT_PRESENT2 + 2;
/// Background material path: exact early-outs where a term is known to be 0 (opt/pbr-tie).
constexpr int PERF_OPT_PBR_EARLY = PERF_OPT_PRESENT2 + 1;
/// Mid ocean shading detail fades with distance past the near ocean (opt/water-lod).
constexpr int PERF_OPT_WATER_LOD = PERF_OPT_PRESENT2 + 3;
/// Bits that failed their perf or parity gate: off in the default mask until fixed (a failed
/// bit is still measurable with an explicit mask). Keep in step with sandover-perf OPT_FAILED.
constexpr u32 PERF_OPT_FAILED = 0;  // none (bit 8 passes with vista-4)
constexpr u32 PERF_OPT_DEFAULT = 0xffffffffu & ~PERF_OPT_FAILED;
/// World sun map prepass: moved cascades only, tree bounds, indirect wind, loader-owned tfrag
/// indices (read at level load, world shadows on). Off by default in sandover-perf static windows.
constexpr int PERF_OPT_WORLD_PREPASS = PERF_OPT_CORE + 2;
/// World map receivers: exact lit and blocked early-outs from a per-cell bound grid
/// (opt/shadows-wave2; the core area is full, so it borrows a free bit of the SSAO area).
constexpr int PERF_OPT_WORLD_BOUNDS = PERF_OPT_SSAO + 2;

/// Background depth prepass bits (pc-set-ssao-option 5): depth of the opaque tfrag and tie of
/// each level, then of each shrub bucket, before their shaded draws.
enum BgPrepass : int {
  BG_PREPASS_ON = 1,          // tfrag + tie per level
  BG_PREPASS_SHRUB = 2,       // each shrub bucket
  BG_PREPASS_NO_WRITES = 4,   // shaded draws of the prepassed set without depth writes
  BG_PREPASS_NO_CUT = 8,      // probe: ignore blended depth writers (not exact)
  BG_PREPASS_CROSS_LEVEL = 16,  // probe: both levels in one run (not exact)
};

/// pc-set-hdr-output indices; values are sent in millionths.
enum class HdrOutputOption {
  ENABLE = 0,       // 0/1; implies the FP16 target while on
  FORMAT = 1,       // HdrOutputFormat; PQ (HDR10) by default
  PAPER_WHITE = 2,  // nits for game white (1.0); shared with TonemapOption::PAPER_WHITE
  PEAK = 3,         // nits; negative = from the display profile / DXGI
  BLACK = 4,        // nits; negative = from the display profile / DXGI
  SATURATION = 5,   // 1 = unchanged
  KNEE = 6,         // shoulder start: 0 = paper white, 1 = peak
  OS_SWITCH = 7,    // 0/1, opt-in: turn Windows HDR on for the display while HDR output runs
  CAL_PATTERN = 8,  // HdrCalPattern drawn over the frame by the final pass; not saved
  PEAK_MILLI = 9,   // PEAK in thousandths, for GOAL (its int math is 32-bit: millionths overflow)
};

/// Calibration test patterns (pc-set-hdr-output 8), drawn while a calibration row is focused.
enum class HdrCalPattern { NONE = 0, PEAK = 1, PAPER_WHITE = 2, BLACK = 3 };

enum class HdrOutputFormat { SCRGB = 0, PQ = 1 };

/// pc-set-water indices (values in millionths).
enum class WaterOption {
  ENABLE = 0,
  WAVE = 1,       // wave-normal slope scale from the ocean texture gradient
  GLINT = 2,      // sun glint intensity (may exceed 1 on the FP16 target)
  GLINT_EXP = 3,  // glint Blinn exponent
  FRESNEL = 4,    // blend toward the horizon colour at grazing angles
  SPARKLE = 5,    // sunlit wave-crest sparkle intensity
  VIEW = 6,       // 1 normal, 2 glint and sparkle, 3 fresnel, 4 view vector, 5 sun direction,
                  // 6 foam, 7 depth ramp and shore fade, 8 wave height, 9 detail fade and lod,
                  // 10 envmap mask (texture alpha), 11 no envmap, 12 near opaque, 13 envmap only,
                  // near envmap factors alone: 14 coverage, 15 texture, 16 dest alpha, 17 vertex colour,
                  // 18 renderer (red near, green mid) and wave weight (blue), 20 seam coverage
                  // (near red, mid green, envmap passes off), 21 dry (the near ocean covers nothing)
  DISPLACE = 7,      // Gerstner amplitude scale; 0 keeps the vanilla vertices
  SUBDIV = 8,        // max tessellation level of an ocean triangle edge (1..32)
  RANGE_FULL_M = 9,  // waves at full amplitude up to this distance (m)
  CREST_FOAM = 10,   // foam where the waves bunch up (Jacobian below the threshold)
  CREST_J = 11,      // Jacobian threshold of the crest foam
  SHORE_FOAM = 12,   // foam where the water is shallow (scene depth under the surface)
  SHORE_M = 13,      // shoreline foam band width (m of water depth)
  SHALLOW = 14,      // shallow-water colour strength
  SHALLOW_M = 15,    // depth (m) over which the shallow colour fades to the level colour
  SSS = 16,          // subsurface tint on crests and backlit wave backs
  CREST_LIGHT = 17,  // crests lighter, troughs darker
  ABSORB = 18,       // near-ocean opacity gained per metre of water seen through
  RANGE_ZERO_M = 19, // waves fall off smoothly to none at this distance (m)
  WAVE0 = 20,        // 20 + 5 * wave + field; kWaterWaves waves, fields in WaterWaveField
  SKY_REFL = 45,     // fresnel reflects the mood environment (sky, horizon, sun glow), 0..1
  SKY_HAZE_M = 46,   // the reflection hazes into the fog colour over this distance (m)
  AMP_NOISE = 47,    // per-wave amplitude noise, 0..1 (fraction of the amplitude)
  PHASE_NOISE = 48,  // per-wave phase noise (turns)
  NOISE_M = 49,      // cell size of the amplitude and phase noise (m)
  SHALLOW_DEPTH_M = 50,  // shallow tint gone and water opaque by this depth (m)
  SHORE_FADE_NEAR_M = 51,  // shallow tint and shore foam fade out with camera distance from here
  SHORE_FADE_FAR_M = 52,   // ... to none here (m)
  SHOAL_M = 53,      // waves shrink to none as the water under them gets this shallow (m)
  TILE_HEX = 54,         // ocean texture hex tiling (rotated, offset samples), 0..1; 0 one repeat
  TILE_HEX_M = 55,       // hex tiling grid spacing (m)
  TILE_HEX_ROT_DEG = 56, // largest rotation of a hex sample (degrees)
  TILE_WARP_M = 57,      // crest warp amplitude (m): each wave's phase shifts by a slow noise
  TILE_WARP_CELL_M = 58, // crest warp cell size (m); the second octave is a quarter as strong
  LAKE_WAVE0 = 60,   // 60 + 5 * wave + field: the lake wave set (kWaterBodies)
  AA_LOD_START = 85,     // texture ripple, sparkle and foam noise start to fade at this lod
                         // (log2 texels or noise cells per pixel)
  AA_LOD_END = 86,       // ... and are gone at this lod
  AA_FOAM_EDGE_PX = 87,  // foam threshold edge softness (pixels); 0 is a hard cut
  AA_WAVE_PX_FULL = 88,  // a Gerstner wave's slope is whole above this many pixels per wavelength
  AA_WAVE_PX_ZERO = 89,  // ... and gone below this many; 0 is off
  CLOCK_S = 90,          // wave clock pinned at this many seconds (A/B captures); negative is live
  SHOAL_OCC = 91,        // shoal ring weight of a sample under a float or deck (negative)
  DEPTH_BLUR_M = 92,     // the shallow tint reads the depth over this radius (m)
  TESS_SEG_M = 93,       // tessellation segment within 250 m (m); a perf probe, 0.75 in play
  SUN_GLINT = 94,        // 0 off, 1 analytic sun glint on water (GGX), shadowed by the world map
  SUN_GLINT_STRENGTH = 95,  // sun radiance scale of the glint
  SUN_GLINT_ROUGH = 96,     // GGX alpha of the water surface between the wave normals
  SUN_GLINT_PEAK = 97,      // soft cap of the glint's linear luminance (1 = SDR white)
  SUN_GLINT_RESET = 98,     // 95-97 back to the engine defaults (sandover-settings-reapply)
  LOD_START_M = 99,  // mid ocean detail starts to fade at this distance (m), perf-opt bit 27
  LOD_END_M = 100,   // ... and is gone (flat normal, no texture ripple or crest sparkle) here
  LOD_PROBE = 101,   // cost probe, mid ocean only: terms off past (n >> 12) m; 1 sun map, 2 glint,
                     // 4 glow, 8 reflection, 16 foam, 32 ripple, 64 sparkle, 128 waves, 256 hex,
                     // 1024 all shading; 2048 distance band view
  OPAQUE_M = 102,    // near ocean fully opaque by this water depth (m)
  ABSORB_R = 103,    // per-channel absorption, times ABSORB
  ABSORB_G = 104,
  ABSORB_B = 105,
};

enum class WaterWaveField { DIR_DEG = 0, AMP_M = 1, LENGTH_M = 2, SPEED = 3, STEEP = 4 };
constexpr int kWaterWaves = 5;  // four chop waves, then the long swell

/// One Gerstner wave of the ocean shading.
struct WaterWave {
  float dir_deg = 0.f;   // travel direction in the world xz plane, degrees from +x
  float amp_m = 0.f;     // vertical amplitude (m)
  float length_m = 1.f;  // wavelength (m)
  float speed = 1.f;     // phase speed (m/s)
  float steep = 0.f;     // 0 round, 1 sharpest crest this wave may take within the sum
};

/// pc-set-swamp indices (values in millionths); tuning and debug. 0-15 apply only in the swamp
/// level, 16-20 (the wet shoreline) only in the other levels with a sea (`kWaterBodies`), 25-30
/// and 33 (the sea mist) only on Misty Island; 21-24 and 31-32 are unused.
enum class SwampOption {
  WATER = 0,           // 0/1: the swamp water (tfrag `swp-water`) as a shaded surface; needs PBR on
  WATER_DARK = 1,      // body colour multiplier (murk), 0..1
  WATER_REFL = 2,      // mood-environment reflection strength
  WATER_RIPPLE = 3,    // rain-ripple normal strength
  WATER_GLINT = 4,     // sun glint on the ripples
  WET = 5,             // 0/1: wet background surfaces while it rains; needs PBR on
  WET_DARK = 6,        // albedo multiplier of fully porous surfaces, 0..1
  WET_COAT = 7,        // water-film reflection and glint strength
  WET_PUDDLE = 8,      // puddle coverage on flat ground, 0..1
  MIST = 9,            // 0/1: ground mist over the water; needs SSAO on (reads its depth)
  MIST_DENSITY = 10,   // extinction at the water line (1/m)
  MIST_HEIGHT_M = 11,  // scale height of the mist layer (m)
  MIST_START_M = 12,   // no mist closer than this (m)
  MIST_MAX = 13,       // largest mist opacity, 0..1
  MIST_SUN = 14,       // forward-scatter glow toward the sun
  MIST_BRIGHT = 15,    // mist colour: fog colour lifted toward white by this much, 0..1
  SHORE = 16,          // 0/1: porous surfaces wet in a band above the sea; needs PBR on
  SHORE_BAND_M = 17,   // band height above the sea on flat ground (m); steep faces get half
  SHORE_DARK = 18,     // linear-light brightness of fully porous surfaces at the water line
  SHORE_FILM = 19,     // water-film sun glint strength
  SHORE_CELL_M = 20,   // world-noise cell of the band's upper edge (m)
  MISTY_MIST = 25,     // 0/1: Misty Island sea-level mist; needs SSAO on
  MISTY_MIST_DENSITY = 26,   // at the sea surface (1/m), at strength 1
  MISTY_MIST_HEIGHT_M = 27,  // scale height (m)
  MISTY_MIST_START_M = 28,   // no mist nearer than this (m)
  MISTY_MIST_MAX = 29,       // largest mist opacity at strength 1, 0..1
  MISTY_MIST_BRIGHT = 30,    // mist colour: fog colour lifted toward white by this much, 0..1
  MISTY_MIST_STRENGTH = 33,  // scales the Misty mist density and its opacity cap
  WET_PBR = 40,        // 0/1: wetness as a water film in the PBR inputs (roughness, relief)
                       // instead of the separate film; swamp and shoreline
  WET_PBR_GLOSS = 41,  // film weight at full wetness on non-sand surfaces, 0..1
  WET_PBR_ROUGH = 42,  // the film's roughness
  WET_PBR_F0 = 43,     // the film's reflectance at normal incidence
  WET_PBR_DARK = 44,   // linear-light brightness of wet non-sand shore surfaces, 0..1
  WET_PBR_SPLASH_FULL_M = 45,  // splash zone on non-sand shore surfaces: fully wet to (m)
  WET_PBR_SPLASH_TOP_M = 46,   // and dry at (m above the sea)
};

/// pc-set-caustics indices (values in millionths); tuning and A/B comparison. Applies only in
/// levels whose mood runs update-mood-caustics (CausticMoods.inc); needs PBR on.
enum class CausticOption {
  MODE = 0,       // 0: the original per-vertex loop; 1: procedural pattern on the frozen mean
  CELL_SCALE = 1, // noise cell as a multiple of the level's own (CausticMoods.inc)
  LOOP_HZ = 2,    // visible loop rate (Hz); not HZ, a Linux kernel-header macro
  DRIFT_MPS = 3,  // drift of the two noise layers (m/s)
  AMP = 4,        // contrast around the mean, relative to the original's swing per entry
  WEB = 5,        // 0: soft blotches; > 0: line half-width of the web, in noise deviations
  WEB_MEAN = 6,   // spatial mean of the web term (sandover-caustics noise)
  CLOCK_S = 7,    // pattern clock held at this many seconds (A/B captures); negative is live
};

/// pc-set-fur indices (values in millionths).
enum class FurOption {
  ENABLE = 0,
  SHELLS = 1,     // shell count drawn over each fur draw
  LENGTH_M = 2,   // tip offset along the normal, metres
  DENSITY = 3,    // strands per texture UV unit
  GRAVITY = 4,    // tip droop, fraction of the length
  CURL = 5,       // sideways swirl, fraction of the length
  ROOT_DARK = 6,  // colour scale at the root shell is 1 - this
  VIEW = 7,       // 1: shells in flat colour by index
  SIZE_EXP = 8,   // length x (texture metres per UV unit / SIZE_REF_M) ^ this, 1-16; 0 off; 1.75 default
  SIZE_REF_M = 9, // metres per UV unit up to which the length is as set (0.3: lurkers keep theirs)
};

/// pc-set-lava indices (values in millionths); see LavaField.h. Both parts ride on PBR.
enum class LavaOption {
  GLOW = 0,           // 0/1: lava-class texels glow where molten, crust shades as stone
  GLOW_GAIN = 1,      // HDR headroom of a fully molten texel: colour x (1 + this)
  GLOW_MASK = 2,      // emissive mask of a fully molten texel (bloom and AO, never a tile light)
  GLOW_KEY_LO = 3,    // chroma key R - max(0.6 G, B) where a texel starts to be molten
  GLOW_KEY_HI = 4,    // chroma key where a texel is fully molten
  UNDER = 5,          // 0/1: the lava field lights the background and characters from below
  UNDER_GAIN = 6,     // the field's light, times the game's lava light (dir2)
  UNDER_REACH_M = 7,  // height above the lava the light reaches (m)
  UNDER_REFL = 8,     // the field's strength in downward reflections
  UNDER_BAKE = 9,     // how much of the bake's own lava share is taken off the diffuse light, 0..1
  VIEW = 10,          // 1: field view factor over a grey frame, 2: the field light alone
  GLOW_CORE = 11,     // white-hot core added to the brightest molten texels
  GLOW_CRUST = 12,    // crust darkening, 0..1
  UNDER_CHAR = 13,    // the field light on characters, times the background's
  PIN = 14,           // 0/1: lava light and glow at the mean of the game's lava clock (captures)
  RESET = 15,         // every option above back to its default (sandover-settings-reapply)
};

/// pc-set-inland-water indices (values in millionths); see InlandWater.h. Needs PBR on.
enum class InlandWaterOption {
  WATER = 0,         // 0/1: clear water (ponds, lakes, river, fountain, paddies, sunken pools)
  DEPTH_M = 1,       // view-ray length through the water that reaches 63 % of the game's tint (m)
  REFL = 2,          // fresnel reflection strength (sky levels)
  REFL_CAP = 3,      // largest reflection weight, 0..1
  GLINT = 4,         // sun glint on the ripple normals
  MUD = 5,           // 0/1: Misty's quicksand as wet mud
  MUD_DARK = 6,      // mud colour multiplier, 0..1
  MUD_REFL_CAP = 7,  // largest mud reflection weight, 0..1
  MUD_ROUGH = 8,     // mud roughness, 0..1
  MUD_GLINT = 9,     // broad sun lobe on the mud
  VIEW = 10,         // debug: 1 the depth factor (share of the game's alpha) as opaque grey
  PICKED = 11,       // 1: no shading, only the envmap layer dropped on sky levels (the first merc variant)
};

/// pc-set-shadow-option indices; values are sent in millionths.
enum class ShadowOption {
  MODE = 0,           // ShadowMode
  STRENGTH = 1,       // scales the removed light share, 1 = physical estimate
  VIEW = 2,           // ShadowView
  RANGE_M = 3,        // length of the view frustum part the sun map covers, metres
  LAMP_SCALE = 4,     // local light power multiplier (actor lights and lamp shadows)
  MARKER = 5,         // soft landing disc under the player: strength, 0 = off
  REACH_M = 6,        // casters higher than this above a receiver cast nothing
  LIGHTS = 7,         // local lights lighting the characters, 0-4; the first two cast (0 = none)
  LIGHT_R0_M = 8,     // local light falloff: power / (1 + d^2 / r0^2)
  LIGHT_REACH_M = 9,  // local lights reach this far (smooth window)
  CAVE_DARK = 10,     // darkcave: characters keep this share of the level light away from crystals
  CANOPY = 11,        // canopy shade strength, 0 = off
  CANOPY_AMBIENT = 12,   // canopy shade: darkening of the shade under full cover, 0..0.8
  CANOPY_SHARP = 13,     // canopy shade: sun patch edge, 0 soft .. 1 hard
  CANOPY_PENUMBRA = 14,  // canopy shade: penumbra width per metre of leaves above
  CANOPY_TINT = 15,      // canopy shade: cool shift of the shade, 0..1
  CANOPY_PATCH = 16,     // canopy shade: sun patch brightening over today, 0..0.5
  TERRAIN_SUN = 20,      // actors lose the sun along the sun ray, their shadow fades; 0 = off
  SKY_HUE = 30,       // sky levels: actor and lamp shadows darken toward the ambient's hue
  SKY_HUE_STRENGTH = 31,  // 0 grey .. 1 the fitted sun/ambient split
  WORLD = 32,             // world sun map: tfrag, tie and shrub receive it; 0 = baked shadows only
  WORLD_ACTORS = 33,      // merc takes the world map per pixel instead of TERRAIN_SUN cover
  WORLD_WATER = 34,       // water may read the world map (ws_water_vis)
  WORLD_SIZE = 35,        // texels per cascade side: 1024, 2048 or 4096
  WORLD_CASCADES = 36,    // 1-3
  WORLD_PENUMBRA = 37,    // penumbra width per metre between blocker and receiver
  WORLD_RANGE_M = 38,     // distance from the camera the last cascade reaches
  WORLD_RESET = 39,       // 32-38 back to their defaults (sandover-settings-reapply)
  QUALITY = 40,           // 0 low (1024 sun map, 1 tap), 1 medium (1536, 4 taps), 2 high (2048, 9)
  CONTACT = 41,           // screen-space contact shadows: 0 off, 1 moderate, 2 strong (TAA only)
  CONTACT_VIEW = 42,      // ContactShadowView
  PARALLAX_SHADOW = 43,   // parallax self-shadow: 0 off, 1 moderate, 2 strong (on with parallax)
  PROBE = 44,             // ShadowProbe bits: cost splits for profiling, not a look
};
/// ShadowOption::PROBE bits (debug, changes the look): split the shadow cost in a perf run.
enum ShadowProbe : int {
  SHADOW_PROBE_WORLD_ONE_TAP = 1,  // world map receivers: one compare tap, no search or kernel
  SHADOW_PROBE_WORLD_SKIP = 2,     // world map receivers: no lookup at all (lit)
  SHADOW_PROBE_ACTOR_SUN = 4,      // background receivers skip the actor sun map
  SHADOW_PROBE_LAMPS = 8,          // background receivers skip the lamp maps
  SHADOW_PROBE_FRAGS = 16,         // count background fragment shader runs (bg-mfrags)
};
/// Debug views of the contact shadow pass (ShadowOption::CONTACT_VIEW).
enum class ContactShadowView { NONE = 0, HITS = 1, RAY = 2, FACTOR = 3, SHARE = 4 };
/// off: vanilla stencil shadow; on: sun and lamp maps, every actor casts.
enum class ShadowMode { OFF = 0, ON = 1 };
enum class ShadowView { NONE = 0, OCCLUSION = 1, SHARE = 2, MAP_COORDS = 3, LOCAL_LIGHTS = 4 };

enum class SunMode { IDENTITY = 0, FOLLOW = 1, DIRECTION = 2, ARC_POS = 3 };
enum class SunNight { FITTED = 0, TABLE = 1, AMBIENT_ONLY = 2 };
enum class SunView { NONE = 0, MODEL = 1, CONFIDENCE = 2, NORMAL = 3, DELTA = 4 };

/// pc-set-pbr-param indices (Jak 1 material classes); values are sent in millionths.
enum class PbrParam {
  ENABLE = 0,
  STRENGTH = 1,    // global scale on detail and highlights; 0 = vanilla look
  DETAIL = 2,      // extra scale on detail only
  SPEC = 3,        // extra scale on highlights only
  VIEW = 4,        // PbrView
  MERC = 5,        // 0: skip characters, 1: shade them
  MERC_VSIGN = 6,  // sign of the merc view vector, +1 or -1
  BACKGROUND = 7,  // 0: skip tfrag/tie/shrub, 1: shade them
  KEY = 8,         // key-light scale where the sun share is ~0
  SOURCE = 9,      // detail normals: 0 procedural (old look), 1 per-texture maps
  TIMING = 10,     // 1: log whole-frame GPU time every 240 frames (SSAO-TIMING name=frame)
  LOOK = 11,       // reload the preset with look n (PbrLook); later params win over it
  METALLIC = 12,   // metallic workflow strength (0: metals shade as dielectrics)
  ENV = 13,        // mood-environment reflection strength
  FRESNEL = 14,    // fresnel rim scale
  AMBIENT = 15,    // ambient response to the normal map in shade
  ROUGH_CONTRAST = 16,  // roughness contrast around 0.5
  BAND = 17,       // 0: GGX lobe, 1: stylized two-step band
  LOBE = 18,       // sun and key lobe scale
  METAL_DARKEN = 19,    // extra diffuse darkening on metallic textures
  MERC_UP = 20,    // sign of world up in the merc frame, +1 or -1
  SPECULAR = 21,   // dielectric reflectance as a multiple of 0.04
  PARALLAX = 22,   // 0: no parallax offset on masonry, 1: on
  PARALLAX_DEPTH = 23,  // parallax depth, metres
  PARALLAX_BASIS = 24,  // 0: per-triangle screen-derivative basis, 1: per-vertex (continuous)
  METAL_HUE = 25,  // 1: metals reflect the bake's hue, neutral mood, half tint; 0: as before
  TRANSMIT = 26,   // scale on the per-class sun transmission (backlit leaves); 0: off
  WRAP = 27,       // scale on the per-class wrapped N.L (soft terminator); 0: off
  WIND_TIE = 28,   // 1: wind TIE draws (palm canopies) take class shading; 0: vanilla as before
  BAKE_WRAP = 29,  // scale on the per-class wrapped N.L of the bake's sun term; 0: off
  SUN_OCCLUSION = 30,  // 1: backlit leaves need the sun to reach them (leaf volume, solid world)
  LEAF_DENSITY = 31,   // optical depth scale of the leaf volume for that test (0..4)
  ICE = 40,        // strength of the ice response on class `ice` (gloss, depth, transmission); 0: off
  GLINT = 32,      // scale on the per-class sun glints (snow glitter); 0: off
  PROBE = 41,      // debug: background term-off probe mask (pbr_bg_frag.glsl), raw integer
};

/// PbrLook: preset sections `[look.<name>]` applied over the base values.
enum class PbrLook { BASE = 0, FAITHFUL = 1, BOLD = 2, CINEMATIC = 3 };

/// PbrView: 0 shaded, 1 class colours, 2 detail normal, 3 reflection and lobes only, 4 sun
/// visibility, 5 roughness (r) and metallic (g), 6 mood hemisphere on the detail normal
enum class PbrView { NONE = 0, CLASS = 1, NORMAL = 2, SPEC = 3, SUN = 4, ROUGH = 5, HEMI = 6 };

/// Unified-fog override for one neighbour level (vista neighbour mode, Jak 1).
struct VistaNeighbourFog {
  char level[16] = "";
  float density_scale = 1.f;             // multiplies vista_atmo_density_per_km
  float tint[3] = {1.f, 1.f, 1.f};       // multiplies the viewer's fog colour
  float max_fog = -1.f;                  // 0-1 cap on the fog amount; < 0: the viewer's
  float haze_scale = 1.f;                // multiplies vista_haze_density_per_km
};

struct GfxGlobalSettings {
  bool debug = true;  // graphics debugging

  // note: this is actually the size of the display that ISN'T letterboxed
  // the excess space is what will be letterboxed away.
  int lbox_w = 640;
  int lbox_h = 480;

  // actual game resolution
  int game_res_w = 640;
  int game_res_h = 480;

  // multi-sampled anti-aliasing sample count. 1 = disabled.
  int msaa_samples = 1;

  // brightness and contrast values set from GOAL (see jak 3)
  int brightness_contrast_color = 0;
  int brightness_contrast_alpha = 128;

  // current renderer
  const GfxRendererModule* renderer;

  // lod settings, used by bucket renderers
  int lod_tfrag = 0;
  int lod_tie = 0;

  // vsync enable
  bool vsync = true;
  bool old_vsync = false;
  // target frame rate
  float target_fps = 60;
  // use custom frame limiter
  bool framelimiter = true;

  // frame timing things
  bool experimental_accurate_lag = false;
  bool sleep_in_frame_limiter = true;

  // fancy effect things
  bool hack_no_tex = false;

  // collision renderer settings
  bool collision_enable = false;
  bool collision_wireframe = true;

  // matching enum in kernel-defs.gc !!
  enum CollisionRendererMode { None, Mode, Event, Material, Skip, SkipHide } collision_mode = Mode;
  std::array<u32, (PAT_MOD_COUNT + 31) / 32> collision_mode_mask = {UINT32_MAX};
  std::array<u32, (PAT_EVT_COUNT + 31) / 32> collision_event_mask = {UINT32_MAX};
  std::array<u32, (PAT_MAT_COUNT + 31) / 32> collision_material_mask = {UINT32_MAX, UINT32_MAX};
  u32 collision_skip_mask = 0;
  u32 collision_skip_hide_mask = 0;
  bool collision_skip_nomask_allowed = true;

  // mid-frame depth capture for SSAO (Jak 1 only), all off by default
  bool ssao_depth_capture = false;
  bool ssao_depth_debug = false;
  int ssao_capture_point = 0;  // 0: after opaque background, 1: after pris (characters)
  int ssao_msaa_mode = 1;      // 0: resolve blit, 1: multisample copy, shader reads sample 0
  bool ssao_gpu_timing = false;
  bool ssao_msaa_probe_request = false;
  // debug probe bits (pc-set-ssao-option 4): switch parts of the AO prerequisites off
  int ssao_prereq_probe = 0;
  // background depth prepass (pc-set-ssao-option 5), BgPrepass bits
  int bg_prepass = BG_PREPASS_ON | BG_PREPASS_SHRUB | BG_PREPASS_NO_WRITES;

  // SSAO (Jak 1 only), off by default
  bool ssao_enable = false;
  int ssao_preset = -1;            // index into the built-in presets, -1: custom parameters
  int ssao_view = 0;               // 0: none, else a debug view (SsaoRenderer::View)
  bool ssao_normal_target = true;  // opaque passes write normals; off: normals from depth
  SsaoParams ssao_params;
  bool ssao_emissive = true;  // needs <level>.unbake version 2
  float ssao_emissive_lo = 0.1f;
  float ssao_emissive_hi = 0.4f;
  // AO scaled by (fog x mist transmission)^weight; 0 off (the old composite), 1 as laid over
  float ssao_fog_weight = 1.f;
  GtaoSettings ssao_gtao;

  // bloom from the emissive mask (Jak 1 only), off by default; pc-set-bloom
  bool bloom_enable = false;
  float bloom_strength = 1.f;
  float bloom_radius = 6.f;      // Gaussian half-width, half-resolution pixels
  float bloom_threshold = 0.3f;  // colour (0..1) subtracted before E weights it
  bool bloom_hue_clip = true;    // sum over 1: add up to sum/max(r,g,b), never below the frame

  // screen-space emissive tile lights (Jak 1 only, needs SSAO on), off by default;
  // pc-set-emissive-lights. Defaults: sandover/presets/emissive.toml [emissive_lights]
  bool emissive_lights_enable = false;
  float emissive_lights_intensity = 8.f;
  float emissive_lights_radius = 4.f;       // metres
  float emissive_lights_falloff = 2.f;      // exponent of (1 - d / radius)
  float emissive_lights_threshold = 0.02f;  // tile flux below this is not a light
  // world light list (5..11): nearest N census emitters, 0 = off; intensity per class
  int world_lights_count = 16;
  float world_lights_strong = 0.08f;    // vents, cells, crystals, lava, lit trim
  float world_lights_weak = 0.01f;      // orbs, eco, crate contents, race rings
  float world_lights_arc = 0.005f;      // lightning arcs: fixed, replaces the tiles near them
  float world_lights_particle = 0.05f;  // part spawners, times the group's strength
  float world_lights_radius_scale = 1.f;
  float world_lights_radius_max = 8.f;  // metres
  float world_lights_lamp = 0.12f;      // lamps, torches (static E on night/flame slots)
  float world_lights_glow = 0.3f;       // 18: decorative props lit by design (snow ice crystals)
  bool world_lights_props = true;       // 19: 0 = glow props and dark eco crystals off (A/B)
  // 35: glow props fade by camera distance over [0.55, 1] x this, ranked by what they add to the
  // view (0 = the 8 nearest to the camera at full power, as before); 36: their slot cap
  float world_lights_glow_fade_m = 70.f;
  int world_lights_glow_slots = 0;  // 0 = every slot the baked lights and dark crystals leave
  bool world_lights_launch = true;      // 38: entity lights follow the game's particle launches
  bool eco_lights = true;               // 27: eco shots and blasts light around them
  float eco_lights_strength = 2.5f;     // 28: their power per unit of colour (WorldLights.cpp)
  // temporal stability (13..16): LightTracker.h
  float emissive_lights_blend = 0.25f;  // share of the new tile value taken per frame
  int emissive_lights_frames = 3;       // frames a light must be seen (missed) to show (hide)
  int emissive_lights_fade = 8;         // frames of fade in and out
  float world_lights_suppress = 1.f;    // tiles within this x a world light's radius give none
  // local-light highlights (20..23): a GGX lobe per tile/world/actor light from the material
  // shading's roughness and F0; needs the tile lights and material shading, so boot stays vanilla
  bool light_highlights = true;
  float light_highlights_strength = 2.f;  // 21: scale of the lobe
  float light_highlights_max = 1.5f;      // 22: soft limit of the summed highlight (max channel)
  float light_highlights_damp = 0.5f;     // 23: roughness cut on porous classes in enclosed levels

  // click-to-inspect ID target and panel (Jak 1 only), off by default; pc-set-inspect
  bool inspect_enable = false;
  // scripted picks (pc-inspect-pick), a ring served one per frame: view x, y (0..1 from the
  // top left) and verdict code
  float inspect_picks[16][3] = {};
  u32 inspect_pick_seq = 0;  // picks requested so far

  // moving sun from the per-vertex sidecar (Jak 1 only), off by default
  bool sun_enable = false;
  int sun_mode = (int)SunMode::FOLLOW;
  float sun_offset_deg = 0.f;
  int sun_night = (int)SunNight::FITTED;
  float sun_strength = 1.f;
  int sun_view = (int)SunView::NONE;
  float sun_arc_pos = 2.f;
  float sun_dir[3] = {-0.31f, 0.95f, -0.06f};  // SunMode::DIRECTION, toward the light

  // sun disc in the sky (SunDisc.h), off by default; defaults are the chosen `sun` preset
  bool sun_disc = false;
  float sun_disc_size_deg = 2.2f;
  float sun_disc_glare = 0.3f;
  float sun_disc_core = 2000.f;
  float sun_disc_rim = 2.f;
  float sun_disc_sdr_core = 12.f;
  float sun_disc_sdr_rim = 1.f;
  float sun_disc_fade = 0.35f;
  int sun_disc_hdr = 0;
  // sky bodies (SunDisc.h, style 1), on the sun switch; options 20-28
  int sky_style = 1;
  float sky_burst = 1.f;
  float sky_wash = 1.f;
  float sky_glow = 3.f;
  float sky_moon = 2.f;
  float sky_moon_sdr = 0.85f;
  float sky_stars = 1.f;
  float sky_moon_deg = 2.9f;
  float sky_glow_sdr = 1.f;
  // what SunDisc drew in the viewer level last frame: 1 our sun, 2 our stars (pc-get-sky-bodies)
  int drawn_sky_bodies = 0;

  // FP16 main colour target with a tonemap in the final pass (Jak 1 only), off by default
  bool hdr_target = false;
  int tonemap_mode = (int)TonemapMode::CLAMP;
  float tonemap_exposure = 1.f;
  float tonemap_knee = 0.8f;
  float tonemap_toon_knee = 0.95f;
  float tonemap_toon_range = 1.5f;
  float tonemap_white_start = 2.f;
  float tonemap_white_full = 3.5f;
  float tonemap_halo_start = 1.2f;
  float tonemap_halo_full = 1.8f;
  float paper_white_nits = 200.f;

  // HDR swapchain output (Jak 1, Windows); calibration values stay plain settings for the menu
  bool hdr_output = false;
  int hdr_format = (int)HdrOutputFormat::PQ;
  float hdr_peak_nits = -1.f;   // < 0: display profile (MHC2) or DXGI
  float hdr_black_nits = -1.f;  // < 0: display profile (MHC2) or DXGI
  float hdr_saturation = 1.f;
  float hdr_knee = 0.f;
  bool hdr_os_switch = false;
  int hdr_cal_pattern = (int)HdrCalPattern::NONE;

  // ocean shading (Jak 1 only), off by default; the values are the `bold` preset
  bool water_enable = false;
  float water_wave = 0.3f;
  float water_glint = 1.6f;
  float water_glint_exp = 256.f;
  float water_fresnel = 0.4f;
  float water_sparkle = 0.5f;
  int water_view = 0;
  float water_displace = 1.f;
  int water_subdiv = 16;
  float water_range_full_m = 300.f;
  float water_range_zero_m = 1200.f;
  float water_crest_foam = 0.8f;
  float water_crest_j = 0.9f;
  float water_shore_foam = 1.f;
  float water_shore_m = 1.f;
  float water_shallow = 0.75f;
  float water_shallow_m = 3.5f;
  float water_sss = 0.6f;
  float water_crest_light = 0.5f;
  float water_absorb = 0.03f;
  WaterWave water_waves[kWaterWaves] = {{17.3f, 0.4f, 16.f, 3.f, 0.85f},
                                        {-33.1f, 0.22f, 9.32f, 2.3f, 0.8f},
                                        {63.8f, 0.1f, 4.5f, 1.7f, 0.7f},
                                        {-71.2f, 0.04f, 2.3f, 1.1f, 0.6f},
                                        {8.6f, 0.25f, 47.f, 5.f, 0.25f}};
  // wave set of the levels kWaterBodies maps to the lake (presets/water.toml [wave_sets.lake])
  WaterWave water_lake_waves[kWaterWaves] = {{20.f, 0.07f, 6.f, 1.6f, 0.45f},
                                             {-30.f, 0.04f, 3.5f, 1.2f, 0.4f},
                                             {70.f, 0.02f, 1.9f, 0.9f, 0.35f},
                                             {-65.f, 0.01f, 1.1f, 0.6f, 0.3f},
                                             {0.f, 0.f, 1.f, 1.f, 0.f}};
  float water_sky_refl = 0.6f;
  float water_sky_haze_m = 400.f;
  float water_amp_noise = 0.35f;
  float water_phase_noise = 0.f;
  float water_noise_m = 180.f;
  float water_shallow_depth_m = 1.75f;
  float water_opaque_m = 12.f;
  float water_absorb_rgb[3] = {1.6f, 0.9f, 0.5f};
  float water_shore_fade_near_m = 40.f;
  float water_shore_fade_far_m = 160.f;
  float water_shoal_m = 2.5f;
  float water_tile_hex = 1.f;
  float water_tile_hex_m = 56.f;
  float water_tile_hex_rot_deg = 180.f;
  float water_tile_warp_m = 10.f;
  float water_tile_warp_cell_m = 180.f;
  float water_aa_lod_start = -1.5f;
  float water_aa_lod_end = 0.5f;
  float water_aa_foam_edge_px = 1.f;
  float water_aa_wave_px_full = 8.f;
  float water_aa_wave_px_zero = 4.f;
  float water_clock_s = -1.f;
  float water_shoal_occ = -4.f;
  float water_depth_blur_m = 1.5f;
  float water_tess_seg_m = 0.75f;
  // analytic sun glint (water_sun_glint.glsl) on the ocean and inland water; off at boot, the
  // settings page's glint row turns it on
  bool water_sun_glint = false;
  float water_sun_glint_strength = 6.f;
  float water_sun_glint_rough = 0.08f;
  float water_sun_glint_peak = 40.f;
  float water_lod_start_m = 280.f;
  float water_lod_end_m = 900.f;
  int water_lod_probe = 0;

  // swamp look (Jak 1, swamp only; water, wet and mist together). water and wet
  // need PBR on, mist needs SSAO on, so the boot state stays vanilla
  bool swamp_water = true;
  float swamp_water_dark = 0.5f;
  float swamp_water_refl = 1.f;
  float swamp_water_ripple = 1.f;
  float swamp_water_glint = 2.f;
  bool swamp_wet = true;
  float swamp_wet_dark = 0.5f;
  float swamp_wet_coat = 1.f;
  float swamp_wet_puddle = 0.5f;
  bool swamp_mist = true;
  float swamp_mist_density = 0.05f;
  float swamp_mist_height_m = 2.5f;
  float swamp_mist_start_m = 3.f;
  float swamp_mist_max = 0.8f;
  float swamp_mist_sun = 1.f;
  float swamp_mist_bright = 0.35f;
  // wet shoreline (Jak 1, sea levels but the swamp); needs PBR on, so the boot state stays vanilla
  bool shore_wet = true;
  float shore_wet_band_m = 1.f;
  float shore_wet_dark = 0.72f;
  float shore_wet_film = 0.5f;
  float shore_wet_cell_m = 2.f;
  // Misty Island sea mist (misty.toml `moderate`, the chosen default): the swamp mist pass, a low
  // layer over the sea (Jak 1); needs SSAO on, so the boot state stays vanilla
  bool misty_mist = true;
  float misty_mist_density = 0.03f;
  float misty_mist_height_m = 6.f;
  float misty_mist_start_m = 10.f;
  float misty_mist_max = 0.7f;
  float misty_mist_bright = 0.4f;
  float misty_mist_strength = 1.f;
  // wet gloss: wetness as a water film in the PBR inputs (wetpbr.toml `l2`, the chosen default);
  // shoreline and swamp wet surfaces, needs PBR on, so the boot state stays vanilla
  bool wet_pbr = true;
  float wet_pbr_gloss = 0.8f;
  float wet_pbr_rough = 0.2f;
  float wet_pbr_f0 = 0.1f;
  float wet_pbr_dark = 0.55f;
  float wet_pbr_splash_full_m = 0.7f;
  float wet_pbr_splash_top_m = 1.3f;

  // sunken caustics (caustics.toml `web`): needs PBR on, so the boot state stays vanilla
  int caustic_mode = 1;
  float caustic_cell_scale = 0.25f;
  float caustic_loop_hz = 0.5f;
  float caustic_drift_mps = 0.067f;
  float caustic_amp = 1.411f;
  float caustic_web = 0.9f;
  float caustic_web_mean = 0.5315f;
  float caustic_clock_hold_s = -1.f;  // >= 0: the pattern clock is held there; < 0 runs live

  // fur shells on merc draws whose texture has the `fur` tag (Jak 1 only), off by default;
  // the values are the `medium` preset
  bool fur_enable = false;
  int fur_shells = 16;
  float fur_length_m = 0.02125f;
  float fur_density = 64.f;
  float fur_gravity = 0.3f;
  float fur_curl = 0.1f;
  float fur_root_dark = 0.35f;
  int fur_view = 0;
  // coarse fur UV (large actors) gets proportionally longer shells; 0 = same length on all
  float fur_size_exp = 1.75f;
  float fur_size_ref_m = 0.3f;

  // lava rules (Jak 1, LavaField.h): the lava class's own glow, and the lava field lighting the
  // background from below; defaults are the `strong` preset (presets/lava.toml), the chosen look;
  // they ride on PBR, so the boot state stays vanilla
  bool lava_glow = true;
  float lava_glow_gain = 2.5f;
  float lava_glow_mask = 1.f;
  float lava_glow_key_lo = 0.3f;
  float lava_glow_key_hi = 0.55f;
  float lava_glow_core = 1.6f;
  float lava_glow_crust = 0.3f;
  bool lava_under = true;
  float lava_under_gain = 4.5f;
  float lava_under_reach_m = 22.f;
  float lava_under_refl = 1.5f;
  float lava_under_bake = 0.25f;
  float lava_under_char = 0.5f;
  int lava_view = 0;
  bool lava_pin = false;

  // inland water (water-anim actors, InlandWater.h): needs PBR on, the depth part SSAO on, so
  // the boot state stays vanilla
  bool inland_water = true;
  float inland_water_depth_m = 1.5f;
  float inland_water_refl = 1.f;
  float inland_water_refl_cap = 0.5f;
  float inland_water_glint = 1.f;
  bool inland_mud = true;
  float inland_mud_dark = 0.8f;
  float inland_mud_refl_cap = 0.15f;
  float inland_mud_rough = 0.35f;
  float inland_mud_glint = 0.3f;
  int inland_water_view = 0;
  bool inland_water_picked = false;

  // actor shadow maps (Jak 1 only); vanilla keeps the stencil shadow and changes nothing
  int shadow_mode = (int)ShadowMode::OFF;
  float shadow_strength = 1.f;
  int shadow_view = (int)ShadowView::NONE;
  float shadow_range_m = 40.f;
  float shadow_lamp_scale = 1.f;
  float shadow_marker = 0.3f;  // landing disc strength, 0 removes it
  float shadow_reach_m = 8.f;
  int shadow_lights = 4;
  float shadow_light_r0_m = 5.f;
  float shadow_light_reach_m = 16.f;
  float shadow_cave_dark = 0.2f;
  float shadow_terrain_sun = 1.f;  // TERRAIN_SUN
  // world sun map (WorldShadows.h), options 32-38; acts only while the actor maps are on
  bool shadow_world = true;
  bool shadow_world_actors = true;
  bool shadow_world_water = true;
  int shadow_world_size = 4096;  // size and cascades follow QUALITY (kWorldShadowTier)
  int shadow_world_cascades = 3;
  float shadow_world_penumbra = 0.012f;
  float shadow_world_range_m = 120.f;
  int shadow_quality = 2;          // QUALITY: map size and PCF taps
  // sky-hued shadows: the `moderate` preset of presets/sky-hue.toml (the chosen default)
  bool shadow_sky_hue = true;
  float shadow_sky_hue_strength = 0.5f;
  float shadow_canopy = 1.f;  // canopy shade (background/Canopy.h), 0 = off
  float shadow_canopy_ambient = 0.45f;  // darkening of the shade under full cover, 0..0.8
  float shadow_canopy_patch = 0.2f;     // sun patches: warm brightening over today, 0..0.5
  float shadow_canopy_sharp = 0.4f;     // sun patch edge: 0 soft, 1 hard
  float shadow_canopy_penumbra = 0.03f;
  float shadow_canopy_tint = 0.5f;      // cool shift of the sky light kept under the canopy
  int shadow_contact = 0;       // CONTACT (ContactShadows.h): 0 off, 1 moderate, 2 strong
  int shadow_contact_view = 0;  // CONTACT_VIEW
  int shadow_probe = 0;         // PROBE (ShadowProbe bits)
  // stylized material shading by class (Jak 1 only), off by default; pc-set-pbr-param
  bool pbr_enable = false;
  float pbr_strength = 1.f;
  float pbr_detail = 1.f;
  float pbr_spec = 1.f;
  float pbr_key = 0.5f;  // key-light highlight where the sidecar has no sun share
  int pbr_view = (int)PbrView::NONE;
  bool pbr_merc = true;
  bool pbr_background = true;
  float pbr_merc_vsign = -1.f;
  int pbr_source = 1;  // PbrSource
  bool pbr_timing = false;
  int pbr_look = (int)PbrLook::FAITHFUL;
  float pbr_metallic = 0.3f;
  float pbr_env = 0.2f;
  float pbr_fresnel = 0.5f;
  float pbr_ambient = 0.15f;
  float pbr_rough_contrast = 1.f;
  float pbr_band = 1.f;
  float pbr_lobe = 0.5f;
  float pbr_metal_darken = 0.f;
  float pbr_merc_up = 1.f;
  float pbr_specular = 1.f;  // dielectric F0 as a multiple of 0.04
  float pbr_metal_hue = 1.f;  // PbrParam::METAL_HUE, 0..1
  float pbr_transmit = 0.f;   // PbrParam::TRANSMIT
  float pbr_wrap = 0.f;       // PbrParam::WRAP
  bool pbr_wind_tie = false;  // PbrParam::WIND_TIE
  float pbr_bake_wrap = 0.f;  // PbrParam::BAKE_WRAP
  bool pbr_transmit_occl = false;     // PbrParam::SUN_OCCLUSION
  float pbr_transmit_density = 2.5f;  // PbrParam::LEAF_DENSITY
  float pbr_ice = 0.f;            // PbrParam::ICE; preset [global] ice
  float pbr_ice_depth_m = 0.15f;  // preset [ice]: depth of the inner layer below the surface
  float pbr_ice_absorb_m = 0.15f;  // path over which the ice colour is applied once
  float pbr_ice_rough = 0.18f;    // surface coat roughness
  float pbr_ice_transmit = 1.f;   // sun through thin ice, x sun colour at strength 1
  float pbr_glint = 0.f;      // PbrParam::GLINT
  int pbr_probe = 0;          // PbrParam::PROBE (debug, cost attribution)
  // glint cells: size (m), share holding a crystal, facet spread and lobe width (rad)
  float pbr_glint_cell = 0.03f;
  float pbr_glint_density = 1.f;
  float pbr_glint_spread = 0.35f;
  float pbr_glint_width = 0.12f;
  // parallax offset on masonry textures with a derived height (preset [parallax])
  bool pbr_parallax = true;
  float pbr_px_depth = 0.05f;  // metres, clamped to kPbrParallaxMaxDepth
  int pbr_px_taps = 3;
  float pbr_px_grazing[2] = {0.15f, 0.45f};  // n.v over which the offset fades in
  float pbr_px_fade[2] = {20.f, 40.f};  // fade-out distance, metres, at pbr_px_fade_ref
  float pbr_px_fade_ref = 2.f;          // metres per texture repeat the fade distances are for
  bool pbr_px_basis = true;  // per-vertex basis (Unbake.h unbake_px_texture) where a tree has one
  int pbr_px_shadow = 2;     // ShadowOption::PARALLAX_SHADOW: 0 off, 1 moderate, 2 strong (default)

  // stereo debug (Jak 1 only): render each frame twice, side by side, off by default
  bool stereo_enable = false;
  float stereo_ipd_m = 0.064f;
  float stereo_world_scale = 1.f;  // < 1 shrinks the world (diorama)
  bool stereo_swap_eyes = false;   // right eye on the left half, for cross-eyed viewing
  // OpenXR (Jak 1 only): submit the stereo eyes to the active runtime, off by default
  bool xr_enable = false;
  // seated head tracking: head translation into the game camera (x this, clamped to 0.5 m)
  float xr_pos_scale = 1.f;
  int xr_recentre_requests = 0;  // bumped to recentre (pc-set-xr-recentre, F6, debug button)
  // scripted XR head (xr::set_fake): yaw, pitch, roll (deg), x, y, z (m), OpenXR axes
  bool xr_fake = false;
  float xr_fake_pose[6] = {0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
  // > 0: the fake yaw sweeps back and forth over +/- the set yaw at this rate (deg/s)
  float xr_fake_sweep_dps = 0.f;
  // > 0: the sweep steps once per display frame instead of following the clock; a new value
  // restarts it at -yaw (pc-set-xr-fake-option 7)
  int xr_fake_sweep_frames = 0;
  // XR frame sequence (pc-set-xr-fake-option 8 N): the next N drawn display frames' eye images
  int xr_seq_frames = 0;
  int xr_seq_serial = 0;

  // vista debug mode (Jak 1 only): draw a far level's background from the current camera
  bool vista_enable = false;
  char vista_level[32] = "village2";
  // comma-separated tfrag texture / tie proto names hidden in the main levels: a token matches
  // as a substring, or exactly (full name or the part after the last '/') when prefixed by '='
  char vista_hide[256] = "v1-lores-vil2,vil1-medres-wood";
  bool vista_hide_standins = true;
  int vista_fog_mode = 0;  // VistaFogMode
  float vista_fog_end_m = 6000.f;
  // unified fog: one exponential distance/height model for all background draws
  float vista_atmo_start_m = 64.f;
  float vista_atmo_density_per_km = 0.35f;
  float vista_atmo_height_m = 300.f;  // scale height of the density falloff; 0: no falloff
  bool vista_fog_floor = true;  // unified fog is never thinner than the game's own fog
  // neighbour-mode haze: extra fog toward a tinted fog colour past a start distance; 0: off
  float vista_haze_density_per_km = 0.f;
  float vista_haze_start_m = 200.f;
  float vista_haze_max = 1.f;  // 0-1 cap on the haze amount
  float vista_haze_tint[3] = {1.f, 1.f, 1.f};  // multiplies the viewer's fog colour
  bool vista_draw_shrub = true;
  bool vista_gpu_timing = false;
  // vista draws wholly behind the depth drawn before them are left out (VistaHiZ); option 15
  bool vista_hiz = true;
  int vista_probe = 0;  // dev probes (pc-set-vista-option 14): 1 scissor the draws to 1 px
  // frames per PERF-* log window (PerfTimers.h), 0 = off, negative = hitch log over -N ms;
  // pc-set-perf-timing
  int perf_timing_window = 0;
  // PerfOptArea bits, PERF_OPT_DEFAULT at boot; pc-set-perf-opt, read with Gfx::perf_opt(bit)
  u32 perf_opt_mask = PERF_OPT_DEFAULT;
  // neighbour fidelity: vista_level "neighbours" keeps every adjacent level resident and draws it
  bool vista_neighbours = false;
  bool vista_own_mood = true;       // neighbours use their own palette weights for the hour
  u32 vista_tod_addr = 0;           // EE address of *time-of-day-context* time (hours), 0: unset
  float vista_hour_override = -1.f;  // >= 0: hour for the neighbours' moods
  bool vista_draw_alpha = true;     // trans, dirt, ice and water tfrag trees of the neighbours
  bool vista_hide_medres = true;    // hide med-res-level actors of drawn neighbours
  bool vista_hide_neighbour_lowres = true;  // hide "lores"/"medres" draws inside the neighbours
  // display-only landmark actors of game-loaded, undisplayed neighbours (GOAL *vista-landmark-types*)
  bool vista_landmarks = true;
  // per-neighbour unified-fog overrides (pc-set-vista-neighbour-fog)
  std::array<VistaNeighbourFog, 8> vista_nb_fog{};
  int vista_nb_fog_count = 0;
  // also keep resident the levels the viewer's stand-ins depict (Neighbours.cpp kStandins)
  bool vista_standin_neighbours = false;
  // Sandover's render path (vanilla rendering off); false keeps upstream's. pc-set-sandover-path
  bool sandover_path = false;
  // antialiasing (AaMode); pc-set-aa
  int aa_mode = 0;
  // temporal AA only: the scene renders at this fraction of the display size and FSR 3.1
  // reconstructs it before the text buckets; 1 = native. pc-set-aa 1
  float render_scale = 1.f;
  // FSR 3.1 temporal upscaler (AA_FSR3, Fsr3Upscaler.h); pc-set-aa 2-5
  bool fsr3_debug = false;       // the SDK's debug view in place of the output
  float fsr3_sharpness = 0.8f;   // RCAS, 0..1 (1 sharpest), the SDK sample's default
  int fsr3_jitter = 1;  // projection jitter: 1 on; diagnostics: 0 off, 2 and 3 with y or x negated
  // NVIDIA DLSS (AA_DLSS, DlssUpscaler.h); pc-set-aa 6
  int dlss_jitter_sign = 0;      // diagnostic: bit 0 negates the jitter x passed to DLSS, bit 1 y
  // AMD FSR 4 (AA_FSR4, Fsr4Upscaler.h); pc-set-aa 7
  int fsr4_jitter_sign = 0;      // diagnostic: bit 0 negates the jitter x passed to FSR 4, bit 1 y
  // motion vectors (MotionVectors.h), infrastructure for temporal passes; pc-set-motion-vectors
  bool mv_enable = false;
  int mv_view = 0;              // motion_vectors::View
  float mv_view_scale = 16.f;   // velocity view: px per frame at full brightness
  float mv_error_gain = 4.f;    // reprojection error views
  float mv_cut_move_m = 2.f;    // camera moves further in one frame: a cut
  float mv_cut_turn_deg = 20.f;  // camera turns further in one frame: a cut
  float mv_object_w_tol = 0.01f;  // object velocity kept while its clip w matches the depth's
  bool mv_log_cuts = false;
  // per-object motion blur (MotionBlur.h); turns the motion vectors on while not off
  int mb_level = 0;  // 0 off, 1 weak, 2 medium, 3 strong (pc-set-motion-blur 0)
  int mb_camera = 0;  // camera motion blur: 0 off, 1 while riding a vehicle, 2 always (10)
  float mb_shutter[3] = {0.25f, 0.5f, 1.f};  // exposure, fraction of the frame interval
  float mb_radius_px = 80.f;  // blur radius cap at 2160 render lines (scales with height)
  int mb_view = 0;            // 1 object velocity, 2 neighbour max tiles
  bool mb_burst_cycle = false;  // screenshot burst frame N renders level N % 4 (comparisons)
};

/// Antialiasing modes (pc-set-aa 0).
enum AaMode : int {
  AA_MSAA = 0,  // upstream's path: the msaa setting's sample count
  AA_FSR3 = 1,  // scene at 1 sample, FSR 3.1 temporal AA (MSAA where unsupported, stereo, XR)
  AA_DLSS = 2,  // scene at 1 sample, DLSS super resolution (FSR 3.1 where unavailable)
  AA_FSR4 = 3,  // scene at 1 sample, FSR 4 on the D3D12 device (FSR 3.1 where unavailable)
};

/// Vista fog policies. OWN and MAIN touch only the vista draws; PUSH_OUT and UNIFIED
/// change every background draw of the frame while the vista is enabled.
enum VistaFogMode : int {
  VISTA_FOG_OWN = 0,       // vista gets its own linear fog ending at vista_fog_end_m
  VISTA_FOG_MAIN = 1,      // vista uses the viewer level's fog unchanged
  VISTA_FOG_PUSH_OUT = 2,  // viewer level's fog end moved to vista_fog_end_m, vista shares it
  VISTA_FOG_UNIFIED = 3,   // vista_atmo_* model replaces the fog distances of all background
};

namespace Gfx {

extern GfxGlobalSettings g_global_settings;

/// Optimization `bit` of pc-set-perf-opt is on (PerfOptArea + 0..3).
inline bool perf_opt(int bit) {
  return (g_global_settings.perf_opt_mask >> bit) & 1u;
}

/// The context runs the FSR 3.1 compute passes (GL 4.3 and the vendored headers); set by the
/// renderer once it has tried to build them.
extern std::atomic<bool> g_fsr3_supported;

/// DLSS can run: an NVIDIA adapter behind the GL context, WGL_NV_DX_interop2, the runtime and
/// the NGX feature; set at startup, cleared if the device or the feature later fails.
extern std::atomic<bool> g_dlss_supported;

/// FSR 4 can run: the D3D12 device on the GL context's adapter (D3d12Bridge.h), the FSR 4
/// runtime and a provider of version 4 or later; set at startup, cleared if it later fails.
extern std::atomic<bool> g_fsr4_supported;

/// An OpenXR session runs (the game's VR settings profile) and the size each eye is drawn at before
/// the render scale (0 until a frame was rendered); written by the render thread, read by
/// pc-get-xr-state.
extern std::atomic<bool> g_xr_session_running;
extern std::atomic<int> g_xr_eye_w;
extern std::atomic<int> g_xr_eye_h;

/// The drawn XR view for the game's culling (pc-get-xr-cull): the head's rotation from the game
/// camera's axes (+x left, +y up, +z forward), its offset in game units and the drawn frustum's
/// half-tangents widened by a head-motion margin. Written by the render thread each XR frame.
struct XrCullView {
  bool valid = false;
  float rot[3][3] = {{1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}};
  float t[3] = {0.f, 0.f, 0.f};
  float tan_x = 1.f, tan_y = 1.f;
  // the game's own projection half-tangents (x-ratio, y-ratio) before the widening, 0 unknown
  float game_tan_x = 0.f, game_tan_y = 0.f;
  double stamp_s = 0.;  // when the head pose was located (steady clock)
  double handed_s = 0., sent_s = 0.;  // the game took it; its chain was sent (0 unknown)
};
void set_xr_cull_view(const XrCullView& v);
XrCullView get_xr_cull_view();
/// The view pc-get-xr-cull last gave the game (its render camera); taken once by the next chain.
void note_xr_cull_handed(const XrCullView& v);
/// The game's own (unwidened) half-tangents for the view it was handed (pc-set-xr-game-tan).
void note_xr_game_tan(float tan_x, float tan_y);
XrCullView take_xr_cull_handed();

/// FSR 3.1 runs: it is the antialiasing mode, or DLSS or FSR 4 is and is unavailable, and the
/// context supports it (in stereo and XR one context per eye, also in place of DLSS).
inline bool fsr3_on() {
  const int m = g_global_settings.aa_mode;
  return (m == AA_FSR3 || (m == AA_DLSS && !g_dlss_supported) ||
          (m == AA_FSR4 && !g_fsr4_supported)) &&
         g_fsr3_supported;
}

/// FSR 4 is the antialiasing mode and available (in stereo and XR one context per eye).
inline bool fsr4_on() {
  return g_global_settings.aa_mode == AA_FSR4 && g_fsr4_supported;
}

/// DLSS is the antialiasing mode and available (stereo and XR run FSR 3.1 per eye instead).
inline bool dlss_on() {
  return g_global_settings.aa_mode == AA_DLSS && g_dlss_supported;
}

/// The game clock's rate while an XR session paces the display (GLDisplay::xr_game_tick).
constexpr int kXrGameHz = 60;

/// kXrGameHz while an XR session paces the game's vsync, else 0 (pc-get-xr-game-rate).
extern std::atomic<int> g_xr_game_hz;

/// The rate the game clock steps at: the XR tick in a session, else the frame rate setting.
inline float game_fps() {
  const int xr = g_xr_game_hz;
  return xr > 0 ? (float)xr : g_global_settings.target_fps;
}

/// Stereo or XR is requested (the renderer's stereo_active, Jak 1 only).
inline bool stereo_requested() {
  return g_global_settings.stereo_enable || g_global_settings.xr_enable;
}

/// Built-in SSAO presets, generated from sandover/presets/ssao.toml.
int SsaoPresetCount();
const char* SsaoPresetName(int index);
/// Load preset `index` into the settings and turn SSAO on. False if out of range.
bool SsaoApplyPreset(int index);
extern game_settings::DebugSettings g_debug_settings;

const GfxRendererModule* GetCurrentRenderer();

u32 Init(GameVersion version);
void Loop(std::function<bool()> f);
u32 Exit();

u32 vsync();
void register_vsync_callback(std::function<void()> f);
void clear_vsync_callback();
u32 sync_path();

// matching enum in kernel-defs.gc !!
enum class RendererTreeType { NONE = 0, TFRAG3 = 1, TIE3 = 2, INVALID };
bool CollisionRendererGetMask(GfxGlobalSettings::CollisionRendererMode mode, s64 mask_id);
void CollisionRendererSetMask(GfxGlobalSettings::CollisionRendererMode mode, s64 mask_id);
void CollisionRendererClearMask(GfxGlobalSettings::CollisionRendererMode mode, s64 mask_id);
void CollisionRendererSetMode(GfxGlobalSettings::CollisionRendererMode mode);

struct SplashScreen {
  std::vector<u8> data;
  int width = 0;
  int height = 0;
  std::atomic<bool> ready{false};
};
extern SplashScreen g_splash;

/// HDR presenter state for the current frame, written by the display on the render thread.
struct HdrPresentState {
  bool active = false;  // the window presents through the DXGI swapchain this frame
  bool hdr = false;     // with active: HDR output (else SDR through DXGI, perf-opt bit 24)
  u32 fbo = 0;          // draw target standing in for the window (HDR: linear scRGB, RGBA16F)
  float paper_white_nits = 200.f;
  float peak_nits = 800.f;
  float black_nits = 0.f;
  float saturation = 1.f;
  float knee = 0.f;
  // what the display reports (MHC2 profile, else DXGI), ignoring overrides; < 0 until queried
  float display_peak_nits = -1.f;
  float display_black_nits = -1.f;
  float desktop_white_nits = -1.f;  // Windows "SDR content brightness"; < 0 unknown
};
extern HdrPresentState g_hdr_present;

/// The window's draw framebuffer: the HDR presenter's target while it runs, else 0.
inline u32 window_fbo() {
  return g_hdr_present.active ? g_hdr_present.fbo : 0;
}

}  // namespace Gfx
