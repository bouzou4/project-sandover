#include "Shrub.h"

#include <algorithm>
#include <cmath>

#include "common/log/log.h"

#include "game/graphics/opengl_renderer/VistaHiZ.h"
#include "game/graphics/opengl_renderer/GlQueries.h"
#include "game/graphics/opengl_renderer/InspectRenderer.h"
#include "game/graphics/opengl_renderer/LavaField.h"
#include "game/graphics/opengl_renderer/Materials.h"
#include "game/graphics/opengl_renderer/SsaoRenderer.h"
#include "game/graphics/opengl_renderer/Swamp.h"
#include "game/graphics/opengl_renderer/buckets.h"
#include "game/graphics/opengl_renderer/background/Caustics.h"
#include "game/graphics/opengl_renderer/background/PaletteGlow.h"

Shrub::Shrub(const std::string& name, int my_id) : BucketRenderer(name, my_id) {
  m_color_result.resize(TIME_OF_DAY_COLOR_COUNT);
}

Shrub::~Shrub() {
  discard_tree_cache();
}

void Shrub::init_shaders(ShaderLibrary& shaders) {
  m_uniforms.decal = glGetUniformLocation(shaders[ShaderId::SHRUB].id(), "decal");
}

void Shrub::render(DmaFollower& dma, SharedRenderState* render_state, ScopedProfilerNode& prof) {
  if (!m_enabled) {
    while (dma.current_tag_offset() != render_state->next_bucket) {
      dma.read_and_advance();
    }
    return;
  }

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
    return;
  }
  if (dma.current_tag_offset() == render_state->next_bucket) {
    return;
  }

  auto pc_port_data = dma.read_and_advance();
  ASSERT(pc_port_data.size_bytes == sizeof(TfragPcPortData));
  memcpy(&m_pc_port_data, pc_port_data.data, sizeof(TfragPcPortData));
  apply_stereo_eye(&m_pc_port_data.camera, render_state);
  m_pc_port_data.level_name[11] = '\0';
  cave_dark_itimes(render_state, &m_pc_port_data);
  caustic_itimes(render_state, &m_pc_port_data);

  if (render_state->version >= GameVersion::Jak2) {
    // jak 2 proto visibility
    auto proto_mask_data = dma.read_and_advance();
    m_proto_vis_data = proto_mask_data.data;
    m_proto_vis_data_size = proto_mask_data.size_bytes;
  }

  while (dma.current_tag_offset() != render_state->next_bucket) {
    dma.read_and_advance();
  }

  TfragRenderSettings settings;
  settings.camera = m_pc_port_data.camera;

  settings.tree_idx = 0;

  update_render_state_from_pc_settings(render_state, m_pc_port_data);

  m_has_level = setup_for_level(m_pc_port_data.level_name, render_state);
  render_all_trees(settings, render_state, prof);
}

