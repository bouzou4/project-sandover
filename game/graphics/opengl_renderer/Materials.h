#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "common/custom_data/Tfrag3Data.h"

#include "third-party/glad/include/glad/glad.h"

/*!
 * Stylized material shading by class (Jak 1), off by default.
 *
 * Every level texture gets a class index from `materials.csv` (keyed by `combo_id`), read once
 * per process from `$SANDOVER_MATERIALS`, else `$SANDOVER_DATA/materials/materials.csv`, else
 * `<project>/sandover/data/materials/materials.csv`. A missing file or an unknown `combo_id`
 * gives `special` (no shading). Background draws (tfrag, tie, shrub), merc and generic set the class
 * as a per-draw uniform `pbr_mat`. Detail comes from per-texture normal and roughness maps
 * (`sandover-materials derive-maps`) bound per draw on unit 17 (`PbrLevelMaps`), or, as the old
 * look, from a procedural 2D array with one detail normal per class on unit 16. With
 * `pbr_enable` off the shaders take the vanilla path unchanged.
 */
enum PbrClass : u8 {
  PBR_NONE = 0,
  PBR_STONE = 1,
  PBR_WOOD = 2,
  PBR_METAL = 3,
  PBR_FOLIAGE = 4,
  PBR_ORGANIC = 5,
  PBR_SPECIAL = 6,
  PBR_GLASS = 7,
  PBR_CLOTH = 8,
  PBR_SNOW = 9,
  PBR_LAVA = 10,
  PBR_PRECURSOR = 11,  // metal with the `precursor` tag
  PBR_ICE = 12,        // glossy surface over a darker, bluer interior (pbr_ice)
  PBR_CLASS_COUNT = 13,
};

/// Upper clamp on the parallax depth (preset `[parallax] depth_m`, pbr param 23), metres.
constexpr float kPbrParallaxMaxDepth = 0.08f;

/// Bit set on a texture's class byte when the row carries the `fur` tag (no shading effect yet).
constexpr u8 kPbrFurBit = 0x40;
/// Bit set when the row carries the `baked-tint` tag: a metal reflection keeps the bake's hue.
constexpr u8 kPbrBakedTintBit = 0x80;
/// Bit set on the swamp level's `swp-water` texture: the swamp water surface (Swamp.h).
constexpr u8 kPbrSwampWaterBit = 0x20;
/// The same bit outside the swamp: a sand texture (`SandTextures.inc`, from the collision pat).
constexpr u8 kPbrSandBit = 0x20;
/// Bit set when the row carries the `shaft` tag: a fake light shaft, not drawn with the class shading on.
constexpr u8 kPbrShaftBit = 0x10;
constexpr u8 kPbrClassMask = 0x0f;
static_assert(PBR_CLASS_COUNT <= 16, "the class byte keeps classes in its low 4 bits");

/// Per-class shading parameters at strength 1. Detail and specular scale with the global strength.
/// Defaults are overridden by `sandover/presets/pbr.toml` (see `pbr_load_preset`).
struct PbrClassParams {
  float normal;   // per-texture normal map strength; 0 = flat
  float detail;   // procedural detail slope scale; 0 = none
  float scale_m;  // world metres per detail tile (backgrounds)
  float uv_rep;   // detail repeats per texture UV unit (merc)
  float spec;     // highlight strength, added colour at full sun
  float gloss;    // Blinn exponent where a texture has no roughness map
  float tint[3];  // highlight colour
  float albedo;   // 0 = tint only, 1 = tint times the normalized texture colour
  float env = 1.f;         // mood-environment reflection scale
  float rough_bias = 0.f;  // added to the roughness after the look's contrast
  float lobe = 1.f;        // sun and key lobe scale
  float sheen = 0.f;       // grazing sheen in the albedo hue
  float transmit = 0.f;        // sun through thin leaves (albedo squared), seen from behind
  float transmit_power = 2.f;  // forward lobe power of the transmission
  float wrap = 0.f;            // wrapped N.L on the sun and key relief: soft terminator
  float bake_wrap = 0.f;       // wrapped N.L on the bake's own sun term (sidecar trees only)
  float glint = 0.f;       // sun glints (snow crystals): one glint's HDR peak x sun colour
};

extern PbrClassParams g_pbr_params[PBR_CLASS_COUNT];

const char* pbr_class_name(int cls);

/// Class byte for every texture of `level`, in `level.textures` order. Thread-safe.
std::vector<u8> pbr_classify_level(const tfrag3::Level& level, const std::string& level_name);

struct SharedRenderState;
struct GoalBackgroundCameraData;
struct UnbakeSidecar;
struct UnbakeTree;

