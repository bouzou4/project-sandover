#include "Unbake.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

#include "common/log/log.h"
#include "common/util/Timer.h"
#include "common/util/exr.h"

#include "game/graphics/gfx.h"
#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/opengl_renderer/GlQueries.h"
#include "game/graphics/opengl_renderer/SsaoRenderer.h"
#include "game/graphics/opengl_renderer/background/background_common.h"

#include "fmt/format.h"
#include "third-party/imgui/imgui.h"

namespace {
constexpr u32 kMinVersion = 1;
constexpr u32 kVersion = 2;
constexpr size_t kHeaderBytes = 32;
constexpr size_t kSlotBytes = 64;
constexpr size_t kTreeBytes = 32;

template <typename T>
T read_at(const std::vector<u8>& data, size_t offset) {
  T v;
  memcpy(&v, data.data() + offset, sizeof(T));
  return v;
}

math::Vector3f normalized(const math::Vector3f& v) {
  float len = v.length();
  return len > 0 ? v / len : v;
}

float angle_between(const math::Vector3f& a, const math::Vector3f& b) {
  return std::acos(std::clamp(normalized(a).dot(normalized(b)), -1.f, 1.f));
}

using V3d = math::Vector<double, 3>;

// faces around a position further than this from the vertex's own faces do not smooth into it
constexpr double kPxCreaseCos = 0.5;

struct PxFace {
  u32 v[3];
  V3d n;       // area-weighted (length = twice the area, m^2), winding orientation
  V3d gu, gv;  // world gradients of u and v, UV per metre
  double area;
};

u32 oct_u16(const V3d& n) {
  const double s = std::abs(n.x()) + std::abs(n.y()) + std::abs(n.z());
  double x = n.x() / s, y = n.y() / s;
  if (n.z() < 0) {
    const double ox = (1 - std::abs(y)) * (x >= 0 ? 1 : -1);
    const double oy = (1 - std::abs(x)) * (y >= 0 ? 1 : -1);
    x = ox;
    y = oy;
  }
  auto q = [](double e) { return (u32)std::clamp(std::lround((e + 1) * 32767.5), 0L, 65535L); };
  return q(x) | (q(y) << 16);
}

u32 half2(double a, double b) {
  auto h = [](double f) { return (u32)exr::float_to_half((float)std::clamp(f, -60000.0, 60000.0)); };
  return h(a) | (h(b) << 16);
}

bool same_mod1(float a, float b) {
  float d = a - b;
  d -= std::round(d);
  return std::abs(d) < 1e-4f;
}

/// Parallax basis texels for one tree (layout in Unbake.h, unbake_px_texture).
std::vector<u32> px_basis(const std::vector<tfrag3::PreloadedVertex>& vtx,
                          const std::vector<u32>& idx,
                          bool strips) {
  const size_t n = vtx.size();
  std::vector<PxFace> faces;
  auto pos = [&](u32 i) { return V3d(vtx[i].x / 4096.0, vtx[i].y / 4096.0, vtx[i].z / 4096.0); };
  auto add = [&](u32 a, u32 b, u32 c) {
    if (a >= n || b >= n || c >= n || a == b || b == c || a == c) {
      return;
    }
    const V3d p0 = pos(a), e1 = pos(b) - p0, e2 = pos(c) - p0;
    const V3d fn = e1.cross(e2);
    const double l2 = fn.squared_length();
    if (l2 < 1e-14) {
      return;
    }
    const V3d r1 = e2.cross(fn) / l2, r2 = fn.cross(e1) / l2;
    const double du1 = vtx[b].s - vtx[a].s, du2 = vtx[c].s - vtx[a].s;
    const double dv1 = vtx[b].t - vtx[a].t, dv2 = vtx[c].t - vtx[a].t;
    faces.push_back({{a, b, c}, fn, r1 * du1 + r2 * du2, r1 * dv1 + r2 * dv2, std::sqrt(l2) / 2});
  };
  if (strips) {
    u32 w[3] = {0, 0, 0};
    int k = 0;
    for (u32 i : idx) {
      if (i == UINT32_MAX) {
        k = 0;
        continue;
      }
      w[0] = w[1];
      w[1] = w[2];
      w[2] = i;
      if (++k >= 3) {
        (k & 1) ? add(w[0], w[1], w[2]) : add(w[0], w[2], w[1]);
      }
    }
  } else {
    for (size_t i = 0; i + 2 < idx.size(); i += 3) {
      add(idx[i], idx[i + 1], idx[i + 2]);
    }
  }

  std::vector<u32> start(n + 1, 0), list(faces.size() * 3);
  for (const auto& f : faces) {
    for (u32 v : f.v) {
      start[v + 1]++;
    }
  }
  for (size_t i = 0; i < n; i++) {
    start[i + 1] += start[i];
  }
  {
    std::vector<u32> fill(start.begin(), start.end() - 1);
    for (u32 fi = 0; fi < faces.size(); fi++) {
      for (u32 v : faces[fi].v) {
        list[fill[v]++] = fi;
      }
    }
  }

  // own fan normal: incident faces aligned to the running sum (winding is not kept consistent)
  std::vector<V3d> own(n, V3d::zero());
  for (size_t v = 0; v < n; v++) {
    V3d acc = V3d::zero();
    for (u32 j = start[v]; j < start[v + 1]; j++) {
      const V3d& fn = faces[list[j]].n;
      acc = acc.dot(fn) < 0 ? acc - fn : acc + fn;
    }
    const double l = acc.length();
    own[v] = l > 0 ? acc / l : V3d::zero();
  }

  struct Key {
    s64 x, y, z;
    bool operator==(const Key& o) const { return x == o.x && y == o.y && z == o.z; }
  };
  struct KeyHash {
    size_t operator()(const Key& k) const {
      return std::hash<s64>()(k.x * 73856093LL ^ k.y * 19349663LL ^ k.z * 83492791LL);
    }
  };
  std::unordered_map<Key, u32, KeyHash> group_of;
  std::vector<u32> group(n);
  std::vector<std::vector<u32>> members;
  for (size_t v = 0; v < n; v++) {
    const Key k{std::llround(vtx[v].x), std::llround(vtx[v].y), std::llround(vtx[v].z)};
    auto [it, fresh] = group_of.try_emplace(k, (u32)members.size());
    if (fresh) {
      members.emplace_back();
    }
    group[v] = it->second;
    members[it->second].push_back((u32)v);
  }

  std::vector<u32> out(n * 4, 0);
  for (size_t v = 0; v < n; v++) {
    const V3d& nv = own[v];
    if (nv.squared_length() == 0) {
      continue;
    }
    V3d N = V3d::zero(), gu = V3d::zero(), gv = V3d::zero();
    double wsum = 0;
    for (u32 w : members[group[v]]) {
      const bool uv_match =
          w == v || (same_mod1(vtx[w].s, vtx[v].s) && same_mod1(vtx[w].t, vtx[v].t));
      for (u32 j = start[w]; j < start[w + 1]; j++) {
        const PxFace& f = faces[list[j]];
        const double d = f.n.dot(nv);
        if (w != v && std::abs(d) < kPxCreaseCos * 2 * f.area) {
          continue;
        }
        N += d < 0 ? f.n * -1.0 : f.n;
        if (uv_match) {
          gu += f.gu * f.area;
          gv += f.gv * f.area;
          wsum += f.area;
        }
      }
    }
    if (wsum <= 0 || N.squared_length() == 0) {
      continue;
    }
    gu /= wsum;
    gv /= wsum;
    out[v * 4] = oct_u16(N / N.length());
    out[v * 4 + 1] = half2(gu.x(), gu.y());
    out[v * 4 + 2] = half2(gu.z(), gv.x());
    out[v * 4 + 3] = half2(gv.y(), gv.z());
  }
  return out;
}

math::Vector3f slerp(const math::Vector3f& a, const math::Vector3f& b, float t) {
  float th = angle_between(a, b);
  if (th < 1e-4f) {
    return normalized(a);
  }
  float s = std::sin(th);
  return normalized(normalized(a) * (std::sin((1 - t) * th) / s) +
                    normalized(b) * (std::sin(t * th) / s));
}

/// Palette blend weights as the SSE blend applies them: (itimes halfword & 0xff) / 64.
void slot_weights(const math::Vector<s32, 4> itimes[4], float w[8][3]) {
  for (int slot = 0; slot < 8; slot++) {
    for (int ch = 0; ch < 3; ch++) {
      u32 word = (u32)itimes[slot / 2][(slot % 2) * 2 + ch / 2];
      u32 hw = (ch % 2) ? (word >> 16) : word;
      w[slot][ch] = (hw & 0xff) / 64.f;
    }
  }
}

struct SlotTarget {
  math::Vector3f dir, lgt, amb;
  int v0 = 0, v1 = 0;
  float vf = 0;
};

SlotTarget identity_target(const UnbakeSidecar& sc, int s) {
  return {sc.slots[s].dir_fit, sc.slots[s].lgt, sc.slots[s].amb, s, s, 0.f};
}

/// Day-sun target at arc position u in [0, n-1] over the fitted day slots.
SlotTarget arc_target(const UnbakeSidecar& sc, const std::vector<int>& day, float u) {
  u = std::clamp(u, 0.f, (float)(day.size() - 1));
  int i0 = std::min((int)u, (int)day.size() - 1);
  int i1 = std::min(i0 + 1, (int)day.size() - 1);
  float f = u - i0;
  const auto& a = sc.slots[day[i0]];
  const auto& b = sc.slots[day[i1]];
  return {slerp(a.dir_fit, b.dir_fit, f),
          a.lgt * (1 - f) + b.lgt * f,
          a.amb * (1 - f) + b.amb * f,
          day[i0],
          day[i1],
          f};
}

/// Moves arc position u by deg degrees of sun travel along the fitted polyline.
float arc_offset(const UnbakeSidecar& sc, const std::vector<int>& day, float u, float deg) {
  std::vector<float> acc(day.size(), 0.f);
  for (size_t i = 1; i < day.size(); i++) {
    acc[i] = acc[i - 1] + angle_between(sc.slots[day[i - 1]].dir_fit, sc.slots[day[i]].dir_fit);
  }
  u = std::clamp(u, 0.f, (float)(day.size() - 1));
  int i0 = std::min((int)u, (int)day.size() - 2);
  float ang = acc[i0] + (u - i0) * (acc[i0 + 1] - acc[i0]) + deg * 3.14159265f / 180.f;
  if (ang <= 0) {
    return 0;
  }
  for (size_t i = 1; i < day.size(); i++) {
    if (ang <= acc[i]) {
      float seg = acc[i] - acc[i - 1];
      return (i - 1) + (seg > 0 ? (ang - acc[i - 1]) / seg : 0.f);
    }
  }
  return (float)(day.size() - 1);
}

/// Arc position of an explicit direction: best-fitting pair of neighbouring day slots.
SlotTarget direction_target(const UnbakeSidecar& sc,
                            const std::vector<int>& day,
                            const math::Vector3f& dir) {
  float best = 1e9f;
  float best_u = 0;
  for (size_t i = 0; i + 1 < day.size(); i++) {
    float a0 = angle_between(sc.slots[day[i]].dir_fit, dir);
    float a1 = angle_between(sc.slots[day[i + 1]].dir_fit, dir);
    float seg = angle_between(sc.slots[day[i]].dir_fit, sc.slots[day[i + 1]].dir_fit);
    float score = a0 + a1 - seg;
    if (score < best) {
      best = score;
      best_u = i + (a0 + a1 > 0 ? a0 / (a0 + a1) : 0.f);
    }
  }
  auto t = arc_target(sc, day, best_u);
  t.dir = normalized(dir);
  return t;
}
}  // namespace

