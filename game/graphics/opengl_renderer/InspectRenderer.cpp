#include "InspectRenderer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "common/log/log.h"
#include "common/util/FileUtil.h"

#include "game/graphics/gfx.h"
#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/opengl_renderer/SsaoRenderer.h"
#include "game/graphics/opengl_renderer/background/Unbake.h"
#include "game/graphics/opengl_renderer/background/background_common.h"
#include "game/graphics/opengl_renderer/buckets.h"
#include "game/graphics/opengl_renderer/loader/Loader.h"
#include "game/graphics/texture/TexturePool.h"

#include "fmt/core.h"
#include "third-party/imgui/imgui.h"

namespace {
bool g_active = false;
std::vector<inspect::DrawRecord> g_records;

// programs whose fragment stage writes the ID target (inspect_frag.glsl)
constexpr ShaderId kInspectShaders[] = {
    ShaderId::TFRAG3,   ShaderId::TFRAG3_SUN, ShaderId::TFRAG3_NO_TEX, ShaderId::SHRUB,
    ShaderId::TIE_WIND, ShaderId::ETIE_BASE,  ShaderId::ETIE,          ShaderId::MERC2,
    ShaderId::EMERC,    ShaderId::GENERIC};
constexpr u32 kUnregistered = 0xff000000;

const char* kKindNames[] = {"none", "tfrag", "tie", "shrub", "merc", "generic"};

// slot roles, mirrored from sandover/tools/sandover/unbake/levels.py (sun slots are "sun")
struct LevelRoles {
  const char* level;
  const char* roles[8];
};
constexpr const char* S = "sun";
const LevelRoles kRoles[] = {
    {"village1", {S, S, S, S, S, "flames", S, S}},
    {"beach", {S, S, S, S, S, "flames", S, S}},
    {"jungle", {S, S, S, S, S, "light", S, S}},
    {"training", {S, S, S, S, S, "light", S, S}},
    {"misty", {S, S, "flames", "flames", "flames", "flames", "light", "light"}},
    {"village2", {S, S, "lightning", "lightning", "flames", "flames", "flames", "flames"}},
    {"swamp", {S, S, "lightning", "lightning", "flames", "flames", "flames", "flames"}},
    {"sunken", {S, S, "light", "fade", "caustics", "caustics", "caustics", "caustics"}},
    {"sunkenb", {S, S, "light", "fade", "caustics", "caustics", "caustics", "caustics"}},
    {"rolling", {S, S, S, S, "light", "light", "light", "light"}},
    {"firecanyon", {S, S, S, S, "lava", "lava", "lava", "lava"}},
    {"snow", {S, S, S, S, "flames", "flames", "flames", "light"}},
    {"ogre", {S, S, S, S, "lightning", "lightning", "-", "-"}},
    {"village3", {S, S, S, S, "flames", "flames", "flames", "-"}},
    {"finalboss", {S, S, S, S, S, "-", S, S}},
    {"maincave", {S, "-", "-", "-", "flames", "flames", "flames", "flames"}},
    {"robocave", {S, "-", "-", "-", "flames", "flames", "flames", "flames"}},
    {"darkcave",
     {S, "fade-lights", "fade-lights", "fade-lights", "fade-lights", "fade-lights", "fade-lights",
      "fade-lights"}},
    {"jungleb", {S, "-", "-", "-", "-", "-", "-", "-"}},
    {"lavatube", {S, "light", "-", "-", "lava", "lava", "lava", "lava"}},
    {"citadel", {S, "light", "-", "-", "flames", "flames", "flames", "-"}},
};

const char* slot_role(const std::string& level, int slot) {
  for (auto& r : kRoles) {
    if (level == r.level) {
      return r.roles[slot];
    }
  }
  return "?";
}

bool inspect_bucket(int b) {
  using jak1::BucketId;
  return (b >= (int)BucketId::TFRAG_TEX_LEVEL0 && b <= (int)BucketId::GENERIC_ALPHA) ||
         (b >= (int)BucketId::PRIS_TEX_LEVEL0 && b <= (int)BucketId::GENERIC_WATER_LEVEL1);
}

std::vector<std::string> split_csv(const std::string& line) {
  std::vector<std::string> out;
  std::string cur;
  bool quoted = false;
  for (size_t i = 0; i < line.size(); i++) {
    char c = line[i];
    if (quoted) {
      if (c == '"' && i + 1 < line.size() && line[i + 1] == '"') {
        cur += '"';
        i++;
      } else if (c == '"') {
        quoted = false;
      } else {
        cur += c;
      }
    } else if (c == '"') {
      quoted = true;
    } else if (c == ',') {
      out.push_back(cur);
      cur.clear();
    } else if (c != '\r') {
      cur += c;
    }
  }
  out.push_back(cur);
  return out;
}

std::string csv_field(const std::string& s) {
  if (s.find_first_of(",\"\n") == std::string::npos) {
    return s;
  }
  std::string out = "\"";
  for (char c : s) {
    out += c == '"' ? std::string("\"\"") : std::string(1, c == '\n' ? ' ' : c);
  }
  return out + "\"";
}

fs::path data_dir() {
  if (const char* d = std::getenv("SANDOVER_DATA"); d && *d) {
    return fs::path(d);
  }
  return file_util::get_jak_project_dir() / "sandover" / "data";
}

// the shared overrides contract keys on combo_id, or on name when combo_id is blank
std::string override_key(const std::string& combo_id, const std::string& name) {
  return combo_id.empty() ? "n:" + name
                          : fmt::format("c:{}", std::strtoul(combo_id.c_str(), nullptr, 0));
}
}  // namespace

