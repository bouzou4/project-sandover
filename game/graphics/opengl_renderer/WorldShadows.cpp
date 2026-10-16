#include "WorldShadows.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <set>
#include <string>
#include <unordered_map>

#include "common/log/log.h"

#include "game/graphics/gfx.h"
#include "game/graphics/opengl_renderer/PerfTimers.h"
#include "game/graphics/opengl_renderer/background/Unbake.h"
#include "game/graphics/opengl_renderer/background/background_common.h"
#include "game/graphics/opengl_renderer/loader/Loader.h"
#include "game/graphics/opengl_renderer/loader/common.h"

#include "third-party/imgui/imgui.h"

namespace {

constexpr float kMeter = 4096.f;
constexpr float kTowardSunM = 300.f;      // casters this far beyond a box toward the sun still count
constexpr u32 kShrubChunkIndices = 1536;  // shrub index runs culled together
constexpr float kWindRadiusM = 20.f;      // bound of a wind tie instance around its origin
constexpr int kOptBit = PERF_OPT_WORLD_PREPASS;
constexpr u32 kQueryEvery = 16;  // with kOptBit: one pass timer query per this many frames

using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point t) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

using Mat4 = std::array<float, 16>;  // column-major

Mat4 mul(const Mat4& a, const Mat4& b) {
  Mat4 r{};
  for (int c = 0; c < 4; c++) {
    for (int row = 0; row < 4; row++) {
      float s = 0;
      for (int k = 0; k < 4; k++) {
        s += a[k * 4 + row] * b[c * 4 + k];
      }
      r[c * 4 + row] = s;
    }
  }
  return r;
}

void basis(const math::Vector3f& f, math::Vector3f* r, math::Vector3f* u) {
  math::Vector3f up = std::abs(f.y()) < 0.99f ? math::Vector3f(0, 1, 0) : math::Vector3f(1, 0, 0);
  *r = up.cross(f).normalized();
  *u = f.cross(*r);
}

float smooth(float a, float b, float x) {
  const float t = std::clamp((x - a) / (b - a), 0.f, 1.f);
  return t * t * (3.f - 2.f * t);
}

/// How a draw casts: nullopt-like `cast = false` for draws that never block the sun.
bool caster_of(const DrawMode& m, s32 tex, float* aref_out) {
  if (!m.get_zt_enable()) {
    return false;
  }
  const auto ab = m.get_alpha_blend();
  const bool blend = m.get_ab_enable() && ab != DrawMode::AlphaBlend::DISABLED &&
                     ab != DrawMode::AlphaBlend::SRC_SRC_SRC_SRC;
  if (blend && ab != DrawMode::AlphaBlend::SRC_DST_SRC_DST) {
    return false;  // additive, subtractive, half-transparent: glows and effects
  }
  if (!m.get_depth_write_enable()) {
    return false;
  }
  if (m.get_at_enable() && m.get_alpha_test() == DrawMode::AlphaTest::NEVER) {
    return false;
  }
  float aref = -1.f;
  if (m.get_at_enable() && m.get_alpha_test() == DrawMode::AlphaTest::GEQUAL) {
    aref = m.get_aref() / 127.f;
  }
  if (blend) {
    aref = std::max(aref, 0.5f);
  }
  if (tex < 0) {
    aref = -1.f;  // animated slot textures: no per-texel alpha here
  }
  *aref_out = aref;
  return true;
}

/// A blended draw whose texels that pass its alpha reference are mostly see-through (ice, glass,
/// water, decals): it lets most of the sun through, while cut-out foliage passes opaque texels.
bool translucent_sheet(const tfrag3::Level& L, s32 tex, float aref) {
  if (tex < 0 || (size_t)tex >= L.textures.size() || L.textures[tex].data.empty()) {
    return false;
  }
  const u32 cut = (u32)std::ceil(std::max(aref, 0.f) * 127.5f);  // alpha * 2 / 255 >= aref
  const u32 opaque = 115;                                        // 0.9 of the 128 convention
  size_t pass = 0, thin = 0;
  for (u32 p : L.textures[tex].data) {
    const u32 a = p >> 24;
    if (a >= cut) {
      pass++;
      thin += a < opaque ? 1 : 0;
    }
  }
  return pass > 0 && thin * 2 > pass;
}

int wrap_of(const DrawMode& m) {
  return (m.get_clamp_s_enable() ? 1 : 0) | (m.get_clamp_t_enable() ? 2 : 0);
}

/// Transposed side planes of a light-space box (background_common's cull layout).
void box_planes(const math::Vector3f& r,
                const math::Vector3f& u,
                const math::Vector3f& o,
                float half,
                math::Vector4f out[4]) {
  const math::Vector3f n[4] = {r, r * -1.f, u, u * -1.f};
  for (int i = 0; i < 4; i++) {
    const float d = n[i].dot(o) - half;
    out[0][i] = n[i].x();
    out[1][i] = n[i].y();
    out[2][i] = n[i].z();
    out[3][i] = d;
  }
}

void set_attribs(GLuint vbo, size_t stride, size_t pos_off, size_t uv_off) {
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)pos_off);
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (void*)uv_off);
}

/// Median ambient gain a of a sidecar's fitted vertices (records x & 0xffff / 16384, q > 0).
float median_ambient_gain(const UnbakeSidecar& sc) {
  std::vector<float> a;
  for (const auto* trees : {&sc.tfrag, &sc.tie}) {
    for (const auto& t : *trees) {
      if (!t.valid) {
        continue;
      }
      for (size_t v = 0; v < t.vertex_count; v += 7) {
        const u32* rec = &t.records[4 * v];
        if ((rec[3] & 0xffu) > 0) {
          a.push_back((rec[0] & 0xffffu) / 16384.f);
        }
      }
    }
  }
  if (a.empty()) {
    return 0.25f;
  }
  std::nth_element(a.begin(), a.begin() + a.size() / 2, a.end());
  return a[a.size() / 2];
}

