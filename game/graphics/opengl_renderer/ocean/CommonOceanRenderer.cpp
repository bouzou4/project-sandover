#include "CommonOceanRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <array>
#include <unordered_map>
#include <unordered_set>

#include "common/log/log.h"
#include "common/util/Timer.h"

#include "game/graphics/gfx.h"
#include "game/graphics/opengl_renderer/Materials.h"
#include "game/graphics/opengl_renderer/PerfTimers.h"
#include "game/graphics/opengl_renderer/SsaoRenderer.h"
#include "game/graphics/screenshot.h"

constexpr int OCEAN_TEX_TBP_JAK1 = 8160;  // todo
constexpr int OCEAN_TEX_TBP_JAK2 = 672;

CommonOceanRenderer::CommonOceanRenderer() {
  m_vertices.resize(4096 * 10);  // todo decrease
  for (auto& buf : m_indices) {
    buf.resize(4096 * 10);
  }

  // create OpenGL objects
  glGenBuffers(1, &m_ogl.vertex_buffer);
  glGenBuffers(NUM_BUCKETS, m_ogl.index_buffer);
  glGenVertexArrays(1, &m_ogl.vao);

  // set up the vertex array
  glBindVertexArray(m_ogl.vao);
  for (int i = 0; i < NUM_BUCKETS; i++) {
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ogl.index_buffer[i]);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, m_indices[i].size() * sizeof(u32), nullptr,
                 GL_STREAM_DRAW);
  }
  glBindBuffer(GL_ARRAY_BUFFER, m_ogl.vertex_buffer);
  glBufferData(GL_ARRAY_BUFFER, m_vertices.size() * sizeof(Vertex), nullptr, GL_STREAM_DRAW);

  // xyz
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0,                            // location 0 in the shader
                        3,                            // 3 floats per vert
                        GL_FLOAT,                     // floats
                        GL_TRUE,                      // normalized, ignored,
                        sizeof(Vertex),               //
                        (void*)offsetof(Vertex, xyz)  // offset in array
  );

  // rgba
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1,                             // location 1 in the shader
                        4,                             // 4 color components
                        GL_UNSIGNED_BYTE,              // u8
                        GL_TRUE,                       // normalized (255 becomes 1)
                        sizeof(Vertex),                //
                        (void*)offsetof(Vertex, rgba)  //
  );

  // stq
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2,                            // location 2 in the shader
                        3,                            // 2 floats per vert
                        GL_FLOAT,                     // floats
                        GL_FALSE,                     // normalized, ignored
                        sizeof(Vertex),               //
                        (void*)offsetof(Vertex, stq)  // offset in array
  );

  // byte data
  glEnableVertexAttribArray(3);
  glVertexAttribIPointer(3,                            // location 3 in the shader
                         4,                            //
                         GL_UNSIGNED_BYTE,             // u8's
                         sizeof(Vertex),               //
                         (void*)offsetof(Vertex, fog)  // offset in array
  );

  // merged envmap pass attributes (locations 4, 5), enabled only while merging
  glGenBuffers(1, &m_ogl.env_buffer);
  glBindBuffer(GL_ARRAY_BUFFER, m_ogl.env_buffer);
  glVertexAttribPointer(4, 3, GL_FLOAT, GL_FALSE, sizeof(EnvAttr), (void*)offsetof(EnvAttr, stq));
  glVertexAttribPointer(5, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(EnvAttr),
                        (void*)offsetof(EnvAttr, rgba));

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindVertexArray(0);
}

CommonOceanRenderer::~CommonOceanRenderer() {
  glDeleteBuffers(1, &m_ogl.vertex_buffer);
  glDeleteBuffers(1, &m_ogl.env_buffer);
  glDeleteBuffers(3, m_ogl.index_buffer);
  glDeleteVertexArrays(1, &m_ogl.vao);
  for (auto* f : {&m_frags_mid, &m_frags_near}) {
    if (f->q[0]) {
      glDeleteQueries(4, f->q);
    }
  }
}

void CommonOceanRenderer::init_for_near() {
  m_next_free_vertex = 0;
  for (auto& x : m_next_free_index) {
    x = 0;
  }
}

void CommonOceanRenderer::kick_from_near(const u8* data) {
  bool eop = false;

  u32 offset = 0;
  while (!eop) {
    GifTag tag(data + offset);
    offset += 16;

    if (tag.nreg() == 3) {
      ASSERT(tag.pre());
      if (GsPrim(tag.prim()).kind() == GsPrim::Kind::TRI_STRIP) {
        handle_near_vertex_gif_data_strip(data, offset, tag.nloop());
      } else {
        handle_near_vertex_gif_data_fan(data, offset, tag.nloop());
      }
      offset += 16 * 3 * tag.nloop();
    } else if (tag.nreg() == 1) {
      handle_near_adgif(data, offset, tag.nloop());
      offset += 16 * 1 * tag.nloop();
    } else {
      ASSERT(false);
    }

    eop = tag.eop();
  }
}

void CommonOceanRenderer::handle_near_vertex_gif_data_strip(const u8* data, u32 offset, u32 loop) {
  m_indices[m_current_bucket][m_next_free_index[m_current_bucket]++] = UINT32_MAX;
  bool reset_last = false;
  for (u32 i = 0; i < loop; i++) {
    auto& dest_vert = m_vertices[m_next_free_vertex++];

    // stq
    memcpy(dest_vert.stq.data(), data + offset, 12);
    offset += 16;

    // rgba
    dest_vert.rgba[0] = data[offset];
    dest_vert.rgba[1] = data[offset + 4];
    dest_vert.rgba[2] = data[offset + 8];
    dest_vert.rgba[3] = data[offset + 12];
    offset += 16;

    // xyz
    u32 x = 0, y = 0;
    memcpy(&x, data + offset, 4);
    memcpy(&y, data + offset + 4, 4);

    u64 upper;
    memcpy(&upper, data + offset + 8, 8);
    u32 z = (upper >> 4) & 0xffffff;
    offset += 16;

    dest_vert.xyz[0] = (float)(x << 16) / (float)UINT32_MAX;
    dest_vert.xyz[1] = (float)(y << 16) / (float)UINT32_MAX;
    dest_vert.xyz[2] = (float)(z << 8) / (float)UINT32_MAX;

    u8 f = (upper >> 36);
    dest_vert.fog = f;

    auto vidx = m_next_free_vertex - 1;
    bool adc = upper & (1ull << 47);
    if (!adc) {
      m_indices[m_current_bucket][m_next_free_index[m_current_bucket]++] = vidx;
      reset_last = false;
    } else {
      if (reset_last) {
        m_next_free_index[m_current_bucket] -= 3;
      }
      m_indices[m_current_bucket][m_next_free_index[m_current_bucket]++] = UINT32_MAX;
      m_indices[m_current_bucket][m_next_free_index[m_current_bucket]++] = vidx - 1;
      m_indices[m_current_bucket][m_next_free_index[m_current_bucket]++] = vidx;
      reset_last = true;
    }
  }
}