namespace inspect {

bool active() {
  return g_active;
}

u32 add(DrawRecord&& record) {
  if (!g_active) {
    return 0;
  }
  g_records.push_back(std::move(record));
  return (u32)g_records.size();
}

void set(GLuint program, u32 record) {
  glUniform1ui(glGetUniformLocation(program, "insp_rec"), record);
}

void write_mask(bool write) {
  if (g_active) {
    glColorMaski(3, write, write, write, write);
  }
}

}  // namespace inspect

struct InspectRenderer::Hit {
  int x = 0, y = 0;
  u32 id[4] = {};
  u8 rgba[4] = {};
  bool has_e = false;
  u8 e = 0;
  bool has_record = false;
  inspect::DrawRecord rec;
  std::string what;  // one-line summary when there is no record

  // texture
  bool tex_ok = false;
  std::string tex_name, tpage;
  u32 combo_id = 0;
  int tex_w = 0, tex_h = 0;
  float u = 0, v = 0;
  int tx = 0, ty = 0;
  u32 texel = 0;
  std::string material = "(no row)";

  // hit triangle (tfrag/tie, geometry LOD 0 data)
  bool geo_ok = false;
  std::string geo_note;
  u32 tri[3] = {};
  float bary[3] = {};
  u32 nearest = 0;
  float pos[3] = {};
  float dist_m = 0;

  // palette slots of the nearest vertex
  bool slots_ok = false;
  u8 slot_rgb[8][3] = {};
  float slot_lum[8] = {};
  float slot_w[8] = {};

  // sidecar record of the nearest vertex
  bool sc_ok = false;
  std::string sc_note;
  int sc_e = 0, sc_src = 0;
  float sc_w = 0;
  u32 night_mask = 0, emitter_mask = 0;

  std::string draw_key;  // "<kind>/<level>/<geom>/<tree>/<draw>", the draw_id context column
};

InspectRenderer::InspectRenderer() = default;

InspectRenderer::~InspectRenderer() {
  if (m_tex) {
    glDeleteTextures(1, &m_tex);
  }
  if (m_rb_fbo) {
    glDeleteFramebuffers(1, &m_rb_fbo);
    glDeleteTextures(3, m_rb_tex);
  }
}

void InspectRenderer::begin(SharedRenderState* rs, bool on) {
  g_active = false;
  g_records.clear();
  if (m_attached_fb && !glIsFramebuffer(m_attached_fb)) {
    m_attached_fb = 0;
  }
  if (!on && !m_attached_fb) {
    return;
  }
  GLint prev_draw = 0, prev_read = 0;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw);
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
  glBindFramebuffer(GL_FRAMEBUFFER, m_attached_fb ? m_attached_fb : rs->render_fb);
  if (!on) {
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT3, GL_TEXTURE_2D, 0, 0);
    m_attached_fb = 0;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev_draw);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_read);
    return;
  }
  glBindFramebuffer(GL_FRAMEBUFFER, rs->render_fb);
  GLint samples = 1;
  glGetIntegerv(GL_SAMPLES, &samples);
  samples = std::max(samples, 1);
  const int w = rs->render_fb_w, h = rs->render_fb_h;
  const GLenum target = samples > 1 ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D;
  if (!m_tex || m_w != w || m_h != h || m_samples != samples) {
    if (m_tex) {
      glDeleteTextures(1, &m_tex);
    }
    glGenTextures(1, &m_tex);
    glBindTexture(target, m_tex);
    if (samples > 1) {
      glTexImage2DMultisample(target, samples, GL_RGBA32UI, w, h, GL_TRUE);
    } else {
      glTexImage2D(target, 0, GL_RGBA32UI, w, h, 0, GL_RGBA_INTEGER, GL_UNSIGNED_INT, nullptr);
      glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    glBindTexture(target, 0);
    m_w = w;
    m_h = h;
    m_samples = samples;
    m_attached_fb = 0;
    lg::info("inspect: ID target {}x{}, {} samples", w, h, samples);
  }
  if (m_attached_fb != (GLuint)rs->render_fb) {
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT3, target, m_tex, 0);
    m_attached_fb = rs->render_fb;
  }
  const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
  glDisable(GL_SCISSOR_TEST);
  const GLenum bufs[4] = {GL_NONE, GL_NONE, GL_NONE, GL_COLOR_ATTACHMENT3};
  glDrawBuffers(4, bufs);
  glColorMaski(3, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  const GLuint zero[4] = {0, 0, 0, 0};
  glClearBufferuiv(GL_COLOR, 3, zero);
  const GLenum first[2] = {GL_COLOR_ATTACHMENT0, GL_NONE};
  glDrawBuffers(2, first);
  if (scissor) {
    glEnable(GL_SCISSOR_TEST);
  }
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev_draw);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_read);

  GLint prog = 0;
  glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
  for (auto id : kInspectShaders) {
    auto& sh = rs->shaders[id];
    sh.activate();
    inspect::set(sh.id(), kUnregistered | (u32)id);
  }
  glUseProgram(prog);
  g_active = true;
}

