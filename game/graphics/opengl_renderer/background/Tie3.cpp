#include "Tie3.h"

#include <cstring>

#include "common/global_profiler/GlobalProfiler.h"
#include "common/log/log.h"
#include "common/util/Assert.h"

#include "game/graphics/opengl_renderer/GlQueries.h"
#include "game/graphics/opengl_renderer/InspectRenderer.h"
#include "game/graphics/opengl_renderer/LavaField.h"
#include "game/graphics/opengl_renderer/Materials.h"
#include "game/graphics/opengl_renderer/WorldShadows.h"
#include "game/graphics/opengl_renderer/background/Canopy.h"
#include "game/graphics/opengl_renderer/PerfTimers.h"
#include "game/graphics/opengl_renderer/SsaoRenderer.h"
#include "game/graphics/opengl_renderer/Swamp.h"
#include "game/graphics/opengl_renderer/background/Caustics.h"
#include "game/graphics/opengl_renderer/buckets.h"

#include "third-party/imgui/imgui.h"

Tie3::Tie3(const std::string& name,
           int my_id,
           int level_id,
           const std::vector<GLuint>* anim_slot_array,
           tfrag3::TieCategory category)
    : BucketRenderer(name, my_id),
      m_level_id(level_id),
      m_default_category(category),
      m_anim_slot_array(anim_slot_array) {
  // regardless of how many we use some fixed max
  // we won't actually interp or upload to gpu the unused ones, but we need a fixed maximum so
  // indexing works properly.
  m_color_result.resize(TIME_OF_DAY_COLOR_COUNT);

  m_wind_data.paused = 0;
  math::Vector4f ones(1, 1, 1, 1);
  m_wind_data.wind_normal = ones;
  m_wind_data.wind_temp = ones;
  for (auto& wv : m_wind_data.wind_array) {
    wv = ones;
  }
  for (auto& wf : m_wind_data.wind_force) {
    wf = 1.f;
  }
}

Tie3::~Tie3() {
  discard_tree_cache();
}

void Tie3::init_shaders(ShaderLibrary& shaders) {
  m_uniforms.decal = glGetUniformLocation(shaders[ShaderId::TFRAG3].id(), "decal");

  m_etie_uniforms.persp0 = glGetUniformLocation(shaders[ShaderId::ETIE].id(), "persp0");
  m_etie_uniforms.persp1 = glGetUniformLocation(shaders[ShaderId::ETIE].id(), "persp1");
  m_etie_uniforms.cam_no_persp = glGetUniformLocation(shaders[ShaderId::ETIE].id(), "cam_no_persp");
  m_etie_uniforms.envmap_tod_tint =
      glGetUniformLocation(shaders[ShaderId::ETIE].id(), "envmap_tod_tint");

  m_etie_base_uniforms.decal = glGetUniformLocation(shaders[ShaderId::ETIE_BASE].id(), "decal");
  m_etie_base_uniforms.persp0 = glGetUniformLocation(shaders[ShaderId::ETIE_BASE].id(), "persp0");
  m_etie_base_uniforms.persp1 = glGetUniformLocation(shaders[ShaderId::ETIE_BASE].id(), "persp1");
  m_etie_base_uniforms.cam_no_persp =
      glGetUniformLocation(shaders[ShaderId::ETIE_BASE].id(), "cam_no_persp");
}

/*!
 * Load a TIE tree from FR3 data.
 * This often causes stutters, so as much as possible, we move stuff to the loader,
 * and this function just updates things to reference loader data.
 */