void CommonOceanRenderer::handle_near_vertex_gif_data_fan(const u8* data, u32 offset, u32 loop) {
  u32 ind_of_fan_start = UINT32_MAX;
  bool fan_running = false;
  // :regs0 (gif-reg-id st) :regs1 (gif-reg-id rgbaq) :regs2 (gif-reg-id xyzf2)
  for (u32 i = 0; i < loop; i++) {
    auto& dest_vert = m_vertices[m_next_free_vertex++];

    // stq
    memcpy(dest_vert.stq.data(), data + offset, 12);
    offset += 16;

    // rgba
    dest_vert.rgba[0] = data[offset];
    dest_vert.rgba[1] = data[offset + 4];
    dest_vert.rgba[2] = data[offset + 8];
    dest_vert.rgba[3] = data[offset + 12];
    offset += 16;

    // xyz
    u32 x = 0, y = 0;
    memcpy(&x, data + offset, 4);
    memcpy(&y, data + offset + 4, 4);

    u64 upper;
    memcpy(&upper, data + offset + 8, 8);
    u32 z = (upper >> 4) & 0xffffff;
    offset += 16;

    dest_vert.xyz[0] = (float)(x << 16) / (float)UINT32_MAX;
    dest_vert.xyz[1] = (float)(y << 16) / (float)UINT32_MAX;
    dest_vert.xyz[2] = (float)(z << 8) / (float)UINT32_MAX;

    u8 f = (upper >> 36);
    dest_vert.fog = f;

    auto vidx = m_next_free_vertex - 1;

    if (ind_of_fan_start == UINT32_MAX) {
      ind_of_fan_start = vidx;
    } else {
      if (fan_running) {
        // hack to draw fans with strips. this isn't efficient, but fans happen extremely rarely
        // (you basically have to put the camera intersecting the ocean and looking fwd)
        m_indices[m_current_bucket][m_next_free_index[m_current_bucket]++] = UINT32_MAX;
        m_indices[m_current_bucket][m_next_free_index[m_current_bucket]++] = vidx;
        m_indices[m_current_bucket][m_next_free_index[m_current_bucket]++] = vidx - 1;
        m_indices[m_current_bucket][m_next_free_index[m_current_bucket]++] = ind_of_fan_start;
      } else {
        fan_running = true;
      }
    }
  }
}

void CommonOceanRenderer::handle_near_adgif(const u8* data, u32 offset, u32 count) {
  u32 most_recent_tbp = 0;

  for (u32 i = 0; i < count; i++) {
    u64 value;
    GsRegisterAddress addr;
    memcpy(&value, data + offset + 16 * i, sizeof(u64));
    memcpy(&addr, data + offset + 16 * i + 8, sizeof(GsRegisterAddress));
    switch (addr) {
      case GsRegisterAddress::MIPTBP1_1:
        // ignore this, it's just mipmapping settings
        break;
      case GsRegisterAddress::TEX1_1: {
        GsTex1 reg(value);
        ASSERT(reg.mmag());
      } break;
      case GsRegisterAddress::CLAMP_1: {
        bool s = value & 0b001;
        bool t = value & 0b100;
        ASSERT(s == t);
        if (s) {
          m_current_bucket = VertexBucket::ENV_MAP;
        }
      } break;
      case GsRegisterAddress::TEX0_1: {
        GsTex0 reg(value);
        ASSERT(reg.tfx() == GsTex0::TextureFunction::MODULATE);
        if (!reg.tcc()) {
          m_current_bucket = VertexBucket::RGB_TEXTURE;
        }
        most_recent_tbp = reg.tbp0();
      } break;
      case GsRegisterAddress::ALPHA_1: {
        GsAlpha reg(value);
        if (reg.a_mode() == GsAlpha::BlendMode::SOURCE &&
            reg.b_mode() == GsAlpha::BlendMode::ZERO_OR_FIXED &&
            reg.c_mode() == GsAlpha::BlendMode::DEST && reg.d_mode() == GsAlpha::BlendMode::DEST) {
          m_current_bucket = VertexBucket::ENV_MAP;
        }
      } break;

      case GsRegisterAddress::FRAME_1: {
        u32 mask = value >> 32;
        if (mask) {
          m_current_bucket = VertexBucket::ALPHA;
        }
      } break;

      default:
        lg::debug("reg: {}", register_address_name(addr));
        break;
    }
  }

  if (m_current_bucket == VertexBucket::ENV_MAP) {
    m_envmap_tex = most_recent_tbp;
  }

  if (m_vertices.size() - 128 < m_next_free_vertex) {
    ASSERT(false);  // add more vertices.
  }
}

void CommonOceanRenderer::water_count_begin(bool on) {
  m_count_on = on && Gfx::g_global_settings.ssao_gpu_timing;
  if (!m_count_on) {
    return;
  }
  if (!m_count_query) {
    glGenQueries(1, &m_count_query);
  }
  if (m_count_pending) {
    GLint ready = 0;
    glGetQueryObjectiv(m_count_query, GL_QUERY_RESULT_AVAILABLE, &ready);
    if (!ready) {
      m_count_on = false;
      return;
    }
    GLuint prims = 0;
    glGetQueryObjectuiv(m_count_query, GL_QUERY_RESULT, &prims);
    m_count_pending = false;
    if (++m_count_frames % 120 == 0) {
      lg::info("WATER-PRIMS triangles={} source={}", prims, m_count_source);
    }
  }
  m_count_source = (u32)m_sub_indices[0].size() / 3;
  glBeginQuery(GL_PRIMITIVES_GENERATED, m_count_query);
}