void InspectRenderer::set_bucket(SharedRenderState* rs, int bucket) {
  if (g_active) {
    SsaoRenderer::set_inspect_drawing(rs, inspect_bucket(bucket));
  }
}

void InspectRenderer::end(SharedRenderState* rs) {
  if (g_active) {
    SsaoRenderer::set_inspect_drawing(rs, false);
  }
}

bool InspectRenderer::read_pixel(SharedRenderState* rs, int x, int y, Hit& hit) {
  if (!m_attached_fb || x < 0 || y < 0 || x >= m_w || y >= m_h) {
    return false;
  }
  if (!m_rb_fbo) {
    glGenFramebuffers(1, &m_rb_fbo);
    glGenTextures(3, m_rb_tex);
    const GLint ifmt[3] = {GL_RGBA32UI, GL_RGBA8, GL_R8};
    const GLenum fmt[3] = {GL_RGBA_INTEGER, GL_RGBA, GL_RED};
    const GLenum type[3] = {GL_UNSIGNED_INT, GL_UNSIGNED_BYTE, GL_UNSIGNED_BYTE};
    for (int i = 0; i < 3; i++) {
      glBindTexture(GL_TEXTURE_2D, m_rb_tex[i]);
      glTexImage2D(GL_TEXTURE_2D, 0, ifmt[i], 1, 1, 0, fmt[i], type[i], nullptr);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
  }
  GLint prev_draw = 0, prev_read = 0, prev_read_buf = 0;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw);
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, m_attached_fb);
  glGetIntegerv(GL_READ_BUFFER, &prev_read_buf);
  GLint em_type = GL_NONE;
  glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT2,
                                        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &em_type);
  hit.has_e = em_type != GL_NONE;
  const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
  glDisable(GL_SCISSOR_TEST);
  const GLenum src[3] = {GL_COLOR_ATTACHMENT3, GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT2};
  for (int i = 0; i < 3; i++) {
    if (i == 2 && !hit.has_e) {
      break;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_attached_fb);
    glReadBuffer(src[i]);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_rb_fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_rb_tex[i],
                           0);
    const GLenum d = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &d);
    glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBlitFramebuffer(x, y, x + 1, y + 1, 0, 0, 1, 1, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_rb_fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    if (i == 0) {
      glReadPixels(0, 0, 1, 1, GL_RGBA_INTEGER, GL_UNSIGNED_INT, hit.id);
    } else if (i == 1) {
      glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, hit.rgba);
    } else {
      glReadPixels(0, 0, 1, 1, GL_RED, GL_UNSIGNED_BYTE, &hit.e);
    }
  }
  glBindFramebuffer(GL_READ_FRAMEBUFFER, m_attached_fb);
  glReadBuffer(prev_read_buf);
  if (scissor) {
    glEnable(GL_SCISSOR_TEST);
  }
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev_draw);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_read);
  (void)rs;
  return true;
}