void Tie3::load_from_fr3_data(const LevelData* loader_data) {
  auto ul = scoped_prof("update-load");
  const tfrag3::Level* lev_data = loader_data->level.get();
  m_wind_vectors.clear();

  // We changed level! free opengl resources allocated for the previous
  discard_tree_cache();

  // resize for the number of trees in this level.
  for (int geo = 0; geo < 4; ++geo) {
    m_trees[geo].resize(lev_data->tie_trees[geo].size());
  }

  u16 max_wind_idx = 0;
  // loop over all "geos" (level of details)
  for (u32 l_geo = 0; l_geo < tfrag3::TIE_GEOS; l_geo++) {
    // loop over all trees
    for (u32 l_tree = 0; l_tree < lev_data->tie_trees[l_geo].size(); l_tree++) {
      auto ul = scoped_prof("load-tree");
      size_t wind_idx_buffer_len = 0;
      size_t num_grps = 0;
      const auto& tree = lev_data->tie_trees[l_geo][l_tree];

      // compute maximum number of vis groups (leaf in the bvh)
      for (auto& draw : tree.static_draws) {
        num_grps += draw.vis_groups.size();
      }

      // compute wind buffer sizes
      for (auto& draw : tree.instanced_wind_draws) {
        wind_idx_buffer_len += draw.vertex_index_stream.size();
      }
      for (auto& inst : tree.wind_instance_info) {
        max_wind_idx = std::max(max_wind_idx, inst.wind_idx);
      }

      // vertex buffer max
      auto& lod_tree = m_trees.at(l_geo);

      // set up resources: create a VAO
      glGenVertexArrays(1, &lod_tree[l_tree].vao);
      glBindVertexArray(lod_tree[l_tree].vao);
      // openGL vertex buffer from loader
      lod_tree[l_tree].vertex_buffer = loader_data->tie_data[l_geo][l_tree].vertex_buffer;
      // draw array from FR3 data
      lod_tree[l_tree].draws = &tree.static_draws;
      // base TOD colors from FR3
      lod_tree[l_tree].colors = &tree.colors;
      // visibility BVH from FR3
      lod_tree[l_tree].vis = &tree.bvh;
      // indices from FR3 (needed on CPU for culling)
      lod_tree[l_tree].index_data = tree.unpacked.indices.data();
      // wind metadata
      lod_tree[l_tree].instance_info = &tree.wind_instance_info;
      lod_tree[l_tree].wind_draws = &tree.instanced_wind_draws;
      // OpenGL index buffer (fixed index buffer for multidraw system)
      lod_tree[l_tree].index_buffer = loader_data->tie_data[l_geo][l_tree].index_buffer;
      lod_tree[l_tree].category_draw_indices = tree.category_draw_indices;
      lod_tree[l_tree].draw_mode = tree.use_strips ? GL_TRIANGLE_STRIP : GL_TRIANGLES;

      // set up vertex attributes
      glBindBuffer(GL_ARRAY_BUFFER, lod_tree[l_tree].vertex_buffer);
      glEnableVertexAttribArray(0);
      glEnableVertexAttribArray(1);
      glEnableVertexAttribArray(2);
      glEnableVertexAttribArray(3);
      glEnableVertexAttribArray(4);

      glVertexAttribPointer(0,                                           // location 0 in the shader
                            3,                                           // 3 values per vert
                            GL_FLOAT,                                    // floats
                            GL_FALSE,                                    // normalized
                            sizeof(tfrag3::PreloadedVertex),             // stride
                            (void*)offsetof(tfrag3::PreloadedVertex, x)  // offset (0)
      );

      glVertexAttribPointer(1,                                           // location 1 in the shader
                            3,                                           // 3 values per vert
                            GL_FLOAT,                                    // floats
                            GL_FALSE,                                    // normalized
                            sizeof(tfrag3::PreloadedVertex),             // stride
                            (void*)offsetof(tfrag3::PreloadedVertex, s)  // offset (0)
      );

      glVertexAttribIPointer(2,                                // location 2 in the shader
                             2,                                // 1 values per vert
                             GL_UNSIGNED_SHORT,                // u16
                             sizeof(tfrag3::PreloadedVertex),  // stride
                             (void*)offsetof(tfrag3::PreloadedVertex, color_index)  // offset (0)
      );

      glVertexAttribPointer(3,                                // location 1 in the shader
                            4,                                // 3 values per vert
                            GL_INT_2_10_10_10_REV,            // floats
                            GL_TRUE,                          // normalized
                            sizeof(tfrag3::PreloadedVertex),  // stride
                            (void*)offsetof(tfrag3::PreloadedVertex, nor)  // offset (0)
      );

      glVertexAttribPointer(4,                                           // location 1 in the shader
                            4,                                           // 3 values per vert
                            GL_UNSIGNED_BYTE,                            // floats
                            GL_TRUE,                                     // normalized
                            sizeof(tfrag3::PreloadedVertex),             // stride
                            (void*)offsetof(tfrag3::PreloadedVertex, r)  // offset (0)
      );

      // allocate dynamic index buffer for the fallback "not multidraw" mode.
      glGenBuffers(1, &lod_tree[l_tree].single_draw_index_buffer);

      // set up wind
      if (wind_idx_buffer_len > 0) {
        lod_tree[l_tree].wind_matrix_cache.resize(tree.wind_instance_info.size());
        lod_tree[l_tree].wind_bent.clear();
        lod_tree[l_tree].wind_vertex_index_buffer =
            loader_data->tie_data[l_geo][l_tree].wind_indices;
        u32 off = 0;
        for (auto& draw : tree.instanced_wind_draws) {
          lod_tree[l_tree].wind_vertex_index_offsets.push_back(off);
          off += draw.vertex_index_stream.size();
        }
      }

      // set up per-proto visibility. Jak 2 needs to enable/disable individual protos.
      lod_tree[l_tree].has_proto_visibility = tree.has_per_proto_visibility_toggle;
      lod_tree[l_tree].proto_names = &tree.proto_names;
      if (tree.has_per_proto_visibility_toggle) {
        lod_tree[l_tree].proto_visibility.init(tree.proto_names);
      }

      // set up time of day texture.
      glActiveTexture(GL_TEXTURE10);
      glGenTextures(1, &lod_tree[l_tree].time_of_day_texture);
      glBindTexture(GL_TEXTURE_1D, lod_tree[l_tree].time_of_day_texture);
      glTexImage1D(GL_TEXTURE_1D, 0, GL_RGBA, kCausticEnvOffset + TIME_OF_DAY_COLOR_COUNT, 0, GL_RGBA,
                   GL_UNSIGNED_INT_8_8_8_8, nullptr);
      glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

      glBindVertexArray(0);

      lod_tree[l_tree].vis_temp.resize(tree.bvh.vis_nodes.size());

      lod_tree[l_tree].draw_idx_temp.resize(tree.static_draws.size());
      lod_tree[l_tree].index_temp.resize(tree.unpacked.indices.size());
      lod_tree[l_tree].multidraw_offset_per_stripdraw.resize(tree.static_draws.size());
      lod_tree[l_tree].multidraw_count_buffer.resize(num_grps);
      lod_tree[l_tree].multidraw_index_offset_buffer.resize(num_grps);
    }
  }

  // set up temporary caches. These are just temporary, so they don't need per-tree versions.

  m_wind_vectors.resize(4 * max_wind_idx + 4);  // 4x u32's per wind.

  // ASSERT(time_of_day_count <= TIME_OF_DAY_COLOR_COUNT);
}

/*!
 * Try loading a level. Hopefully it has been preloaded and this is fast.
 */
bool Tie3::try_loading_level(const std::string& level, SharedRenderState* render_state) {
  // make sure we have the level data.
  Timer tfrag3_setup_timer;
  auto lev_data = render_state->loader->get_tfrag3_level(level);

  if (!lev_data) {
    // not loaded
    m_has_level = false;
    m_textures = nullptr;
    m_level = nullptr;
    m_unbake = nullptr;
    m_level_name = "";
    discard_tree_cache();
    return false;
  }

  if (m_has_level && lev_data->load_id != m_load_id) {
    m_has_level = false;
    m_textures = nullptr;
    m_level_name = "";
    discard_tree_cache();
    return try_loading_level(level, render_state);
  }

  // loading was successful. Link textures/load ID.
  m_textures = &lev_data->textures;
  m_level = lev_data->level.get();
  m_unbake = lev_data->unbake.get();
  m_tex_class = &lev_data->tex_class;
  m_pbr_maps = lev_data->pbr_maps.get();
  m_load_id = lev_data->load_id;

  // see if this is the first time we've gotten the level
  if (m_level_name != level) {
    // it is! do the one time load.
    load_from_fr3_data(lev_data);
    m_has_level = true;
    m_level_name = level;
  } else {
    m_has_level = true;
  }

  if (tfrag3_setup_timer.getMs() > 5) {
    lg::info("TIE setup: {:.1f}ms", tfrag3_setup_timer.getMs());
  }

  return m_has_level;
}

void Tie3::discard_tree_cache() {
  for (int geo = 0; geo < 4; ++geo) {
    for (auto& tree : m_trees[geo]) {
      glq::invalidate_state();
      caustic_forget_texture(tree.time_of_day_texture);
      glBindTexture(GL_TEXTURE_1D, tree.time_of_day_texture);
      glDeleteTextures(1, &tree.time_of_day_texture);
      // glDeleteBuffers(1, &tree.index_buffer);
      glDeleteBuffers(1, &tree.single_draw_index_buffer);
      glDeleteVertexArrays(1, &tree.vao);
    }

    m_trees[geo].clear();
  }
}

