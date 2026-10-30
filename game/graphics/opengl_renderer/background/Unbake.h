#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"
#include "common/custom_data/Tfrag3Data.h"
#include "common/math/Vector.h"
#include "common/util/FileUtil.h"

#include "third-party/glad/include/glad/glad.h"

/*!
 * Per-vertex lighting sidecar (`<level>.unbake`, Jak 1), written by `sandover-unbake
 * export-sidecar` from the fits. One RGBA32UI texel per engine vertex of each tfrag and tie
 * tree (geometry LOD 0), in the order the loader uploads `unpacked.vertices`:
 * x = a | k << 16 (u16, value * 16384), y = octahedral normal (2 x u16 unorm),
 * z = 4-bit visibility per palette slot,
 * w = confidence q (u8) | flags (bits 8-9) | emissive E (u8, bits 16-23, version 2)
 *     | E source (bits 24-27: palette slot 0-7, 8 static, 9 night slots).
 * Units are palette u8 steps, 128 = 1.0.
 */
struct UnbakeSlot {
  math::Vector3f dir_fit;    // fitted sun direction (toward the light)
  math::Vector3f lgt;        // fitted sun colour
  math::Vector3f amb;        // fitted ambient colour
  math::Vector3f dir_table;  // mood-lights table direction, for comparison
};

struct UnbakeTree {
  u32 vertex_count = 0;
  bool valid = false;
  std::vector<u32> records;  // 4 per vertex
  mutable GLuint gl_buffer = 0;
  mutable GLuint gl_texture = 0;
  std::vector<u32> px_basis;  // 4 per vertex, see unbake_px_texture
  mutable GLuint px_buffer = 0;
  mutable GLuint px_texture = 0;
};

struct UnbakeSidecar {
  u32 slot_mask = 0;     // palette slots with a fitted sun
  u32 day_mask = 0;      // subset that the moving sun replaces, never a night slot
  u32 night_mask = 0;    // slots that weight E's night source
  u32 emitter_mask = 0;  // effect slots that are light sources (not lightning/caustics/fades)
  bool has_emissive = false;
  UnbakeSlot slots[8];
  std::vector<UnbakeTree> tfrag;  // indexed by tfrag_trees[0] tree index
  std::vector<UnbakeTree> tie;    // indexed by tie_trees[0] tree index
  // baked glow per palette entry of each tree palette (PaletteGlow.h, loader thread)
  std::unordered_map<const tfrag3::PackedTimeOfDay*, std::vector<u8>> glow;
};

/*!
 * Order-sensitive digest of a tree's float32 positions, matching `pos_digest` in
 * sandover/tools/sandover/unbake/sidecar.py.
 */
u64 unbake_pos_digest(const float* xyz, size_t vertex_count, size_t stride_floats);

/*!
 * Where `<level>.unbake` is read from: the first that exists of `fr3_dir`,
 * `$SANDOVER_DATA/lighting` and `<project>/sandover/data/lighting` (where the package ships
 * them, so a launcher install needs no copy step). Empty if none has it.
 */
fs::path unbake_path(const fs::path& fr3_dir, const std::string& level_name);

/// unbake_path with the environment and project dir passed in.
fs::path unbake_find(const fs::path& fr3_dir,
                     const std::string& level_name,
                     const char* sandover_data,
                     const fs::path& project_dir);

/*!
 * Read and check `path` against the unpacked level, logging which file loaded. Trees whose vertex
 * count or position digest differ are marked invalid. Returns null (with one log line) if the
 * path is empty, the file is absent or unusable.
 */
std::unique_ptr<UnbakeSidecar> unbake_load(const fs::path& path,
                                           const tfrag3::Level& level,
                                           const std::string& level_name);

/*!
 * Buffer texture for a tree, created on first use (render thread). 0 if the tree is invalid.
 *
 * SSAO hook: record.y holds a smooth normal (area-weighted, summed over position-welded
 * vertices, world space) for every statically drawn tfrag/tie vertex; record.w bit 8 marks it
 * defined, bit 9 marks an orientation taken from winding rather than the fit. A pass that
 * wants it binds this texture and fetches `texelFetch(tex, gl_VertexID)` in a vertex shader
 * drawn with the tree's own VAO and index buffer (as tfrag3_sun.vert does), decodes it with
 * `oct_decode` and writes it to its normal target; both signs of a winding-oriented normal
 * should be treated as equivalent (e.g. flip toward the camera).
 */