namespace {
/// Barycentric coordinates of p in the triangle (a, b, c) of 2D points; false if degenerate.
bool bary2(const float* a, const float* b, const float* c, const float* p, float* out) {
  const float v0x = b[0] - a[0], v0y = b[1] - a[1];
  const float v1x = c[0] - a[0], v1y = c[1] - a[1];
  const float v2x = p[0] - a[0], v2y = p[1] - a[1];
  const float den = v0x * v1y - v1x * v0y;
  if (std::abs(den) < 1e-12f) {
    return false;
  }
  out[1] = (v2x * v1y - v1x * v2y) / den;
  out[2] = (v0x * v2y - v2x * v0y) / den;
  out[0] = 1.f - out[1] - out[2];
  return true;
}

/// Triangles (last vertex = provoking) of one index range, strips with restart or lists.
template <typename F>
void for_each_tri(const std::vector<u32>& idx, u32 first, u32 count, bool strips, F&& f) {
  if (!strips) {
    for (u32 i = first; i + 2 < first + count; i += 3) {
      f(idx[i], idx[i + 1], idx[i + 2]);
    }
    return;
  }
  u32 run = 0;
  for (u32 i = first; i < first + count && i < idx.size(); i++) {
    if (idx[i] == UINT32_MAX) {
      run = 0;
      continue;
    }
    if (++run >= 3) {
      f(idx[i - 2], idx[i - 1], idx[i]);
    }
  }
}

}  // namespace