void CommonOceanRenderer::water_count_end(bool on) {
  if (on && m_count_on) {
    glEndQuery(GL_PRIMITIVES_GENERATED);
    m_count_pending = true;
  }
}

constexpr GLenum kFragInvocations = 0x82F4;  // GL_FRAGMENT_SHADER_INVOCATIONS_ARB

void CommonOceanRenderer::frags_begin(FragCount& f, perf::Counter c) {
  f.active = false;
  if (!Gfx::g_global_settings.ssao_gpu_timing) {
    return;
  }
  if (!f.q[0]) {
    glGenQueries(4, f.q);
  }
  for (int i = 0; i < 4; i++) {
    if (!f.pending[i]) {
      continue;
    }
    GLint ready = 0;
    glGetQueryObjectiv(f.q[i], GL_QUERY_RESULT_AVAILABLE, &ready);
    if (ready) {
      GLuint64 n = 0;
      glGetQueryObjectui64v(f.q[i], GL_QUERY_RESULT, &n);
      f.pending[i] = false;
      f.last = (double)n / 1e6;
    }
  }
  if (f.last >= 0.) {
    perf::count(c, f.last);
  }
  if (f.pending[f.next]) {
    return;
  }
  glBeginQuery(kFragInvocations, f.q[f.next]);
  f.active = true;
}

void CommonOceanRenderer::frags_end(FragCount& f) {
  if (f.active) {
    glEndQuery(kFragInvocations);
    f.pending[f.next] = true;
    f.next = (f.next + 1) % 4;
    f.active = false;
  }
}

void CommonOceanRenderer::triangulate() {
  for (int b = 0; b < NUM_BUCKETS; b++) {
    auto& out = m_sub_indices[b];
    out.clear();
    u32 prev[2] = {0, 0};
    int run = 0;
    for (u32 i = 0; i < m_next_free_index[b]; i++) {
      const u32 v = m_indices[b][i];
      if (v == UINT32_MAX) {
        run = 0;
        continue;
      }
      if (run >= 2 && prev[0] != prev[1] && prev[1] != v && prev[0] != v) {
        out.push_back(prev[0]);
        out.push_back(prev[1]);
        out.push_back(v);
      }
      prev[0] = prev[1];
      prev[1] = v;
      run++;
    }
  }
}

/// Pairs each mid envmap patch with the textured patch of identical corners, so the textured
/// pass can add its envmap term; envmap patches without a twin stay in m_env_rest. False if
/// nothing pairs.
bool CommonOceanRenderer::merge_envmap() {
  const auto& s0 = m_sub_indices[0];
  const auto& s1 = m_sub_indices[1];
  if (s0.empty() || s1.empty()) {
    return false;
  }
  using Key = std::array<u32, 12>;
  struct KeyHash {
    size_t operator()(const Key& k) const {
      size_t h = 1469598103934665603ull;
      for (u32 v : k) {
        h = (h ^ v) * 1099511628211ull;
      }
      return h;
    }
  };
  auto key_of = [&](const u32* c) {
    Key k;
    for (int j = 0; j < 3; j++) {
      const Vertex& v = m_vertices[c[j]];
      memcpy(&k[j * 4], &v.xyz, 12);
      k[j * 4 + 3] = v.pad[0];
    }
    return k;
  };
  // the last envmap patch of a place is the one the reversed pass drew first, which counted
  std::unordered_map<Key, u32, KeyHash> env;
  env.reserve(s1.size() / 3);
  for (u32 p = 0; p + 2 < s1.size(); p += 3) {
    env[key_of(&s1[p])] = p;
  }
  const u32 base = m_next_free_vertex;
  m_merge_verts.assign(m_vertices.begin(), m_vertices.begin() + base);
  m_env_attr.assign(base, EnvAttr{});
  m_merge_idx.clear();
  u32 paired = 0;
  for (u32 p = 0; p + 2 < s0.size(); p += 3) {
    auto it = env.find(key_of(&s0[p]));
    for (int j = 0; j < 3; j++) {
      EnvAttr a{};
      if (it != env.end()) {
        const Vertex& e = m_vertices[s1[(it->second & 0x7fffffffu) + j]];
        a.stq = e.stq;
        a.rgba = e.rgba;
        a.rgba[3] = 255;  // has an envmap term
      }
      m_merge_idx.push_back((u32)m_merge_verts.size());
      m_merge_verts.push_back(m_vertices[s0[p + j]]);
      m_env_attr.push_back(a);
    }
    if (it != env.end()) {
      it->second |= 0x80000000u;  // paired; a twin's earlier copies add nothing after it
      paired++;
    }
  }
  m_env_rest.clear();
  for (u32 p = 0; p + 2 < s1.size(); p += 3) {
    if (!(env[key_of(&s1[p])] & 0x80000000u)) {
      m_env_rest.insert(m_env_rest.end(), s1.begin() + p, s1.begin() + p + 3);
    }
  }
  return paired > 0;
}

namespace {
// world xz extent of the last near ocean: the mid mesh's holes there are not its outer edge
struct {
  bool ok = false;
  float x0, x1, z0, z1;
} g_near_box;

// world xz extent of the mid ocean's outer rim, where the flat vanilla ocean-far takes over
struct {
  bool ok = false;
  float x0, x1, z0, z1;
} g_mid_rim;

constexpr double kLattice = 24576.;  // 6 m: the mid grid is 12 m

/// World position of a GS vertex: where its pixel ray meets the plane y = plane_y, as
/// water_plane_point (water_waves.glsl) finds it; from the vertex's depth when the ray misses.
bool water_vertex_world(const double inv[4][4],
                        const SharedRenderState* rs,
                        const math::Vector<float, 3>& xyz,
                        float plane_y,
                        double* x,
                        double* z) {
  const auto fc = frame_camera(*rs);
  const auto& hv = *fc.hvdf_off;
  const double fog = fc.fog->x();
  auto world = [&](double gz, double P[3]) {
    const double c[4] = {(xyz[0] * 4096. - hv.x()) / fog, (xyz[1] * 4096. - hv.y()) / fog,
                         (gz - hv.z()) / fog, 1.};
    double p[4] = {0, 0, 0, 0};
    for (int r = 0; r < 4; r++) {
      for (int k = 0; k < 4; k++) {
        p[r] += inv[r][k] * c[k];
      }
    }
    if (std::abs(p[3]) < 1e-30) {
      return false;
    }
    for (int k = 0; k < 3; k++) {
      P[k] = p[k] / p[3];
    }
    return true;
  };
  double z1, z2, A[3], B[3];
  if (water_ray_depths(rs, &z1, &z2) && world(z1, A) && world(z2, B)) {
    const double d[3] = {B[0] - A[0], B[1] - A[1], B[2] - A[2]};
    const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    const double s = (plane_y - A[1]) / d[1];
    if (std::abs(d[1]) > 1e-6 * len && s > -0.999) {
      *x = A[0] + d[0] * s;
      *z = A[2] + d[2] * s;
      return true;
    }
  }
  double P[3];
  if (!world(xyz[2] * 16777216., P)) {
    return false;
  }
  *x = P[0];
  *z = P[2];
  return true;
}
}  // namespace

