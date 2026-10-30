#pragma once
#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/opengl_renderer/PerfTimers.h"
#include "game/graphics/opengl_renderer/SsaoDepth.h"

class CommonOceanRenderer {
 public:
  CommonOceanRenderer();
  ~CommonOceanRenderer();

  void init_for_near();
  void kick_from_near(const u8* data);
  void flush_near(SharedRenderState* render_state, ScopedProfilerNode& prof);

  void init_for_mid();
  void kick_from_mid(const u8* data);
  /// Draws the mid ocean, or keeps it for flush_mid_deferred while the water shading is on.
  void flush_mid(SharedRenderState* render_state, ScopedProfilerNode& prof);
  /// Draws a mid ocean that flush_mid kept, after the opaque background (the water shading
  /// reads the scene depth under the surface). Does nothing if none is pending.
  void flush_mid_deferred(SharedRenderState* render_state, ScopedProfilerNode& prof);
  bool mid_pending() const { return m_mid_pending; }

 private:
  void draw_mid(SharedRenderState* render_state, ScopedProfilerNode& prof, bool deferred);
  // water shading: plane height, wave weights and the triangle lists for tessellation
  void water_prepare(SharedRenderState* render_state, bool mid, bool* plane_ok, float* plane_y);
  void water_edge_weights(SharedRenderState* render_state,
                          const double inv[4][4],
                          float plane_y,
                          bool mid);
  void near_rim_on_plane();
  void triangulate();
  bool merge_envmap();
  void water_count_begin(bool on);
  void water_count_end(bool on);

  void handle_near_vertex_gif_data_fan(const u8* data, u32 offset, u32 loop);
  void handle_near_vertex_gif_data_strip(const u8* data, u32 offset, u32 loop);

  void handle_near_adgif(const u8* data, u32 offset, u32 count);

  void handle_mid_adgif(const u8* data, u32 offset);

  enum VertexBucket {
    RGB_TEXTURE = 0,
    ALPHA = 1,
    ENV_MAP = 2,
  };
  u32 m_current_bucket = VertexBucket::RGB_TEXTURE;

  struct Vertex {
    math::Vector<float, 3> xyz;
    math::Vector<u8, 4> rgba;
    math::Vector<float, 3> stq;
    u8 fog;
    u8 pad[3];
  };
  static_assert(sizeof(Vertex) == 32);

  static constexpr int NUM_BUCKETS = 3;

  std::vector<Vertex> m_vertices;
  u32 m_next_free_vertex = 0;

  std::vector<u32> m_indices[NUM_BUCKETS];
  u32 m_next_free_index[NUM_BUCKETS] = {0};

  u32 m_envmap_tex = 0;

  bool m_mid_pending = false;
  bool m_use_sub = false;  // this flush draws m_sub_indices as tessellated triangles
  std::vector<u32> m_sub_indices[NUM_BUCKETS];
  // the mid envmap pass's stq and colour per textured vertex, for the merged pass (perf bit 14)
  struct EnvAttr {
    math::Vector<float, 3> stq;
    math::Vector<u8, 4> rgba;
  };
  static_assert(sizeof(EnvAttr) == 16);
  // merged draw: the original vertices, then one copy per textured patch corner (with its
  // envmap partner's attributes, alpha 255, or none, alpha 0); indices into that buffer
  std::vector<Vertex> m_merge_verts;
  std::vector<EnvAttr> m_env_attr;
  std::vector<u32> m_merge_idx;
  std::vector<u32> m_env_rest;  // envmap patches with no textured twin, drawn as before

  // near_rim_on_plane's result for the last index list and position classes
  struct {
    bool valid = false;
    std::vector<u64> pos_key, edge_key;  // open-addressing tables
    std::vector<u32> pos_first, edge_tris;
    std::vector<u32> cls, cls_now, idx;
    std::vector<u8> on_rim, share;
  } m_rim;

  // tessellated triangle count, logged with the SSAO GPU timing switch
  GLuint m_count_query = 0;
  bool m_count_on = false;
  bool m_count_pending = false;
  u32 m_count_frames = 0;
  u32 m_count_source = 0;

  // fragment shader runs of a textured draw, read a few frames late (SSAO GPU timing switch)
  struct FragCount {
    GLuint q[4] = {};
    bool pending[4] = {};
    int next = 0;
    bool active = false;
    double last = -1.;
  };
  FragCount m_frags_mid, m_frags_near;
  void frags_begin(FragCount& f, perf::Counter c);
  void frags_end(FragCount& f);

  // with the SSAO GPU timing switch
  GpuTimer m_t_near{"ocean-near"};
  GpuTimer m_t_mid{"ocean-mid"};

  struct {
    GLuint vertex_buffer, index_buffer[NUM_BUCKETS], vao, env_buffer;
  } m_ogl;
};