void InspectRenderer::resolve(SharedRenderState* rs, Hit& hit) {
  const u32 rec_id = hit.id[0];
  hit.u = *reinterpret_cast<const float*>(&hit.id[2]);
  hit.v = *reinterpret_cast<const float*>(&hit.id[3]);
  if (rec_id == 0) {
    hit.what = "nothing wrote the ID target here (sky, ocean, sprites, or no geometry)";
    return;
  }
  if ((rec_id & 0xff000000) == kUnregistered) {
    hit.what = fmt::format("a draw of shader {} that does not register (e.g. tie wind, envmap)",
                           rec_id & 0xffffff);
    return;
  }
  if (rec_id > g_records.size()) {
    hit.what = fmt::format("record {} out of range ({} this frame)", rec_id, g_records.size());
    return;
  }
  hit.has_record = true;
  hit.rec = g_records[rec_id - 1];
  const auto& r = hit.rec;
  hit.draw_key = fmt::format("{}/{}/{}/{}/{}", kKindNames[r.kind], r.level, r.geom, r.tree, r.draw);

  if (r.kind == inspect::GENERIC) {
    hit.tex_name = rs->texture_pool->get_debug_texture_name_from_tbp(r.tbp);
    hit.tpage = fmt::format("tbp 0x{:x}", r.tbp);
    return;
  }
  const LevelData* ld = r.data ? r.data : rs->loader->get_tfrag3_level(r.level);
  if (!ld || !ld->level) {
    hit.what = "level " + r.level + " no longer loaded";
    return;
  }
  const auto& lev = *ld->level;
  if (r.tex >= 0 && r.tex < (s32)lev.textures.size()) {
    const auto& t = lev.textures[r.tex];
    hit.tex_ok = true;
    hit.tex_name = t.debug_name;
    hit.tpage = t.debug_tpage_name;
    hit.combo_id = t.combo_id;
    hit.tex_w = t.w;
    hit.tex_h = t.h;
    const DrawMode& mode = r.mode;
    auto wrap = [](float c, int n, bool clamp) {
      int i = (int)std::floor(c * n);
      return clamp ? std::clamp(i, 0, n - 1) : ((i % n) + n) % n;
    };
    hit.tx = wrap(hit.u, t.w, mode.get_clamp_s_enable());
    hit.ty = wrap(hit.v, t.h, mode.get_clamp_t_enable());
    if (!t.data.empty()) {
      hit.texel = t.data[hit.ty * t.w + hit.tx];
    }
    auto m = m_materials.find(t.combo_id);
    if (m != m_materials.end()) {
      hit.material = m->second.cls + (m->second.tags.empty() ? "" : " [" + m->second.tags + "]");
    }
  } else if (r.tex < 0) {
    hit.tex_name = fmt::format("anim slot {}", -(r.tex + 1));
  }

  if (r.kind != inspect::TFRAG && r.kind != inspect::TIE) {
    return;
  }
  const std::vector<tfrag3::PreloadedVertex>* verts = nullptr;
  const std::vector<u32>* indices = nullptr;
  const tfrag3::StripDraw* draw = nullptr;
  const tfrag3::PackedTimeOfDay* colors = nullptr;
  bool strips = true;
  if (r.kind == inspect::TFRAG) {
    const auto& trees = lev.tfrag_trees[r.geom];
    if (r.tree < 0 || r.tree >= (int)trees.size()) {
      hit.geo_note = "tree index out of range";
      return;
    }
    const auto& t = trees[r.tree];
    verts = &t.unpacked.vertices;
    indices = &t.unpacked.indices;
    draw = r.draw < (int)t.draws.size() ? &t.draws[r.draw] : nullptr;
    colors = &t.colors;
    strips = t.use_strips;
  } else {
    const auto& trees = lev.tie_trees[r.geom];
    if (r.tree < 0 || r.tree >= (int)trees.size()) {
      hit.geo_note = "tree index out of range";
      return;
    }
    const auto& t = trees[r.tree];
    verts = &t.unpacked.vertices;
    indices = &t.unpacked.indices;
    draw = r.draw < (int)t.static_draws.size() ? &t.static_draws[r.draw] : nullptr;
    colors = &t.colors;
    strips = t.use_strips;
  }
  const u32 vid = hit.id[1];
  if (!draw || vid >= verts->size() || indices->empty()) {
    hit.geo_note = "vertex or index data not available";
    return;
  }

  // hit triangle: those whose provoking (last) vertex is vid, the one containing the hit UV
  const float p[2] = {hit.u, hit.v};
  float best = -1e30f;
  u32 first = draw->unpacked.idx_of_first_idx_in_full_buffer;
  for (const auto& grp : draw->vis_groups) {
    for_each_tri(*indices, first, grp.num_inds, strips, [&](u32 a, u32 b, u32 c) {
      if (c != vid) {
        return;
      }
      const auto &va = (*verts)[a], &vb = (*verts)[b], &vc = (*verts)[c];
      const float ta[2] = {va.s, va.t}, tb[2] = {vb.s, vb.t}, tc[2] = {vc.s, vc.t};
      float w[3];
      if (!bary2(ta, tb, tc, p, w)) {
        return;
      }
      const float m = std::min({w[0], w[1], w[2]});
      if (m > best) {
        best = m;
        hit.tri[0] = a;
        hit.tri[1] = b;
        hit.tri[2] = c;
        std::copy(w, w + 3, hit.bary);
      }
    });
    first += grp.num_inds;
  }
  if (best > -0.05f) {
    hit.geo_ok = true;
    for (int k = 0; k < 3; k++) {
      const auto& vv = (*verts)[hit.tri[k]];
      hit.pos[0] += hit.bary[k] * vv.x;
      hit.pos[1] += hit.bary[k] * vv.y;
      hit.pos[2] += hit.bary[k] * vv.z;
    }
    const int k = (int)(std::max_element(hit.bary, hit.bary + 3) - hit.bary);
    hit.nearest = hit.tri[k];
  } else {
    hit.geo_note = best > -1e29f ? "hit UV outside the provoking vertex's triangles"
                                 : "no triangle with a usable UV ends at the provoking vertex";
    hit.geo_note += "; position and slots are the provoking vertex's";
    hit.nearest = vid;
    const auto& vv = (*verts)[vid];
    hit.pos[0] = vv.x;
    hit.pos[1] = vv.y;
    hit.pos[2] = vv.z;
  }
  const float dx = hit.pos[0] - r.cam_trans.x(), dy = hit.pos[1] - r.cam_trans.y(),
              dz = hit.pos[2] - r.cam_trans.z();
  hit.dist_m = std::sqrt(dx * dx + dy * dy + dz * dz) / 4096.f;
  for (auto& c : hit.pos) {
    c /= 4096.f;
  }

  float w[8][3];
  unbake_slot_weights(r.itimes, w);
  const u16 ci = (*verts)[hit.nearest].color_index;
  if (ci < colors->color_count) {
    hit.slots_ok = true;
    for (int s = 0; s < 8; s++) {
      for (int ch = 0; ch < 3; ch++) {
        hit.slot_rgb[s][ch] = colors->read(ci, s, ch);
      }
      hit.slot_lum[s] =
          0.299f * hit.slot_rgb[s][0] + 0.587f * hit.slot_rgb[s][1] + 0.114f * hit.slot_rgb[s][2];
      hit.slot_w[s] = w[s][1];
    }
  }

  const UnbakeSidecar* sc = ld->unbake.get();
  if (!sc) {
    hit.sc_note = "no sidecar for this level";
  } else if (r.geom != 0) {
    hit.sc_note = "sidecar covers geometry LOD 0 only";
  } else {
    const auto& list = r.kind == inspect::TFRAG ? sc->tfrag : sc->tie;
    if (r.tree >= (int)list.size() || !list[r.tree].valid) {
      hit.sc_note = "tree not in the sidecar";
    } else if (!sc->has_emissive) {
      hit.sc_note = "sidecar version 1 (no E)";
    } else {
      const u32 word = list[r.tree].records.at(4 * hit.nearest + 3);
      hit.sc_ok = true;
      hit.sc_e = (word >> 16) & 0xff;
      hit.sc_src = (word >> 24) & 0xf;
      GoalBackgroundCameraData cam{};
      for (int i = 0; i < 4; i++) {
        cam.itimes[i] = r.itimes[i];
      }
      float ew[kUnbakeEmissiveSources];
      unbake_emissive_weights(*sc, cam, ew);
      hit.sc_w = ew[std::min(hit.sc_src, kUnbakeEmissiveSources - 1)];
      hit.night_mask = sc->night_mask;
      hit.emitter_mask = sc->emitter_mask;
    }
  }
}

