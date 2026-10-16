#pragma once

#include <optional>

#include "common/util/FilteredValue.h"

#include "game/graphics/gfx.h"
#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/opengl_renderer/background/background_common.h"
#include "game/graphics/pipelines/opengl.h"

struct UnbakeSidecar;

struct PbrLevelMaps;

class Shrub : public BucketRenderer {
 public:
  Shrub(const std::string& name, int my_id);
  ~Shrub();
  void init_shaders(ShaderLibrary& shaders) override;

  bool setup_for_level(const std::string& level, SharedRenderState* render_state);
  void render_all_trees(const TfragRenderSettings& settings,
                        SharedRenderState* render_state,
                        ScopedProfilerNode& prof);
  void render_tree(int idx,
                   const TfragRenderSettings& settings,
                   SharedRenderState* render_state,
                   ScopedProfilerNode& prof);
  void render(DmaFollower& dma, SharedRenderState* render_state, ScopedProfilerNode& prof) override;
  void draw_debug_window() override;

 private:
  void update_load(const LevelData* loader_data);
  void discard_tree_cache();

  struct Tree {
    GLuint vertex_buffer;
    GLuint index_buffer;
    GLuint single_draw_index_buffer;
    GLuint time_of_day_texture;
    GLuint vao;
    u32 vert_count;
    const std::vector<tfrag3::ShrubDraw>* draws = nullptr;
    const std::vector<tfrag3::TieWindInstance>* instance_info = nullptr;
    const tfrag3::PackedTimeOfDay* colors = nullptr;
    const u32* index_data = nullptr;
    std::vector<bool> proto_vis_mask;
    std::unordered_map<std::string, std::vector<u32>> proto_name_to_idx;
    // frustum-cull chunks: runs of whole strips in draw order, built on first use
    const tfrag3::ShrubTree* src = nullptr;
    struct Chunk {
      math::Vector4f bsphere;
      u32 first_index, num_indices;
      float max_strip;  // largest strip (instance) extent in the chunk
    };
    std::vector<Chunk> chunks;
    std::vector<std::pair<u32, u32>> draw_chunks;  // per draw: first chunk, count
    std::vector<u8> chunk_vis;

    struct {
      u32 draws = 0;
      u32 wind_draws = 0;
      Filtered<float> cull_time;
      Filtered<float> index_time;
      Filtered<float> tod_time;
      Filtered<float> setup_time;
      Filtered<float> draw_time;
      Filtered<float> tree_time;
    } perf;
  };

  struct {
    GLuint decal;
  } m_uniforms;

  void build_chunks(Tree& tree);
  /// Multidraw lists holding only the frustum-visible chunks; returns false if not usable.
  bool make_culled_multidraws(Tree& tree,
                              const TfragRenderSettings& settings,
                              const SharedRenderState* render_state);
  std::vector<Tree> m_trees;
  std::string m_level_name;
  const std::vector<GLuint>* m_textures;
  const UnbakeSidecar* m_unbake = nullptr;
  const std::vector<u8>* m_tex_class = nullptr;
  PbrLevelMaps* m_pbr_maps = nullptr;
  u64 m_load_id = -1;

  std::vector<math::Vector<u8, 4>> m_color_result;

  static constexpr int TIME_OF_DAY_COLOR_COUNT = 8192;
  bool m_has_level = false;

  struct Cache {
    std::vector<std::pair<int, int>> draw_idx_temp;
    std::vector<u32> index_temp;
    std::vector<std::pair<int, int>> multidraw_offset_per_stripdraw;
    std::vector<GLsizei> multidraw_count_buffer;
    std::vector<void*> multidraw_index_offset_buffer;
  } m_cache;
  TfragPcPortData m_pc_port_data;
  const u8* m_proto_vis_data = nullptr;
  int m_proto_vis_data_size = 0;
};