void Shrub::update_load(const LevelData* loader_data) {
  const tfrag3::Level* lev_data = loader_data->level.get();
  // We changed level!
  discard_tree_cache();
  m_trees.resize(lev_data->shrub_trees.size());

  size_t max_draws = 0;
  u32 time_of_day_count = 0;
  size_t max_num_grps = 0;
  size_t max_inds = 0;

  for (u32 l_tree = 0; l_tree < lev_data->shrub_trees.size(); l_tree++) {
    size_t num_grps = 0;

    const auto& tree = lev_data->shrub_trees[l_tree];
    max_draws = std::max(tree.static_draws.size(), max_draws);
    for (auto& draw : tree.static_draws) {
      (void)draw;
      // num_grps += draw.vis_groups.size(); TODO
      max_num_grps += 1;
    }
    max_num_grps = std::max(max_num_grps, num_grps);

    time_of_day_count = std::max(tree.time_of_day_colors.color_count, time_of_day_count);
    max_inds = std::max(tree.indices.size(), max_inds);
    u32 verts = tree.unpacked.vertices.size();
    glGenVertexArrays(1, &m_trees[l_tree].vao);
    glBindVertexArray(m_trees[l_tree].vao);
    m_trees[l_tree].vertex_buffer = loader_data->shrub_vertex_data[l_tree];
    m_trees[l_tree].vert_count = verts;
    m_trees[l_tree].draws = &tree.static_draws;
    m_trees[l_tree].proto_vis_mask.clear();
    m_trees[l_tree].proto_vis_mask.resize(tree.proto_names.size(), true);
    m_trees[l_tree].proto_name_to_idx.clear();
    size_t i = 0;
    for (auto& name : tree.proto_names) {
      m_trees[l_tree].proto_name_to_idx[name].push_back(i++);
    }
    m_trees[l_tree].src = &tree;
    m_trees[l_tree].chunks.clear();
    m_trees[l_tree].draw_chunks.clear();
    m_trees[l_tree].colors = &tree.time_of_day_colors;
    m_trees[l_tree].index_data = tree.indices.data();
    glBindBuffer(GL_ARRAY_BUFFER, m_trees[l_tree].vertex_buffer);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glEnableVertexAttribArray(3);

    glVertexAttribPointer(0,                                          // location 0 in the shader
                          3,                                          // 3 values per vert
                          GL_FLOAT,                                   // floats
                          GL_FALSE,                                   // normalized
                          sizeof(tfrag3::ShrubGpuVertex),             // stride
                          (void*)offsetof(tfrag3::ShrubGpuVertex, x)  // offset (0)
    );

    glVertexAttribPointer(1,                                          // location 1 in the shader
                          2,                                          // 3 values per vert
                          GL_FLOAT,                                   // floats
                          GL_FALSE,                                   // normalized
                          sizeof(tfrag3::ShrubGpuVertex),             // stride
                          (void*)offsetof(tfrag3::ShrubGpuVertex, s)  // offset (0)
    );

    glVertexAttribPointer(2,                               // location 1 in the shader
                          3,                               // 4 color components
                          GL_UNSIGNED_BYTE,                // u8
                          GL_TRUE,                         // normalized (255 becomes 1)
                          sizeof(tfrag3::ShrubGpuVertex),  //
                          (void*)offsetof(tfrag3::ShrubGpuVertex, rgba_base)  //
    );

    glVertexAttribIPointer(3,                               // location 2 in the shader
                           1,                               // 1 values per vert
                           GL_UNSIGNED_SHORT,               // u16
                           sizeof(tfrag3::ShrubGpuVertex),  // stride
                           (void*)offsetof(tfrag3::ShrubGpuVertex, color_index)  // offset (0)
    );

    glGenBuffers(1, &m_trees[l_tree].single_draw_index_buffer);
    glGenBuffers(1, &m_trees[l_tree].index_buffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_trees[l_tree].index_buffer);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, tree.indices.size() * sizeof(u32), tree.indices.data(),
                 GL_STATIC_DRAW);

    glActiveTexture(GL_TEXTURE10);
    glGenTextures(1, &m_trees[l_tree].time_of_day_texture);
    glBindTexture(GL_TEXTURE_1D, m_trees[l_tree].time_of_day_texture);
    glTexImage1D(GL_TEXTURE_1D, 0, GL_RGBA, kCausticEnvOffset + TIME_OF_DAY_COLOR_COUNT, 0, GL_RGBA,
                 GL_UNSIGNED_INT_8_8_8_8, nullptr);
    glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glBindVertexArray(0);
  }

  m_cache.multidraw_offset_per_stripdraw.resize(max_draws);
  m_cache.multidraw_count_buffer.resize(max_num_grps);
  m_cache.multidraw_index_offset_buffer.resize(max_num_grps);
  m_cache.draw_idx_temp.resize(max_draws);
  m_cache.index_temp.resize(max_inds);
  ASSERT(time_of_day_count <= TIME_OF_DAY_COLOR_COUNT);
}

bool Shrub::setup_for_level(const std::string& level, SharedRenderState* render_state) {
  // make sure we have the level data.
  Timer tfrag3_setup_timer;
  auto lev_data = render_state->loader->get_tfrag3_level(level);

  if (!lev_data) {
    // not loaded
    m_has_level = false;
    m_textures = nullptr;
    m_level_name = "";
    discard_tree_cache();
    return false;
  }

  if (m_has_level && lev_data->load_id != m_load_id) {
    m_has_level = false;
    m_textures = nullptr;
    m_level_name = "";
    discard_tree_cache();
    return setup_for_level(level, render_state);
  }

  m_textures = &lev_data->textures;
  m_unbake = lev_data->unbake.get();
  m_tex_class = &lev_data->tex_class;
  m_tex_selflit = &lev_data->tex_selflit;
  m_pbr_maps = lev_data->pbr_maps.get();
  m_load_id = lev_data->load_id;

  if (m_level_name != level) {
    update_load(lev_data);
    m_has_level = true;
    m_level_name = level;
  } else {
    m_has_level = true;
  }

  if (tfrag3_setup_timer.getMs() > 5) {
    lg::info("Shrub setup: {:.1f}ms", tfrag3_setup_timer.getMs());
  }

  return m_has_level;
}