bool Tie3::set_up_common_data_from_dma(DmaFollower& dma, SharedRenderState* render_state) {
  auto data0 = dma.read_and_advance();
  ASSERT(data0.vif1() == 0 || data0.vifcode1().kind == VifCode::Kind::NOP);
  ASSERT(data0.vif0() == 0 || data0.vifcode0().kind == VifCode::Kind::NOP ||
         data0.vifcode0().kind == VifCode::Kind::MARK);
  ASSERT(data0.size_bytes == 0);

  if (dma.current_tag().kind == DmaTag::Kind::CALL) {
    // renderer didn't run, let's just get out of here.
    for (int i = 0; i < 4; i++) {
      dma.read_and_advance();
    }
    ASSERT(dma.current_tag_offset() == render_state->next_bucket);
    return false;
  }

  if (dma.current_tag_offset() == render_state->next_bucket) {
    return false;
  }

  auto gs_test = dma.read_and_advance();
  if (gs_test.size_bytes == 160) {
  } else {
    ASSERT(gs_test.size_bytes == 32);

    auto tie_consts = dma.read_and_advance();
    ASSERT(tie_consts.size_bytes == 9 * 16);
  }

  auto mscalf = dma.read_and_advance();
  ASSERT(mscalf.size_bytes == 0);

  auto row = dma.read_and_advance();
  ASSERT(row.size_bytes == 32);

  auto next = dma.read_and_advance();
  if (next.size_bytes == 32) {
    next = dma.read_and_advance();
  }
  ASSERT(next.size_bytes == 0);

  auto pc_port_data = dma.read_and_advance();
  ASSERT(pc_port_data.size_bytes == sizeof(TfragPcPortData));
  memcpy(&m_pc_port_data, pc_port_data.data, sizeof(TfragPcPortData));
  apply_stereo_eye(&m_pc_port_data.camera, render_state);
  m_pc_port_data.level_name[11] = '\0';
  cave_dark_itimes(render_state, &m_pc_port_data);
  caustic_itimes(render_state, &m_pc_port_data);

  if (render_state->version == GameVersion::Jak1) {
    auto wind_data = dma.read_and_advance();
    ASSERT(wind_data.size_bytes == sizeof(WindWork));
    memcpy(&m_wind_data, wind_data.data, sizeof(WindWork));
    // 2 = capture mode reset (tie-methods.gc add-pc-wind-data); also counts as paused
    if (m_wind_data.paused == 2) {
      std::fill(m_wind_vectors.begin(), m_wind_vectors.end(), 0.f);
    }
  }

  if (render_state->version >= GameVersion::Jak2) {
    // jak 2 proto visibility
    auto proto_mask_data = dma.read_and_advance();
    m_common_data.proto_vis_data = proto_mask_data.data;
    m_common_data.proto_vis_data_size = proto_mask_data.size_bytes;
  }

  // envmap color
  auto envmap_color = dma.read_and_advance();
  ASSERT(envmap_color.size_bytes == 16);
  memcpy(m_common_data.envmap_color.data(), envmap_color.data, 16);
  m_common_data.envmap_color /= 128.f;
  if (render_state->version == GameVersion::Jak1) {
    m_common_data.envmap_color *= 2;
  }
  m_common_data.envmap_color *= m_envmap_strength;

  m_common_data.frame_idx = render_state->frame_idx;

  while (dma.current_tag_offset() != render_state->next_bucket) {
    dma.read_and_advance();
  }

  m_common_data.settings.camera = m_pc_port_data.camera;

  m_common_data.settings.tree_idx = 0;

  if (render_state->occlusion_vis[m_level_id].valid) {
    m_common_data.settings.occlusion_culling = render_state->occlusion_vis[m_level_id].data;
  } else {
    m_common_data.settings.occlusion_culling = 0;
  }

  update_render_state_from_pc_settings(render_state, m_pc_port_data);

  m_has_level = try_loading_level(m_pc_port_data.level_name, render_state);
  return true;
}
/*!
 * Render method called from bucket render system.
 * Does common setup for all category, but only renderers default_category.
 */
void Tie3::render(DmaFollower& dma, SharedRenderState* render_state, ScopedProfilerNode& prof) {
  if (!m_enabled) {
    while (dma.current_tag_offset() != render_state->next_bucket) {
      dma.read_and_advance();
    }
    return;
  }

  if (render_state->stereo_active) {
    // both eyes integrate wind from the same starting state
    if (render_state->stereo_repeat_pass) {
      m_wind_vectors = m_stereo_wind_vectors;
    } else {
      m_stereo_wind_vectors = m_wind_vectors;
    }
  }

  m_vista_hide_this_frame = vista_hide_active(render_state, m_level_id);
  if (m_vista_hide_this_frame) {
    m_vista_filter = active_vista_filter(render_state);
  }
  if (set_up_common_data_from_dma(dma, render_state)) {
    setup_all_trees(lod(), m_common_data.settings, m_common_data.proto_vis_data,
                    m_common_data.proto_vis_data_size, !render_state->no_multidraw, prof);

    draw_matching_draws_for_all_trees(lod(), m_common_data.settings, render_state, prof,
                                      m_default_category);
  }
}

void Tie3::render_from_another(SharedRenderState* render_state,
                               ScopedProfilerNode& prof,
                               tfrag3::TieCategory category) {
  if (render_state->frame_idx != m_common_data.frame_idx) {
    return;
  }
  draw_matching_draws_for_all_trees(lod(), m_common_data.settings, render_state, prof, category);
}

void Tie3::render_vista(const std::string& level,
                        const char* hide_filter,
                        const TfragRenderSettings& settings,
                        SharedRenderState* render_state,
                        ScopedProfilerNode& prof) {
  m_vista_hide_this_frame = hide_filter && hide_filter[0];
  m_vista_filter = m_vista_hide_this_frame ? hide_filter : "";
  m_wind_data.paused = 1;
  m_has_level = try_loading_level(level, render_state);
  if (!m_has_level) {
    return;
  }
  m_common_data.settings = settings;
  m_common_data.frame_idx = render_state->frame_idx;
  setup_all_trees(lod(), settings, nullptr, 0, !render_state->no_multidraw, prof);
  draw_matching_draws_for_all_trees(lod(), settings, render_state, prof,
                                    tfrag3::TieCategory::NORMAL);
  draw_matching_draws_for_all_trees(lod(), settings, render_state, prof,
                                    tfrag3::TieCategory::NORMAL_ENVMAP);
}

const std::vector<u8>* Tie3::vista_proto_flags(Tree& tree) {
  if (!tree.proto_names || tree.proto_names->empty()) {
    return nullptr;
  }
  const char* filter = m_vista_filter;
  if (tree.vista_proto_vis.size() != tree.proto_names->size() || tree.vista_filter != filter) {
    tree.vista_filter = filter;
    tree.vista_proto_vis.resize(tree.proto_names->size());
    for (size_t i = 0; i < tree.proto_names->size(); i++) {
      tree.vista_proto_vis[i] = !vista_name_matches(tree.proto_names->at(i), filter);
    }
  }
  return &tree.vista_proto_vis;
}

void Tie3::draw_matching_draws_for_all_trees(int geom,
                                             const TfragRenderSettings& settings,
                                             SharedRenderState* render_state,
                                             ScopedProfilerNode& prof,
                                             tfrag3::TieCategory category) {
  for (u32 i = 0; i < m_trees[geom].size(); i++) {
    draw_matching_draws_for_tree(i, geom, settings, render_state, prof, category);
  }
}

void Tie3::setup_all_trees(int geom,
                           const TfragRenderSettings& settings,
                           const u8* proto_vis_data,
                           size_t proto_vis_data_size,
                           bool use_multidraw,
                           ScopedProfilerNode& prof) {
  for (u32 i = 0; i < m_trees[geom].size(); i++) {
    setup_tree(i, geom, settings, proto_vis_data, proto_vis_data_size, use_multidraw, prof);
  }
}