/// The near ocean's rest points come from its own depth (the game's wave heights, #213), the
/// mid's from the flat plane. Where they meet, the near's rim takes the plane too, as the mid's
/// hole edge does, so both displace each shared or T-joined vertex alike (pad[1]: depth share).
/// The rim depends only on the index list and on which vertices share a position, so it is
/// rebuilt only when one of them changes.
void CommonOceanRenderer::near_rim_on_plane() {
  const u32 nv = m_next_free_vertex;
  const u32 ni = m_next_free_index[0];
  auto& rc = m_rim;
  auto mix = [](u64 k) {
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdull;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53ull;
    return (u32)(k ^ (k >> 33));
  };
  auto table_size = [](u32 n) {
    u32 s = 16;
    while (s < n) {
      s <<= 1;
    }
    return s;
  };
  constexpr u64 kEmpty = ~0ull;
  // position class of every vertex: the first vertex at its GS x, y
  const u32 hs = table_size(2 * nv + 1);
  rc.pos_key.assign(hs, kEmpty);
  rc.pos_first.resize(hs);
  rc.cls_now.resize(nv);
  for (u32 i = 0; i < nv; i++) {
    u64 x = 0, y = 0;
    memcpy(&x, &m_vertices[i].xyz[0], 4);
    memcpy(&y, &m_vertices[i].xyz[1], 4);
    const u64 k = x << 32 | y;
    u32 h = mix(k) & (hs - 1);
    while (rc.pos_key[h] != kEmpty && rc.pos_key[h] != k) {
      h = (h + 1) & (hs - 1);
    }
    if (rc.pos_key[h] == kEmpty) {
      rc.pos_key[h] = k;
      rc.pos_first[h] = i;
    }
    rc.cls_now[i] = rc.pos_first[h];
  }
  const bool hit = rc.valid && rc.cls == rc.cls_now && rc.idx.size() == ni &&
                   std::equal(rc.idx.begin(), rc.idx.end(), m_indices[0].begin());
  if (!hit) {
    rc.cls.swap(rc.cls_now);
    rc.idx.assign(m_indices[0].begin(), m_indices[0].begin() + ni);
    // triangles per edge between classes; an edge with one is the rim
    const u32 es = table_size(4 * ni + 1);
    rc.edge_key.assign(es, kEmpty);
    rc.edge_tris.assign(es, 0);
    auto add = [&](u32 a, u32 b) {
      const u32 ca = rc.cls[a], cb = rc.cls[b];
      if (ca == cb) {
        return;
      }
      const u64 e = ca < cb ? ((u64)ca << 32 | cb) : ((u64)cb << 32 | ca);
      u32 h = mix(e) & (es - 1);
      while (rc.edge_key[h] != kEmpty && rc.edge_key[h] != e) {
        h = (h + 1) & (es - 1);
      }
      rc.edge_key[h] = e;
      rc.edge_tris[h]++;
    };
    u32 prev[2] = {0, 0};
    int run = 0;
    for (u32 i = 0; i < ni; i++) {
      const u32 v = rc.idx[i];
      if (v == UINT32_MAX) {
        run = 0;
        continue;
      }
      if (run >= 2) {
        add(prev[0], prev[1]);
        add(prev[1], v);
        add(v, prev[0]);
      }
      prev[0] = prev[1];
      prev[1] = v;
      run++;
    }
    rc.on_rim.assign(nv, 0);
    for (u32 h = 0; h < es; h++) {
      if (rc.edge_tris[h] == 1) {
        rc.on_rim[(u32)(rc.edge_key[h] >> 32)] = 1;
        rc.on_rim[(u32)rc.edge_key[h]] = 1;
      }
    }
    rc.share.resize(nv);
    for (u32 i = 0; i < nv; i++) {
      rc.share[i] = rc.on_rim[rc.cls[i]] ? 0 : 255;
    }
    rc.valid = true;
  }
  perf::count(perf::Counter::OCEAN_RIM_REBUILT, hit ? 0. : 1.);
  for (u32 i = 0; i < nv; i++) {
    m_vertices[i].pad[1] = rc.share[i];
  }
}