void Shrub::discard_tree_cache() {
  for (auto& tree : m_trees) {
    glq::invalidate_state();
    caustic_forget_texture(tree.time_of_day_texture);
    palette_glow_forget(tree.time_of_day_texture);
    glBindTexture(GL_TEXTURE_1D, tree.time_of_day_texture);
    glDeleteTextures(1, &tree.time_of_day_texture);
    glDeleteBuffers(1, &tree.index_buffer);
    glDeleteBuffers(1, &tree.single_draw_index_buffer);
    glDeleteVertexArrays(1, &tree.vao);
  }

  m_trees.clear();
}

void Shrub::build_chunks(Tree& tree) {
  // a chunk ends before a strip that would stretch it past this extent (game units)
  constexpr float kChunkExtent = 24.f * 4096.f;
  const auto& verts = tree.src->unpacked.vertices;
  const auto& inds = tree.src->indices;
  tree.chunks.clear();
  tree.draw_chunks.clear();
  for (const auto& draw : *tree.draws) {
    const u32 first_chunk = tree.chunks.size();
    const u32 end = draw.first_index_index + draw.num_indices;
    math::Vector3f lo, hi;
    float max_strip = 0.f;
    u32 chunk_start = draw.first_index_index;
    bool open = false;
    auto close = [&](u32 chunk_end) {
      const math::Vector3f c = (lo + hi) * 0.5f;
      const float r = (hi - lo).length() * 0.5f;
      tree.chunks.push_back({math::Vector4f(c.x(), c.y(), c.z(), r), chunk_start,
                             chunk_end - chunk_start, max_strip});
    };
    u32 i = draw.first_index_index;
    while (i < end) {
      // one strip: [i, j) plus its restart index
      math::Vector3f slo(1e30f, 1e30f, 1e30f), shi(-1e30f, -1e30f, -1e30f);
      u32 j = i;
      for (; j < end && inds[j] != UINT32_MAX; j++) {
        const auto& v = verts.at(inds[j]);
        const math::Vector3f p(v.x, v.y, v.z);
        slo = slo.min(p);
        shi = shi.max(p);
      }
      const u32 next = j < end ? j + 1 : end;
      const math::Vector3f sext = shi - slo;
      const float strip = std::max({sext.x(), sext.y(), sext.z()});
      if (j > i) {
        if (open) {
          const math::Vector3f nlo = lo.min(slo), nhi = hi.max(shi);
          const math::Vector3f ext = nhi - nlo;
          if (std::max({ext.x(), ext.y(), ext.z()}) > kChunkExtent) {
            close(i);
            chunk_start = i;
            lo = slo;
            hi = shi;
            max_strip = strip;
          } else {
            lo = nlo;
            hi = nhi;
            max_strip = std::max(max_strip, strip);
          }
        } else {
          chunk_start = i;
          lo = slo;
          hi = shi;
          max_strip = strip;
          open = true;
        }
      }
      i = next;
    }
    if (open) {
      close(end);
    }
    tree.draw_chunks.emplace_back(first_chunk, (u32)tree.chunks.size() - first_chunk);
  }
  tree.chunk_vis.resize(tree.chunks.size());
}