u64 unbake_pos_digest(const float* xyz, size_t vertex_count, size_t stride_floats) {
  constexpr u64 kM = 0x9E3779B97F4A7C15ull;
  u64 h = 0;
  u64 j = 1;
  for (size_t i = 0; i < vertex_count; i++) {
    for (int c = 0; c < 3; c++) {
      u32 bits;
      memcpy(&bits, xyz + i * stride_floats + c, 4);
      h += ((u64)bits + 1) * (j * kM);
      j++;
    }
  }
  return h;
}

fs::path unbake_find(const fs::path& fr3_dir,
                     const std::string& level_name,
                     const char* sandover_data,
                     const fs::path& project_dir) {
  const auto name = level_name + ".unbake";
  std::vector<fs::path> dirs = {fr3_dir};
  if (sandover_data && *sandover_data) {
    dirs.push_back(fs::path(sandover_data) / "lighting");
  }
  dirs.push_back(project_dir / "sandover" / "data" / "lighting");
  for (const auto& d : dirs) {
    if (fs::exists(d / name)) {
      return d / name;
    }
  }
  return {};
}

fs::path unbake_path(const fs::path& fr3_dir, const std::string& level_name) {
  return unbake_find(fr3_dir, level_name, std::getenv("SANDOVER_DATA"),
                     file_util::get_jak_project_dir());
}