void InspectRenderer::load_tables() {
  m_tables_loaded = true;
  m_materials.clear();
  m_overrides.clear();
  fs::path mat;
  if (const char* m = std::getenv("SANDOVER_MATERIALS"); m && *m) {
    mat = m;
  } else {
    mat = data_dir() / "materials" / "materials.csv";
  }
  m_materials_path = mat.string();
  if (const char* o = std::getenv("SANDOVER_EMISSIVE_OVERRIDES"); o && *o) {
    m_overrides_path = o;
  } else {
    m_overrides_path = (data_dir() / "materials" / "emissive-overrides.csv").string();
  }
  std::ifstream f(mat);
  std::string line;
  if (f && std::getline(f, line)) {
    auto head = split_csv(line);
    auto col = [&](const char* n) {
      return (int)(std::find(head.begin(), head.end(), n) - head.begin());
    };
    const int c_id = col("combo_id"), c_cls = col("class"), c_tags = col("tags"),
              c_note = col("note");
    while (std::getline(f, line)) {
      auto row = split_csv(line);
      if ((int)row.size() <= std::max({c_id, c_cls})) {
        continue;
      }
      Material m;
      m.cls = row[c_cls];
      m.tags = c_tags < (int)row.size() ? row[c_tags] : "";
      m.note = c_note < (int)row.size() ? row[c_note] : "";
      m_materials[(u32)std::strtoul(row[c_id].c_str(), nullptr, 0)] = m;
    }
  }
  std::ifstream o(m_overrides_path);
  if (o && std::getline(o, line)) {
    auto head = split_csv(line);
    auto col = [&](const char* n) {
      return (int)(std::find(head.begin(), head.end(), n) - head.begin());
    };
    const int c_id = col("combo_id"), c_name = col("name"), c_verdict = col("verdict");
    while (std::getline(o, line)) {
      auto row = split_csv(line);
      if ((int)row.size() <= std::max({c_id, c_name, c_verdict})) {
        continue;
      }
      const auto key = override_key(row[c_id], row[c_name]);
      if (row[c_verdict].empty()) {
        m_overrides.erase(key);
      } else {
        m_overrides[key] = row[c_verdict];
      }
    }
  }
  lg::info("inspect: {} material rows from {}, {} overrides", m_materials.size(), m_materials_path,
           m_overrides.size());
}

void InspectRenderer::write_override(const char* verdict) {
  if (!m_hit || !m_hit->tex_ok) {
    return;
  }
  const auto& h = *m_hit;
  const bool exists = fs::exists(m_overrides_path);
  fs::create_directories(fs::path(m_overrides_path).parent_path());
  std::ofstream f(m_overrides_path, std::ios::app | std::ios::binary);
  if (!f) {
    m_status = "cannot write " + m_overrides_path;
    return;
  }
  if (!exists) {
    f << "combo_id,tpage,name,level,draw_id,verdict,note,timestamp\n";
  }
  const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  char ts[32];
  std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S+00:00", std::gmtime(&now));
  const std::string combo = fmt::format("0x{:08x}", h.combo_id);
  f << fmt::format("{},{},{},{},{},{},{},{}\n", combo, csv_field(h.tpage), csv_field(h.tex_name),
                   h.rec.level, h.draw_key, verdict, csv_field(m_note), ts);
  const auto key = override_key(combo, h.tex_name);
  if (*verdict) {
    m_overrides[key] = verdict;
  } else {
    m_overrides.erase(key);
  }
  m_status = fmt::format("{} -> {}", h.tex_name, *verdict ? verdict : "cleared");
  lg::info("inspect: {}", m_status);
}

void InspectRenderer::frame(SharedRenderState* rs) {
  if (!Gfx::g_global_settings.inspect_enable || rs->version != GameVersion::Jak1) {
    return;
  }
  if (!m_tables_loaded) {
    load_tables();
  }
  const ImGuiIO& io = ImGui::GetIO();
  if (g_active && io.KeyCtrl && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
      !io.WantCaptureMouse && rs->draw_region_w > 0 && rs->draw_region_h > 0) {
    const float mx = io.MousePos.x * io.DisplayFramebufferScale.x;
    const float my = io.MousePos.y * io.DisplayFramebufferScale.y;
    pick(rs, (mx - rs->draw_offset_x) / rs->draw_region_w,
         (my - rs->draw_offset_y) / rs->draw_region_h);
  }
  const auto& gs = Gfx::g_global_settings;
  if (g_active && gs.inspect_pick_seq != m_pick_seq) {
    if (gs.inspect_pick_seq - m_pick_seq > 16) {
      m_pick_seq = gs.inspect_pick_seq - 16;
    }
    const auto& p = gs.inspect_picks[m_pick_seq++ % 16];
    const int verdict = (int)p[2] & 3;
    if (pick(rs, p[0], p[1]) && verdict) {
      const char* verdicts[] = {"", "emissive", "not-emissive", ""};
      snprintf(m_note, sizeof(m_note), "scripted");
      write_override(verdicts[verdict]);
    }
  }
  draw_panel();
}