void CommonOceanRenderer::water_edge_weights(SharedRenderState* render_state,
                                             const double inv[4][4],
                                             float plane_y,
                                             bool mid) {
  // lattice key of every vertex (0 when it has no world position)
  std::vector<u32> key(m_next_free_vertex, 0);
  double ox = 0, oz = 0;
  bool have_origin = false;
  float x0 = 1e30f, x1 = -1e30f, z0 = 1e30f, z1 = -1e30f;
  for (u32 i = 0; i < m_next_free_vertex; i++) {
    m_vertices[i].pad[0] = 255;
    double x, z;
    if (!water_vertex_world(inv, render_state, m_vertices[i].xyz, plane_y, &x, &z)) {
      continue;
    }
    x0 = std::min(x0, (float)x);
    x1 = std::max(x1, (float)x);
    z0 = std::min(z0, (float)z);
    z1 = std::max(z1, (float)z);
    if (!have_origin) {
      ox = x - kLattice * std::round(x / kLattice);
      oz = z - kLattice * std::round(z / kLattice);
      have_origin = true;
    }
    const long long kx = std::llround((x - ox) / kLattice) & 0x7fff;
    const long long kz = std::llround((z - oz) / kLattice) & 0x7fff;
    key[i] = (u32)((kx << 16) | kz) + 1;
  }
  if (!mid) {
    g_near_box = {m_next_free_vertex > 0, x0, x1, z0, z1};
    Timer rim_timer;
    near_rim_on_plane();
    perf::count(perf::Counter::OCEAN_RIM_CPU_MS, rim_timer.getMs());
    return;
  }
  // edges used by a single triangle of the textured pass are the mesh's edge
  std::unordered_map<u64, u32> edges;
  auto add = [&](u32 a, u32 b) {
    if (a && b && a != b) {
      edges[a < b ? ((u64)a << 32 | b) : ((u64)b << 32 | a)]++;
    }
  };
  u32 prev[2] = {0, 0};
  int run = 0;
  for (u32 i = 0; i < m_next_free_index[0]; i++) {
    const u32 v = m_indices[0][i];
    if (v == UINT32_MAX) {
      run = 0;
      continue;
    }
    if (run >= 2) {
      add(key[prev[0]], key[prev[1]]);
      add(key[prev[1]], key[v]);
      add(key[v], key[prev[0]]);
    }
    prev[0] = prev[1];
    prev[1] = v;
    run++;
  }
  std::unordered_set<u32> rim;
  for (const auto& [e, n] : edges) {
    if (n == 1) {
      rim.insert((u32)(e >> 32));
      rim.insert((u32)e);
    }
  }
  const float pad = 49152.f;  // 12 m
  const bool near_ok = g_near_box.ok;
  g_mid_rim = {false, 1e30f, -1e30f, 1e30f, -1e30f};
  for (u32 i = 0; i < m_next_free_vertex; i++) {
    if (!key[i] || !rim.count(key[i])) {
      continue;
    }
    double x, z;
    water_vertex_world(inv, render_state, m_vertices[i].xyz, plane_y, &x, &z);
    if (near_ok && x > g_near_box.x0 - pad && x < g_near_box.x1 + pad &&
        z > g_near_box.z0 - pad && z < g_near_box.z1 + pad) {
      continue;  // a hole the near ocean fills
    }
    m_vertices[i].pad[0] = 0;
    g_mid_rim = {true, std::min(g_mid_rim.x0, (float)x), std::max(g_mid_rim.x1, (float)x),
                 std::min(g_mid_rim.z0, (float)z), std::max(g_mid_rim.z1, (float)z)};
  }
}


void CommonOceanRenderer::water_prepare(SharedRenderState* render_state,
                                        bool mid,
                                        bool* plane_ok,
                                        float* plane_y) {
  m_use_sub = false;
  *plane_ok = false;
  *plane_y = 0.f;
  if (mid) {
    g_mid_rim.ok = false;
  }
  if (!water_active(render_state)) {
    return;
  }
  const auto& gs = Gfx::g_global_settings;
  // the game's own ocean height, which the far ocean also sits at
  *plane_ok = true;
  *plane_y = water_ocean_height(render_state);
  double inv[4][4];
  const bool have_inv = m_next_free_vertex && water_gs_inverse(render_state, inv);
  // per vertex: wave weight (pad[0], 0 on the mid's outer edge, where the flat ocean-far takes
  // over) and the share of its own depth in its rest point (pad[1], near_rim_on_plane)
  for (u32 i = 0; i < m_next_free_vertex; i++) {
    m_vertices[i].pad[0] = 255;
    m_vertices[i].pad[1] = mid ? 0 : 255;
  }
  if (have_inv && *plane_ok) {
    water_edge_weights(render_state, inv, *plane_y, mid);
  } else if (!mid) {
    g_near_box.ok = false;
  }
  if (gs.water_displace > 0.f && render_state->shaders.ocean_water_ready()) {
    triangulate();
    m_use_sub = true;
  }
}

void CommonOceanRenderer::flush_near(SharedRenderState* render_state, ScopedProfilerNode& prof) {
  const bool timing = Gfx::g_global_settings.ssao_gpu_timing;
  if (timing) {
    m_t_near.begin();
  }
  glBindVertexArray(m_ogl.vao);
  glBindBuffer(GL_ARRAY_BUFFER, m_ogl.vertex_buffer);
  glEnable(GL_PRIMITIVE_RESTART);
  glPrimitiveRestartIndex(UINT32_MAX);
  bool plane_ok = false;
  float plane_y = 0.f;
  Timer prep_timer;
  water_prepare(render_state, false, &plane_ok, &plane_y);
  perf::count(perf::Counter::OCEAN_NEAR_CPU_MS, prep_timer.getMs());
  glBufferData(GL_ARRAY_BUFFER, m_next_free_vertex * sizeof(Vertex), m_vertices.data(),
               GL_STREAM_DRAW);
  const ShaderId sid = m_use_sub ? ShaderId::OCEAN_WATER : ShaderId::OCEAN_COMMON;
  render_state->shaders[sid].activate();
  {
    auto sg = stereo_gs_parallax(*render_state);
    glUniform2f(
        glGetUniformLocation(render_state->shaders[sid].id(), "stereo_gs"),
        sg.x(), sg.y());
    set_xr_gs_uniforms(render_state->shaders[sid].id(), *render_state, 1);
  }
  glUniform4f(glGetUniformLocation(render_state->shaders[sid].id(), "fog_color"),
              render_state->fog_color[0] / 255.f, render_state->fog_color[1] / 255.f,
              render_state->fog_color[2] / 255.f, render_state->fog_intensity / 255);
  // the near vertices carry the game's own wave heights: rebuild them from depth (precise this
  // close), since moving them along the pixel ray onto the plane shifts them metres sideways;
  // only the rim, where the mid meets them, takes the plane (pad[1])
  water_setup_ocean(render_state, render_state->shaders[sid].id(), plane_y, plane_ok);
  glUniform4f(glGetUniformLocation(render_state->shaders[sid].id(), "water_rim"), 1.f, 0.f, 0.f,
              0.f);
  // the glint writes the emissive mask so bloom picks it up
  const bool water_em = water_active(render_state) && render_state->emissive_mask;

  glDepthMask(GL_FALSE);
  glEnable(GL_DEPTH_TEST);
  glEnable(GL_BLEND);
  glDepthFunc(GL_GEQUAL);

  for (int bucket = 0; bucket < 3; bucket++) {
    switch (bucket) {
      case 0: {
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
        glBlendEquation(GL_FUNC_ADD);
        auto tbp =
            render_state->version == GameVersion::Jak1 ? OCEAN_TEX_TBP_JAK1 : OCEAN_TEX_TBP_JAK2;
        auto tex = render_state->texture_pool->lookup(tbp);
        if (!tex) {
          tex = render_state->texture_pool->get_placeholder_texture();
        }
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, *tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glUniform1i(
            glGetUniformLocation(render_state->shaders[sid].id(), "tex_T0"), 0);
        glUniform1i(
            glGetUniformLocation(render_state->shaders[sid].id(), "bucket"), 0);
        // with water on the near texture has mips (OceanTexture::handle_ocean_texture_jak1)
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                        water_active(render_state) ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      }

      break;
      case 1:
        glBlendFuncSeparate(GL_ZERO, GL_ONE, GL_ONE, GL_ZERO);
        glUniform1f(
            glGetUniformLocation(render_state->shaders[sid].id(), "alpha_mult"),
            1.f);
        glUniform1i(
            glGetUniformLocation(render_state->shaders[sid].id(), "bucket"), 1);
        break;
      case 2:
        auto tex = render_state->texture_pool->lookup(m_envmap_tex);
        if (!tex) {
          tex = render_state->texture_pool->get_placeholder_texture();
        }
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, *tex);

        glBlendFuncSeparate(GL_DST_ALPHA, GL_ONE, GL_ONE, GL_ZERO);
        glBlendEquation(GL_FUNC_ADD);
        glUniform1i(
            glGetUniformLocation(render_state->shaders[sid].id(), "bucket"), 2);
        break;
    }
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ogl.index_buffer[bucket]);
    const u32 count = m_use_sub ? (u32)m_sub_indices[bucket].size() : m_next_free_index[bucket];
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, count * sizeof(u32),
                 m_use_sub ? m_sub_indices[bucket].data() : m_indices[bucket].data(),
                 GL_STREAM_DRAW);
    if (bucket == 0 && water_em) {
      SsaoRenderer::emissive_draw_buffers(true);
    }
    if (bucket == 0) {
      frags_begin(m_frags_near, perf::Counter::OCEAN_NEAR_MFRAGS);
    }
    if (m_use_sub) {
      glPatchParameteri(GL_PATCH_VERTICES, 3);
      water_count_begin(bucket == 0);
      glDrawElements(GL_PATCHES, count, GL_UNSIGNED_INT, nullptr);
      water_count_end(bucket == 0);
    } else {
      glDrawElements(GL_TRIANGLE_STRIP, count, GL_UNSIGNED_INT, nullptr);
    }
    if (bucket == 0) {
      frags_end(m_frags_near);
    }
    if (bucket == 0 && water_em) {
      SsaoRenderer::emissive_draw_buffers(false);
    }
    prof.add_draw_call();
    prof.add_tri(m_use_sub ? count / 3 : count);
  }
  if (timing) {
    m_t_near.end();
  }
}