std::unique_ptr<UnbakeSidecar> unbake_load(const fs::path& path,
                                           const tfrag3::Level& level,
                                           const std::string& level_name) {
  if (path.empty() || !fs::exists(path)) {
    lg::info("unbake: {} no sidecar found, stock lighting", level_name);
    return nullptr;
  }
  lg::info("unbake: {} sidecar {}", level_name, path.string());
  std::vector<u8> data;
  try {
    data = file_util::read_binary_file(path);
  } catch (const std::exception& e) {
    lg::warn("unbake: {} ignored, {}", path.string(), e.what());
    return nullptr;
  }
  if (data.size() < kHeaderBytes + 8 * kSlotBytes || memcmp(data.data(), "SOVU", 4) != 0 ||
      read_at<u32>(data, 4) < kMinVersion || read_at<u32>(data, 4) > kVersion ||
      read_at<u32>(data, 8) != tfrag3::TFRAG3_VERSION) {
    lg::warn("unbake: {} ignored, bad header or version", path.string());
    return nullptr;
  }
  auto result = std::make_unique<UnbakeSidecar>();
  const u32 tree_count = read_at<u32>(data, 12);
  result->slot_mask = read_at<u32>(data, 16);
  result->day_mask = read_at<u32>(data, 20);
  result->has_emissive = read_at<u32>(data, 4) >= 2;
  result->night_mask = result->has_emissive ? read_at<u32>(data, 24) : 0;
  result->emitter_mask = result->has_emissive ? read_at<u32>(data, 28) : 0;
  // older headers counted the moon slot as day (misty 1, firecanyon/snow 3, rolling 2-3)
  result->day_mask &= ~result->night_mask;
  for (int s = 0; s < 8; s++) {
    float f[16];
    memcpy(f, data.data() + kHeaderBytes + s * kSlotBytes, sizeof(f));
    auto& slot = result->slots[s];
    slot.dir_fit = math::Vector3f(f[0], f[1], f[2]);
    slot.lgt = math::Vector3f(f[4], f[5], f[6]);
    slot.amb = math::Vector3f(f[8], f[9], f[10]);
    slot.dir_table = math::Vector3f(f[12], f[13], f[14]);
  }
  result->tfrag.resize(level.tfrag_trees[0].size());
  result->tie.resize(level.tie_trees[0].size());

  int ok = 0;
  size_t verts = 0;
  double px_ms = 0;
  std::string bad;
  const size_t table = kHeaderBytes + 8 * kSlotBytes;
  for (u32 i = 0; i < tree_count && table + (i + 1) * kTreeBytes <= data.size(); i++) {
    const size_t e = table + i * kTreeBytes;
    const u32 kind = read_at<u32>(data, e);
    const u32 idx = read_at<u32>(data, e + 4);
    const u32 count = read_at<u32>(data, e + 8);
    const u64 digest = read_at<u64>(data, e + 16);
    const u64 offset = read_at<u64>(data, e + 24);
    auto& out = kind == 0 ? result->tfrag : result->tie;
    const std::vector<tfrag3::PreloadedVertex>* vtx = nullptr;
    const std::vector<u32>* ind = nullptr;
    bool strips = true;
    if (kind == 0 && idx < level.tfrag_trees[0].size()) {
      vtx = &level.tfrag_trees[0][idx].unpacked.vertices;
      ind = &level.tfrag_trees[0][idx].unpacked.indices;
      strips = level.tfrag_trees[0][idx].use_strips;
    } else if (kind == 1 && idx < level.tie_trees[0].size()) {
      vtx = &level.tie_trees[0][idx].unpacked.vertices;
      ind = &level.tie_trees[0][idx].unpacked.indices;
      strips = level.tie_trees[0][idx].use_strips;
    }
    const char* kind_name = kind == 0 ? "tfrag" : "tie";
    if (!vtx || vtx->empty() || vtx->size() != count || offset > data.size() ||
        (u64)count * 16 > data.size() - offset) {
      bad += fmt::format(" {}{}:count", kind_name, idx);
      continue;
    }
    static_assert(sizeof(tfrag3::PreloadedVertex) % 4 == 0);
    const u64 have =
        unbake_pos_digest(&vtx->front().x, vtx->size(), sizeof(tfrag3::PreloadedVertex) / 4);
    if (have != digest) {
      bad += fmt::format(" {}{}:digest", kind_name, idx);
      continue;
    }
    auto& tree = out.at(idx);
    tree.vertex_count = count;
    tree.records.resize((size_t)count * 4);
    memcpy(tree.records.data(), data.data() + offset, (size_t)count * 16);
    Timer px_timer;
    tree.px_basis = px_basis(*vtx, *ind, strips);
    px_ms += px_timer.getMs();
    tree.valid = true;
    ok++;
    verts += count;
  }
  lg::info("unbake: {} parallax basis for {} vertices in {:.0f} ms ({:.1f} MB)", level_name, verts,
           px_ms, verts * 16 / 1048576.0);
  if (bad.empty()) {
    lg::info("unbake: {} {}/{} trees match ({} vertices)", level_name, ok, tree_count, verts);
  } else {
    lg::warn("unbake: {} {}/{} trees match ({} vertices), mismatch:{}", level_name, ok, tree_count,
             verts, bad);
  }
  if (ok == 0) {
    return nullptr;
  }
  return result;
}