bool InspectRenderer::pick(SharedRenderState* rs, float fx, float fy) {
  if (!(fx >= 0 && fx < 1 && fy >= 0 && fy < 1)) {
    return false;
  }
  auto hit = std::make_unique<Hit>();
  hit->x = (int)(fx * rs->render_fb_w);
  hit->y = rs->render_fb_h - 1 - (int)(fy * rs->render_fb_h);
  if (!read_pixel(rs, hit->x, hit->y, *hit)) {
    return false;
  }
  resolve(rs, *hit);
  m_hit = std::move(hit);
  m_status.clear();
  lg::info("inspect: {}", summary());
  return true;
}

std::string InspectRenderer::summary() const {
  const auto& h = *m_hit;
  std::string s =
      fmt::format("pixel {},{} rgba {} {} {} {} mask {}", h.x, h.y, h.rgba[0], h.rgba[1], h.rgba[2],
                  h.rgba[3], h.has_e ? fmt::format("{:.3f}", h.e / 255.f) : "off");
  if (!h.has_record) {
    return s + " | " + h.what;
  }
  const auto& r = h.rec;
  s += fmt::format(" | {} {} geom {} tree {} draw {}{} | tex {} tpage {} combo 0x{:08x} class {}",
                   kKindNames[r.kind], r.level, r.geom, r.tree, r.draw,
                   r.model.empty() ? "" : " model " + r.model, h.tex_name, h.tpage, h.combo_id,
                   h.material);
  s += fmt::format(" | uv {:.4f} {:.4f} texel {},{} = 0x{:08x}", h.u, h.v, h.tx, h.ty, h.texel);
  if (!h.what.empty()) {
    s += " | " + h.what;
  }
  if (r.kind == inspect::TFRAG || r.kind == inspect::TIE) {
    s += h.geo_ok ? fmt::format(" | tri {} {} {} bary {:.2f} {:.2f} {:.2f}", h.tri[0], h.tri[1],
                                h.tri[2], h.bary[0], h.bary[1], h.bary[2])
                  : " | " + h.geo_note;
    s += fmt::format(" | world {:.2f} {:.2f} {:.2f} dist {:.1f} m vertex {}", h.pos[0], h.pos[1],
                     h.pos[2], h.dist_m, h.nearest);
    s += h.sc_ok ? fmt::format(" | E {:.3f} src {} w {:.2f}", h.sc_e / 255.f, h.sc_src, h.sc_w)
                 : " | sidecar: " + h.sc_note;
    if (h.slots_ok) {
      s += " | slots luma*w";
      for (int k = 0; k < 8; k++) {
        s += fmt::format(" {}:{:.0f}x{:.2f}", k, h.slot_lum[k], h.slot_w[k]);
      }
    }
  }
  return s;
}