/// True once the class shading has run this session; until then the off path makes no GL calls.
bool pbr_was_used();

/// True if the class shading runs this frame (Jak 1 and the toggle on).
bool pbr_active(const SharedRenderState* render_state);

/*!
 * Background program setup for one tree: toggle, class tables, the level's blended sun (from the
 * sidecar; no sidecar means detail and highlights are skipped), the detail array on unit 16 and,
 * when `tree` is valid, its sidecar records on unit 11. The program must be active.
 */
void pbr_setup_background(SharedRenderState* render_state,
                          GLuint program,
                          const UnbakeSidecar* sidecar,
                          const UnbakeTree* tree,
                          const GoalBackgroundCameraData& camera);

/// Merc program setup: toggle, class tables and the detail array. The program must be active.
void pbr_setup_merc(SharedRenderState* render_state, GLuint program);

/*!
 * Generic program setup (camera-space vertices): toggle, class tables, and the sun and key light
 * of the last background setup rotated into camera space. `on` false turns it off.
 */
void pbr_setup_generic(SharedRenderState* render_state, GLuint program, bool on);

/// True if the ocean shading runs this frame (Jak 1 and `pc-set-water` on).
bool water_active(const SharedRenderState* render_state);

/// Records the level's sun and the camera for the ocean when the ocean shading is on (the class
/// shading records them itself), and picks the frame's wave set from `level_name`: the last
/// drawn level with an entry in the body table wins, as update-ocean picks the ocean map.
/// Call from background draws; makes no GL calls.
void water_note_background(SharedRenderState* render_state,
                           const UnbakeSidecar* sidecar,
                           const GoalBackgroundCameraData& camera,
                           const std::string& level_name);

/// Records the frame lights exactly as a background draw of this sidecar and camera would
/// (the class shading or the ocean note); no-op when neither is on. Makes no GL calls.
void note_background_lights(SharedRenderState* render_state,
                            const UnbakeSidecar* sidecar,
                            const GoalBackgroundCameraData& camera);

/*!
 * Ocean program setup (`ocean_common`): toggle, camera-to-world rotation and GS inverse
 * projection for the view ray, the last recorded sun (world space) and the water presets.
 * Also the Gerstner waves, the world-to-GS matrix and its inverse for the vertex
 * displacement, the ocean plane height (`plane_y`, game units; `plane_ok` false makes the
 * shader use the depth-reconstructed position) and the scene depth under the water
 * (`render_state->water_depth_tex`, texture unit 7), the frame's wave set (sea or
 * lake), the wave noise and the mood environment the fresnel term reflects. Off sets only
 * `water_on` = 0. The program must be active.
 */
void water_setup_ocean(SharedRenderState* render_state, GLuint program, float plane_y = 0.f,
                       bool plane_ok = false);

/// Frame index and camera position (game units) of the last `water_cam` upload to `program`
/// (water_setup_ocean with camera data); false if it never had one.
bool water_cam_stamp(GLuint program, u64* frame, math::Vector3f* pos);

/// Inverse of the frame's world-to-GS matrix (`camera_matrix`, homogeneous, row-major: world =
/// inv * ((gs - hvdf) / fog, 1)). False without background camera data.
bool water_gs_inverse(const SharedRenderState* render_state, double inv[4][4]);

/// GS depths of the points one and two units of view depth in front of the frame camera, with
/// unit = 64 m: every pixel ray's points at those depths are exact, well-conditioned samples of
/// the ray under the inverse of the frame camera's matrix. False without camera data.
bool water_ray_depths(const SharedRenderState* render_state, double* z1, double* z2);

/// Rest height of the ocean this frame (game units): the ocean map's start-corner y as draw-ocean
/// (ocean.gc) sets it from the camera position. The mid and far ocean both sit at this height.
float water_ocean_height(const SharedRenderState* render_state);

/// True if `level_name` takes the sea wave set in the body table (`kWaterBodies`).
bool water_level_has_sea(const std::string& level_name);

/// World-space direction toward the frame's blended day sun and its colour, as the last
/// background setup computed them; false when that level has no fitted sun.
bool pbr_frame_sun(math::Vector3f* dir, math::Vector3f* col);

/// Sun glint strength on water (pc-set-water 95) scaled by the day share `day`, so the glint
/// fades out with the day slots at dusk and is gone at night.
float water_sun_glint_strength(float day);

/// Day sun of the level with sidecar `sc` as its background last computed it (world, toward the
/// light), its colour and day share, if set up in `frame` or the one before; else the frame's
/// sun. A level drawn later in the frame (an enclosed neighbour such as jungleb) does not replace
/// it.
bool water_level_sun(const UnbakeSidecar* sc, u64 frame, math::Vector3f* dir, math::Vector3f* col,
                     float* day);