bool Shrub::make_culled_multidraws(Tree& tree,
                                   const TfragRenderSettings& settings,
                                   const SharedRenderState* render_state) {
  if (!tree.src) {
    return false;
  }
  if (tree.draw_chunks.size() != tree.draws->size()) {
    build_chunks(tree);
  }
  if (m_cache.multidraw_count_buffer.size() < tree.chunks.size()) {
    m_cache.multidraw_count_buffer.resize(tree.chunks.size());
    m_cache.multidraw_index_offset_buffer.resize(tree.chunks.size());
  }
  // sub-pixel cut: chunks whose every strip spans under a pixel at 4K width or wider
  const bool subpx = Gfx::perf_opt(PERF_OPT_VISTA + 3);
  // perspective xx = x-pix / (x-ratio * d) with x-pix 256, d 1024 (math-camera.gc)
  const float xx = std::abs(settings.camera.perspective[0].x());
  const float x_ratio = xx > 0.f ? std::clamp(256.f / (xx * 1024.f), 0.1f, 5.f) : 5.f;
  const float px_per_rad = 0.5f * std::max(render_state->render_fb_w, 3840) / x_ratio;
  const math::Vector3f cam = settings.camera.trans.xyz();
  const VistaHiZ* hiz =
      m_my_id < 0 && render_state->vista_hiz && vista_hiz_applies(*tree.draws)
          ? render_state->vista_hiz
          : nullptr;
  for (size_t c = 0; c < tree.chunks.size(); c++) {
    const auto& ch = tree.chunks[c];
    bool vis = sphere_in_view_ref(ch.bsphere, settings.camera.planes);
    if (vis && subpx && px_per_rad > 0.f) {
      const float d = (ch.bsphere.xyz() - cam).length() - ch.bsphere.w();
      vis = d <= 0.f || ch.max_strip * px_per_rad >= d;
    }
    if (vis && hiz) {
      vis = hiz->visible(ch.bsphere);
    }
    tree.chunk_vis[c] = vis;
  }
  u32 md = 0;
  for (size_t d = 0; d < tree.draws->size(); d++) {
    const auto [first, count] = tree.draw_chunks[d];
    const u32 md_start = md;
    u32 run_end = UINT32_MAX;  // index just past the last emitted range, for merging
    for (u32 c = first; c < first + count; c++) {
      if (!tree.chunk_vis[c]) {
        continue;
      }
      const auto& ch = tree.chunks[c];
      if (md > md_start && run_end == ch.first_index) {
        m_cache.multidraw_count_buffer[md - 1] += ch.num_indices;
      } else {
        m_cache.multidraw_count_buffer[md] = ch.num_indices;
        m_cache.multidraw_index_offset_buffer[md] = (void*)(ch.first_index * sizeof(u32));
        md++;
      }
      run_end = ch.first_index + ch.num_indices;
    }
    m_cache.multidraw_offset_per_stripdraw[d] = {(int)md_start, (int)(md - md_start)};
  }
  return true;
}

void Shrub::render_all_trees(const TfragRenderSettings& settings,
                             SharedRenderState* render_state,
                             ScopedProfilerNode& prof) {
  for (u32 i = 0; i < m_trees.size(); i++) {
    render_tree(i, settings, render_state, prof);
  }
}