void Tie3::setup_tree(int idx,
                      int geom,
                      const TfragRenderSettings& settings,
                      const u8* proto_vis_data,
                      size_t proto_vis_data_size,
                      bool use_multidraw,
                      ScopedProfilerNode& prof) {
  // reset perf
  auto& tree = m_trees.at(geom).at(idx);
  // don't render if we haven't loaded
  if (!m_has_level) {
    return;
  }

  // update time of day
  if (m_color_result.size() < tree.colors->color_count) {
    m_color_result.resize(tree.colors->color_count);
  }

  interp_time_of_day(settings.camera.itimes, *tree.colors, m_color_result.data());

  glActiveTexture(GL_TEXTURE10);
  glBindTexture(GL_TEXTURE_1D, tree.time_of_day_texture);
  glTexSubImage1D(GL_TEXTURE_1D, 0, 0, tree.colors->color_count, GL_RGBA,
                  GL_UNSIGNED_INT_8_8_8_8_REV, m_color_result.data());

  // update proto vis mask
  if (proto_vis_data) {
    tree.proto_visibility.update(proto_vis_data, proto_vis_data_size);
  }

  if (!m_debug_all_visible) {
    // need culling data
    cull_check_all_slow(settings.camera.planes, tree.vis->vis_nodes, settings.occlusion_culling,
                        tree.vis_temp.data());
  }

  const std::vector<u8>* proto_flags =
      tree.has_proto_visibility ? &tree.proto_visibility.vis_flags : nullptr;
  if (!proto_flags && m_vista_hide_this_frame) {
    proto_flags = vista_proto_flags(tree);
  }

  u32 num_tris = 0;
  if (use_multidraw) {
    if (m_debug_all_visible) {
      num_tris = make_all_visible_multidraws(
          tree.multidraw_offset_per_stripdraw.data(), tree.multidraw_count_buffer.data(),
          tree.multidraw_index_offset_buffer.data(), *tree.draws);
    } else {
      Timer index_timer;
      if (proto_flags) {
        num_tris = make_multidraws_from_vis_and_proto_string(
            tree.multidraw_offset_per_stripdraw.data(), tree.multidraw_count_buffer.data(),
            tree.multidraw_index_offset_buffer.data(), *tree.draws, tree.vis_temp, *proto_flags);
      } else {
        num_tris = make_multidraws_from_vis_string(
            tree.multidraw_offset_per_stripdraw.data(), tree.multidraw_count_buffer.data(),
            tree.multidraw_index_offset_buffer.data(), *tree.draws, tree.vis_temp);
      }
    }
  } else {
    u32 idx_buffer_size;
    if (m_debug_all_visible) {
      idx_buffer_size =
          make_all_visible_index_list(tree.draw_idx_temp.data(), tree.index_temp.data(),
                                      *tree.draws, tree.index_data, &num_tris);
    } else {
      if (proto_flags) {
        idx_buffer_size = make_index_list_from_vis_and_proto_string(
            tree.draw_idx_temp.data(), tree.index_temp.data(), *tree.draws, tree.vis_temp,
            *proto_flags, tree.index_data, &num_tris);
      } else {
        idx_buffer_size =
            make_index_list_from_vis_string(tree.draw_idx_temp.data(), tree.index_temp.data(),
                                            *tree.draws, tree.vis_temp, tree.index_data, &num_tris);
      }
    }

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, tree.single_draw_index_buffer);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx_buffer_size * sizeof(u32), tree.index_temp.data(),
                 GL_STREAM_DRAW);
  }

  prof.add_tri(num_tris);
}

namespace {
void set_uniform(GLuint uniform, const math::Vector4f& val) {
  glUniform4f(uniform, val.x(), val.y(), val.z(), val.w());
}
}  // namespace

void init_etie_cam_uniforms(const EtieUniforms& uniforms, const GoalBackgroundCameraData& data) {
  glUniformMatrix4fv(uniforms.cam_no_persp, 1, GL_FALSE, data.rot[0].data());

  math::Vector4f perspective[2];
  float inv_fog = 1.f / data.fog[0];
  auto& hvdf_off = data.hvdf_off;
  float pxx = data.perspective[0].x();
  float pyy = data.perspective[1].y();
  float pzz = data.perspective[2].z();
  float pzw = data.perspective[2].w();
  float pwz = data.perspective[3].z();
  float scale = pzw * inv_fog;
  perspective[0].x() = scale * hvdf_off.x();
  perspective[0].y() = scale * hvdf_off.y();
  perspective[0].z() = scale * hvdf_off.z() + pzz;
  perspective[0].w() = scale;

  perspective[1].x() = pxx;
  perspective[1].y() = pyy;
  perspective[1].z() = pwz;
  perspective[1].w() = 0;

  set_uniform(uniforms.persp0, perspective[0]);
  set_uniform(uniforms.persp1, perspective[1]);
}