/// The level water_note_background picked for the ocean's wave set this frame (its sidecar).
const UnbakeSidecar* water_ocean_level();

/// Class of a texture by combo_id (tpage << 16 | index), `special` if unknown. Thread-safe.
u8 pbr_class_for_combo(u32 combo_id);
/// `pbr_mat` value for a texture by combo_id: class plus the `baked-tint` bit.
int pbr_mat_for_combo(u32 combo_id);

/// Class byte of texture `tex` (index into the level's textures), `special` if out of range.
inline u8 pbr_texture_class(const std::vector<u8>* classes, s32 tex) {
  if (!classes || tex < 0 || tex >= (s32)classes->size()) {
    return PBR_SPECIAL;
  }
  return (*classes)[tex] & kPbrClassMask;
}

/// `pbr_mat` value of texture `tex`: its class plus the `baked-tint` bit.
inline int pbr_texture_mat(const std::vector<u8>* classes, s32 tex) {
  if (!classes || tex < 0 || tex >= (s32)classes->size()) {
    return PBR_SPECIAL;
  }
  return (*classes)[tex] & (kPbrClassMask | kPbrBakedTintBit | kPbrSwampWaterBit);
}

/// Where the detail normal comes from (`pbr_source`).
enum class PbrSource { PROCEDURAL = 0, MAPS = 1 };

/*!
 * Per-texture normal/roughness maps of one level (or the common textures), created on the loader
 * thread with the level and decoded from `$SANDOVER_DATA/materials/pbr-maps/<tpage>/<name>` either
 * there (toggle on at load) or on first use. Each map is one RGBA8 texture: rgb the tangent-space
 * normal (glTF, +Y up the image), a the roughness. Textures without maps (flat, missing, not in
 * the index) use a shared 1x1 flat default. Owned by `LevelData`; GL objects are released with
 * `pbr_maps_release` on the render thread.
 */
struct PbrLevelMaps;

/// Maps holder for `level`'s textures; decodes right away when the toggle is on. Thread-safe.
std::shared_ptr<PbrLevelMaps> pbr_maps_create(const tfrag3::Level& level,
                                              const std::string& level_name);

/// Hands the level's map textures to `garbage` (deleted later by the loader) and forgets them.
void pbr_maps_release(PbrLevelMaps* maps, std::vector<GLuint>& garbage);

/// Per-draw material state for one tree or batch: class and maps, set only when they change.
class PbrDrawState {
 public:
  /// `program` must be active; `classes`/`maps` may be null (class `special`, flat maps).
  void begin(GLuint program, const std::vector<u8>* classes, PbrLevelMaps* maps);
  /// Class and maps of level texture `tex` (negative: anim slot or eye, flat default).
  void set(s32 tex);
  /// `pbr_mat` value (class and flag bits), maps texture and metallic given directly (generic).
  void set_direct(int mat, GLuint map, float metallic);

 private:
  GLint m_mat_loc = -1, m_draw_loc = -1;
  const std::vector<u8>* m_classes = nullptr;
  PbrLevelMaps* m_maps = nullptr;
  int m_last_mat = -1;
  s32 m_last_tex = INT32_MIN;
  GLuint m_last_map = UINT32_MAX;
};

/// Maps texture and metallic for a pool texture by combo_id (generic); flat default if none.
void pbr_maps_for_combo(u32 combo_id, GLuint* map, float* metallic);

/*!
 * Reads the class parameters and global defaults from `$SANDOVER_PBR_PRESET`, else
 * `<project>/sandover/presets/pbr.toml`, then the sections of the active look
 * (`[look.<name>]`, `[look.<name>.class.<class>]`; `pbr_look`, or `[global] look` at start-up).
 * Called once at renderer start, from the debug window and by `pc-set-pbr-param 11`; never
 * turns the toggle on.
 */
void pbr_load_preset();

/// True if texture `tex` carries the `shaft` tag (background draws skip it while the class shading runs).
inline bool pbr_texture_shaft(const std::vector<u8>* classes, s32 tex) {
  return classes && tex >= 0 && tex < (s32)classes->size() && ((*classes)[tex] & kPbrShaftBit);
}

/// True if texture `tex` (index into the level's textures) carries the `fur` tag.
inline bool pbr_texture_fur(const std::vector<u8>* classes, s32 tex) {
  return classes && tex >= 0 && tex < (s32)classes->size() && ((*classes)[tex] & kPbrFurBit);
}

void pbr_draw_debug_window();