void CommonOceanRenderer::kick_from_mid(const u8* data) {
  bool eop = false;

  u32 offset = 0;
  while (!eop) {
    GifTag tag(data + offset);
    offset += 16;

    // unpack registers.
    // faster to do it once outside of the nloop loop.
    GifTag::RegisterDescriptor reg_desc[16];
    u32 nreg = tag.nreg();
    for (u32 i = 0; i < nreg; i++) {
      reg_desc[i] = tag.reg(i);
    }

    auto format = tag.flg();
    if (format == GifTag::Format::PACKED) {
      if (tag.nreg() == 1) {
        ASSERT(!tag.pre());
        ASSERT(tag.nloop() == 5);
        handle_mid_adgif(data, offset);
        offset += 5 * 16;
      } else {
        ASSERT(tag.nreg() == 3);
        ASSERT(tag.pre());
        m_current_bucket = GsPrim(tag.prim()).abe() ? 1 : 0;

        int count = tag.nloop();
        if (GsPrim(tag.prim()).kind() == GsPrim::Kind::TRI_STRIP) {
          handle_near_vertex_gif_data_strip(data, offset, tag.nloop());
        } else {
          handle_near_vertex_gif_data_fan(data, offset, tag.nloop());
        }
        offset += 3 * 16 * count;
        // todo handle.
      }
    } else {
      ASSERT(false);  // format not packed or reglist.
    }

    eop = tag.eop();
  }
}

void CommonOceanRenderer::handle_mid_adgif(const u8* data, u32 offset) {
  u32 most_recent_tbp = 0;

  for (u32 i = 0; i < 5; i++) {
    u64 value;
    GsRegisterAddress addr;
    memcpy(&value, data + offset + 16 * i, sizeof(u64));
    memcpy(&addr, data + offset + 16 * i + 8, sizeof(GsRegisterAddress));
    switch (addr) {
      case GsRegisterAddress::MIPTBP1_1:
      case GsRegisterAddress::MIPTBP2_1:
        // ignore this, it's just mipmapping settings
        break;
      case GsRegisterAddress::TEX1_1: {
        GsTex1 reg(value);
        ASSERT(reg.mmag());
      } break;
      case GsRegisterAddress::CLAMP_1: {
        bool s = value & 0b001;
        bool t = value & 0b100;
        ASSERT(s == t);
      } break;
      case GsRegisterAddress::TEX0_1: {
        GsTex0 reg(value);
        ASSERT(reg.tfx() == GsTex0::TextureFunction::MODULATE);
        most_recent_tbp = reg.tbp0();
      } break;
      case GsRegisterAddress::ALPHA_1: {
      } break;

      default:
        lg::debug("reg: {}", register_address_name(addr));
        break;
    }
  }

  if (most_recent_tbp != OCEAN_TEX_TBP_JAK2) {
    m_envmap_tex = most_recent_tbp;
  }

  if (m_vertices.size() - 128 < m_next_free_vertex) {
    ASSERT(false);  // add more vertices.
  }
}

void CommonOceanRenderer::init_for_mid() {
  m_mid_pending = false;
  m_next_free_vertex = 0;
  for (auto& x : m_next_free_index) {
    x = 0;
  }
}

void reverse_indices(u32* indices, u32 count) {
  if (count) {
    for (u32 a = 0, b = count - 1; a < b; a++, b--) {
      std::swap(indices[a], indices[b]);
    }
  }
}

/// Reverse the order of triangles but not the corners within each, so every patch tessellates
/// exactly as in the textured pass and the envmap pass meets its depth (GEQUAL) everywhere.
void reverse_patches(u32* indices, u32 count) {
  for (u32 a = 0, b = count / 3; a + 1 < b; a++, b--) {
    for (int k = 0; k < 3; k++) {
      std::swap(indices[3 * a + k], indices[3 * (b - 1) + k]);
    }
  }
}

