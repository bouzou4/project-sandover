#pragma once
#include <map>
#include <tuple>
#include <unordered_map>

#include "game/graphics/opengl_renderer/BucketRenderer.h"

struct MercDebugStats {
  int num_models = 0;
  int num_missing_models = 0;
  int num_chains = 0;
  int num_effects = 0;
  int num_predicted_draws = 0;
  int num_predicted_tris = 0;
  int num_bones_uploaded = 0;
  int num_lights = 0;
  int num_draw_flush = 0;

  int num_envmap_effects = 0;
  int num_envmap_tris = 0;

  int num_upload_bytes = 0;
  int num_uploads = 0;

  struct DrawDebug {
    DrawMode mode;
    int num_tris;
  };
  struct EffectDebug {
    bool envmap = false;
    DrawMode envmap_mode;
    std::vector<DrawDebug> draws;
  };
  struct ModelDebug {
    std::string name;
    std::string level;
    std::vector<EffectDebug> effects;
  };

  std::vector<ModelDebug> model_list;

  bool collect_debug_model_list = false;
};

class Merc2 {
 public:
  Merc2(ShaderLibrary& shaders, const std::vector<GLuint>* anim_slot_array);
  ~Merc2();
  void draw_debug_window(MercDebugStats* stats);
  void render(DmaFollower& dma,
              SharedRenderState* render_state,
              ScopedProfilerNode& prof,
              MercDebugStats* stats);
  /// The bucket the next render() draws (motion vectors keep a previous camera per bucket).
  void set_bucket(int id) { m_bucket = id; }
  static constexpr int kMaxBlerc = 40;

  /// One depth target of the actor shadow pre-pass (Jak 1).
  struct ShadowView {
    GLuint fbo = 0;
    int size = 0;
    math::Vector4f cam_to_clip[4];  // merc camera space to the view's clip space
    // lamp views: the light (merc camera space), w a radius; models rooted inside it house the
    // light and cast nothing into this view (w <= 0: no such test)
    math::Vector4f fixture = math::Vector4f(0.f, 0.f, 0.f, 0.f);
  };
  /*!
   * Draw one merc bucket's models into each view's depth target instead of the frame (actor
   * shadow pre-pass). Records each model's root position (camera space) and the first model's
   * lights for shadow_actor_positions() / shadow_first_lights().
   */
  void render_shadow(DmaFollower& dma,
                     SharedRenderState* render_state,
                     ScopedProfilerNode& prof,
                     const std::vector<ShadowView>& views);
  void clear_shadow_records();
  /// False for light emitters (is_light_emitter) and the beach windmills: they never cast.
  static bool casts_actor_shadow(const char* model_name);
  /// Orbs, power cells, hut lamps, cave and dark eco crystals: they give light, not receive it.
  static bool is_light_emitter(const char* model_name);
  const std::vector<math::Vector3f>& shadow_actor_positions() const { return m_shadow_actor_pos; }
  /// Camera-space direction toward light 0, its colour and the ambient of the first model drawn.
  bool shadow_first_lights(math::Vector3f* dir, math::Vector3f* color, math::Vector3f* amb) const;
  /// Root position (camera space) of Jak's model in the last shadow pre-pass.
  bool shadow_player_position(math::Vector3f* pos) const {
    *pos = m_shadow_player_pos;
    return m_have_shadow_player;
  }

 private:
  const std::vector<GLuint>* m_anim_slot_array;
  enum MercDataMemory {
    LOW_MEMORY = 0,
    BUFFER_BASE = 442,
    // this negative offset is what broke jak graphics in Dobiestation for a long time.
    BUFFER_OFFSET = -442
  };

  struct LowMemory {
    u8 tri_strip_tag[16];
    u8 ad_gif_tag[16];
    math::Vector4f hvdf_offset;
    math::Vector4f perspective[4];
    math::Vector4f fog;
  } m_low_memory;
  static_assert(sizeof(LowMemory) == 0x80);

  struct VuLights {
    math::Vector3f direction0;
    u32 w0;  // 12
    math::Vector3f direction1;
    u32 w1;  // 28
    math::Vector3f direction2;
    u32 w2;  // 44
    math::Vector4f color0;
    math::Vector4f color1;
    math::Vector4f color2;
    math::Vector4f ambient;
  };

  void handle_pc_model(const DmaTransfer& setup,
                       SharedRenderState* render_state,
                       ScopedProfilerNode& prof,
                       MercDebugStats* stats);
  u32 alloc_lights(const VuLights& lights);
  /// Share of lights 0 and 1 that is the time-of-day sun rather than a designed light kit, 0..1.
  static float sun_share_of_rig(const VuLights& lights);

  struct ModBuffers {
    GLuint vao, vertex;
  };

  static constexpr int kMaxEffect = 64;
  bool m_effect_debug_mask[kMaxEffect];

