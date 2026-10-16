#pragma once

#include <array>
#include <memory>
#include <unordered_map>
#include <vector>

#include "common/math/Vector.h"

#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/opengl_renderer/GlQueries.h"
#include "game/graphics/opengl_renderer/Shader.h"

#include "common/custom_data/Tfrag3Data.h"

struct LevelData;
struct UnbakeSidecar;

/*!
 * World sun shadow map (Jak 1): the static world of the loaded levels (tfrag, tie including wind
 * instances, shrub; alpha-tested draws with their texture's alpha; translucent blended sheets
 * such as snow's ice overlay cast nothing) drawn depth-only from the sun
 * into up to three cascades of one depth array, each an orthographic box around the bounding
 * sphere of its slice of the view, snapped to whole texels. Drawn in the actor shadow pre-pass,
 * along the sun disc's arc (sun_disc_direction) by day and toward the moon (moon_disc_direction)
 * at night, faded by world_shadows_sun_strength; the actor sun map, roof cover and canopy take
 * the same light and fade (ActorShadows::prepass).
 *
 * Receivers (world_shadow_frag.glsl): tfrag, tie and shrub darken only the bake's direct-sun share
 * that the map takes away, max(share - V * share_lit, 0) (kb/findings/2026-10-08-world-sun-
 * shadows.md); merc removes its rig's sun by the map per pixel in place of the terrain cover;
 * water may read ws_water_vis. Options: ShadowOption::WORLD .. WORLD_RESET (32-39).
 */
class WorldShadows {
 public:
  WorldShadows();
  ~WorldShadows();
  WorldShadows(const WorldShadows&) = delete;
  WorldShadows& operator=(const WorldShadows&) = delete;

  /*!
   * Draw the cascades for this frame and fill rs->world_shadow (inactive when no receiver wants
   * the map). `c2w` / `w2c` map merc camera space to world and back (column-major), `cam_pos` /
   * `fwd` the camera in
   * world game units, `tan_xy` the half field of view tangents, `sun_dir` toward the shadow's
   * light (the sun, or the moon at night), `strength` its fade (0 draws nothing), `sc` / `itimes`
   * the sidecar and palette the ambient floor reads.
   */
  void prepass(SharedRenderState* rs,
               const float c2w[16],
               const float w2c[16],
               const math::Vector3f& cam_pos,
               const math::Vector3f& fwd,
               const float tan_xy[2],
               const math::Vector3f& sun_dir,
               float strength,
               const UnbakeSidecar* sc,
               const math::Vector<s32, 4>* itimes);
  void draw_debug_window();
  /// For the log: the palette-blend sun the arc replaced and the hour (-1: no arc).
  void note_sun(const math::Vector3f& palette_dir, float hour) {
    m_palette_dir = palette_dir;
    m_hour = hour;
  }

  static constexpr int kUnitMap = 49;  // sampler2DArrayShadow ws_map (ALLOCATIONS)
  static constexpr int kUnitRaw = 50;  // sampler2DArray ws_raw, the same depths