GLuint unbake_texture(const UnbakeTree& tree) {
  if (!tree.valid) {
    return 0;
  }
  if (!tree.gl_texture) {
    glGenBuffers(1, &tree.gl_buffer);
    glBindBuffer(GL_TEXTURE_BUFFER, tree.gl_buffer);
    glBufferData(GL_TEXTURE_BUFFER, tree.records.size() * sizeof(u32), tree.records.data(),
                 GL_STATIC_DRAW);
    glGenTextures(1, &tree.gl_texture);
    glBindTexture(GL_TEXTURE_BUFFER, tree.gl_texture);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32UI, tree.gl_buffer);
    glBindBuffer(GL_TEXTURE_BUFFER, 0);
    glBindTexture(GL_TEXTURE_BUFFER, 0);
  }
  return tree.gl_texture;
}

GLuint unbake_px_texture(const UnbakeTree& tree) {
  if (!tree.valid || tree.px_basis.empty()) {
    return 0;
  }
  if (!tree.px_texture) {
    glGenBuffers(1, &tree.px_buffer);
    glBindBuffer(GL_TEXTURE_BUFFER, tree.px_buffer);
    glBufferData(GL_TEXTURE_BUFFER, tree.px_basis.size() * sizeof(u32), tree.px_basis.data(),
                 GL_STATIC_DRAW);
    glGenTextures(1, &tree.px_texture);
    glBindTexture(GL_TEXTURE_BUFFER, tree.px_texture);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32UI, tree.px_buffer);
    glBindBuffer(GL_TEXTURE_BUFFER, 0);
    glBindTexture(GL_TEXTURE_BUFFER, 0);
  }
  return tree.px_texture;
}