void Tie3::draw_matching_draws_for_tree(int idx,
                                        int geom,
                                        const TfragRenderSettings& settings,
                                        SharedRenderState* render_state,
                                        ScopedProfilerNode& prof,
                                        tfrag3::TieCategory category) {
  auto& tree = m_trees.at(geom).at(idx);

  // don't render if we haven't loaded
  if (!m_has_level) {
    return;
  }
  bool use_envmap = tfrag3::is_envmap_first_draw_category(category);
  const UnbakeTree* sun = nullptr;
  if (!use_envmap && geom == 0 && unbake_active(render_state, m_unbake) &&
      idx < (int)m_unbake->tie.size() && m_unbake->tie[idx].valid) {
    sun = &m_unbake->tie[idx];
  }
  auto shader_id =
      use_envmap ? ShaderId::ETIE_BASE : (sun ? ShaderId::TFRAG3_SUN : ShaderId::TFRAG3);

  // setup OpenGL shader
  first_tfrag_draw_setup(settings.camera, render_state, shader_id);
  if (sun) {
    unbake_set_uniforms(render_state, *m_unbake, settings.camera, unbake_texture(*sun));
  }
  if (!use_envmap && geom == 0 && SsaoRenderer::normals_active() && m_unbake &&
      idx < (int)m_unbake->tie.size() && m_unbake->tie[idx].valid) {
    unbake_bind_normals(render_state->shaders[shader_id].id(), m_unbake->tie[idx]);
  }
  const bool emissive = render_state->emissive_mask &&
                        m_my_id <= (int)jak1::BucketId::SHRUB_GENERIC_LEVEL1;
  // actor shadows also read E, to keep shadows off emissive surfaces
  const bool em_values = emissive || (render_state->shadow.active &&
                                      m_my_id <= (int)jak1::BucketId::SHRUB_GENERIC_LEVEL1);
  const UnbakeTree* em_tree =
      geom == 0 && m_unbake && idx < (int)m_unbake->tie.size() ? &m_unbake->tie[idx] : nullptr;
  if (em_values) {
    unbake_set_emissive_uniforms(render_state->shaders[shader_id].id(), m_unbake, em_tree,
                                 settings.camera, m_my_id >= 0);
    if (emissive) {
      SsaoRenderer::emissive_draw_buffers(true);
    }
  } else {
    glUniform1i(GLQ_LOC(PERF_OPT_GEOMETRY + 2, render_state->shaders[shader_id].id(), "em_on"), 0);
  }
  const GLint decal_loc =
      use_envmap
          ? m_etie_base_uniforms.decal
          : (sun ? GLQ_LOC(PERF_OPT_GEOMETRY + 2, render_state->shaders[shader_id].id(), "decal")
                 : m_uniforms.decal);

  if (use_envmap) {
    // if we use envmap, use the envmap-style math for the base draw to avoid rounding issue.
    init_etie_cam_uniforms(m_etie_base_uniforms, m_common_data.settings.camera);
  }
  caustic_setup_tree(render_state, render_state->shaders[shader_id].id(), m_level_name,
                     settings.camera.itimes, *tree.colors, tree.time_of_day_texture,
                     CausticKind::TIE);
  const bool pbr = pbr_active(render_state);
  PbrDrawState pbr_draw;
  if (pbr) {
    const GLuint id = render_state->shaders[shader_id].id();
    const bool has = geom == 0 && m_unbake && idx < (int)m_unbake->tie.size();
    pbr_setup_background(render_state, id, m_unbake, has ? &m_unbake->tie[idx] : nullptr,
                         settings.camera);
    // the rain cover binds the heightfield from unit 0, the renderer convention between passes
    glActiveTexture(GL_TEXTURE0);
    swamp_setup_background(render_state, id, m_level_name);
    lava_field::setup_background(render_state, id, m_level_name);
    pbr_draw.begin(id, m_tex_class, m_pbr_maps);
  }

  glBindVertexArray(tree.vao);
  glBindBuffer(GL_ARRAY_BUFFER, tree.vertex_buffer);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,
               render_state->no_multidraw ? tree.single_draw_index_buffer : tree.index_buffer);

  glActiveTexture(GL_TEXTURE10);
  glBindTexture(GL_TEXTURE_1D, tree.time_of_day_texture);

  glActiveTexture(GL_TEXTURE0);
  glEnable(GL_PRIMITIVE_RESTART);
  glPrimitiveRestartIndex(UINT32_MAX);

  int hl_blend = -1;  // material_blend_guard
  int last_texture = -1;
  for (size_t draw_idx = tree.category_draw_indices[(int)category];
       draw_idx < tree.category_draw_indices[(int)category + 1]; draw_idx++) {
    const auto& draw = tree.draws->operator[](draw_idx);
    const auto& multidraw_indices = tree.multidraw_offset_per_stripdraw[draw_idx];
    const auto& singledraw_indices = tree.draw_idx_temp[draw_idx];

    if (render_state->no_multidraw) {
      if (singledraw_indices.second == 0) {
        continue;
      }
    } else {
      if (multidraw_indices.second == 0) {
        continue;
      }
    }

    if (draw.tree_tex_id != last_texture) {
      if (draw.tree_tex_id >= 0) {
        glBindTexture(GL_TEXTURE_2D, m_textures->at(draw.tree_tex_id));
      } else {
        glBindTexture(GL_TEXTURE_2D, m_anim_slot_array->at(-(draw.tree_tex_id + 1)));
      }
      last_texture = draw.tree_tex_id;
    }
    if (pbr) {
      pbr_draw.set(draw.tree_tex_id);
    }

    auto double_draw = setup_tfrag_shader(render_state, draw.mode, shader_id);
    if (emissive && render_state->light_material) {
      SsaoRenderer::material_blend_guard(render_state->shaders[shader_id].id(),
                                         SsaoRenderer::draw_blends(draw.mode), true, hl_blend);
    }

    glUniform1i(decal_loc, draw.mode.get_decal() ? 1 : 0);
    if (inspect::active()) {
      inspect::DrawRecord rec;
      rec.kind = inspect::TIE;
      rec.level = m_level_name;
      rec.geom = geom;
      rec.tree = idx;
      rec.draw = (int)draw_idx;
      rec.tex = draw.tree_tex_id;
      rec.mode = draw.mode;
      std::copy(settings.camera.itimes, settings.camera.itimes + 4, rec.itimes);
      rec.cam_trans = settings.camera.trans;
      inspect::set(render_state->shaders[shader_id].id(), inspect::add(std::move(rec)));
    }

    prof.add_draw_call();

    if (render_state->no_multidraw) {
      glDrawElements(tree.draw_mode, singledraw_indices.second, GL_UNSIGNED_INT,
                     (void*)(singledraw_indices.first * sizeof(u32)));
    } else {
      glMultiDrawElements(
          tree.draw_mode, &tree.multidraw_count_buffer[multidraw_indices.first], GL_UNSIGNED_INT,
          &tree.multidraw_index_offset_buffer[multidraw_indices.first], multidraw_indices.second);
    }

    switch (double_draw.kind) {
      case DoubleDrawKind::NONE:
        break;
      case DoubleDrawKind::AFAIL_NO_DEPTH_WRITE:
        ASSERT(false);
        prof.add_draw_call();
        glUniform1f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, render_state->shaders[ShaderId::TFRAG3].id(),
                            "alpha_min"),
                    -10.f);
        glUniform1f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, render_state->shaders[ShaderId::TFRAG3].id(),
                            "alpha_max"),
                    double_draw.aref_second);
        glDepthMask(GL_FALSE);
        if (render_state->no_multidraw) {
          glDrawElements(tree.draw_mode, singledraw_indices.second, GL_UNSIGNED_INT,
                         (void*)(singledraw_indices.first * sizeof(u32)));
        } else {
          glMultiDrawElements(tree.draw_mode, &tree.multidraw_count_buffer[multidraw_indices.first],
                              GL_UNSIGNED_INT,
                              &tree.multidraw_index_offset_buffer[multidraw_indices.first],
                              multidraw_indices.second);
        }
        break;
      default:
        ASSERT(false);
    }
  }

  if (emissive) {
    SsaoRenderer::emissive_draw_buffers(false);
  }

  if (!m_hide_wind && category == tfrag3::TieCategory::NORMAL) {
    auto wind_prof = prof.make_scoped_child("wind");
    render_tree_wind(idx, geom, settings, render_state, wind_prof);
  }

  glBindVertexArray(0);

  if (use_envmap && m_draw_envmap_second_draw) {
    inspect::write_mask(false);
    envmap_second_pass_draw(tree, settings, render_state, prof,
                            tfrag3::get_second_draw_category(category),
                            em_values ? em_tree : nullptr);
    inspect::write_mask(true);
  }
}