void CommonOceanRenderer::flush_mid(SharedRenderState* render_state, ScopedProfilerNode& prof) {
  if (water_active(render_state) && render_state->has_pc_data) {
    m_mid_pending = true;
    return;
  }
  draw_mid(render_state, prof, false);
}

void CommonOceanRenderer::flush_mid_deferred(SharedRenderState* render_state,
                                             ScopedProfilerNode& prof) {
  if (!m_mid_pending) {
    return;
  }
  m_mid_pending = false;
  draw_mid(render_state, prof, true);
}

void CommonOceanRenderer::draw_mid(SharedRenderState* render_state,
                                   ScopedProfilerNode& prof,
                                   bool deferred) {
  const bool timing = Gfx::g_global_settings.ssao_gpu_timing;
  if (timing) {
    m_t_mid.begin();
  }
  glBindVertexArray(m_ogl.vao);
  glBindBuffer(GL_ARRAY_BUFFER, m_ogl.vertex_buffer);
  glEnable(GL_PRIMITIVE_RESTART);
  glPrimitiveRestartIndex(UINT32_MAX);
  bool plane_ok = false;
  float plane_y = 0.f;
  water_prepare(render_state, true, &plane_ok, &plane_y);
  glBufferData(GL_ARRAY_BUFFER, m_next_free_vertex * sizeof(Vertex), m_vertices.data(),
               GL_STREAM_DRAW);
  const ShaderId sid = m_use_sub ? ShaderId::OCEAN_WATER : ShaderId::OCEAN_COMMON;
  render_state->shaders[sid].activate();
  {
    auto sg = stereo_gs_parallax(*render_state);
    glUniform2f(
        glGetUniformLocation(render_state->shaders[sid].id(), "stereo_gs"),
        sg.x(), sg.y());
    set_xr_gs_uniforms(render_state->shaders[sid].id(), *render_state, 1);
  }
  glUniform4f(glGetUniformLocation(render_state->shaders[sid].id(), "fog_color"),
              render_state->fog_color[0] / 255.f, render_state->fog_color[1] / 255.f,
              render_state->fog_color[2] / 255.f, render_state->fog_intensity / 255);
  water_setup_ocean(render_state, render_state->shaders[sid].id(), plane_y,
                    plane_ok);
  {
    // the shading fades to the game's own colour toward the outer rim (box in metres)
    const auto& r = g_mid_rim;
    const float k = 1.f / 4096.f;
    glUniform4f(glGetUniformLocation(render_state->shaders[sid].id(), "water_rim"),
                r.ok ? r.x0 * k : 1.f, r.ok ? r.x1 * k : 0.f, r.z0 * k, r.z1 * k);
  }
  // SANDOVER_WATER_DIAG: which camera the mid ocean displaces with, and whether it was deferred
  static const bool diag = std::getenv("SANDOVER_WATER_DIAG") != nullptr;
  if (diag && water_active(render_state) &&
      (render_state->frame_idx % 30 == 0 || g_burst_frame >= 0)) {
    u64 cf = 0;
    math::Vector3f cp(0, 0, 0);
    const bool stamped = water_cam_stamp(render_state->shaders[sid].id(), &cf, &cp);
    const auto& now = *frame_camera(*render_state).pos;
    lg::info(
        "WATER-MID f={} deferred={} pc={} sub={} cam_stamped={} cam_age={} cam_from=({:.1f} "
        "{:.1f} {:.1f}) cam_now=({:.1f} {:.1f} {:.1f}) verts={} patches={}",
        render_state->frame_idx, deferred, render_state->has_pc_data, m_use_sub, stamped,
        stamped ? (long long)(render_state->frame_idx - cf) : -1LL, cp.x() / 4096.f,
        cp.y() / 4096.f, cp.z() / 4096.f, now.x() / 4096.f, now.y() / 4096.f, now.z() / 4096.f,
        m_next_free_vertex, m_use_sub ? (u32)m_sub_indices[0].size() / 3 : 0u);
  }
  // the glint writes the emissive mask so bloom picks it up
  const bool water_em = water_active(render_state) && render_state->emissive_mask;

  glDepthMask(GL_TRUE);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_ALWAYS);
  glDisable(GL_BLEND);
  if (deferred) {
    // the background is already drawn: test against it, and win ties with the far ocean
    glDepthFunc(GL_GEQUAL);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(2.f, 64.f);
  }

  // note:
  // there are some places where the game draws the same section of ocean twice, in this order:
  // - low poly mesh with ocean texture
  // - low poly mesh with envmap texture
  // - high poly mesh with ocean texture (overwrites previous draw)
  // - high poly mesh with envmap texture (overwrites previous draw)

  // we draw all ocean textures together and all envmap textures togther. luckily, there's a trick
  // we can use to get the same result.
  // first, we'll draw all ocean textures. The high poly mesh is drawn second, so it wins.
  // then, we'll draw all envmaps, but with two changes:
  // - first, we draw it in reverse, so the high poly versions are drawn first
  // - second, we'll modify the shader to set alpha = 0 of the destination. when the low poly
  //    version is drawn on top, it won't draw at all because of the blending mode
  //    (s_factor = DST_ALPHA, d_factor = 1)

  const auto& gs = Gfx::g_global_settings;
  // one pass for both (perf bit 14): with the depth test each pixel keeps the patch the
  // textured pass would, and the envmap it adds is that patch's
  const bool merge_on =
      m_use_sub && deferred && Gfx::perf_opt(PERF_OPT_WATER + 2) && gs.water_view == 0;
  const bool merge = merge_on && merge_envmap();
  if (merge_on && perf::on() && render_state->frame_idx % 600 == 0) {
    lg::info("WATER-MID envmap merge={} textured={} envmap={} unpaired envmap={} (patch corners)",
             merge, m_sub_indices[0].size(), m_sub_indices[1].size(),
             merge ? m_env_rest.size() : m_sub_indices[1].size());
  }
  const GLuint prog = render_state->shaders[sid].id();
  glUniform1i(glGetUniformLocation(prog, "water_env_merge"),
              merge ? (render_state->hdr_target ? 1 : 2) : 0);
  if (merge) {
    glBindBuffer(GL_ARRAY_BUFFER, m_ogl.vertex_buffer);
    glBufferData(GL_ARRAY_BUFFER, m_merge_verts.size() * sizeof(Vertex), m_merge_verts.data(),
                 GL_STREAM_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, m_ogl.env_buffer);
    glBufferData(GL_ARRAY_BUFFER, m_env_attr.size() * sizeof(EnvAttr), m_env_attr.data(),
                 GL_STREAM_DRAW);
    glEnableVertexAttribArray(4);
    glEnableVertexAttribArray(5);
    auto etex = render_state->texture_pool->lookup(m_envmap_tex);
    if (!etex) {
      etex = render_state->texture_pool->get_placeholder_texture();
    }
    glActiveTexture(GL_TEXTURE6);
    glBindTexture(GL_TEXTURE_2D, *etex);
    // as the envmap pass sets them on its texture
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glUniform1i(glGetUniformLocation(prog, "tex_env"), 6);
    glActiveTexture(GL_TEXTURE0);
  }

  // draw it in reverse (merged: only the envmap patches without a textured twin)
  if (merge) {
    reverse_patches(m_env_rest.data(), (u32)m_env_rest.size());
  } else if (m_use_sub) {
    reverse_patches(m_sub_indices[1].data(), (u32)m_sub_indices[1].size());
  } else if (!m_use_sub) {
    reverse_indices(m_indices[1].data(), m_next_free_index[1]);
  }

  if (m_use_sub) {
    perf::count(perf::Counter::OCEAN_MID_PATCHES, (double)(m_sub_indices[0].size() / 3));
  }
  // perf bit 26: depth first, then the shading at EQUAL runs once per pixel. Same program and
  // offset give the same depths (invariant gl_Position); the last fragment at a tie still wins.
  const bool once = m_use_sub && deferred && Gfx::perf_opt(PERF_OPT_OCEAN_ONCE) &&
                    gs.water_view == 0;
  const std::vector<u32>& tex_list = merge ? m_merge_idx : m_sub_indices[0];
  if (once) {
    perf::Scope pd(perf::Group::OCEAN_MID_DEPTH);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ogl.index_buffer[0]);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, tex_list.size() * sizeof(u32), tex_list.data(),
                 GL_STREAM_DRAW);
    // no water shading and no bucket: the fragment shader does one texture read
    glUniform1i(glGetUniformLocation(prog, "water_on"), 0);
    glUniform1i(glGetUniformLocation(prog, "bucket"), 5);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glPatchParameteri(GL_PATCH_VERTICES, 3);
    glDrawElements(GL_PATCHES, (GLsizei)tex_list.size(), GL_UNSIGNED_INT, nullptr);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUniform1i(glGetUniformLocation(prog, "water_on"), 1);
    glDepthMask(GL_FALSE);
    glDepthFunc(GL_EQUAL);
    prof.add_draw_call();
  }
  const int passes = merge && m_env_rest.empty() ? 1 : 2;
  for (int bucket = 0; bucket < passes; bucket++) {
    perf::Scope ps(bucket == 0 ? perf::Group::OCEAN_MID_TEX : perf::Group::OCEAN_MID_ENVMAP);
    switch (bucket) {
      case 0: {
        auto tbp =
            render_state->version == GameVersion::Jak1 ? OCEAN_TEX_TBP_JAK1 : OCEAN_TEX_TBP_JAK2;
        auto tex = render_state->texture_pool->lookup(tbp);
        if (!tex) {
          tex = render_state->texture_pool->get_placeholder_texture();
        }
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, *tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glUniform1i(
            glGetUniformLocation(render_state->shaders[sid].id(), "tex_T0"), 0);
        glUniform1i(
            glGetUniformLocation(render_state->shaders[sid].id(), "bucket"), 3);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      }

      break;
      case 1:
        if (once) {
          glDepthMask(GL_TRUE);
          glDepthFunc(GL_GEQUAL);
        }
        glEnable(GL_BLEND);
        auto tex = render_state->texture_pool->lookup(m_envmap_tex);
        if (!tex) {
          tex = render_state->texture_pool->get_placeholder_texture();
        }
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, *tex);

        glBlendFuncSeparate(GL_DST_ALPHA, GL_ONE, GL_ONE, GL_ZERO);
        glBlendEquation(GL_FUNC_ADD);
        glUniform1i(
            glGetUniformLocation(render_state->shaders[sid].id(), "bucket"), 4);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        break;
    }
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ogl.index_buffer[bucket]);
    const std::vector<u32>* list = m_use_sub ? &m_sub_indices[bucket] : nullptr;
    if (merge) {
      list = bucket == 0 ? &m_merge_idx : &m_env_rest;
    }
    const u32 count = list ? (u32)list->size() : m_next_free_index[bucket];
    if (!(once && bucket == 0)) {  // the depth pass uploaded it
      glBufferData(GL_ELEMENT_ARRAY_BUFFER, count * sizeof(u32),
                   list ? list->data() : m_indices[bucket].data(), GL_STREAM_DRAW);
    }
    if (bucket == 0 && water_em) {
      SsaoRenderer::emissive_draw_buffers(true);
    }
    if (bucket == 0) {
      frags_begin(m_frags_mid, perf::Counter::OCEAN_MID_MFRAGS);
    }
    if (m_use_sub) {
      glPatchParameteri(GL_PATCH_VERTICES, 3);
      water_count_begin(bucket == 0);
      glDrawElements(GL_PATCHES, count, GL_UNSIGNED_INT, nullptr);
      water_count_end(bucket == 0);
    } else {
      glDrawElements(GL_TRIANGLE_STRIP, count, GL_UNSIGNED_INT, nullptr);
    }
    if (bucket == 0) {
      frags_end(m_frags_mid);
    }
    if (bucket == 0 && water_em) {
      SsaoRenderer::emissive_draw_buffers(false);
    }
    prof.add_draw_call();
    prof.add_tri(m_use_sub ? count / 3 : count);
  }
  if (merge) {
    glDisableVertexAttribArray(4);
    glDisableVertexAttribArray(5);
    glUniform1i(glGetUniformLocation(prog, "water_env_merge"), 0);
  }
  if (once) {
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_GEQUAL);
  }
  if (deferred) {
    glDisable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(0.f, 0.f);
  }
  if (timing) {
    m_t_mid.end();
  }
}