 private:
  struct Chunk {
    u32 first = 0, count = 0;  // indices
    math::Vector4f sphere;     // game units, w radius
  };
  struct CasterDraw {
    s32 tex = -1;           // level texture, -1 none (opaque)
    float aref = -1.f;  // discard below, < 0 no test
    int wrap = 0;       // the draw's clamp bits: 1 s, 2 t (as the colour pass samples it)
  };
  struct StaticTree {
    GLuint vao = 0;
    GLuint own_index_buffer = 0;  // 0: the loader's
    GLenum mode = GL_TRIANGLE_STRIP;
    const std::vector<tfrag3::StripDraw>* draws = nullptr;
    const tfrag3::BVH* bvh = nullptr;
    std::vector<int> draw_ids;     // casting draws
    std::vector<CasterDraw> cast;  // per draw id
    std::vector<u8> vis;
    std::vector<std::pair<int, int>> per_draw;  // multidraw range per draw
    std::vector<GLsizei> counts;
    std::vector<void*> offsets;
    bool bounded = false;  // every group is culled by a node, and `bound` holds every node
    math::Vector4f bound;  // game units, w radius
  };
  struct ShrubTree {
    GLuint vao = 0;
    GLuint index_buffer = 0;
    std::vector<std::vector<Chunk>> chunks;  // per draw
    std::vector<CasterDraw> cast;
  };
  struct WindGroup {
    u32 first = 0, count = 0, inst = 0;  // indices into wind_indices; instance matrix
    math::Vector4f sphere;
  };
  struct WindTree {
    GLuint vao = 0;
    const tfrag3::TieTree* src = nullptr;
    std::vector<u32> draw_offsets;
    std::vector<CasterDraw> cast;
    GLuint inst_buffer = 0;                      // instance matrices for the indirect path
    std::vector<std::vector<WindGroup>> groups;  // per draw, casting draws only
  };
  struct LevelCache {
    u64 load_id = 0;
    const LevelData* level = nullptr;
    std::vector<StaticTree> statics;
    std::vector<ShrubTree> shrubs;
    std::vector<WindTree> winds;
    int tris = 0;
  };
  LevelCache build(const LevelData* lev);
  void release(LevelCache& c);
  void ensure_target(int size);
  struct Locs {
    GLint clip = -1, inst = -1, inst_on = -1, uv = -1, aref = -1;
  };
  void draw_level(LevelCache& c,
                  const math::Vector4f planes[4],
                  const Locs& loc,
                  bool fast,
                  int* draws,
                  u64* tris);

  std::unordered_map<const LevelData*, LevelCache> m_levels;
  std::unique_ptr<Shader> m_shader;
  GLuint m_tex = 0;
  GLuint m_fbo[WorldShadowFrame::kMaxCascades] = {};
  int m_size = 0;
  GLuint m_sampler_cmp = 0, m_sampler_raw = 0;
  GLuint m_sampler_alpha[4] = {};  // by CasterDraw::wrap
  glq::SavedState m_state{"world-shadows", PERF_OPT_GEOMETRY + 0, 1u};

  // what each cascade holds; redrawn only when its clip or the caster set changes (kOptBit)
  struct Drawn {
    bool ok = false;
    u64 gen = 0;
    GLuint tex = 0;  // the map and its size the cascade was drawn into
    int size = 0;
    std::array<float, 16> clip{};
    int draws = 0;
    u64 tris = 0;
  };
  Drawn m_drawn[WorldShadowFrame::kMaxCascades];
  u64 m_gen = 1;  // bumped when a level cache comes or goes or the map is reallocated
  GLuint m_indirect = 0;
  struct IndirectCmd {
    u32 count, instance_count, first_index;
    s32 base_vertex;
    u32 base_instance;
  };
  std::vector<IndirectCmd> m_cmds;
  std::vector<std::pair<u32, u32>> m_cmd_ranges;  // per wind draw: first command, count

  GLuint m_query = 0;
  bool m_query_pending = false;
  int m_query_age = 0;
  u32 m_frame = 0;
  int m_redrawn_sum = 0;
  double m_gpu_ms_sum = 0, m_cpu_ms_sum = 0;
  int m_gpu_n = 0, m_cpu_n = 0;
  int m_last_draws = 0;
  u64 m_last_tris = 0;
  float m_amb_a = 0.f;
  const UnbakeSidecar* m_amb_sc = nullptr;
  int m_log = 0;
  math::Vector3f m_palette_dir{0.f, 1.f, 0.f};
  float m_hour = -1.f;
};

/*!
 * Set the receiver uniforms (ws_*) for `program`, which must be active, and bind the map to units
 * 49 and 50. camera_space selects the merc matrices. Inactive frames set ws_on = 0 (and keep a
 * dummy depth array bound so the samplers always see their type).
 */
void world_shadows_set_uniforms(const SharedRenderState* rs, GLuint program, bool camera_space);

/// Fade of a shadow light (world map, actor sun map, roof cover, canopy) for the palette weight of
/// its slots (day for the sun, night for the moon) and its height (y of the unit direction toward
/// it): 0 without its slots and below the horizon.
float world_shadows_sun_strength(float day, float sun_y);