void unbake_bind_normals(GLuint program, const UnbakeTree& tree) {
  glActiveTexture(GL_TEXTURE11);
  glBindTexture(GL_TEXTURE_BUFFER, unbake_texture(tree));
  glActiveTexture(GL_TEXTURE0);
  SsaoRenderer::set_normal_mode(program, 2);
}

void unbake_release(const UnbakeSidecar& sidecar,
                    std::vector<GLuint>& buffers,
                    std::vector<GLuint>& textures) {
  for (const auto* trees : {&sidecar.tfrag, &sidecar.tie}) {
    for (const auto& t : *trees) {
      if (t.gl_texture) {
        textures.push_back(t.gl_texture);
        buffers.push_back(t.gl_buffer);
        t.gl_texture = t.gl_buffer = 0;
      }
      if (t.px_texture) {
        textures.push_back(t.px_texture);
        buffers.push_back(t.px_buffer);
        t.px_texture = t.px_buffer = 0;
      }
    }
  }
}

void unbake_emissive_weights(const UnbakeSidecar& sc,
                             const GoalBackgroundCameraData& camera,
                             float out[kUnbakeEmissiveSources]) {
  float w[8][3];
  slot_weights(camera.itimes, w);
  float night = 0;
  for (int s = 0; s < 8; s++) {
    out[s] = (sc.emitter_mask & (1u << s)) ? std::clamp(w[s][1], 0.f, 1.f) : 0.f;
    if (sc.night_mask & (1u << s)) {
      night += w[s][1];
    }
  }
  out[8] = 1.f;
  out[9] = std::clamp(night, 0.f, 1.f);
}