void InspectRenderer::draw_panel() {
  ImGui::SetNextWindowSize(ImVec2(520, 560), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Inspect")) {
    ImGui::End();
    return;
  }
  ImGui::TextUnformatted("Ctrl + left click on the game view.");
  if (!m_hit) {
    ImGui::End();
    return;
  }
  const auto& h = *m_hit;
  ImGui::Separator();
  ImGui::Text("pixel %d,%d  colour %d %d %d %d", h.x, h.y, h.rgba[0], h.rgba[1], h.rgba[2],
              h.rgba[3]);
  if (h.has_e) {
    ImGui::Text("emissive mask at pixel: %.3f", h.e / 255.f);
  } else {
    ImGui::TextUnformatted("emissive mask: off (SSAO mask or bloom not running)");
  }
  if (!h.has_record) {
    ImGui::TextWrapped("hit: %s", h.what.c_str());
    ImGui::End();
    return;
  }
  const auto& r = h.rec;
  ImGui::Text("draw: %s level %s geom %d tree %d draw %d (record %u)", kKindNames[r.kind],
              r.level.c_str(), r.geom, r.tree, r.draw, h.id[0]);
  if (!r.model.empty()) {
    ImGui::Text("model: %s", r.model.c_str());
  }
  ImGui::Text("texture: %s  tpage %s", h.tex_name.c_str(), h.tpage.c_str());
  if (h.tex_ok) {
    ImGui::Text("combo_id 0x%08x (tpage %u, idx %u), %dx%d", h.combo_id, h.combo_id >> 16,
                h.combo_id & 0xffff, h.tex_w, h.tex_h);
    ImGui::Text("material class: %s", h.material.c_str());
    const u32 t = h.texel;
    ImGui::Text("UV %.4f %.4f -> texel %d,%d = %u %u %u %u", h.u, h.v, h.tx, h.ty, t & 0xff,
                (t >> 8) & 0xff, (t >> 16) & 0xff, t >> 24);
    ImGui::SameLine();
    ImGui::ColorButton(
        "##texel",
        ImVec4((t & 0xff) / 255.f, ((t >> 8) & 0xff) / 255.f, ((t >> 16) & 0xff) / 255.f, 1.f),
        ImGuiColorEditFlags_NoTooltip, ImVec2(14, 14));
  } else {
    ImGui::Text("UV %.4f %.4f", h.u, h.v);
  }
  if (r.kind == inspect::TFRAG || r.kind == inspect::TIE) {
    if (h.geo_ok) {
      ImGui::Text("triangle %u %u %u  bary %.2f %.2f %.2f", h.tri[0], h.tri[1], h.tri[2], h.bary[0],
                  h.bary[1], h.bary[2]);
    } else {
      ImGui::TextWrapped("provoking vertex %u: %s", h.id[1], h.geo_note.c_str());
    }
    ImGui::Text("world %.2f %.2f %.2f m, %.1f m from camera; nearest vertex %u", h.pos[0], h.pos[1],
                h.pos[2], h.dist_m, h.nearest);
    if (h.sc_ok) {
      const char* src = h.sc_src == 8 ? "static" : h.sc_src == 9 ? "night slots" : "slot";
      ImGui::Text("sidecar E %.3f  source %s%s  runtime weight %.2f  -> %.3f", h.sc_e / 255.f, src,
                  h.sc_src < 8 ? fmt::format(" {}", h.sc_src).c_str() : "", h.sc_w,
                  h.sc_e / 255.f * h.sc_w);
    } else {
      ImGui::Text("sidecar: %s", h.sc_note.c_str());
    }
    if (h.slots_ok && ImGui::BeginTable("slots", 6, ImGuiTableFlags_Borders)) {
      ImGui::TableSetupColumn("slot");
      ImGui::TableSetupColumn("role");
      ImGui::TableSetupColumn("rgb");
      ImGui::TableSetupColumn("luma");
      ImGui::TableSetupColumn("weight");
      ImGui::TableSetupColumn("luma x w");
      ImGui::TableHeadersRow();
      for (int s = 0; s < 8; s++) {
        std::string role = slot_role(r.level, s);
        if (h.night_mask & (1u << s)) {
          role += " (night)";
        }
        if (h.emitter_mask & (1u << s)) {
          role += " (emitter)";
        }
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("%d", s);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(role.c_str());
        ImGui::TableNextColumn();
        ImGui::Text("%3d %3d %3d", h.slot_rgb[s][0], h.slot_rgb[s][1], h.slot_rgb[s][2]);
        ImGui::TableNextColumn();
        ImGui::Text("%.0f", h.slot_lum[s]);
        ImGui::TableNextColumn();
        ImGui::Text("%.2f", h.slot_w[s]);
        ImGui::TableNextColumn();
        ImGui::Text("%.0f", h.slot_lum[s] * h.slot_w[s]);
      }
      ImGui::EndTable();
    }
  }

  if (h.tex_ok) {
    ImGui::Separator();
    auto it = m_overrides.find(override_key(fmt::format("0x{:08x}", h.combo_id), h.tex_name));
    ImGui::Text("texture override: %s", it == m_overrides.end() ? "none" : it->second.c_str());
    ImGui::InputText("note", m_note, sizeof(m_note));
    if (ImGui::Button("mark emissive")) {
      write_override("emissive");
    }
    ImGui::SameLine();
    if (ImGui::Button("mark not emissive")) {
      write_override("not-emissive");
    }
    ImGui::SameLine();
    if (ImGui::Button("clear")) {
      write_override("");
    }
    if (!m_status.empty()) {
      ImGui::TextUnformatted(m_status.c_str());
    }
  }
  ImGui::End();
}

void InspectRenderer::draw_debug_window() {
  ImGui::Checkbox("Inspect (Ctrl + click)", &Gfx::g_global_settings.inspect_enable);
  if (ImGui::Button("Reload materials / overrides")) {
    load_tables();
  }
  ImGui::Text("records last frame: %d", (int)g_records.size());
}