  struct MercMat {
    math::Vector4f tmat[4];
    math::Vector4f nmat[3];
  };

  struct ShaderMercMat {
    math::Vector4f tmat[4];
    math::Vector4f nmat[3];
    math::Vector4f pad;
    std::string to_string() const;
  };
  u32 alloc_bones(int count, ShaderMercMat* data, const ShaderMercMat* prev = nullptr);
  static constexpr int MAX_SKEL_BONES = 128;
  static constexpr int BONE_VECTORS_PER_BONE = 7;
  static constexpr int MAX_SHADER_BONE_VECTORS = 1024 * 32;  // ??

  static constexpr int MAX_LEVELS = 3;
  static constexpr int MAX_DRAWS_PER_LEVEL = 2048 * 2;
  static constexpr int MAX_ENVMAP_DRAWS_PER_LEVEL = MAX_DRAWS_PER_LEVEL;

  math::Vector4f m_shader_bone_vector_buffer[MAX_SHADER_BONE_VECTORS];
  // motion vectors: the previous frame's bones at the same indices (ub_prev_bones, binding 2)
  math::Vector4f m_prev_bone_vector_buffer[MAX_SHADER_BONE_VECTORS];
  GLuint m_prev_bones_buffer = 0;
  bool m_prev_bones_used = false;  // this flush allocated previous bones

  /// The bones (tmat) a model's draw-control sent in a frame, by skeleton slot.
  struct MvBones {
    u64 mask[2] = {0, 0};
    math::Vector4f tmat[MAX_SKEL_BONES][4];
  };
  // keyed by (process pid << 32 | draw-control address), sent by pc-merc-draw-request (Jak 1)
  std::unordered_map<u64, MvBones> m_mv_cur, m_mv_prev;
  u64 m_mv_frame = UINT64_MAX;
  /// A bucket's merc setup camera, this frame and the last (buckets can differ: HUD, props).
  struct MvCam {
    LowMemory cur, prev;
    u64 frame = UINT64_MAX;
    bool prev_ok = false;
  };
  std::unordered_map<int, MvCam> m_mv_cams;  // by bucket id * 2 + eye slot
  const MvCam* m_mv_cam = nullptr;          // the bucket being drawn
  int m_bucket = -1;
  void mv_roll(u64 frame);

  struct Uniforms {
    GLuint program = 0;
    GLuint light_direction[3];
    GLuint light_color[3];
    GLuint light_ambient;

    GLuint hvdf_offset;
    GLuint fog;

    GLuint tbone;
    GLuint nbone;

    GLuint fog_color;
    GLuint perspective_matrix;

    GLuint ignore_alpha;
    GLuint decal;

    GLuint gfx_hack_no_tex;

    GLuint fade;
    GLint pbr_mat = -1;

    GLint fur_on = -1;
    GLint fur_shape = -1;
    GLint fur_down = -1;
    GLint fur_tex = -1;
    GLint fur_body = -1;

    // motion vectors (merc2 only)
    GLint mv_on = -1;
    GLint mv_cam = -1;
    GLint mv_prev_persp = -1;
    GLint mv_prev_hvdf = -1;
    GLint mv_prev_fog = -1;
    GLint mv_size = -1;
  };

  Uniforms m_merc_uniforms, m_emerc_uniforms;

  void init_shader_common(Shader& shader, Uniforms* uniforms, bool include_lights);
  void handle_setup_dma(DmaFollower& dma, SharedRenderState* render_state);
  void handle_all_dma(DmaFollower& dma,
                      SharedRenderState* render_state,
                      ScopedProfilerNode& prof,
                      MercDebugStats* stats);
  void handle_merc_chain(DmaFollower& dma,
                         SharedRenderState* render_state,
                         ScopedProfilerNode& prof,
                         MercDebugStats* stats);

  void switch_to_merc2(SharedRenderState* render_state);
  void switch_to_emerc(SharedRenderState* render_state);

  GLuint m_vao;

  void setup_merc_vao();

  u64 m_fur_log_frame = 0;
  int m_fur_draws_window = 0;
  // median metres per UV unit of a model's draws of one texture, by (load id, model, texture)
  std::map<std::tuple<u64, const void*, s32>, float> m_fur_m_per_uv;
  // GPU time of the fur draws: timestamp pairs per frame, read three frames later
  static constexpr int kFurRing = 4;
  struct FurQueries {
    std::vector<GLuint> q;
    u32 used = 0;
    u64 frame = 0;
  };
  FurQueries m_fur_q[kFurRing];
  double m_fur_gpu_ms = 0;
  int m_fur_gpu_frames = 0;
  void fur_timer_begin_frame(u64 frame);
  void fur_timer_mark(u64 frame);
  void setup_fur_uniforms(SharedRenderState* render_state, const Uniforms& uniforms);