namespace {
struct LastWeights {
  const UnbakeSidecar* sc = nullptr;
  float w[kUnbakeEmissiveSources] = {};
};
LastWeights g_last_weights[8];
int g_last_weights_next = 0;

void note_emissive_weights(const UnbakeSidecar* sc, const float* w) {
  LastWeights* slot = nullptr;
  for (auto& e : g_last_weights) {
    if (e.sc == sc) {
      slot = &e;
      break;
    }
  }
  if (!slot) {
    slot = &g_last_weights[g_last_weights_next];
    g_last_weights_next = (g_last_weights_next + 1) % 8;
    slot->sc = sc;
  }
  std::copy(w, w + kUnbakeEmissiveSources, slot->w);
}
}  // namespace

bool unbake_last_emissive_weights(const UnbakeSidecar* sc, float out[kUnbakeEmissiveSources]) {
  for (const auto& e : g_last_weights) {
    if (sc && e.sc == sc) {
      std::copy(e.w, e.w + kUnbakeEmissiveSources, out);
      return true;
    }
  }
  return false;
}

void unbake_set_emissive_uniforms(GLuint program,
                                  const UnbakeSidecar* sc,
                                  const UnbakeTree* tree,
                                  const GoalBackgroundCameraData& camera,
                                  bool note) {
  const bool on = sc && sc->has_emissive && tree && tree->valid;
  glUniform1i(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "em_on"), on ? 1 : 0);
  if (!on) {
    return;
  }
  float w[kUnbakeEmissiveSources];
  unbake_emissive_weights(*sc, camera, w);
  if (note) {
    note_emissive_weights(sc, w);
  }
  glUniform1fv(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "em_w"), kUnbakeEmissiveSources, w);
  glActiveTexture(GL_TEXTURE11);
  glBindTexture(GL_TEXTURE_BUFFER, unbake_texture(*tree));
  glActiveTexture(GL_TEXTURE0);
}

void unbake_slot_weights(const math::Vector<s32, 4> itimes[4], float w[8][3]) {
  slot_weights(itimes, w);
}