namespace {
void update_vis_mask(std::vector<bool>& vis_mask,
                     const u8* data,
                     u32 data_size,
                     const std::unordered_map<std::string, std::vector<u32>>& name_to_idx) {
  char name_buffer[256];  // ??

  for (u32 i = 0; i < vis_mask.size(); i++) {
    vis_mask[i] = true;
  }

  const u8* end = data + data_size;
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
        for (auto x : name_to_idx.at(name_buffer)) {
          vis_mask[x] = 0;
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
}  // namespace

void Shrub::render_tree(int idx,
                        const TfragRenderSettings& settings,
                        SharedRenderState* render_state,
                        ScopedProfilerNode& prof) {
  Timer tree_timer;
  auto& tree = m_trees.at(idx);
  tree.perf.draws = 0;
  tree.perf.wind_draws = 0;
  if (!m_has_level) {
    return;
  }

  if (m_color_result.size() < tree.colors->color_count) {
    m_color_result.resize(tree.colors->color_count);
  }

  Timer interp_timer;
  interp_time_of_day(settings.camera.itimes, *tree.colors, m_color_result.data());
  tree.perf.tod_time.add(interp_timer.getSeconds());

  Timer setup_timer;
  glActiveTexture(GL_TEXTURE10);
  glBindTexture(GL_TEXTURE_1D, tree.time_of_day_texture);
  glTexSubImage1D(GL_TEXTURE_1D, 0, 0, tree.colors->color_count, GL_RGBA,
                  GL_UNSIGNED_INT_8_8_8_8_REV, m_color_result.data());

  first_tfrag_draw_setup(settings.camera, render_state, ShaderId::SHRUB);
  caustic_setup_tree(render_state, render_state->shaders[ShaderId::SHRUB].id(), m_level_name,
                     settings.camera.itimes, *tree.colors, tree.time_of_day_texture,
                     CausticKind::SHRUB);
  palette_glow_setup_tree(render_state, render_state->shaders[ShaderId::SHRUB].id(), m_unbake,
                          *tree.colors, tree.time_of_day_texture);
  const bool pbr = pbr_active(render_state);
  PbrDrawState pbr_draw;
  if (pbr) {
    const GLuint id = render_state->shaders[ShaderId::SHRUB].id();
    pbr_setup_background(render_state, id, m_unbake, nullptr, settings.camera);
    // the rain cover binds the heightfield from unit 0, the renderer convention between passes
    glActiveTexture(GL_TEXTURE0);
    swamp_setup_background(render_state, id, m_level_name);
    lava_field::setup_background(render_state, id, m_level_name);
    pbr_draw.begin(id, m_tex_class, m_pbr_maps);
  }
  SelfLitDraw selflit;
  selflit.begin(render_state, render_state->shaders[ShaderId::SHRUB].id(), m_tex_selflit);
  // highlight material only: shrubs leave the emissive mask's E as it is
  const bool material =
      render_state->light_material && m_my_id <= (int)jak1::BucketId::SHRUB_GENERIC_LEVEL1;
  int hl_blend = -1;  // material_blend_guard sets the material output per draw

  glBindVertexArray(tree.vao);
  glBindBuffer(GL_ARRAY_BUFFER, tree.vertex_buffer);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,
               render_state->no_multidraw ? tree.single_draw_index_buffer : tree.index_buffer);
  glActiveTexture(GL_TEXTURE0);
  glEnable(GL_PRIMITIVE_RESTART);
  glPrimitiveRestartIndex(UINT32_MAX);
  if (m_proto_vis_data) {
    update_vis_mask(tree.proto_vis_mask, m_proto_vis_data, m_proto_vis_data_size,
                    tree.proto_name_to_idx);
  }
  tree.perf.tod_time.add(setup_timer.getSeconds());

  int last_texture = -1;
  int depth_only = -1;
  bool em_on = false;
  bool draw_em = false;  // this draw writes lava E (material on)

  tree.perf.cull_time.add(0);
  Timer index_timer;
  bool culled = false;
  if (render_state->no_multidraw) {
    u32 idx_buffer_size = make_all_visible_index_list(
        m_cache.draw_idx_temp.data(), m_cache.index_temp.data(), *tree.draws, tree.index_data);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx_buffer_size * sizeof(u32), m_cache.index_temp.data(),
                 GL_STREAM_DRAW);
  } else if ((m_my_id < 0 ? Gfx::perf_opt(PERF_OPT_VISTA + 1)
                          : Gfx::perf_opt(PERF_OPT_GEOMETRY + 3)) &&
             make_culled_multidraws(tree, settings, render_state)) {
    culled = true;
  } else {
    make_all_visible_multidraws(m_cache.multidraw_offset_per_stripdraw.data(),
                                m_cache.multidraw_count_buffer.data(),
                                m_cache.multidraw_index_offset_buffer.data(), *tree.draws);
  }

  tree.perf.index_time.add(index_timer.getSeconds());

  Timer draw_timer;