void Tie3::envmap_second_pass_draw(const Tree& tree,
                                   const TfragRenderSettings& settings,
                                   SharedRenderState* render_state,
                                   ScopedProfilerNode& prof,
                                   tfrag3::TieCategory category,
                                   const UnbakeTree* em_tree) {
  first_tfrag_draw_setup(settings.camera, render_state, ShaderId::ETIE);
  unbake_set_emissive_uniforms(render_state->shaders[ShaderId::ETIE].id(),
                               em_tree ? m_unbake : nullptr, em_tree, settings.camera,
                               m_my_id >= 0);
  glBindVertexArray(tree.vao);
  glBindBuffer(GL_ARRAY_BUFFER, tree.vertex_buffer);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,
               render_state->no_multidraw ? tree.single_draw_index_buffer : tree.index_buffer);

  init_etie_cam_uniforms(m_etie_uniforms, m_common_data.settings.camera);
  set_uniform(m_etie_uniforms.envmap_tod_tint, m_common_data.envmap_color);

  int last_texture = -1;
  for (size_t draw_idx = tree.category_draw_indices[(int)category];
       draw_idx < tree.category_draw_indices[(int)category + 1]; draw_idx++) {
    const auto& draw = tree.draws->operator[](draw_idx);
    const auto& multidraw_indices = tree.multidraw_offset_per_stripdraw[draw_idx];
    const auto& singledraw_indices = tree.draw_idx_temp[draw_idx];

    if (render_state->no_multidraw) {
      if (singledraw_indices.second == 0) {
        continue;
      }
    } else {
      if (multidraw_indices.second == 0) {
        continue;
      }
    }

    if (draw.tree_tex_id != last_texture) {
      if (draw.tree_tex_id >= 0) {
        glBindTexture(GL_TEXTURE_2D, m_textures->at(draw.tree_tex_id));
      } else {
        glBindTexture(GL_TEXTURE_2D, m_anim_slot_array->at(-(draw.tree_tex_id + 1)));
      }

      last_texture = draw.tree_tex_id;
    }

    auto double_draw = setup_tfrag_shader(render_state, draw.mode, ShaderId::ETIE);

    prof.add_draw_call();

    if (render_state->no_multidraw) {
      glDrawElements(tree.draw_mode, singledraw_indices.second, GL_UNSIGNED_INT,
                     (void*)(singledraw_indices.first * sizeof(u32)));
    } else {
      glMultiDrawElements(
          tree.draw_mode, &tree.multidraw_count_buffer[multidraw_indices.first], GL_UNSIGNED_INT,
          &tree.multidraw_index_offset_buffer[multidraw_indices.first], multidraw_indices.second);
    }

    switch (double_draw.kind) {
      case DoubleDrawKind::NONE:
        break;
      default:
        ASSERT(false);
    }
  }
}

void Tie3::draw_debug_window() {
  ImGui::Checkbox("envmap 2nd draw", &m_draw_envmap_second_draw);
  ImGui::SliderFloat("envmap str", &m_envmap_strength, 0, 2);
  ImGui::SameLine();
  ImGui::Checkbox("All Visible", &m_debug_all_visible);
  ImGui::Checkbox("Hide Wind", &m_hide_wind);
  ImGui::SliderFloat("Wind Multiplier", &m_wind_multiplier, 0., 40.f);
  ImGui::Separator();
}

void TieProtoVisibility::init(const std::vector<std::string>& names) {
  vis_flags.resize(names.size());
  for (auto& x : vis_flags) {
    x = 1;
  }
  all_visible = true;
  name_to_idx.clear();
  size_t i = 0;
  for (auto& name : names) {
    name_to_idx[name].push_back(i++);
  }
}

void TieProtoVisibility::update(const u8* data, size_t size) {
  char name_buffer[256];  // ??

  if (!all_visible) {
    for (auto& x : vis_flags) {
      x = 1;
    }
    all_visible = true;
  }

  const u8* end = data + size;

  while (true) {
    int name_idx = 0;
    while (*data) {
      name_buffer[name_idx++] = *data;
      data++;
    }
    if (name_idx) {
      ASSERT(name_idx < 254);
      name_buffer[name_idx] = '\0';
      const auto& it = name_to_idx.find(name_buffer);
      if (it != name_to_idx.end()) {
        all_visible = false;
        for (auto x : name_to_idx.at(name_buffer)) {
          vis_flags[x] = 0;
        }
      }
    }

    while (*data == 0) {
      if (data >= end) {
        return;
      }
      data++;
    }
  }
}

void vector_min_in_place(math::Vector4f& v, float val) {
  for (int i = 0; i < 4; i++) {
    if (v[i] > val) {
      v[i] = val;
    }
  }
}

math::Vector4f vector_max(const math::Vector4f& v, float val) {
  math::Vector4f result;
  for (int i = 0; i < 4; i++) {
    result[i] = std::max(val, v[i]);
  }
  return result;
}

void do_wind_math(u16 wind_idx,
                  float* wind_vector_data,
                  const Tie3::WindWork& wind_work,
                  float stiffness,
                  std::array<math::Vector4f, 4>& mat) {
  float* my_vector = wind_vector_data + (4 * wind_idx);
  const auto& work_vector = wind_work.wind_array[(wind_work.wind_time + wind_idx) & 63];
  constexpr float cx = 0.5;
  constexpr float cy = 100.0;
  constexpr float cz = 0.0166;
  constexpr float cw = -1.0;

  // ld s1, 8(s5)                    # load wind vector 1
  // pextlw s1, r0, s1               # convert to 2x 64 bits, by shifting left
  // qmtc2.i vf18, s1                # put in vf
  float vf18_x = my_vector[2];
  float vf18_z = my_vector[3];

  // ld s2, 0(s5)                    # load wind vector 0
  // pextlw s3, r0, s2               # convert to 2x 64 bits, by shifting left
  // qmtc2.i vf17, s3                # put in vf
  float vf17_x = my_vector[0];
  float vf17_z = my_vector[1];

  // lqc2 vf16, 12(s3)               # load wind vector
  math::Vector4f vf16 = work_vector;

  // vmula.xyzw acc, vf16, vf1       # acc = vf16
  // vmsubax.xyzw acc, vf18, vf19    # acc = vf16 - vf18 * wind_const.x
  // vmsuby.xyzw vf16, vf17, vf19
  // # vf16 -= (vf18 * wind_const.x) + (vf17 * wind_const.y)
  vf16.x() -= cx * vf18_x + cy * vf17_x;
  vf16.z() -= cx * vf18_z + cy * vf17_z;

  // vmulaz.xyzw acc, vf16, vf19     # acc = vf16 * wind_const.z
  // vmadd.xyzw vf18, vf1, vf18
  // # vf18 += vf16 * wind_const.z
  math::Vector4f vf18(vf18_x, 0.f, vf18_z, 0.f);
  vf18 += vf16 * cz;

  // vmulaz.xyzw acc, vf18, vf19    # acc = vf18 * wind_const.z
  // vmadd.xyzw vf17, vf17, vf1
  // # vf17 += vf18 * wind_const.z
  math::Vector4f vf17(vf17_x, 0.f, vf17_z, 0.f);
  vf17 += vf18 * cz;

  // vitof12.xyzw vf11, vf11 # normal convert
  // vitof12.xyzw vf12, vf12 # normal convert

  // vminiw.xyzw vf17, vf17, vf0
  vector_min_in_place(vf17, 1.f);

  // qmfc2.i s3, vf18
  // ppacw s3, r0, s3

  // vmaxw.xyzw vf27, vf17, vf19
  auto vf27 = vector_max(vf17, cw);

  // vmulw.xyzw vf27, vf27, vf15
  vf27 *= stiffness;

  // vmulax.yw acc, vf0, vf0
  // vmulay.xz acc, vf27, vf10
  // vmadd.xyzw vf10, vf1, vf10
  mat[0].x() += vf27.x() * mat[0].y();
  mat[0].z() += vf27.z() * mat[0].y();

  // qmfc2.i s2, vf27
  if (!wind_work.paused) {
    my_vector[0] = vf27.x();
    my_vector[1] = vf27.z();
    my_vector[2] = vf18.x();
    my_vector[3] = vf18.z();
  }

  // vmulax.yw acc, vf0, vf0
  // vmulay.xz acc, vf27, vf11
  // vmadd.xyzw vf11, vf1, vf11
  mat[1].x() += vf27.x() * mat[1].y();
  mat[1].z() += vf27.z() * mat[1].y();

  // ppacw s2, r0, s2
  // vmulax.yw acc, vf0, vf0
  // vmulay.xz acc, vf27, vf12
  // vmadd.xyzw vf12, vf1, vf12
  mat[2].x() += vf27.x() * mat[2].y();
  mat[2].z() += vf27.z() * mat[2].y();

  //
  // if not paused
  // sd s3, 8(s5)
  // sd s2, 0(s5)
}