bool unbake_active(const SharedRenderState* render_state, const UnbakeSidecar* sidecar) {
  // actor shadows read the sun share from the same shader
  return sidecar && render_state->version == GameVersion::Jak1 &&
         (Gfx::g_global_settings.sun_enable || render_state->shadow.active);
}

bool unbake_sun_summary(const UnbakeSidecar& sc,
                        const math::Vector<s32, 4> itimes[4],
                        math::Vector3f* dir,
                        float* share) {
  float w[8][3];
  slot_weights(itimes, w);
  math::Vector3f d(0, 0, 0);
  float sun = 0, total = 0;
  for (int s = 0; s < 8; s++) {
    if (!(sc.slot_mask & (1u << s))) {
      continue;
    }
    const float ws = (w[s][0] + w[s][1] + w[s][2]) / 3.f;
    const auto& l = sc.slots[s].lgt;
    const auto& a = sc.slots[s].amb;
    const float ls = 0.299f * l.x() + 0.587f * l.y() + 0.114f * l.z();
    const float la = 0.299f * a.x() + 0.587f * a.y() + 0.114f * a.z();
    d += normalized(sc.slots[s].dir_fit) * (ws * ls);
    sun += ws * ls;
    total += ws * (ls + la);
  }
  if (d.length() < 1e-6f || total <= 0) {
    return false;
  }
  *dir = normalized(d);
  *share = sun / total;
  return true;
}