/// A sphere holding every vis node's sphere, or false when a group is drawn without a node.
bool tree_bound(const tfrag3::BVH& bvh,
                const std::vector<tfrag3::StripDraw>& draws,
                math::Vector4f* out) {
  if (bvh.vis_nodes.empty()) {
    return false;
  }
  for (const auto& d : draws) {
    for (const auto& g : d.vis_groups) {
      if (g.vis_idx_in_pc_bvh == UINT16_MAX) {
        return false;
      }
    }
  }
  math::Vector3f lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
  for (const auto& n : bvh.vis_nodes) {
    for (int k = 0; k < 3; k++) {
      lo[k] = std::min(lo[k], n.bsphere[k] - n.bsphere.w());
      hi[k] = std::max(hi[k], n.bsphere[k] + n.bsphere.w());
    }
  }
  const math::Vector3f mid = (lo + hi) * 0.5f;
  float rad = 0.f;
  for (const auto& n : bvh.vis_nodes) {
    const math::Vector3f c(n.bsphere.x(), n.bsphere.y(), n.bsphere.z());
    rad = std::max(rad, (c - mid).length() + n.bsphere.w());
  }
  // padded past float rounding: a tree is skipped only when every node would be culled
  *out = math::Vector4f(mid.x(), mid.y(), mid.z(), rad * 1.001f + 64.f);
  return true;
}

}  // namespace

float world_shadows_sun_strength(float day, float sun_y) {
  // hidden at night and with the sun at the horizon
  return smooth(0.05f, 0.3f, day) * smooth(0.03f, 0.08f, sun_y);
}

WorldShadows::WorldShadows() = default;

WorldShadows::~WorldShadows() {
  for (auto& [k, c] : m_levels) {
    release(c);
  }
  if (m_tex) {
    glDeleteTextures(1, &m_tex);
    glDeleteFramebuffers(WorldShadowFrame::kMaxCascades, m_fbo);
  }
  for (GLuint s : {m_sampler_cmp, m_sampler_raw, m_sampler_alpha[0], m_sampler_alpha[1],
                   m_sampler_alpha[2], m_sampler_alpha[3]}) {
    if (s) {
      glDeleteSamplers(1, &s);
    }
  }
  if (m_query) {
    glDeleteQueries(1, &m_query);
  }
  if (m_indirect) {
    glDeleteBuffers(1, &m_indirect);
  }
}