  std::vector<ModBuffers> m_mod_vtx_buffers;
  u32 m_next_mod_vtx_buffer = 0;

  static constexpr int MAX_MOD_VTX = UINT16_MAX;
  std::vector<tfrag3::MercVertex> m_mod_vtx_temp;

  struct UnpackTempVtx {
    float pos[4];
    float nrm[4];
    float uv[2];
  };
  std::vector<UnpackTempVtx> m_mod_vtx_unpack_temp;

  ModBuffers alloc_mod_vtx_buffer(const LevelData* lev);

  GLuint m_bones_buffer;

  enum DrawFlags {
    IGNORE_ALPHA = 1,
    MOD_VTX = 2,
    NO_TEXTURE = 4,
    EMITTER = 8,  // is_light_emitter: vanilla level light, no local lights
    MV_PREV = 16,  // motion vectors: the previous bones are in m_prev_bone_vector_buffer
    MV_HUD = 32,   // motion vectors: a HUD model (pc-merc-draw-request pid bit 30)
  };

  struct Draw {
    u32 first_index;
    u32 index_count;
    DrawMode mode;
    s32 texture;
    u32 num_triangles;
    u16 first_bone;
    u16 light_idx;
    u8 flags;
    ModBuffers mod_vtx_buffer;
    u8 fade[4];
    // no strip hack for custom models
    u8 no_strip;
    float fur_scale;  // fur shell length factor (fur_size_scale)
    u64 hash;
    const std::string* model_name;  // inspect mode only
  };

  struct LevelDrawBucket {
    const LevelData* level = nullptr;
    std::vector<Draw> draws;
    std::vector<Draw> envmap_draws;
    u32 next_free_draw = 0;
    u32 next_free_envmap_draw = 0;

    void reset() {
      level = nullptr;
      next_free_draw = 0;
      next_free_envmap_draw = 0;
    }
  };

  struct DrawArgs {
    LevelDrawBucket* lev_bucket;
    const u8* fade;
    bool jak1_water_mode;
    bool ignore_alpha;
    bool disable_fog;
    bool no_texture;
    u64 hash;
    u32 lights;
    u32 first_bone;
    const std::string* model_name;
    bool emitter;
    bool mv_prev;
    bool mv_hud;
    const LevelData* level;
    const tfrag3::MercModel* model;
  };

  float fur_size_scale(const LevelData* lev, const tfrag3::MercModel* model, s32 tex);

  Draw* alloc_normal_draw(const tfrag3::MercDraw& mdraw, const DrawArgs& args);

  Draw* try_alloc_envmap_draw(const tfrag3::MercDraw& mdraw,
                              const DrawMode& envmap_mode,
                              u32 envmap_texture,
                              const DrawArgs& args);

  void do_draws(const Draw* draw_array,
                const LevelData* lev,
                u32 num_draws,
                const Uniforms& uniforms,
                ScopedProfilerNode& prof,
                bool set_fade,
                SharedRenderState* render_state);

  static constexpr int MAX_LIGHTS = 1024;
  VuLights m_lights_buffer[MAX_LIGHTS];
  float m_light_cover[MAX_LIGHTS];   // per model: cover x the sun's share of its light rig
  float m_caster_cover[MAX_LIGHTS];  // per model: actor_shadows_cover at its root
  float m_light_rig_sun[MAX_LIGHTS];  // per model: the sun's share of its rig, 0 for emitters
  u32 m_caster_skip[MAX_LIGHTS];      // per model: bit v, no draw into shadow view v
  u32 m_next_free_light = 0;

  const std::vector<ShadowView>* m_shadow_views = nullptr;
  math::Vector3f m_shadow_player_pos;
  bool m_have_shadow_player = false;
  std::vector<math::Vector3f> m_shadow_actor_pos;
  VuLights m_shadow_lights;
  bool m_have_shadow_lights = false;
  void do_shadow_draws(SharedRenderState* render_state,
                       const Draw* draw_array,
                       const LevelData* lev,
                       u32 num_draws);

  std::vector<LevelDrawBucket> m_level_draw_buckets;
  u32 m_next_free_level_bucket = 0;
  u32 m_next_free_bone_vector = 0;
  size_t m_opengl_buffer_alignment = 0;

  void flush_draw_buckets(SharedRenderState* render_state,
                          ScopedProfilerNode& prof,
                          MercDebugStats* stats);
  void model_mod_draws(int num_effects,
                       const tfrag3::MercModel* model,
                       const LevelData* lev,
                       const u8* input_data,
                       const SharedRenderState* render_state,
                       ModBuffers* mod_opengl_buffers,
                       MercDebugStats* stats);
  void model_mod_blerc_draws(int num_effects,
                             const tfrag3::MercModel* model,
                             const LevelData* lev,
                             ModBuffers* mod_opengl_buffers,
                             const float* blerc_weights,
                             MercDebugStats* stats);
};