  for (size_t draw_idx = 0; draw_idx < tree.draws->size(); draw_idx++) {
    const auto& draw = tree.draws->operator[](draw_idx);
    if (!tree.proto_vis_mask.at(draw.proto_idx)) {
      continue;
    }
    const auto& multidraw_indices = m_cache.multidraw_offset_per_stripdraw[draw_idx];
    const auto& singledraw_indices = m_cache.draw_idx_temp[draw_idx];

    if (render_state->no_multidraw) {
      if (singledraw_indices.second == 0) {
        continue;
      }
    } else {
      if (multidraw_indices.second == 0) {
        continue;
      }
    }

    if ((int)draw.tree_tex_id != last_texture) {
      glBindTexture(GL_TEXTURE_2D, m_textures->at(draw.tree_tex_id));
      last_texture = draw.tree_tex_id;
    }
    selflit.set((s32)draw.tree_tex_id);
    if (pbr) {
      pbr_draw.set((s32)draw.tree_tex_id);
      const bool em = lava_field::shrub_writes_emissive(
          render_state, pbr_texture_class(m_tex_class, (s32)draw.tree_tex_id));
      if (material) {
        draw_em = em;
      } else if (em != em_on) {
        em_on = em;
        SsaoRenderer::emissive_draw_buffers(em);
      }
    }

    glUniform1i(m_uniforms.decal, draw.mode.get_decal() ? 1 : 0);

    auto double_draw = setup_tfrag_shader(render_state, draw.mode, ShaderId::SHRUB);
    if (!bg_prepass_draw(render_state, draw.mode, double_draw,
                         render_state->shaders[ShaderId::SHRUB].id(), depth_only)) {
      continue;
    }
    if (material) {
      SsaoRenderer::material_blend_guard(render_state->shaders[ShaderId::SHRUB].id(),
                                         SsaoRenderer::draw_blends(draw.mode), draw_em, hl_blend);
    }
    if (inspect::active()) {
      inspect::DrawRecord rec;
      rec.kind = inspect::SHRUB;
      rec.level = m_level_name;
      rec.geom = 0;
      rec.tree = idx;
      rec.draw = (int)draw_idx;
      rec.tex = (s32)draw.tree_tex_id;
      rec.mode = draw.mode;
      std::copy(settings.camera.itimes, settings.camera.itimes + 4, rec.itimes);
      rec.cam_trans = settings.camera.trans;
      inspect::set(render_state->shaders[ShaderId::SHRUB].id(), inspect::add(std::move(rec)));
    }

    prof.add_draw_call();
    if (culled) {
      u64 n = 0;
      for (int k = 0; k < multidraw_indices.second; k++) {
        n += m_cache.multidraw_count_buffer[multidraw_indices.first + k];
      }
      prof.add_tri((int)(draw.num_triangles * n / std::max(draw.num_indices, 1u)));
    } else {
      prof.add_tri(draw.num_triangles);
    }

    tree.perf.draws++;

    if (render_state->no_multidraw) {
      glDrawElements(GL_TRIANGLE_STRIP, singledraw_indices.second, GL_UNSIGNED_INT,
                     (void*)(singledraw_indices.first * sizeof(u32)));
    } else {
      glMultiDrawElements(GL_TRIANGLE_STRIP,
                          &m_cache.multidraw_count_buffer[multidraw_indices.first], GL_UNSIGNED_INT,
                          &m_cache.multidraw_index_offset_buffer[multidraw_indices.first],
                          multidraw_indices.second);
    }
    if (render_state->bg_prepass) {
      continue;
    }

    switch (double_draw.kind) {
      case DoubleDrawKind::NONE:
        break;
      case DoubleDrawKind::AFAIL_NO_DEPTH_WRITE:
        tree.perf.draws++;
        prof.add_draw_call();
        glUniform1f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, render_state->shaders[ShaderId::SHRUB].id(),
                            "alpha_min"),
                    -10.f);
        glUniform1f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, render_state->shaders[ShaderId::SHRUB].id(),
                            "alpha_max"),
                    double_draw.aref_second);
        glDepthMask(GL_FALSE);
        if (render_state->no_multidraw) {
          glDrawElements(GL_TRIANGLE_STRIP, singledraw_indices.second, GL_UNSIGNED_INT,
                         (void*)(singledraw_indices.first * sizeof(u32)));
        } else {
          glMultiDrawElements(
              GL_TRIANGLE_STRIP, &m_cache.multidraw_count_buffer[multidraw_indices.first],
              GL_UNSIGNED_INT, &m_cache.multidraw_index_offset_buffer[multidraw_indices.first],
              multidraw_indices.second);
        }
        break;
      default:
        ASSERT(false);
    }
  }

  if (em_on || material) {
    SsaoRenderer::emissive_draw_buffers(false);
  }
  glBindVertexArray(0);
  tree.perf.draw_time.add(draw_timer.getSeconds());
  tree.perf.tree_time.add(tree_timer.getSeconds());
}

void Shrub::draw_debug_window() {}