GLuint unbake_texture(const UnbakeTree& tree);

/*!
 * Parallax basis of a tree, built at load from its mesh (not from the sidecar file), one
 * RGBA32UI texel per vertex like the records: x = octahedral normal (2 x u16 unorm, as
 * record.y), y..w = the world gradients of u and v in UV per metre as six halves (gu.xy,
 * gu.z gv.x, gv.yz). The normal is area-weighted over the faces around the vertex's
 * position that lie within 60 degrees of the vertex's own faces, orientation arbitrary (flip
 * toward the camera); the gradients are area-weighted over the same faces where the texture
 * coordinates agree modulo 1. All zero where undefined. Interpolated, both are continuous
 * across triangle edges, unlike screen-derivative frames. 0 if the tree has none.
 */
GLuint unbake_px_texture(const UnbakeTree& tree);

/// Bind the tree's records to unit 11 and switch the active TFRAG3 / TFRAG3_SUN / ETIE program
/// `program` to its smooth normals for the SSAO normal target.
void unbake_bind_normals(GLuint program, const UnbakeTree& tree);

/// GL objects owned by the sidecar, for the loader's deferred delete lists.
void unbake_release(const UnbakeSidecar& sidecar,
                    std::vector<GLuint>& buffers,
                    std::vector<GLuint>& textures);

struct GoalBackgroundCameraData;
struct SharedRenderState;

/*!
 * Sun uniforms for the TFRAG3_SUN program, for one level and frame: palette weights from the
 * port data's itimes, the fitted per-slot model and the per-slot target the sun controller
 * picks (Gfx::g_global_settings.sun_*). The shader must be active.
 */
void unbake_set_uniforms(SharedRenderState* render_state,
                         const UnbakeSidecar& sidecar,
                         const GoalBackgroundCameraData& camera,
                         GLuint texture);

/// Palette weight of each slot and channel (1 = full) from the frame's packed itimes.
void unbake_slot_weights(const math::Vector<s32, 4> itimes[4], float w[8][3]);

constexpr int kUnbakeEmissiveSources = 10;

/*!
 * Runtime weight of each E source for this frame: the palette weight of emitter slots (0 for
 * other slots), 1 for the static source, the summed night-slot weight for the night source.
 * Clamped to [0, 1].
 */
void unbake_emissive_weights(const UnbakeSidecar& sidecar,
                             const GoalBackgroundCameraData& camera,
                             float out[kUnbakeEmissiveSources]);

/// E source weights last noted for `sidecar` by unbake_set_emissive_uniforms (render thread,
/// level buckets only, never vista draws under another level's camera); false if none yet. Lets the
/// world lights follow the static emitters' time of day.
bool unbake_last_emissive_weights(const UnbakeSidecar* sidecar,
                                  float out[kUnbakeEmissiveSources]);

/*!
 * Emissive-mask uniforms (em_on, em_w, tex_T11) for a TFRAG3 or TFRAG3_SUN draw of one tree.
 * `tree` may be null (no data): E is then written as 0.
 */
void unbake_set_emissive_uniforms(GLuint program,
                                  const UnbakeSidecar* sidecar,
                                  const UnbakeTree* tree,
                                  const GoalBackgroundCameraData& camera,
                                  bool note = true);

/// Palette blend weights per slot and channel from the port data's itimes.
void unbake_slot_weights(const math::Vector<s32, 4> itimes[4], float w[8][3]);

/*!
 * Blend of the fitted suns for the current palette weights: the direction toward the light,
 * weighted by each slot's sun luminance, and the share of the sun in the fitted light for a
 * vertex with unit ambient and sun gain facing the sun. False if no fitted slot is live.
 */
bool unbake_sun_summary(const UnbakeSidecar& sidecar,
                        const math::Vector<s32, 4> itimes[4],
                        math::Vector3f* dir,
                        float* share);

/// True if the sun term should run for this level (Jak 1, sidecar present, moving sun or actor
/// shadows on).
bool unbake_active(const SharedRenderState* render_state, const UnbakeSidecar* sidecar);

/// Debug window contents for the "Sun" ImGui node.
void unbake_draw_debug_window();