void WorldShadows::ensure_target(int size) {
  if (!m_sampler_cmp) {
    glGenSamplers(1, &m_sampler_cmp);
    glSamplerParameteri(m_sampler_cmp, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glSamplerParameteri(m_sampler_cmp, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    glSamplerParameteri(m_sampler_cmp, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glSamplerParameteri(m_sampler_cmp, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glSamplerParameteri(m_sampler_cmp, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(m_sampler_cmp, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenSamplers(1, &m_sampler_raw);
    glSamplerParameteri(m_sampler_raw, GL_TEXTURE_COMPARE_MODE, GL_NONE);
    glSamplerParameteri(m_sampler_raw, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glSamplerParameteri(m_sampler_raw, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glSamplerParameteri(m_sampler_raw, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(m_sampler_raw, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenSamplers(4, m_sampler_alpha);
    for (int w = 0; w < 4; w++) {
      const GLuint a = m_sampler_alpha[w];
      glSamplerParameteri(a, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glSamplerParameteri(a, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glSamplerParameteri(a, GL_TEXTURE_WRAP_S, (w & 1) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
      glSamplerParameteri(a, GL_TEXTURE_WRAP_T, (w & 2) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    }
  }
  if (m_tex && m_size == size) {
    return;
  }
  if (m_tex) {
    glDeleteTextures(1, &m_tex);
    glDeleteFramebuffers(WorldShadowFrame::kMaxCascades, m_fbo);
  }
  m_size = size;
  m_gen++;
  glGenTextures(1, &m_tex);
  glBindTexture(GL_TEXTURE_2D_ARRAY, m_tex);
  glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH_COMPONENT32F, size, size,
               WorldShadowFrame::kMaxCascades, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
  glGenFramebuffers(WorldShadowFrame::kMaxCascades, m_fbo);
  for (int c = 0; c < WorldShadowFrame::kMaxCascades; c++) {
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo[c]);
    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, m_tex, 0, c);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    ASSERT(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  lg::info("world-shadows: {}x{} x {} cascades, {:.0f} MB", size, size,
           WorldShadowFrame::kMaxCascades,
           size * (double)size * WorldShadowFrame::kMaxCascades * 4 / (1024.0 * 1024.0));
}

WorldShadows::LevelCache WorldShadows::build(const LevelData* lev) {
  LevelCache c;
  c.level = lev;
  c.load_id = lev->load_id;
  const bool fast = Gfx::perf_opt(kOptBit);
  const auto t_start = Clock::now();
  size_t tfrag_index_bytes = 0;
  const auto& L = *lev->level;
  std::unordered_map<s32, bool> sheet;
  std::set<std::string> dropped;
  // translucent SRC_DST sheets cast nothing (the bake does not shadow under them either)
  auto cast_of = [&](const DrawMode& m, s32 tex, float* aref) {
    if (!caster_of(m, tex, aref)) {
      return false;
    }
    const bool blend =
        m.get_ab_enable() && m.get_alpha_blend() == DrawMode::AlphaBlend::SRC_DST_SRC_DST;
    if (!blend || *aref < 0.f) {
      return true;
    }
    auto it = sheet.find(tex);
    if (it == sheet.end()) {
      it = sheet.emplace(tex, translucent_sheet(L, tex, *aref)).first;
      if (it->second) {
        dropped.insert(L.textures[tex].debug_name);
      }
    }
    return !it->second;
  };
  auto casters = [&](const std::vector<tfrag3::StripDraw>& draws, int from, int to, StaticTree* t) {
    for (int i = from; i < to; i++) {
      float aref;
      if (cast_of(draws[i].mode, draws[i].tree_tex_id, &aref)) {
        t->draw_ids.push_back(i);
        t->cast[i] = {draws[i].tree_tex_id, aref, wrap_of(draws[i].mode)};
      }
    }
  };
  auto finish = [&](StaticTree& t) {
    size_t groups = 0;
    for (const auto& d : *t.draws) {
      groups += d.vis_groups.size();
    }
    t.vis.resize(t.bvh->vis_nodes.size());
    t.per_draw.resize(t.draws->size());
    t.counts.resize(groups);
    t.offsets.resize(groups);
  };
  using K = tfrag3::TFragmentTreeKind;
  for (size_t i = 0; i < L.tfrag_trees[0].size(); i++) {
    const auto& tree = L.tfrag_trees[0][i];
    if (tree.kind != K::NORMAL && tree.kind != K::TRANS && tree.kind != K::DIRT &&
        tree.kind != K::ICE) {
      continue;
    }
    StaticTree t;
    t.draws = &tree.draws;
    t.bvh = &tree.bvh;
    t.mode = tree.use_strips ? GL_TRIANGLE_STRIP : GL_TRIANGLES;
    t.cast.resize(tree.draws.size());
    casters(tree.draws, 0, (int)tree.draws.size(), &t);
    if (t.draw_ids.empty()) {
      continue;
    }
    glGenVertexArrays(1, &t.vao);
    glBindVertexArray(t.vao);
    set_attribs(lev->tfrag_vertex_data[0][i], sizeof(tfrag3::PreloadedVertex),
                offsetof(tfrag3::PreloadedVertex, x), offsetof(tfrag3::PreloadedVertex, s));
    if (fast && i < lev->tfrag_index_data.size()) {
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, lev->tfrag_index_data[i]);  // the loader's
    } else {
      glGenBuffers(1, &t.own_index_buffer);
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, t.own_index_buffer);
      glBufferData(GL_ELEMENT_ARRAY_BUFFER, tree.unpacked.indices.size() * sizeof(u32),
                   tree.unpacked.indices.data(), GL_STATIC_DRAW);
      tfrag_index_bytes += tree.unpacked.indices.size() * sizeof(u32);
    }
    finish(t);
    t.bounded = tree_bound(tree.bvh, tree.draws, &t.bound);
    c.statics.push_back(std::move(t));
  }
  const double ms_tfrag = ms_since(t_start);
  using C = tfrag3::TieCategory;
  for (size_t i = 0; i < L.tie_trees[0].size(); i++) {
    const auto& tree = L.tie_trees[0][i];
    const auto& gl = lev->tie_data[0][i];
    StaticTree t;
    t.draws = &tree.static_draws;
    t.bvh = &tree.bvh;
    t.mode = tree.use_strips ? GL_TRIANGLE_STRIP : GL_TRIANGLES;
    t.cast.resize(tree.static_draws.size());
    for (C cat : {C::NORMAL, C::TRANS, C::NORMAL_ENVMAP, C::TRANS_ENVMAP}) {
      casters(tree.static_draws, tree.category_draw_indices[(int)cat],
              tree.category_draw_indices[(int)cat + 1], &t);
    }
    if (!t.draw_ids.empty()) {
      std::sort(t.draw_ids.begin(), t.draw_ids.end());
      glGenVertexArrays(1, &t.vao);
      glBindVertexArray(t.vao);
      set_attribs(gl.vertex_buffer, sizeof(tfrag3::PreloadedVertex),
                  offsetof(tfrag3::PreloadedVertex, x), offsetof(tfrag3::PreloadedVertex, s));
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl.index_buffer);
      finish(t);
      t.bounded = tree_bound(tree.bvh, tree.static_draws, &t.bound);
      c.statics.push_back(std::move(t));
    }
    if (gl.has_wind && !tree.instanced_wind_draws.empty()) {
      WindTree w;
      w.src = &tree;
      u32 off = 0;
      for (const auto& d : tree.instanced_wind_draws) {
        w.draw_offsets.push_back(off);
        off += d.vertex_index_stream.size();
        float aref = -1.f;
        const bool cast = cast_of(d.mode, d.tree_tex_id, &aref);
        w.cast.push_back({cast ? d.tree_tex_id : -2, aref, wrap_of(d.mode)});
      }
      glGenVertexArrays(1, &w.vao);
      glBindVertexArray(w.vao);
      set_attribs(gl.vertex_buffer, sizeof(tfrag3::PreloadedVertex),
                  offsetof(tfrag3::PreloadedVertex, x), offsetof(tfrag3::PreloadedVertex, s));
      if (fast && GLAD_GL_VERSION_4_3) {
        // instance matrices as a per-instance attribute: one indirect draw per draw and cascade
        const auto& info = tree.wind_instance_info;
        std::vector<float> mats(info.size() * 16);
        for (size_t k = 0; k < info.size(); k++) {
          for (int col = 0; col < 4; col++) {
            for (int r = 0; r < 3; r++) {
              mats[k * 16 + col * 4 + r] = info[k].matrix[col][r];
            }
            mats[k * 16 + col * 4 + 3] = col == 3 ? 1.f : 0.f;
          }
        }
        glGenBuffers(1, &w.inst_buffer);
        glBindBuffer(GL_ARRAY_BUFFER, w.inst_buffer);
        glBufferData(GL_ARRAY_BUFFER, mats.size() * sizeof(float), mats.data(), GL_STATIC_DRAW);
        for (int col = 0; col < 4; col++) {
          glEnableVertexAttribArray(2 + col);
          glVertexAttribPointer(2 + col, 4, GL_FLOAT, GL_FALSE, 16 * sizeof(float),
                                (void*)(col * 4 * sizeof(float)));
          glVertexAttribDivisor(2 + col, 1);
        }
        w.groups.resize(tree.instanced_wind_draws.size());
        for (size_t di = 0; di < tree.instanced_wind_draws.size(); di++) {
          if (w.cast[di].tex == -2) {
            continue;
          }
          u32 off = 0;
          for (const auto& grp : tree.instanced_wind_draws[di].instance_groups) {
            const auto& m = info.at(grp.instance_idx).matrix;
            WindGroup g;
            g.first = off + w.draw_offsets[di];
            g.count = grp.num;
            g.inst = grp.instance_idx;
            g.sphere = math::Vector4f(m[3].x(), m[3].y(), m[3].z(), kWindRadiusM * kMeter);
            w.groups[di].push_back(g);
            off += grp.num;
          }
        }
      }
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl.wind_indices);
      c.winds.push_back(std::move(w));
    }
  }
  const double ms_tie = ms_since(t_start) - ms_tfrag;
  for (size_t i = 0; i < L.shrub_trees.size(); i++) {
    const auto& tree = L.shrub_trees[i];
    ShrubTree s;
    s.chunks.resize(tree.static_draws.size());
    s.cast.resize(tree.static_draws.size(), {-2, -1.f});
    const auto& verts = tree.unpacked.vertices;
    bool any = false;
    for (size_t d = 0; d < tree.static_draws.size(); d++) {
      const auto& draw = tree.static_draws[d];
      float aref;
      if (!cast_of(draw.mode, (s32)draw.tree_tex_id, &aref)) {
        continue;
      }
      any = true;
      s.cast[d] = {(s32)draw.tree_tex_id, aref, wrap_of(draw.mode)};
      // split the strip stream at restarts into runs of about kShrubChunkIndices
      const u32 end = draw.first_index_index + draw.num_indices;
      u32 start = draw.first_index_index;
      math::Vector3f lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
      for (u32 k = draw.first_index_index; k <= end; k++) {
        const bool last = k == end;
        const u32 idx = last ? UINT32_MAX : tree.indices[k];
        if (idx != UINT32_MAX && idx < verts.size()) {
          const auto& v = verts[idx];
          lo = math::Vector3f(std::min(lo.x(), v.x), std::min(lo.y(), v.y), std::min(lo.z(), v.z));
          hi = math::Vector3f(std::max(hi.x(), v.x), std::max(hi.y(), v.y), std::max(hi.z(), v.z));
        }
        if (last || (idx == UINT32_MAX && k + 1 - start >= kShrubChunkIndices)) {
          const u32 stop = last ? end : k + 1;
          if (stop > start && hi.x() >= lo.x()) {
            const math::Vector3f mid = (lo + hi) * 0.5f;
            Chunk ch;
            ch.first = start;
            ch.count = stop - start;
            ch.sphere = math::Vector4f(mid.x(), mid.y(), mid.z(), (hi - lo).length() * 0.5f);
            s.chunks[d].push_back(ch);
          }
          start = stop;
          lo = math::Vector3f(1e30f, 1e30f, 1e30f);
          hi = math::Vector3f(-1e30f, -1e30f, -1e30f);
        }
      }
    }
    if (!any) {
      continue;
    }
    glGenVertexArrays(1, &s.vao);
    glBindVertexArray(s.vao);
    set_attribs(lev->shrub_vertex_data[i], sizeof(tfrag3::ShrubGpuVertex),
                offsetof(tfrag3::ShrubGpuVertex, x), offsetof(tfrag3::ShrubGpuVertex, s));
    glGenBuffers(1, &s.index_buffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s.index_buffer);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, tree.indices.size() * sizeof(u32), tree.indices.data(),
                 GL_STATIC_DRAW);
    c.shrubs.push_back(std::move(s));
  }
  glBindVertexArray(0);
  std::string names;
  for (const auto& n : dropped) {
    names += " " + n;
  }
  lg::info("world-shadows: {} translucent sheets cast nothing:{}", lev->level->level_name,
           names.empty() ? " none" : names);
  const double ms_all = ms_since(t_start);
  lg::info(
      "world-shadows: {} caster cache built in {:.1f} ms (tfrag {:.1f} ms, {:.1f} MB indices; tie "
      "{:.1f} ms; shrub {:.1f} ms)",
      lev->level->level_name, ms_all, ms_tfrag, tfrag_index_bytes / (1024.0 * 1024.0), ms_tie,
      ms_all - ms_tfrag - ms_tie);
  return c;
}

void WorldShadows::release(LevelCache& c) {
  for (auto& t : c.statics) {
    glDeleteVertexArrays(1, &t.vao);
    if (t.own_index_buffer) {
      glDeleteBuffers(1, &t.own_index_buffer);
    }
  }
  for (auto& s : c.shrubs) {
    glDeleteVertexArrays(1, &s.vao);
    glDeleteBuffers(1, &s.index_buffer);
  }
  for (auto& w : c.winds) {
    glDeleteVertexArrays(1, &w.vao);
    if (w.inst_buffer) {
      glDeleteBuffers(1, &w.inst_buffer);
    }
  }
  c.statics.clear();
  c.shrubs.clear();
  c.winds.clear();
}

void WorldShadows::draw_level(LevelCache& c,
                              const math::Vector4f planes[4],
                              const Locs& loc,
                              bool fast,
                              int* draws,
                              u64* tris) {
  const auto& textures = c.level->textures;
  s32 last_tex = -3;
  float last_aref = -2.f;
  int last_wrap = -1;
  auto use = [&](const CasterDraw& d) {
    if (d.aref != last_aref) {
      glUniform1f(loc.aref, d.aref);
      last_aref = d.aref;
    }
    if (d.aref >= 0.f && d.wrap != last_wrap) {
      glBindSampler(0, m_sampler_alpha[d.wrap]);
      last_wrap = d.wrap;
    }
    if (d.aref >= 0.f && d.tex >= 0 && d.tex != last_tex && d.tex < (s32)textures.size()) {
      glBindTexture(GL_TEXTURE_2D, textures[d.tex]);
      last_tex = d.tex;
    }
  };

  glUniform1i(loc.inst_on, 0);
  glUniform1f(loc.uv, 1.f);
  for (auto& t : c.statics) {
    if (fast && t.bounded && !sphere_in_view_ref(t.bound, planes)) {
      continue;
    }
    cull_check_all_slow(planes, t.bvh->vis_nodes, nullptr, t.vis.data());
    *tris += make_multidraws_from_vis_string(t.per_draw.data(), t.counts.data(), t.offsets.data(),
                                             *t.draws, t.vis);
    glBindVertexArray(t.vao);
    for (int id : t.draw_ids) {
      const auto& md = t.per_draw[id];
      if (md.second == 0) {
        continue;
      }
      use(t.cast[id]);
      glMultiDrawElements(t.mode, &t.counts[md.first], GL_UNSIGNED_INT, &t.offsets[md.first],
                          md.second);
      (*draws)++;
    }
  }

  glUniform1f(loc.uv, 1.f / 4096.f);
  std::vector<GLsizei> counts;
  std::vector<void*> offsets;
  for (auto& s : c.shrubs) {
    glBindVertexArray(s.vao);
    for (size_t d = 0; d < s.chunks.size(); d++) {
      if (s.cast[d].tex == -2 || s.chunks[d].empty()) {
        continue;
      }
      counts.clear();
      offsets.clear();
      for (const auto& ch : s.chunks[d]) {
        if (sphere_in_view_ref(ch.sphere, planes)) {
          counts.push_back(ch.count);
          offsets.push_back((void*)(ch.first * sizeof(u32)));
          *tris += ch.count / 3;
        }
      }
      if (counts.empty()) {
        continue;
      }
      use(s.cast[d]);
      glMultiDrawElements(GL_TRIANGLE_STRIP, counts.data(), GL_UNSIGNED_INT, offsets.data(),
                          (GLsizei)counts.size());
      (*draws)++;
    }
  }

  glUniform1f(loc.uv, 1.f);
  int inst_on = -1;
  for (auto& w : c.winds) {
    const GLenum mode = w.src->use_strips ? GL_TRIANGLE_STRIP : GL_TRIANGLES;
    if (fast && w.inst_buffer) {
      m_cmds.clear();
      m_cmd_ranges.clear();
      for (const auto& groups : w.groups) {
        const u32 start = (u32)m_cmds.size();
        for (const auto& g : groups) {
          if (sphere_in_view_ref(g.sphere, planes)) {
            m_cmds.push_back({g.count, 1, g.first, 0, g.inst});
            *tris += g.count / 3;
          }
        }
        m_cmd_ranges.push_back({start, (u32)m_cmds.size() - start});
      }
      if (m_cmds.empty()) {
        continue;
      }
      if (!m_indirect) {
        glGenBuffers(1, &m_indirect);
      }
      glBindBuffer(GL_DRAW_INDIRECT_BUFFER, m_indirect);
      glBufferData(GL_DRAW_INDIRECT_BUFFER, m_cmds.size() * sizeof(IndirectCmd), m_cmds.data(),
                   GL_STREAM_DRAW);
      if (inst_on != 2) {
        glUniform1i(loc.inst_on, 2);
        inst_on = 2;
      }
      glBindVertexArray(w.vao);
      for (size_t di = 0; di < m_cmd_ranges.size(); di++) {
        const auto& [first, count] = m_cmd_ranges[di];
        if (count == 0) {
          continue;
        }
        use(w.cast[di]);
        glMultiDrawElementsIndirect(mode, GL_UNSIGNED_INT, (void*)(first * sizeof(IndirectCmd)),
                                    (GLsizei)count, 0);
        (*draws)++;
      }
      glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
      continue;
    }
    if (inst_on != 1) {
      glUniform1i(loc.inst_on, 1);
      inst_on = 1;
    }
    glBindVertexArray(w.vao);
    for (size_t di = 0; di < w.src->instanced_wind_draws.size(); di++) {
      const auto& draw = w.src->instanced_wind_draws[di];
      u32 off = 0;
      for (const auto& grp : draw.instance_groups) {
        const u32 first = off;
        off += grp.num;
        if (w.cast[di].tex == -2) {
          continue;
        }
        const auto& m = w.src->wind_instance_info.at(grp.instance_idx).matrix;
        const math::Vector4f sph(m[3].x(), m[3].y(), m[3].z(), kWindRadiusM * kMeter);
        if (!sphere_in_view_ref(sph, planes)) {
          continue;
        }
        float inst[16];
        for (int col = 0; col < 4; col++) {
          for (int k = 0; k < 3; k++) {
            inst[col * 4 + k] = m[col][k];
          }
          inst[col * 4 + 3] = col == 3 ? 1.f : 0.f;
        }
        glUniformMatrix4fv(loc.inst, 1, GL_FALSE, inst);
        use(w.cast[di]);
        glDrawElements(mode, grp.num, GL_UNSIGNED_INT,
                       (void*)((first + w.draw_offsets[di]) * sizeof(u32)));
        *tris += grp.num / 3;
        (*draws)++;
      }
    }
  }
}

void WorldShadows::prepass(SharedRenderState* rs,
                           const float c2w_in[16],
                           const float w2c_in[16],
                           const math::Vector3f& cam_pos,
                           const math::Vector3f& fwd,
                           const float tan_xy[2],
                           const math::Vector3f& sun_dir_in,
                           float strength,
                           const UnbakeSidecar* sc,
                           const math::Vector<s32, 4>* itimes) {
  auto& fr = rs->world_shadow;
  fr.active = false;
  fr.world = fr.actors = fr.water = false;
  fr.cascades = 0;
  const auto& gs = Gfx::g_global_settings;
  if (!(gs.shadow_world || gs.shadow_world_actors || gs.shadow_world_water) || !rs->loader) {
    return;
  }
  const math::Vector3f sun = sun_dir_in.normalized();
  if (strength <= 0.f) {
    return;
  }
  const auto t0 = Clock::now();
  const bool fast = Gfx::perf_opt(kOptBit);
  ensure_target(gs.shadow_world_size);

  // cascades: bounding spheres of the view slices, boxes snapped to whole texels
  const int n = std::clamp(gs.shadow_world_cascades, 1, WorldShadowFrame::kMaxCascades);
  const float range = gs.shadow_world_range_m * kMeter;
  float splits[WorldShadowFrame::kMaxCascades + 1] = {0.5f * kMeter};
  if (n == 1) {
    splits[1] = range;
  } else if (n == 2) {
    splits[1] = range / 6.f;
    splits[2] = range;
  } else {
    splits[1] = range / 12.f;
    splits[2] = range * 0.29f;
    splits[3] = range;
  }
  const float t2 = tan_xy[0] * tan_xy[0] + tan_xy[1] * tan_xy[1];
  // perf-opt 28: the first cascade fills a 2048 corner of a 4096 layer
  int csize[WorldShadowFrame::kMaxCascades] = {m_size, m_size, m_size};
  if (Gfx::perf_opt(PERF_OPT_WORLD_KERNEL) && m_size >= 4096) {
    csize[0] = m_size / 2;
  }
  const math::Vector3f f = sun * -1.f;
  math::Vector3f r, u;
  basis(f, &r, &u);
  Mat4 c2w, w2c;
  memcpy(c2w.data(), c2w_in, sizeof(Mat4));
  memcpy(w2c.data(), w2c_in, sizeof(Mat4));

  struct Box {
    Mat4 clip;
    math::Vector4f planes[4];
    float texel, depth;
  } boxes[WorldShadowFrame::kMaxCascades];
  for (int c = 0; c < n; c++) {
    const float zn = splits[c], zf = splits[c + 1];
    float zc = 0.5f * (zn + zf) * (1.f + t2);
    float rad;
    if (zc >= zf) {
      zc = zf;
      rad = zf * std::sqrt(t2);
    } else {
      rad = std::sqrt((zf - zc) * (zf - zc) + zf * zf * t2);
    }
    rad *= 1.02f;
    const math::Vector3f centre = cam_pos + fwd * zc;
    const float texel = 2.f * rad / csize[c];
    const float cx = std::floor(r.dot(centre) / texel) * texel;
    const float cy = std::floor(u.dot(centre) / texel) * texel;
    const float cz = f.dot(centre);
    const math::Vector3f o = r * cx + u * cy + f * cz;
    Mat4 view{};
    const math::Vector3f* axes[3] = {&r, &u, &f};
    for (int row = 0; row < 3; row++) {
      for (int k = 0; k < 3; k++) {
        view[k * 4 + row] = (*axes[row])[k];
      }
      view[12 + row] = -axes[row]->dot(o);
    }
    view[15] = 1.f;
    const float z_near = -rad - kTowardSunM * kMeter, z_far = rad;
    Mat4 proj{};
    proj[0] = 1.f / rad;
    proj[5] = 1.f / rad;
    proj[10] = 2.f / (z_far - z_near);
    proj[14] = -(z_far + z_near) / (z_far - z_near);
    proj[15] = 1.f;
    boxes[c].clip = mul(proj, view);
    boxes[c].texel = texel;
    boxes[c].depth = z_far - z_near;
    box_planes(r, u, o, rad, boxes[c].planes);
  }

  // level caches: built on first use, dropped when the level goes
  std::vector<LevelData*> in_use = rs->loader->get_in_use_levels();
  for (auto it = m_levels.begin(); it != m_levels.end();) {
    const bool live = std::any_of(in_use.begin(), in_use.end(), [&](const LevelData* l) {
      return l == it->first && l->load_id == it->second.load_id;
    });
    if (!live) {
      release(it->second);
      it = m_levels.erase(it);
      m_gen++;
    } else {
      ++it;
    }
  }
  for (auto* lev : in_use) {
    if (lev->level && !m_levels.count(lev)) {
      m_levels.emplace(lev, build(lev));
      m_gen++;
    }
  }

  // ambient floor for receivers without sidecar data: the level's median a times the live ambient
  float amb_floor = 0.f;
  if (sc && itimes) {
    if (sc != m_amb_sc) {
      m_amb_a = median_ambient_gain(*sc);
      m_amb_sc = sc;
    }
    float w[8][3];
    unbake_slot_weights(itimes, w);
    math::Vector3f amb(0, 0, 0);
    for (int s = 0; s < 8; s++) {
      if (sc->slot_mask & (1u << s)) {
        const float ws = (w[s][0] + w[s][1] + w[s][2]) / 3.f;
        for (int k = 0; k < 3; k++) {
          amb[k] += std::max(sc->slots[s].amb[k], 0.f) * ws;
        }
      }
    }
    amb_floor = m_amb_a * (0.299f * amb.x() + 0.587f * amb.y() + 0.114f * amb.z()) / 127.5f;
  }

  // draw
  if (!m_shader) {
    m_shader = std::make_unique<Shader>("world_shadow", GameVersion::Jak1);
  }
  if (!m_shader->okay()) {
    return;
  }
  // a cascade whose clip and casters are those it was drawn with already holds this frame's map
  bool redraw[WorldShadowFrame::kMaxCascades] = {};
  int n_redraw = 0;
  for (int c = 0; c < n; c++) {
    const auto& d = m_drawn[c];
    redraw[c] = !fast || !d.ok || d.gen != m_gen || d.tex != m_tex || d.size != csize[c] ||
                std::memcmp(d.clip.data(), boxes[c].clip.data(), sizeof(Mat4)) != 0;
    n_redraw += redraw[c] ? 1 : 0;
  }
  m_frame++;
  perf::Scope ps(perf::Group::WORLD_SHADOWS);
  if (!m_query) {
    glGenQueries(1, &m_query);
  } else if (m_query_pending && (!fast || ++m_query_age >= 3)) {
    GLint ready = 0;
    glGetQueryObjectiv(m_query, GL_QUERY_RESULT_AVAILABLE, &ready);
    if (ready) {
      GLuint64 ns = 0;
      glGetQueryObjectui64v(m_query, GL_QUERY_RESULT, &ns);
      m_gpu_ms_sum += ns * 1e-6;
      m_gpu_n++;
      m_query_pending = false;
    }
  }
  const bool timing = !m_query_pending && (!fast || m_frame % kQueryEvery == 0);
  if (timing) {
    glBeginQuery(GL_TIME_ELAPSED, m_query);
    m_query_age = 0;
  }
  if (n_redraw > 0) {
    m_state.capture();
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glEnable(GL_DEPTH_CLAMP);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(1.5f, 2.f);
    glEnable(GL_PRIMITIVE_RESTART);
    glPrimitiveRestartIndex(UINT32_MAX);
    m_shader->activate();
    const GLuint prog = m_shader->id();
    Locs loc;
    loc.clip = GLQ_LOC(PERF_OPT_GEOMETRY + 2, prog, "ws_clip");
    loc.inst = GLQ_LOC(PERF_OPT_GEOMETRY + 2, prog, "ws_inst");
    loc.inst_on = GLQ_LOC(PERF_OPT_GEOMETRY + 2, prog, "ws_inst_on");
    loc.uv = GLQ_LOC(PERF_OPT_GEOMETRY + 2, prog, "ws_uv_scale");
    loc.aref = GLQ_LOC(PERF_OPT_GEOMETRY + 2, prog, "ws_alpha_ref");
    glActiveTexture(GL_TEXTURE0);
    glBindSampler(0, m_sampler_alpha[0]);
    GLint old_depth_func;
    GLfloat old_clear_depth;
    glGetIntegerv(GL_DEPTH_FUNC, &old_depth_func);
    glGetFloatv(GL_DEPTH_CLEAR_VALUE, &old_clear_depth);
    glClearDepth(1.0);
    for (int c = 0; c < n; c++) {
      if (!redraw[c]) {
        continue;
      }
      glBindFramebuffer(GL_FRAMEBUFFER, m_fbo[c]);
      glViewport(0, 0, csize[c], csize[c]);
      glClear(GL_DEPTH_BUFFER_BIT);
      glUniformMatrix4fv(loc.clip, 1, GL_FALSE, boxes[c].clip.data());
      auto& d = m_drawn[c];
      d.draws = 0;
      d.tris = 0;
      for (auto& [lev, cache] : m_levels) {
        draw_level(cache, boxes[c].planes, loc, fast, &d.draws, &d.tris);
      }
      d.ok = true;
      d.gen = m_gen;
      d.tex = m_tex;
      d.size = csize[c];
      std::memcpy(d.clip.data(), boxes[c].clip.data(), sizeof(Mat4));
    }
    glBindSampler(0, 0);
    glBindVertexArray(0);
    glDisable(GL_DEPTH_CLAMP);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glClearDepth(old_clear_depth);
    m_state.restore();
    glDepthFunc(old_depth_func);
  }
  // the map stays on its units for every receiver this frame
  glActiveTexture(GL_TEXTURE0 + kUnitMap);
  glBindTexture(GL_TEXTURE_2D_ARRAY, m_tex);
  glBindSampler(kUnitMap, m_sampler_cmp);
  glActiveTexture(GL_TEXTURE0 + kUnitRaw);
  glBindTexture(GL_TEXTURE_2D_ARRAY, m_tex);
  glBindSampler(kUnitRaw, m_sampler_raw);
  glActiveTexture(GL_TEXTURE0);
  if (timing) {
    glEndQuery(GL_TIME_ELAPSED);
    m_query_pending = true;
  }
  const double cpu_ms = ms_since(t0);
  m_cpu_ms_sum += cpu_ms;
  m_cpu_n++;
  m_redrawn_sum += n_redraw;
  int draws = 0;
  u64 tris = 0;
  for (int c = 0; c < n; c++) {
    draws += m_drawn[c].draws;
    tris += m_drawn[c].tris;
  }
  m_last_draws = draws;
  m_last_tris = tris;
  perf::count(perf::Counter::WORLD_CPU_MS, cpu_ms);
  perf::count(perf::Counter::WORLD_CASCADES, (double)n_redraw);

  fr.active = true;
  fr.world = gs.shadow_world;
  fr.actors = gs.shadow_world_actors;
  fr.water = gs.shadow_world_water;
  fr.cascades = n;
  fr.strength = strength;
  fr.penumbra = gs.shadow_world_penumbra;
  fr.amb_floor = amb_floor;
  fr.tex = m_tex;
  fr.size = m_size;
  for (int c = 0; c < n; c++) {
    memcpy(fr.mtx_world[c], boxes[c].clip.data(), sizeof(Mat4));
    const Mat4 cam = mul(boxes[c].clip, c2w);
    memcpy(fr.mtx_cam[c], cam.data(), sizeof(Mat4));
    fr.texel_world[c] = boxes[c].texel;
    fr.depth_range[c] = boxes[c].depth;
    fr.uv_scale[c] = (float)csize[c] / m_size;
  }
  fr.sun_world = math::Vector4f(sun.x(), sun.y(), sun.z(), strength);
  math::Vector3f sc_dir(w2c[0] * sun.x() + w2c[4] * sun.y() + w2c[8] * sun.z(),
                        w2c[1] * sun.x() + w2c[5] * sun.y() + w2c[9] * sun.z(),
                        w2c[2] * sun.x() + w2c[6] * sun.y() + w2c[10] * sun.z());
  sc_dir = sc_dir.length() > 1e-6f ? sc_dir.normalized() : math::Vector3f(0, 1, 0);
  fr.sun_cam = math::Vector4f(sc_dir.x(), sc_dir.y(), sc_dir.z(), strength);

  if (m_log++ % 120 == 0 && !rs->stereo_repeat_pass) {
    lg::info(
        "world-shadows: hour {:.2f}, palette sun ({:.2f} {:.2f} {:.2f}), {} cascades of {} (texel "
        "{:.1f} / {:.1f} / {:.1f} cm), sun ({:.2f} {:.2f} {:.2f}) fade {:.2f}, {} draws {} tris, "
        "amb "
        "floor {:.3f} (a {:.3f}), pass gpu {:.3f} ms cpu {:.3f} ms, {:.2f} cascades redrawn per "
        "frame",
        m_hour, m_palette_dir.x(), m_palette_dir.y(), m_palette_dir.z(), n, m_size,
        boxes[0].texel / kMeter * 100.f, n > 1 ? boxes[1].texel / kMeter * 100.f : 0.f,
        n > 2 ? boxes[2].texel / kMeter * 100.f : 0.f, sun.x(), sun.y(), sun.z(), strength, draws,
        tris, amb_floor, m_amb_a, m_gpu_n ? m_gpu_ms_sum / m_gpu_n : 0.0,
        m_cpu_n ? m_cpu_ms_sum / m_cpu_n : 0.0, m_cpu_n ? (double)m_redrawn_sum / m_cpu_n : 0.0);
    m_gpu_ms_sum = m_cpu_ms_sum = 0;
    m_gpu_n = m_cpu_n = 0;
    m_redrawn_sum = 0;
  }
}

void WorldShadows::draw_debug_window() {
  auto& gs = Gfx::g_global_settings;
  ImGui::Checkbox("World map: background", &gs.shadow_world);
  ImGui::Checkbox("World map: actors (else terrain cover)", &gs.shadow_world_actors);
  ImGui::Checkbox("World map: water", &gs.shadow_world_water);
  ImGui::SliderInt("World map cascades", &gs.shadow_world_cascades, 1, 3);
  ImGui::SliderFloat("World map penumbra per m", &gs.shadow_world_penumbra, 0.f, 0.05f);
  ImGui::SliderFloat("World map range (m)", &gs.shadow_world_range_m, 20.f, 400.f);
  ImGui::Text("map %d, %zu levels, %d draws, %llu tris", m_size, m_levels.size(), m_last_draws,
              (unsigned long long)m_last_tris);
}

void world_shadows_set_uniforms(const SharedRenderState* rs, GLuint program, bool camera_space) {
  const auto& fr = rs->world_shadow;
  if (!fr.active) {
    glUniform1i(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "ws_on"), 0);
    glUniform1i(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "ws_n"), 0);
    // the samplers must always see a depth array on their units
    static GLuint dummy = 0;
    if (!dummy) {
      glActiveTexture(GL_TEXTURE0 + WorldShadows::kUnitMap);
      glGenTextures(1, &dummy);
      glBindTexture(GL_TEXTURE_2D_ARRAY, dummy);
      const float one = 1.f;
      glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH_COMPONENT32F, 1, 1, 1, 0, GL_DEPTH_COMPONENT,
                   GL_FLOAT, &one);
      glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
      glActiveTexture(GL_TEXTURE0 + WorldShadows::kUnitRaw);
      glBindTexture(GL_TEXTURE_2D_ARRAY, dummy);
      glActiveTexture(GL_TEXTURE0);
    }
    return;
  }
  const int on = (fr.world ? 1 : 0) | (fr.actors ? 2 : 0) | (fr.water ? 4 : 0);
  glUniform1i(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "ws_on"), on);
  glUniform1i(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "ws_n"), fr.cascades);
  glUniformMatrix4fv(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "ws_mtx"), fr.cascades, GL_FALSE,
                     camera_space ? fr.mtx_cam[0][0].data() : fr.mtx_world[0][0].data());
  float par[WorldShadowFrame::kMaxCascades * 4] = {};
  for (int c = 0; c < fr.cascades; c++) {
    par[c * 4 + 0] = fr.texel_world[c];
    par[c * 4 + 1] = fr.depth_range[c];
    par[c * 4 + 2] = fr.uv_scale[c];
  }
  glUniform4fv(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "ws_par"), fr.cascades, par);
  glUniform4fv(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "ws_sun"), 1,
               camera_space ? fr.sun_cam.data() : fr.sun_world.data());
  glUniform1f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "ws_pen"), fr.penumbra);
  glUniform1f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "ws_size"), (float)fr.size);
  glUniform1f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "ws_amb"), fr.amb_floor);
  glUniform1i(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "ws_gate"),
              Gfx::perf_opt(PERF_OPT_WORLD_GATE) ? 1 : 0);
  glUniform1i(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "ws_kernel"),
              Gfx::perf_opt(PERF_OPT_WORLD_KERNEL) ? 1 : 0);
}