void unbake_set_uniforms(SharedRenderState* render_state,
                         const UnbakeSidecar& sc,
                         const GoalBackgroundCameraData& camera,
                         GLuint texture) {
  const auto& gs = Gfx::g_global_settings;
  const GLuint id = render_state->shaders[ShaderId::TFRAG3_SUN].id();
  float w[8][3];
  slot_weights(camera.itimes, w);

  std::vector<int> day;
  for (int s = 0; s < 8; s++) {
    if (sc.day_mask & (1u << s)) {
      day.push_back(s);
    }
  }

  // arc position of the vanilla blend: the live day slots and their weights
  float wsum = 0, usum = 0;
  for (size_t i = 0; i < day.size(); i++) {
    wsum += w[day[i]][1];
    usum += w[day[i]][1] * i;
  }
  const bool day_live = wsum > 0 && day.size() >= 2;
  float u = day_live ? usum / wsum : 0.f;
  SlotTarget day_target;
  // with the moving sun off (shadows only) the targets are the fitted slots and the delta is 0
  const auto mode = gs.sun_enable ? (SunMode)gs.sun_mode : SunMode::IDENTITY;
  const int night = gs.sun_enable ? gs.sun_night : (int)SunNight::FITTED;
  if (day_live) {
    if (mode == SunMode::ARC_POS) {
      u = gs.sun_arc_pos;
    }
    if (mode == SunMode::DIRECTION) {
      day_target =
          direction_target(sc, day, math::Vector3f(gs.sun_dir[0], gs.sun_dir[1], gs.sun_dir[2]));
    } else {
      day_target = arc_target(sc, day, arc_offset(sc, day, u, gs.sun_offset_deg));
    }
  }

  int mask = 0;
  math::Vector3f wv[8], dir[8], lgt[8], amb[8], tdir[8], tlgt[8], tamb[8];
  int tv[16];
  float tvf[8], sw[8];
  // with the world map the shadows cast one light: the sun takes the day slots' direct share,
  // the moon (night) the night slots'
  const bool one_light = gs.shadow_world || gs.shadow_world_actors || gs.shadow_world_water;
  const bool moon = render_state->shadow.active && render_state->shadow.moon;
  for (int s = 0; s < 8; s++) {
    SlotTarget t = identity_target(sc, s);
    const bool fitted = sc.slot_mask & (1u << s);
    const bool is_day = sc.day_mask & (1u << s);
    if (fitted && is_day && day_live && mode != SunMode::IDENTITY) {
      t = day_target;
    } else if (fitted && !is_day) {
      if (night == (int)SunNight::TABLE) {
        t.dir = normalized(sc.slots[s].dir_table);
      } else if (night == (int)SunNight::AMBIENT_ONLY) {
        t.lgt = math::Vector3f(0, 0, 0);
      }
    }
    wv[s] = math::Vector3f(w[s][0], w[s][1], w[s][2]);
    if (fitted && (w[s][0] > 0 || w[s][1] > 0 || w[s][2] > 0)) {
      mask |= 1 << s;
    }
    dir[s] = sc.slots[s].dir_fit;
    lgt[s] = sc.slots[s].lgt;
    amb[s] = sc.slots[s].amb;
    tdir[s] = t.dir;
    tlgt[s] = t.lgt;
    tamb[s] = t.amb;
    tv[2 * s] = t.v0;
    tv[2 * s + 1] = t.v1;
    tvf[s] = t.vf;
    const bool is_night = sc.night_mask & (1u << s);
    sw[s] = !one_light || (moon ? is_night : is_day) ? 1.f : 0.f;
  }

  glActiveTexture(GL_TEXTURE11);
  glBindTexture(GL_TEXTURE_BUFFER, texture);
  glActiveTexture(GL_TEXTURE0);
  glUniform1i(GLQ_LOC(PERF_OPT_GEOMETRY + 2, id, "ub_mask"), mask);
  glUniform3fv(GLQ_LOC(PERF_OPT_GEOMETRY + 2, id, "ub_w"), 8, wv[0].data());
  glUniform3fv(GLQ_LOC(PERF_OPT_GEOMETRY + 2, id, "ub_dir"), 8, dir[0].data());
  glUniform3fv(GLQ_LOC(PERF_OPT_GEOMETRY + 2, id, "ub_lgt"), 8, lgt[0].data());
  glUniform3fv(GLQ_LOC(PERF_OPT_GEOMETRY + 2, id, "ub_amb"), 8, amb[0].data());
  glUniform3fv(GLQ_LOC(PERF_OPT_GEOMETRY + 2, id, "ub_tdir"), 8, tdir[0].data());
  glUniform3fv(GLQ_LOC(PERF_OPT_GEOMETRY + 2, id, "ub_tlgt"), 8, tlgt[0].data());
  glUniform3fv(GLQ_LOC(PERF_OPT_GEOMETRY + 2, id, "ub_tamb"), 8, tamb[0].data());
  glUniform2iv(GLQ_LOC(PERF_OPT_GEOMETRY + 2, id, "ub_tv"), 8, tv);
  glUniform1fv(GLQ_LOC(PERF_OPT_GEOMETRY + 2, id, "ub_tvf"), 8, tvf);
  glUniform1fv(GLQ_LOC(PERF_OPT_GEOMETRY + 2, id, "ub_sw"), 8, sw);
  glUniform1f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, id, "ub_scale"),
              gs.sun_enable ? gs.sun_strength / 127.5f : 0.f);
  glUniform1i(GLQ_LOC(PERF_OPT_GEOMETRY + 2, id, "ub_view"), gs.sun_enable ? gs.sun_view : 0);
}

void unbake_draw_debug_window() {
  auto& gs = Gfx::g_global_settings;
  ImGui::Checkbox("Enable (needs <level>.unbake)", &gs.sun_enable);
  const char* modes[] = {"identity (fitted sun)", "follow the arc", "direction", "arc position"};
  ImGui::Combo("Mode", &gs.sun_mode, modes, 4);
  ImGui::SliderFloat("Arc offset (deg)", &gs.sun_offset_deg, -60.f, 60.f);
  ImGui::SliderFloat("Arc position", &gs.sun_arc_pos, 0.f, 4.f);
  ImGui::SliderFloat3("Direction", gs.sun_dir, -1.f, 1.f);
  const char* nights[] = {"fitted moon", "table moon", "ambient only"};
  ImGui::Combo("Night", &gs.sun_night, nights, 3);
  ImGui::SliderFloat("Strength", &gs.sun_strength, 0.f, 3.f);
  const char* views[] = {"none", "model", "confidence", "normal", "delta"};
  ImGui::Combo("View", &gs.sun_view, views, 5);
}