void Tie3::render_tree_wind(int idx,
                            int geom,
                            const TfragRenderSettings& settings,
                            SharedRenderState* render_state,
                            ScopedProfilerNode& prof) {
  auto& tree = m_trees.at(geom).at(idx);
  if (tree.wind_draws->empty()) {
    return;
  }

  auto& cam_bad = settings.camera.camera;
  std::array<math::Vector4f, 4> cam;
  for (int i = 0; i < 4; i++) {
    cam[i] = cam_bad[i];
  }
  auto apply_cam = [&](const std::array<math::Vector4f, 4>& mat, std::array<math::Vector4f, 4>& out) {
    out[0] = cam[0] * mat[0].x() + cam[1] * mat[0].y() + cam[2] * mat[0].z();
    out[1] = cam[0] * mat[1].x() + cam[1] * mat[1].y() + cam[2] * mat[1].z();
    out[2] = cam[0] * mat[2].x() + cam[1] * mat[2].y() + cam[2] * mat[2].z();
    out[3] = cam[0] * mat[3].x() + cam[1] * mat[3].y() + cam[2] * mat[3].z() + cam[3];
  };
  // vista: the bend never changes, so it is computed once and only visible instances get the camera
  const bool canopy = render_state->version == GameVersion::Jak1 && render_state->shadow.active &&
                      Gfx::g_global_settings.shadow_canopy > 0.f;
  const auto& gs = Gfx::g_global_settings;
  const bool pbr = pbr_active(render_state) && gs.pbr_background && gs.pbr_wind_tie;
  const bool ws = render_state->world_shadow.active && render_state->world_shadow.world;
  const bool world = canopy || pbr || ws;  // keep the bent world matrix per instance
  const bool vista_cache = m_my_id < 0 && m_wind_data.paused == 1 && Gfx::perf_opt(PERF_OPT_VISTA + 2);
  if (vista_cache) {
    // a cached matrix is reused only under the bit-identical camera it was made with
    if (tree.wind_cam_epoch == 0 ||
        std::memcmp(tree.wind_cam.data(), cam.data(), sizeof(cam)) != 0) {
      tree.wind_cam = cam;
      tree.wind_cam_epoch++;
    }
    if (tree.wind_bent.size() != tree.instance_info->size() ||
        tree.wind_bent_mult != m_wind_multiplier) {
      tree.wind_bent_mult = m_wind_multiplier;
      tree.wind_bent.resize(tree.instance_info->size());
      for (size_t inst_id = 0; inst_id < tree.instance_info->size(); inst_id++) {
        auto& info = tree.instance_info->operator[](inst_id);
        auto mat = info.matrix;
        do_wind_math(info.wind_idx, m_wind_vectors.data(), m_wind_data,
                     info.stiffness * m_wind_multiplier, mat);
        tree.wind_bent[inst_id] = mat;
      }
      tree.wind_inst_epoch.assign(tree.instance_info->size(), 0);
    }
  } else {
    tree.wind_bent.clear();  // the loop below overwrites wind_matrix_cache
    if (world) {
      tree.wind_world.resize(tree.instance_info->size());
    }
    // note: this isn't the most efficient because we might compute wind matrices for invisible
    // instances. TODO: add vis ids to the instance info to avoid this
    memset(tree.wind_matrix_cache.data(), 0, sizeof(float) * 16 * tree.wind_matrix_cache.size());

    for (size_t inst_id = 0; inst_id < tree.instance_info->size(); inst_id++) {
      auto& info = tree.instance_info->operator[](inst_id);
      auto& out = tree.wind_matrix_cache[inst_id];
      // auto& mat = tree.instance_info->operator[](inst_id).matrix;
      auto mat = info.matrix;

      ASSERT(info.wind_idx * 4 <= m_wind_vectors.size());
      do_wind_math(info.wind_idx, m_wind_vectors.data(), m_wind_data,
                   info.stiffness * m_wind_multiplier, mat);
      if (world) {
        tree.wind_world[inst_id] = mat;
      }

      // vmulax.xyzw acc, vf20, vf10
      // vmadday.xyzw acc, vf21, vf10
      // vmaddz.xyzw vf10, vf22, vf10
      out[0] = cam[0] * mat[0].x() + cam[1] * mat[0].y() + cam[2] * mat[0].z();

      // vmulax.xyzw acc, vf20, vf11
      // vmadday.xyzw acc, vf21, vf11
      // vmaddz.xyzw vf11, vf22, vf11
      out[1] = cam[0] * mat[1].x() + cam[1] * mat[1].y() + cam[2] * mat[1].z();

      // vmulax.xyzw acc, vf20, vf12
      // vmadday.xyzw acc, vf21, vf12
      // vmaddz.xyzw vf12, vf22, vf12
      out[2] = cam[0] * mat[2].x() + cam[1] * mat[2].y() + cam[2] * mat[2].z();

      // vmulax.xyzw acc, vf20, vf13
      // vmadday.xyzw acc, vf21, vf13
      // vmaddaz.xyzw acc, vf22, vf13
      // vmaddw.xyzw vf13, vf23, vf0
      out[3] = cam[0] * mat[3].x() + cam[1] * mat[3].y() + cam[2] * mat[3].z() + cam[3];
    }
  }

  auto shader_id = ShaderId::TIE_WIND;
  first_tfrag_draw_setup(settings.camera, render_state, shader_id);
  const GLuint wind_prog = render_state->shaders[shader_id].id();
  if (canopy) {
    canopy_set_uniforms(wind_prog, false);
  } else {
    canopy_disable(wind_prog);
  }
  world_shadows_set_uniforms(render_state, wind_prog, false);
  glUniform1f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, wind_prog, "shadow_strength"),
              render_state->shadow.active ? render_state->shadow.strength : 0.f);
  PbrDrawState pbr_draw;
  if (pbr) {
    // no sidecar records for wind instances: the sun share comes from the bake
    pbr_setup_background(render_state, wind_prog, m_unbake, nullptr, settings.camera);
    swamp_setup_background(render_state, wind_prog, m_level_name);
    pbr_draw.begin(wind_prog, m_tex_class, m_pbr_maps);
  }
  glBindVertexArray(tree.vao);
  glBindBuffer(GL_ARRAY_BUFFER, tree.vertex_buffer);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,
               render_state->no_multidraw ? tree.single_draw_index_buffer : tree.index_buffer);

  glActiveTexture(GL_TEXTURE10);
  glBindTexture(GL_TEXTURE_1D, tree.time_of_day_texture);

  glActiveTexture(GL_TEXTURE0);
  glEnable(GL_PRIMITIVE_RESTART);
  glPrimitiveRestartIndex(UINT32_MAX);

  int last_texture = -1;
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, tree.wind_vertex_index_buffer);
  const GLint cam_loc =
      GLQ_LOC(PERF_OPT_GEOMETRY + 2, render_state->shaders[shader_id].id(), "camera");
  int canopy_emitter = -1;
  // no highlight on wind TIE (the class shading here writes no highlight material): clear it
  const bool material =
      render_state->light_material && m_my_id <= (int)jak1::BucketId::SHRUB_GENERIC_LEVEL1;
  if (material) {
    SsaoRenderer::emissive_draw_buffers(true, false, true);
  }

  for (size_t draw_idx = 0; draw_idx < tree.wind_draws->size(); draw_idx++) {
    const auto& draw = tree.wind_draws->operator[](draw_idx);

    if (draw.tree_tex_id != last_texture) {
      if (draw.tree_tex_id >= 0) {
        glBindTexture(GL_TEXTURE_2D, m_textures->at(draw.tree_tex_id));
      } else {
        glBindTexture(GL_TEXTURE_2D, m_anim_slot_array->at(-(draw.tree_tex_id + 1)));
      }
      last_texture = draw.tree_tex_id;
    }
    if (pbr) {
      pbr_draw.set(draw.tree_tex_id);
    }
    auto double_draw = setup_tfrag_shader(render_state, draw.mode, shader_id);
    if (canopy) {
      const bool tex_ok = m_level && draw.tree_tex_id >= 0 &&
                          draw.tree_tex_id < (int)m_level->textures.size();
      const s64 combo = tex_ok ? (s64)m_level->textures[draw.tree_tex_id].combo_id : -1;
      canopy_set_emitter(wind_prog, canopy_keeps_light(draw.mode, combo), &canopy_emitter);
    }

    int off = 0;
    for (auto& grp : draw.instance_groups) {
      if (!m_debug_all_visible && !tree.vis_temp.at(grp.vis_idx)) {
        off += grp.num;
        continue;  // invisible, skip.
      }

      if (vista_cache && tree.wind_inst_epoch[grp.instance_idx] != tree.wind_cam_epoch) {
        tree.wind_inst_epoch[grp.instance_idx] = tree.wind_cam_epoch;
        apply_cam(tree.wind_bent[grp.instance_idx], tree.wind_matrix_cache[grp.instance_idx]);
      }
      glUniformMatrix4fv(cam_loc, 1, GL_FALSE,
                         tree.wind_matrix_cache.at(grp.instance_idx)[0].data());
      if (world) {
        const auto& w = vista_cache ? tree.wind_bent[grp.instance_idx]
                                    : tree.wind_world[grp.instance_idx];
        if (canopy || ws) {
          canopy_set_instance(wind_prog, w[0].data());
        }
        if (pbr) {
          glUniformMatrix4fv(GLQ_LOC(PERF_OPT_GEOMETRY + 2, wind_prog, "pbr_inst"), 1, GL_FALSE,
                             w[0].data());
        }
      }

      prof.add_draw_call();
      prof.add_tri(grp.num);

      glDrawElements(tree.draw_mode, grp.num, GL_UNSIGNED_INT,
                     (void*)((off + tree.wind_vertex_index_offsets.at(draw_idx)) * sizeof(u32)));
      off += grp.num;

      switch (double_draw.kind) {
        case DoubleDrawKind::NONE:
          break;
        case DoubleDrawKind::AFAIL_NO_DEPTH_WRITE:
          prof.add_draw_call();
          prof.add_tri(grp.num);
          glUniform1f(
              GLQ_LOC(PERF_OPT_GEOMETRY + 2, render_state->shaders[shader_id].id(), "alpha_min"),
              -10.f);
          glUniform1f(
              GLQ_LOC(PERF_OPT_GEOMETRY + 2, render_state->shaders[shader_id].id(), "alpha_max"),
              double_draw.aref_second);
          glDepthMask(GL_FALSE);
          glDrawElements(tree.draw_mode, draw.vertex_index_stream.size(), GL_UNSIGNED_INT,
                         (void*)0);
          break;
        default:
          ASSERT(false);
      }
    }
  }
  if (material) {
    SsaoRenderer::emissive_draw_buffers(false);
  }
}

Tie3AnotherCategory::Tie3AnotherCategory(const std::string& name,
                                         int my_id,
                                         Tie3* parent,
                                         tfrag3::TieCategory category)
    : BucketRenderer(name, my_id), m_parent(parent), m_category(category) {}

void Tie3AnotherCategory::draw_debug_window() {
  ImGui::Text("Child of this renderer:");
  m_parent->draw_debug_window();
}

void Tie3AnotherCategory::render(DmaFollower& dma,
                                 SharedRenderState* render_state,
                                 ScopedProfilerNode& prof) {
  auto first_tag = dma.current_tag();
  dma.read_and_advance();
  if (first_tag.kind != DmaTag::Kind::CNT || first_tag.qwc != 0) {
    fmt::print("Bucket renderer {} ({}) was supposed to be empty, but wasn't\n", m_my_id, m_name);
    ASSERT(false);
  }
  m_parent->render_from_another(render_state, prof, m_category);
}

Tie3WithEnvmapJak1::Tie3WithEnvmapJak1(const std::string& name, int my_id, int level_id)
    : Tie3(name, my_id, level_id, nullptr, tfrag3::TieCategory::NORMAL) {}

void Tie3WithEnvmapJak1::render(DmaFollower& dma,
                                SharedRenderState* render_state,
                                ScopedProfilerNode& prof) {
  {
    perf::Scope ps(perf::Group::TIE);
    Tie3::render(dma, render_state, prof);
  }
  if (m_enable_envmap) {
    perf::Scope ps(perf::Group::ETIE);
    render_from_another(render_state, prof, tfrag3::TieCategory::NORMAL_ENVMAP);
  }
}

void Tie3WithEnvmapJak1::draw_debug_window() {
  ImGui::Checkbox("envmap", &m_enable_envmap);
  Tie3::draw_debug_window();
}
