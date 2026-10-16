#include "ActorShadows.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>

#include "common/log/log.h"

#include "game/graphics/gfx.h"
#include "game/graphics/opengl_renderer/GlQueries.h"
#include "game/graphics/opengl_renderer/LevelHeightfield.h"
#include "game/graphics/opengl_renderer/Neighbours.h"
#include "game/graphics/opengl_renderer/PerfTimers.h"
#include "game/graphics/opengl_renderer/SunDisc.h"
#include "game/graphics/opengl_renderer/WorldLights.h"
#include "game/graphics/opengl_renderer/background/Canopy.h"
#include "game/graphics/opengl_renderer/background/Unbake.h"
#include "game/graphics/opengl_renderer/background/background_common.h"
#include "game/graphics/opengl_renderer/loader/common.h"

#include "third-party/imgui/imgui.h"

namespace {

constexpr float kMeter = 4096.f;
constexpr float kFixtureM = 1.5f;  // a model rooted this close to a local light houses it
constexpr int kSunUnit = 12;
constexpr int kLampUnit0 = 13;
constexpr int kCasterUnit = 44;  // named sampler outside the auto-bound tex_T0-29 (ALLOCATIONS)
constexpr int kOccUnit = 15;
// occupancy grid of this frame's sun map, 0 when the opt is off or shadows are
GLuint g_occ_tex = 0;

// column-major 4x4, as glUniformMatrix4fv takes it
using Mat4 = std::array<float, 16>;

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

bool invert(const Mat4& m, Mat4* out) {
  // cofactor expansion (MESA gluInvertMatrix)
  Mat4 inv;
  inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] +
           m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
  inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] -
           m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
  inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] +
           m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
  inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] -
            m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
  inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] -
           m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
  inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] +
           m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
  inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] -
           m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
  inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] +
            m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
  inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] +
           m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
  inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] -
           m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
  inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] +
            m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
  inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] -
            m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
  inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] -
           m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
  inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] +
           m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
  inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] -
            m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
  inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] +
            m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
  float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
  if (std::abs(det) < 1e-30f) {
    return false;
  }
  for (auto& x : inv) {
    x /= det;
  }
  *out = inv;
  return true;
}

math::Vector4f xform(const Mat4& m, const math::Vector4f& v) {
  math::Vector4f r;
  for (int row = 0; row < 4; row++) {
    r[row] = m[row] * v[0] + m[4 + row] * v[1] + m[8 + row] * v[2] + m[12 + row] * v[3];
  }
  return r;
}

void store(const Mat4& m, math::Vector4f out[4]) {
  memcpy(out, m.data(), sizeof(Mat4));
}

/// Rows (r, u, f, 1) of a view basis at `origin`, then `proj`.
Mat4 view_matrix(const math::Vector3f& r,
                 const math::Vector3f& u,
                 const math::Vector3f& f,
                 const math::Vector3f& origin) {
  Mat4 v{};
  const math::Vector3f* axes[3] = {&r, &u, &f};
  for (int row = 0; row < 3; row++) {
    for (int c = 0; c < 3; c++) {
      v[c * 4 + row] = (*axes[row])[c];
    }
    v[12 + row] = -axes[row]->dot(origin);
  }
  v[15] = 1.f;
  return v;
}

void basis(const math::Vector3f& f, math::Vector3f* r, math::Vector3f* u) {
  math::Vector3f up = std::abs(f.y()) < 0.99f ? math::Vector3f(0, 1, 0) : math::Vector3f(1, 0, 0);
  *r = up.cross(f).normalized();
  *u = f.cross(*r);
}

float lum(const math::Vector3f& c) {
  return 0.299f * c.x() + 0.587f * c.y() + 0.114f * c.z();
}

constexpr float kKeepBonus = 1.5f;  // a light already casting ranks this much higher
constexpr float kJakCentreM = 0.9f;
constexpr float kFadeS = 0.3f;  // a local light fades in or out over this long

bool read_port_data(DmaFollower& dma, u32 end, TfragPcPortData* out) {
  bool found = false;
  while (dma.current_tag_offset() != end) {
    auto t = dma.read_and_advance();
    if (!found && t.size_bytes == sizeof(TfragPcPortData) && t.vifcode1().kind == VifCode::Kind::PC_PORT) {
      memcpy(out, t.data, sizeof(TfragPcPortData));
      out->level_name[11] = '\0';
      found = true;
    }
  }
  return found;
}

void skip_to(DmaFollower& dma, u32 end) {
  while (dma.current_tag_offset() != end) {
    dma.read_and_advance();
  }
}

math::Vector3f unit_lum(const math::Vector3f& c) {
  const float l = lum(c);
  return l > 1e-3f ? c / l : math::Vector3f(1, 1, 1);
}

}  // namespace

ActorShadows::Target ActorShadows::make_target(int size, bool caster_colour) {
  Target t;
  glGenTextures(1, &t.tex);
  glBindTexture(GL_TEXTURE_2D, t.tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, size, size, 0, GL_DEPTH_COMPONENT,
               GL_UNSIGNED_INT, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
  glBindTexture(GL_TEXTURE_2D, 0);
  glGenFramebuffers(1, &t.fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, t.tex, 0);
  if (caster_colour) {
    glGenTextures(1, &t.colour);
    glBindTexture(GL_TEXTURE_2D, t.colour);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, size, size, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.colour, 0);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
  } else {
    glDrawBuffer(GL_NONE);
  }
  glReadBuffer(GL_NONE);
  ASSERT(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  return t;
}

ActorShadows::ActorShadows() {
  make_targets(2);
}

void ActorShadows::make_targets(int q) {
  // runs mid-frame on a quality change: put back the framebuffer and texture make_target rebinds
  GLint fbo = 0, tex = 0;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
  release_targets();
  m_quality = q;
  m_sun_size = q <= 0 ? kSunSize / 2 : q == 1 ? kSunSize * 3 / 4 : kSunSize;
  m_lamp_size = q <= 0 ? kLampSize / 2 : q == 1 ? kLampSize * 3 / 4 : kLampSize;
  m_sun = make_target(m_sun_size, true);
  for (auto& l : m_lamps) {
    l = make_target(m_lamp_size);
  }
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glBindTexture(GL_TEXTURE_2D, tex);
}

void ActorShadows::release_targets() {
  for (auto* t : {&m_sun, &m_lamps[0], &m_lamps[1]}) {
    if (t->fbo) {
      glDeleteFramebuffers(1, &t->fbo);
      glDeleteTextures(1, &t->tex);
    }
    if (t->colour) {
      glDeleteTextures(1, &t->colour);
    }
    *t = Target{};
  }
  if (m_occ_tex) {
    glDeleteFramebuffers(1, &m_occ_fbo);
    glDeleteTextures(1, &m_occ_tex);
    glDeleteFramebuffers(1, &m_occ_mid_fbo);
    glDeleteTextures(1, &m_occ_mid_tex);
    m_occ_tex = m_occ_fbo = m_occ_mid_tex = m_occ_mid_fbo = 0;
  }
  g_occ_tex = 0;
}

ActorShadows::~ActorShadows() {
  release_targets();
  if (m_occ_sampler) {
    glDeleteSamplers(1, &m_occ_sampler);
    glDeleteVertexArrays(1, &m_occ_vao);
  }
}

void ActorShadows::build_occupancy() {
  const int n0 = m_sun_size / 8, n1 = m_sun_size / kOccCell;
  if (!m_occ_sampler) {
    m_occ_shader = std::make_unique<Shader>("actor_shadow_occ", GameVersion::Jak1);
    glGenSamplers(1, &m_occ_sampler);
    glSamplerParameteri(m_occ_sampler, GL_TEXTURE_COMPARE_MODE, GL_NONE);
    glSamplerParameteri(m_occ_sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glSamplerParameteri(m_occ_sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenVertexArrays(1, &m_occ_vao);
  }
  if (!m_occ_tex) {
    GLuint tex[2], fbo[2];
    glGenTextures(2, tex);
    glGenFramebuffers(2, fbo);
    for (int i = 0; i < 2; i++) {
      const int n = i ? n1 : n0;
      glBindTexture(GL_TEXTURE_2D, tex[i]);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, n, n, 0, GL_RED, GL_FLOAT, nullptr);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glBindFramebuffer(GL_FRAMEBUFFER, fbo[i]);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex[i], 0);
      const GLenum buf = GL_COLOR_ATTACHMENT0;
      glDrawBuffers(1, &buf);
      ASSERT(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    m_occ_mid_tex = tex[0];
    m_occ_mid_fbo = fbo[0];
    m_occ_tex = tex[1];
    m_occ_fbo = fbo[1];
  }
  if (!m_occ_shader->okay()) {
    g_occ_tex = 0;
    return;
  }
  m_occ_state.capture();
  glDisable(GL_DEPTH_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  m_occ_shader->activate();
  const GLuint prog = m_occ_shader->id();
  glBindVertexArray(m_occ_vao);
  glActiveTexture(GL_TEXTURE0 + kSunUnit);
  glBindTexture(GL_TEXTURE_2D, m_sun.tex);
  glBindSampler(kSunUnit, m_occ_sampler);
  glBindFramebuffer(GL_FRAMEBUFFER, m_occ_mid_fbo);
  glViewport(0, 0, n0, n0);
  glUniform1i(GLQ_LOC(PERF_OPT_GEOMETRY + 2, prog, "pass"), 0);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  // the sampler stays bound through pass 1: its program still points tex_T12 at the sun map, a
  // compare-mode depth texture a plain sampler2D must not see (driver warning 0x20096)
  glActiveTexture(GL_TEXTURE0 + kOccUnit);
  glBindTexture(GL_TEXTURE_2D, m_occ_mid_tex);
  glBindFramebuffer(GL_FRAMEBUFFER, m_occ_fbo);
  glViewport(0, 0, n1, n1);
  glUniform1i(GLQ_LOC(PERF_OPT_GEOMETRY + 2, prog, "pass"), 1);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glBindSampler(kSunUnit, 0);
  glActiveTexture(GL_TEXTURE0);
  m_occ_state.restore();
  g_occ_tex = m_occ_tex;
}

void ActorShadows::fade_lights(std::vector<world_lights::ActorLight>* lights) {
  // each light's weight moves toward 1 while chosen and toward 0 once dropped, over kFadeS;
  // a dropped light keeps lighting (fading) with its last values until its weight reaches 0
  const auto now = std::chrono::steady_clock::now();
  float dt = m_have_fade_time ? std::chrono::duration<float>(now - m_fade_time).count() : 0.f;
  m_fade_time = now;
  m_have_fade_time = true;
  dt = std::clamp(dt, 0.f, 0.1f);
  const float step = dt / kFadeS;
  std::vector<FadedLight> next;
  // a dark eco crystal follows its flash exactly: full while chosen, gone once dropped
  const auto snap = [](const world_lights::ActorLight& L) {
    return L.gate == world_lights::DARK_CRYSTAL || L.gate == world_lights::ECO;
  };
  for (const auto& L : *lights) {
    float w = 0.f;
    for (const auto& f : m_faded) {
      if (f.light.row == L.row) {
        w = f.weight;
      }
    }
    next.push_back({L, snap(L) ? 1.f : std::min(1.f, w + step)});
  }
  for (const auto& f : m_faded) {
    const bool chosen = std::any_of(lights->begin(), lights->end(),
                                    [&](const auto& L) { return L.row == f.light.row; });
    if (!chosen && !snap(f.light) && f.weight - step > 0.f) {
      next.push_back({f.light, f.weight - step});
    }
  }
  std::stable_sort(next.begin(), next.end(), [](const FadedLight& a, const FadedLight& b) {
    return a.weight * a.light.influence > b.weight * b.light.influence;
  });
  if (next.size() > (size_t)ActorShadowFrame::kMaxLights) {
    next.resize(ActorShadowFrame::kMaxLights);
  }
  m_faded = next;
  lights->clear();
  for (const auto& f : m_faded) {
    auto L = f.light;
    for (float& c : L.power) {
      c *= f.weight;
    }
    L.fade = f.weight;
    lights->push_back(L);
  }
}

void ActorShadows::prepass(DmaFollower dma,
                           SharedRenderState* rs,
                           Merc2& merc,
                           Generic2& generic,
                           ScopedProfilerNode& prof) {
  auto& fr = rs->shadow;
  if (rs->stereo_repeat_pass) {
    return;  // the maps are world and mono-camera space: one build serves both eyes
  }
  fr.active = false;
  fr.terrain = 0.f;
  fr.num_lamps = 0;
  rs->world_shadow.active = false;
  g_occ_tex = 0;
  const auto& gs = Gfx::g_global_settings;
  if (rs->version != GameVersion::Jak1 || gs.shadow_mode == (int)ShadowMode::OFF) {
    canopy_off();
    return;
  }
  if (std::clamp(gs.shadow_quality, 0, 2) != m_quality) {
    make_targets(std::clamp(gs.shadow_quality, 0, 2));
  }

  const u32 base = rs->buckets_base;
  const DmaFollower start = dma;
  auto bucket_end = [&](int b) { return base + 16 * (b + 1); };
  const int tfrag_buckets[2] = {(int)jak1::BucketId::TFRAG_LEVEL0,
                                (int)jak1::BucketId::TFRAG_LEVEL1};
  using B = jak1::BucketId;
  // caster buckets in DMA order; true = merc, false = generic
  const std::pair<B, bool> casters[] = {
      {B::MERC_TFRAG_TEX_LEVEL0, true}, {B::GENERIC_TFRAG_TEX_LEVEL0, false},
      {B::MERC_TFRAG_TEX_LEVEL1, true}, {B::GENERIC_TFRAG_TEX_LEVEL1, false},
      {B::MERC_AFTER_ALPHA, true},      {B::MERC_PRIS_LEVEL0, true},
      {B::GENERIC_PRIS_LEVEL0, false},  {B::MERC_PRIS_LEVEL1, true},
      {B::GENERIC_PRIS_LEVEL1, false},  {B::MERC_AFTER_PRIS, true},
      {B::GENERIC_PRIS, false}};

  // camera and mood weights from the first tfrag bucket that ran (both levels' port data kept)
  TfragPcPortData port[2];
  bool have_port[2] = {false, false};
  for (int i = 0; i < 2; i++) {
    skip_to(dma, base + 16 * tfrag_buckets[i]);
    have_port[i] = read_port_data(dma, bucket_end(tfrag_buckets[i]), &port[i]);
  }
  const TfragPcPortData* cam_port = have_port[0] ? &port[0] : (have_port[1] ? &port[1] : nullptr);
  if (!cam_port) {
    canopy_off();
    return;
  }
  const auto& cam = cam_port->camera;

  Mat4 w2c{};
  for (int c = 0; c < 4; c++) {
    for (int row = 0; row < 4; row++) {
      w2c[c * 4 + row] = cam.rot[c][row];
    }
  }
  // affine: the w lanes of the GOAL rows are not meaningful
  w2c[3] = w2c[7] = w2c[11] = 0.f;
  w2c[15] = 1.f;
  Mat4 c2w;
  if (!invert(w2c, &c2w)) {
    canopy_off();
    return;
  }
  auto cam_dir_to_world = [&](const math::Vector3f& d) {
    auto v = xform(c2w, math::Vector4f(d.x(), d.y(), d.z(), 0.f));
    return math::Vector3f(v.x(), v.y(), v.z());
  };
  const math::Vector3f cam_pos(cam.trans.x(), cam.trans.y(), cam.trans.z());
  const math::Vector3f fwd = cam_dir_to_world(math::Vector3f(0, 0, 1)).normalized();

  // sun direction and global share: the sidecar's fitted slots, else the previous frame's actor
  // light 0 (the mood-table sun the characters are lit with)
  m_level = cam_port->level_name;
  m_sun_from_sidecar = false;
  math::Vector3f sun_dir = m_fallback_dir;
  float share = m_fallback_share;
  const UnbakeSidecar* sun_sc = nullptr;
  const TfragPcPortData* sun_port = nullptr;
  float canopy_day = 0.f;
  for (int i = 0; i < 2 && !m_sun_from_sidecar; i++) {
    if (!have_port[i]) {
      continue;
    }
    const auto* lev = rs->loader->get_tfrag3_level(port[i].level_name);
    if (lev && lev->unbake &&
        unbake_sun_summary(*lev->unbake, port[i].camera.itimes, &sun_dir, &share)) {
      m_sun_from_sidecar = true;
      m_level = port[i].level_name;
      sun_sc = lev->unbake.get();
      sun_port = &port[i];
      // day weight of the palette blend: the canopy runs on the sun, never on moonlight
      float w[8][3];
      unbake_slot_weights(port[i].camera.itimes, w);
      canopy_day = 0.f;
      for (int sl = 0; sl < 8; sl++) {
        if (lev->unbake->day_mask & (1u << sl)) {
          canopy_day += (w[sl][0] + w[sl][1] + w[sl][2]) / 3.f;
        }
      }
      canopy_day = std::clamp(canopy_day, 0.f, 1.f);
    }
  }
  if (!m_sun_from_sidecar && !m_have_fallback) {
    share = 0.f;  // no light information yet (first frame)
  }
  const bool world_map = gs.shadow_world || gs.shadow_world_actors || gs.shadow_world_water;
  float sun_gain = 1.f;
  float world_strength = -1.f;  // < 0: the map's own fade on the palette sun (no sky)
  float light_day = canopy_day;
  bool moon = false;
  if (world_map) {
    // one light for every shadow: the sun on the disc's arc by day, the night slots' moon at
    // night; the stronger casts with the difference, so the hand-over passes through zero
    const TfragPcPortData* ports[2] = {have_port[0] ? &port[0] : nullptr,
                                       have_port[1] ? &port[1] : nullptr};
    math::Vector3f arc, moon_dir;
    float arc_day, hour, moon_w = 0.f, moon_share = 0.f;
    const bool sun_ok = sun_disc_direction(rs, ports, &m_mood, &arc, &arc_day, &hour);
    const bool moon_ok = moon_disc_direction(rs, ports, &moon_dir, &moon_w, &moon_share);
    if (sun_ok) {
      m_world.note_sun(sun_dir, hour);
    }
    const float g_sun = sun_ok ? world_shadows_sun_strength(canopy_day, arc.normalized().y()) : 0.f;
    const float g_moon =
        moon_ok ? world_shadows_sun_strength(moon_w, moon_dir.normalized().y()) : 0.f;
    const bool sky =
        m_sun_from_sidecar &&
        heightfield_level_has_sky(neighbours::viewer().empty() ? m_level : neighbours::viewer());
    if (sun_ok || moon_ok || sky) {
      moon = g_moon > g_sun;
      sun_gain = std::abs(g_sun - g_moon);
      if (moon) {
        m_last_light = moon_dir;
        light_day = moon_w;
      } else if (sun_ok) {
        m_last_light = arc;
      }
      m_have_last_light = m_have_last_light || sun_ok || moon_ok;
      if (m_have_last_light) {
        sun_dir = m_last_light;
      }
      world_strength = sun_gain;
    }
    m_moon_share = moon_ok ? moon_share : 0.f;
  }
  m_sun_dir = sun_dir;
  m_sun_gain = sun_gain;
  // the world map's fade, known before the casters draw: their cover must already step aside
  const float map_strength = !world_map || !rs->loader ? 0.f
                             : world_strength >= 0.f
                                 ? world_strength
                                 : world_shadows_sun_strength(canopy_day, sun_dir.normalized().y());
  m_moon = moon;

  // sun map: orthographic, fit to a sphere around the near part of the view frustum
  const float range = gs.shadow_range_m * kMeter;
  const float radius = 0.8f * range;
  const float toward_sun = 60.f * kMeter;
  const math::Vector3f f = sun_dir.normalized() * -1.f;
  math::Vector3f r, u;
  basis(f, &r, &u);
  const math::Vector3f center = cam_pos + fwd * (0.55f * range);
  const float texel = 2.f * radius / m_sun_size;
  // snap the centre to whole texels so the map does not shimmer as the camera moves
  const math::Vector3f snapped = r * (std::floor(r.dot(center) / texel) * texel) +
                                 u * (std::floor(u.dot(center) / texel) * texel) +
                                 f * f.dot(center);
  Mat4 sun_view = view_matrix(r, u, f, snapped);
  const float zn = -radius - toward_sun, zf = radius;
  Mat4 sun_proj{};
  sun_proj[0] = 1.f / radius;
  sun_proj[5] = 1.f / radius;
  sun_proj[10] = 2.f / (zf - zn);
  sun_proj[14] = -(zf + zn) / (zf - zn);
  sun_proj[15] = 1.f;
  const Mat4 sun_world = mul(sun_proj, sun_view);

  // local lights (world light table): the strongest at Jak light the characters, the first two
  // also cast from Jak (spot maps aimed at him)
  std::vector<Merc2::ShadowView> views;
  views.push_back({m_sun.fbo, m_sun_size, {}});
  store(mul(sun_world, c2w), views[0].cam_to_clip);
  std::vector<world_lights::ActorLight> lights;
  std::string crystal_log;
  float glow_at_jak = 0.f;
  fr.actor_dark = 1.f;
  fr.num_crystals = 0;
  fr.bg_dark = gs.shadow_cave_dark;
  if (m_have_prev_player && rs->loader) {
    std::vector<world_lights::LevelWeights> levels;
    for (auto* lev : rs->loader->get_in_use_levels()) {
      world_lights::LevelWeights lw;
      lw.name = rs->loader->name_of(lev);
      // this frame's own camera for the level: the "last" weights are whichever draw wrote
      // them last (a neighbour or vista draw uses another level's hour), so they can alternate
      const TfragPcPortData* own = nullptr;
      for (int i = 0; i < 2; i++) {
        if (have_port[i] && lw.name == port[i].level_name) {
          own = &port[i];
        }
      }
      if (lev->unbake && own) {
        unbake_emissive_weights(*lev->unbake, own->camera, lw.w);
        lw.have = true;
      } else {
        lw.have = lev->unbake && unbake_last_emissive_weights(lev->unbake.get(), lw.w);
      }
      levels.push_back(std::move(lw));
    }
    const float at_m[3] = {m_prev_player.x() / kMeter, m_prev_player.y() / kMeter + kJakCentreM,
                           m_prev_player.z() / kMeter};
    lights = world_lights::select_actor(levels, at_m, std::clamp(gs.shadow_lights, 0, 4),
                                        gs.shadow_light_reach_m, gs.shadow_light_r0_m, m_prev_lamps,
                                        kKeepBonus);
    float lit[ActorShadowFrame::kMaxCrystals][4];
    fr.num_crystals = world_lights::cave_crystals(levels, lit, ActorShadowFrame::kMaxCrystals);
    for (int i = 0; i < fr.num_crystals; i++) {
      const auto c = xform(
          w2c, math::Vector4f(lit[i][0] * kMeter, lit[i][1] * kMeter, lit[i][2] * kMeter, 1.f));
      fr.crystal_cam[i] = math::Vector4f(c.x(), c.y(), c.z(), lit[i][3]);
      crystal_log += fmt::format(" ({:.1f} {:.1f} {:.1f}) glow {:.2f}", lit[i][0], lit[i][1],
                                 lit[i][2], lit[i][3]);
      const float d = std::sqrt((lit[i][0] - at_m[0]) * (lit[i][0] - at_m[0]) +
                                (lit[i][1] - at_m[1]) * (lit[i][1] - at_m[1]) +
                                (lit[i][2] - at_m[2]) * (lit[i][2] - at_m[2]));
      glow_at_jak =
          std::max(glow_at_jak, lit[i][3] * (1.f - std::clamp((d - 5.f) / 65.f, 0.f, 1.f)));
    }
    if (neighbours::viewer() == "darkcave") {
      fr.actor_dark = gs.shadow_cave_dark;
    }
  }
  {
    // the cave factor moves toward its target over kFadeS, so Jak does not pop at a level border
    const auto now = std::chrono::steady_clock::now();
    const float dt =
        m_have_dark_time
            ? std::clamp(std::chrono::duration<float>(now - m_dark_time).count(), 0.f, 0.1f)
            : 1.f;
    m_dark_time = now;
    m_have_dark_time = true;
    const float step = dt / kFadeS * (1.f - std::min(gs.shadow_cave_dark, 0.99f));
    m_actor_dark += std::clamp(fr.actor_dark - m_actor_dark, -step, step);
    fr.actor_dark = m_actor_dark;
  }
  fade_lights(&lights);
  {
    // chosen lights: every frame in debug view 4 or while one fades, else when the set changes
    static const char* kCls[] = {"strong", "weak", "arc", "particle", "lamp", "glow"};
    std::string ids;
    for (const auto& L : lights) {
      ids += std::to_string(L.row) + " ";
    }
    ids += "c" + std::to_string(fr.num_crystals);
    const bool fading =
        std::any_of(lights.begin(), lights.end(), [](const auto& L) { return L.fade < 1.f; });
    if (gs.shadow_view == 4 || fading || ids != m_last_light_ids) {
      m_last_light_ids = ids;
      std::string msg;
      for (size_t i = 0; i < lights.size() && i < (size_t)ActorShadowFrame::kMaxLights; i++) {
        const auto& L = lights[i];
        msg += fmt::format(
            " | #{} row {} {} {} gate {} aid {} at ({:.1f} {:.1f} {:.1f}) d {:.1f} m state {:.2f} fade "
            "{:.2f} "
            "power ({:.2f} {:.2f} {:.2f}) at-jak {:.3f}{}",
            i, L.row, L.level, kCls[std::min<int>(L.cls, 5)], (int)L.gate, L.aid, L.pos_m[0], L.pos_m[1],
            L.pos_m[2], L.dist_m, L.state, L.fade, L.power[0], L.power[1], L.power[2], L.influence,
            i < ActorShadowFrame::kMaxLamps ? (L.added ? " map added" : " map") : "");
      }
      const float dark_at_jak = std::min(fr.actor_dark + (1.f - fr.actor_dark) * glow_at_jak, 1.f);
      lg::info(
          "local-lights: jak ({:.1f} {:.1f} {:.1f}) dark {:.2f} at-jak {:.2f}, {} lit crystals{}, "
          "{} lights{}",
          m_prev_player.x() / kMeter, m_prev_player.y() / kMeter, m_prev_player.z() / kMeter,
          fr.actor_dark, dark_at_jak, fr.num_crystals, crystal_log, lights.size(), msg);
    }
  }
  const int n_lights = std::min((int)lights.size(), ActorShadowFrame::kMaxLights);
  const int n_maps = std::min(n_lights, ActorShadowFrame::kMaxLamps);
  m_prev_lamps.clear();
  m_num_lights = n_lights;
  const math::Vector3f target = m_prev_player + math::Vector3f(0.f, kJakCentreM * kMeter, 0.f);
  for (int i = 0; i < ActorShadowFrame::kMaxLamps; i++) {
    fr.lamp_row[i] = i < n_maps ? lights[i].row : -1;
    fr.lamp_added[i] = i < n_maps && lights[i].added ? 1.f : 0.f;
  }
  for (int i = 0; i < ActorShadowFrame::kMaxLights; i++) {
    fr.light_added[i] = i < n_lights && lights[i].added ? 1.f : 0.f;
  }
  for (int i = 0; i < n_lights; i++) {
    const auto& L = lights[i];
    const math::Vector3f p(L.pos_m[0] * kMeter, L.pos_m[1] * kMeter, L.pos_m[2] * kMeter);
    fr.lamp_pos_world[i] = math::Vector4f(p.x(), p.y(), p.z(), L.r0_m * kMeter);
    auto pc = xform(w2c, math::Vector4f(p.x(), p.y(), p.z(), 1.f));
    fr.lamp_pos_cam[i] = math::Vector4f(pc.x(), pc.y(), pc.z(), L.r0_m * kMeter);
    const float k = gs.shadow_lamp_scale;
    fr.lamp_color[i] = math::Vector4f(L.power[0] * k, L.power[1] * k, L.power[2] * k,
                                      L.radius_m * kMeter);
    if (i >= n_maps) {
      continue;
    }
    m_prev_lamps.push_back(L.row);
    // spot aimed at Jak, wide enough for him and whoever stands next to him
    const float d = std::max((target - p).length(), 0.5f * kMeter);
    math::Vector3f lf = (target - p) / d, lr, lu;
    float half = std::clamp(std::atan(2.2f * kMeter / d), 0.35f, 1.3f);
    if (L.added) {
      // a light the bake lacks shadows Jak and every actor within its radius: aim at their mean
      // and open the spot to the widest of them plus a metre and a half of shadow
      std::vector<math::Vector3f> dirs = {lf};
      for (const auto& a : m_prev_actors) {
        const math::Vector3f to = a + math::Vector3f(0.f, 0.6f * kMeter, 0.f) - p;
        if (to.length() > 0.3f * kMeter && to.length() < L.radius_m * kMeter) {
          dirs.push_back(to.normalized());
        }
      }
      math::Vector3f mean(0.f, 0.f, 0.f);
      for (const auto& v : dirs) {
        mean += v;
      }
      if (mean.length() > 1e-3f) {
        lf = mean.normalized();
      }
      float widest = 0.f;
      for (const auto& v : dirs) {
        widest = std::max(widest, std::acos(std::clamp(v.dot(lf), -1.f, 1.f)));
      }
      half = std::clamp(widest + std::atan(1.5f * kMeter / d), 0.35f, 1.3f);
    }
    basis(lf, &lr, &lu);
    const Mat4 v = view_matrix(lr, lu, lf, p);
    const float n = 0.2f * kMeter, fa = d + gs.shadow_light_reach_m * kMeter;
    const float s = 1.f / std::tan(half);
    Mat4 proj{};
    proj[0] = s;
    proj[5] = s;
    proj[10] = (fa + n) / (fa - n);
    proj[11] = 1.f;
    proj[14] = -2.f * fa * n / (fa - n);
    const Mat4 lw = mul(proj, v);
    store(lw, fr.lamp_world[i]);
    store(mul(lw, c2w), fr.lamp_cam[i]);
    views.push_back({m_lamps[i].fbo, m_lamp_size, {}});
    memcpy(views.back().cam_to_clip, fr.lamp_cam[i], sizeof(fr.lamp_cam[i]));
    views.back().fixture = math::Vector4f(pc.x(), pc.y(), pc.z(), kFixtureM * kMeter);
  }

  // draw the actors (timed: GPU with a query read back a frame later, CPU wall time)
  const auto t0 = std::chrono::steady_clock::now();
  if (!m_query) {
    glGenQueries(1, &m_query);
  } else if (m_query_pending) {
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
  const bool timing = !m_query_pending;
  if (timing) {
    glBeginQuery(GL_TIME_ELAPSED, m_query);
  }
  GLint old_draw_fb, old_read_fb, old_vp[4], old_depth_func;
  GLboolean old_depth_mask;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_draw_fb);
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read_fb);
  glGetIntegerv(GL_VIEWPORT, old_vp);
  glGetIntegerv(GL_DEPTH_FUNC, &old_depth_func);
  glGetBooleanv(GL_DEPTH_WRITEMASK, &old_depth_mask);
  GLfloat old_clear_depth;
  glGetFloatv(GL_DEPTH_CLEAR_VALUE, &old_clear_depth);
  const GLenum caps[] = {GL_SCISSOR_TEST, GL_BLEND, GL_STENCIL_TEST, GL_DEPTH_TEST, GL_CULL_FACE};
  GLboolean old_caps[5];
  for (int i = 0; i < 5; i++) {
    old_caps[i] = glIsEnabled(caps[i]);
  }
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_CULL_FACE);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LESS);
  glDepthMask(GL_TRUE);
  glClearDepth(1.0);  // the frame clears to 0 for its GEQUAL test
  perf::count(perf::Counter::SHADOW_MAPS, (double)views.size());
  GLfloat old_clear_colour[4];
  glGetFloatv(GL_COLOR_CLEAR_VALUE, old_clear_colour);
  GLboolean old_colour_mask[4];
  glGetBooleanv(GL_COLOR_WRITEMASK, old_colour_mask);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glClearColor(1.f, 1.f, 1.f, 1.f);
  for (const auto& v : views) {
    perf::Scope ps(perf::Group::SHADOW_CLEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, v.fbo);
    glViewport(0, 0, v.size, v.size);
    glClear(v.fbo == m_sun.fbo ? GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT : GL_DEPTH_BUFFER_BIT);
  }
  glClearColor(old_clear_colour[0], old_clear_colour[1], old_clear_colour[2], old_clear_colour[3]);

  // terrain cover inputs for this frame's merc draws (casters here, lighting later); actors take
  // the world map per pixel instead as it fades in, casters included
  fr.terrain = gs.shadow_terrain_sun * (gs.shadow_world_actors ? 1.f - map_strength : 1.f);
  fr.num_terrain_maps = 0;
  for (auto* lev : rs->loader ? rs->loader->get_in_use_levels() : std::vector<LevelData*>{}) {
    if (lev->heightfield && fr.num_terrain_maps < ActorShadowFrame::kMaxTerrainMaps) {
      fr.terrain_maps[fr.num_terrain_maps++] = lev->heightfield.get();
    }
  }
  store(c2w, fr.cam_to_world);
  {
    // the cover fades with a low sun as the canopy does (sun.y 0.25 to 0.1): no long sideways march
    const math::Vector3f d = sun_dir.normalized();
    const float t = std::clamp((d.y() - 0.1f) / (0.25f - 0.1f), 0.f, 1.f);
    fr.terrain_sun = math::Vector4f(d.x(), d.y(), d.z(), t * t * (3.f - 2.f * t) * sun_gain);
  }
  merc.clear_shadow_records();
  std::vector<Generic2::ShadowView> gviews;
  for (const auto& v : views) {
    gviews.push_back({v.fbo, v.size, v.cam_to_clip[0].data()});
  }
  // the camera port data sits in bucket 13 at most, so restart the walk from bucket 0
  dma = start;
  const u32 saved_next = rs->next_bucket;
  for (const auto& [bucket, is_merc] : casters) {
    const int b = (int)bucket;
    skip_to(dma, base + 16 * b);
    rs->next_bucket = bucket_end(b);
    if (is_merc) {
      perf::Scope ps(perf::Group::SHADOW_MERC);
      merc.render_shadow(dma, rs, prof, views);
    } else {
      perf::Scope ps(perf::Group::SHADOW_GENERIC);
      generic.render_shadow(dma, rs, gviews);
    }
    skip_to(dma, bucket_end(b));
  }
  rs->next_bucket = saved_next;
  if (Gfx::perf_opt(PERF_OPT_GEOMETRY + 0)) {
    build_occupancy();
  }

  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, old_draw_fb);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, old_read_fb);
  glViewport(old_vp[0], old_vp[1], old_vp[2], old_vp[3]);
  glDepthFunc(old_depth_func);
  glClearDepth(old_clear_depth);
  if (timing) {
    glEndQuery(GL_TIME_ELAPSED);
    m_query_pending = true;
  }
  m_cpu_ms_sum += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  m_cpu_n++;
  glDepthMask(old_depth_mask);
  glColorMask(old_colour_mask[0], old_colour_mask[1], old_colour_mask[2], old_colour_mask[3]);
  for (int i = 0; i < 5; i++) {
    old_caps[i] ? glEnable(caps[i]) : glDisable(caps[i]);
  }

  // records for the next frame: actor positions, fallback sun from the first actor's light 0
  m_prev_actors.clear();
  for (const auto& a : merc.shadow_actor_positions()) {
    auto w = xform(c2w, math::Vector4f(a.x(), a.y(), a.z(), 1.f));
    m_prev_actors.emplace_back(w.x(), w.y(), w.z());
  }
  m_actor_count = (int)m_prev_actors.size();
  math::Vector3f ldir, lcol, lamb;
  if (merc.shadow_first_lights(&ldir, &lcol, &lamb) && ldir.length() > 1e-6f) {
    m_fallback_dir = cam_dir_to_world(ldir).normalized();
    const float sun = lum(lcol), total = sun + lum(lamb);
    m_fallback_share = total > 1e-6f ? sun / total : 0.f;
    m_have_fallback = true;
  }

  static int s_log_count = 0;
  if (s_log_count++ % 240 == 0 && !rs->stereo_repeat_pass) {
    lg::info(
        "actor-shadows: [{} {}] {} casters, sun from {} ({:.2f} {:.2f} {:.2f}), share {:.2f}, {} "
        "lights, "
        "pre-pass gpu {:.3f} ms cpu {:.3f} ms, terrain {:.2f} maps {} cover at Jak {:.2f}, "
        "sun gain {:.2f} ({}, moon share {:.2f})",
        have_port[0] ? port[0].level_name : "-", have_port[1] ? port[1].level_name : "-",
        m_actor_count, m_sun_from_sidecar ? m_level : "actor light 0", m_sun_dir.x(), m_sun_dir.y(),
        m_sun_dir.z(), share, n_lights, m_gpu_n ? m_gpu_ms_sum / m_gpu_n : 0.0,
        m_cpu_n ? m_cpu_ms_sum / m_cpu_n : 0.0, fr.terrain, fr.num_terrain_maps, m_player_cover,
        m_sun_gain, m_moon ? "moon" : "sun", m_moon_share);
    m_gpu_ms_sum = m_cpu_ms_sum = 0;
    m_gpu_n = m_cpu_n = 0;
  }
  canopy_prepass(rs, sun_dir.data(), c2w.data(), light_day, sun_gain);
  fr.active = true;
  fr.view = gs.shadow_view;
  fr.strength = gs.shadow_strength;
  fr.sun_gain = sun_gain;
  fr.moon = moon;
  fr.marker = gs.shadow_marker;
  // reach is a height above the receiver: along the ray it grows as the sun gets low
  fr.reach = gs.shadow_reach_m * kMeter / std::max(sun_dir.normalized().y(), 0.15f) / (zf - zn);
  fr.marker_pos_world = math::Vector4f(0, 0, 0, 0);
  math::Vector3f player;
  m_have_prev_player = merc.shadow_player_position(&player);
  if (m_have_prev_player) {
    auto w = xform(c2w, math::Vector4f(player.x(), player.y(), player.z(), 1.f));
    m_prev_player = math::Vector3f(w.x(), w.y(), w.z());
    const float cover = actor_shadows_cover(rs, player);
    m_player_cover = cover;
    if (gs.shadow_marker > 0.f) {
      fr.marker_pos_world = math::Vector4f(w.x(), w.y(), w.z(), 0.45f * kMeter);
    }
  }
  fr.share_global = share;
  fr.caster_tex = m_sun.colour;
  // sky-hued shadows: the fitted slots' sun and ambient, blended as unbake_sun_summary blends them
  fr.hue_t = 0.f;
  if (gs.shadow_sky_hue && sun_sc && heightfield_level_has_sky(neighbours::viewer())) {
    float w[8][3];
    unbake_slot_weights(sun_port->camera.itimes, w);
    math::Vector3f lgt(0, 0, 0), amb(0, 0, 0);
    for (int s = 0; s < 8; s++) {
      if (sun_sc->slot_mask & (1u << s)) {
        const float ws = (w[s][0] + w[s][1] + w[s][2]) / 3.f;
        for (int c = 0; c < 3; c++) {
          lgt[c] += std::max(sun_sc->slots[s].lgt[c], 0.f) * ws;
          amb[c] += std::max(sun_sc->slots[s].amb[c], 0.f) * ws;
        }
      }
    }
    if (lum(lgt) > 1e-3f && lum(amb) > 1e-3f) {
      const auto ln = unit_lum(lgt), an = unit_lum(amb);
      fr.hue_t = gs.shadow_sky_hue_strength;
      fr.hue_lgt = math::Vector4f(ln.x(), ln.y(), ln.z(), 0.f);
      fr.hue_amb = math::Vector4f(an.x(), an.y(), an.z(), 0.f);
    }
  }
  fr.num_lamps = n_maps;
  fr.num_lights = n_lights;
  store(sun_world, fr.sun_world);
  store(mul(sun_world, c2w), fr.sun_cam);
  fr.sun_texel = 1.f / m_sun_size;
  fr.lamp_texel = 1.f / m_lamp_size;
  fr.taps = m_quality <= 0 ? 1 : m_quality == 1 ? 4 : 9;
  fr.sun_tex = m_sun.tex;
  for (int i = 0; i < ActorShadowFrame::kMaxLamps; i++) {
    fr.lamp_tex[i] = m_lamps[i].tex;
  }
  if (world_map) {
    // half field of view tangents from the game's perspective (as SunDisc maps pixels)
    const auto& persp = cam.perspective;
    const float fogx = cam.fog.x();
    const float pzw = std::abs(persp[2][3]);
    const float pxx = std::abs(fogx * persp[0][0] / 256.f);
    const float pyy = std::abs(fogx * persp[1][1] / 128.f * (512.f / 448.f));
    float tan_xy[2] = {1.f, 1.f};
    if (pxx > 0.f && pyy > 0.f && pzw > 0.f) {
      tan_xy[0] = std::clamp(pzw / pxx, 0.2f, 4.f);
      tan_xy[1] = std::clamp(pzw / pyy, 0.2f, 4.f);
    }
    // the ambient floor comes from the level the camera is in, as the arc does
    const UnbakeSidecar* amb_sc = sun_sc;
    const TfragPcPortData* amb_port = sun_port;
    for (int i = 0; i < 2; i++) {
      const auto* lev =
          have_port[i] ? rs->loader->get_tfrag3_level(port[i].level_name) : nullptr;
      if (lev && lev->unbake && neighbours::viewer() == port[i].level_name) {
        amb_sc = lev->unbake.get();
        amb_port = &port[i];
      }
    }
    m_world.prepass(rs, c2w.data(), w2c.data(), cam_pos, fwd, tan_xy, sun_dir, map_strength, amb_sc,
                    amb_port ? amb_port->camera.itimes : nullptr);
  }
}

void ActorShadows::draw_debug_window() {
  auto& gs = Gfx::g_global_settings;
  const char* modes[] = {"off (vanilla)", "on (sun and lamps, all actors)"};
  ImGui::Combo("Setting", &gs.shadow_mode, modes, 2);
  ImGui::SliderFloat("Strength", &gs.shadow_strength, 0.f, 4.f);
  const char* views[] = {"none", "occlusion", "light share", "map coordinates", "local lights"};
  ImGui::Combo("View", &gs.shadow_view, views, 5);
  ImGui::SliderFloat("Range (m)", &gs.shadow_range_m, 10.f, 120.f);
  ImGui::SliderFloat("Local light scale", &gs.shadow_lamp_scale, 0.f, 4.f);
  ImGui::SliderInt("Local lights (first two cast)", &gs.shadow_lights, 0, 4);
  ImGui::SliderFloat("Local light r0 (m)", &gs.shadow_light_r0_m, 0.5f, 10.f);
  ImGui::SliderFloat("Local light reach (m)", &gs.shadow_light_reach_m, 2.f, 30.f);
  ImGui::SliderFloat("Darkcave level light", &gs.shadow_cave_dark, 0.f, 1.f);
  ImGui::SliderFloat("Landing disc", &gs.shadow_marker, 0.f, 1.f);
  ImGui::SliderFloat("Caster reach (m)", &gs.shadow_reach_m, 0.5f, 30.f);
  ImGui::SliderFloat("Terrain sun occlusion", &gs.shadow_terrain_sun, 0.f, 1.f);
  ImGui::Checkbox("Sky-hued shadows", &gs.shadow_sky_hue);
  ImGui::SliderFloat("Sky hue strength", &gs.shadow_sky_hue_strength, 0.f, 2.f);
  ImGui::SliderFloat("Canopy shade", &gs.shadow_canopy, 0.f, 1.f);
  ImGui::SliderFloat("Canopy shade darkening", &gs.shadow_canopy_ambient, 0.f, 0.8f);
  ImGui::SliderFloat("Canopy sun patch boost", &gs.shadow_canopy_patch, 0.f, 0.5f);
  ImGui::SliderFloat("Canopy sun patch edge", &gs.shadow_canopy_sharp, 0.f, 1.f);
  ImGui::SliderFloat("Canopy penumbra per m", &gs.shadow_canopy_penumbra, 0.f, 0.2f);
  ImGui::SliderFloat("Canopy cool shade", &gs.shadow_canopy_tint, 0.f, 1.f);
  ImGui::Text("level %s, sun from %s, dir %.2f %.2f %.2f", m_level.c_str(),
              m_sun_from_sidecar ? "sidecar" : "actor light 0", m_sun_dir.x(), m_sun_dir.y(),
              m_sun_dir.z());
  ImGui::Text("actors %d, local lights %d, actor light x%.2f", m_actor_count, m_num_lights,
              m_actor_dark);
  m_world.draw_debug_window();
}

void actor_shadows_set_cave_uniforms(const SharedRenderState* rs, GLuint program) {
  const auto& fr = rs->shadow;
  const int n = fr.active ? std::clamp(fr.num_crystals, 0, ActorShadowFrame::kMaxCrystals) : 0;
  glUniform1f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "cave_dark_base"),
              fr.active ? fr.actor_dark : 1.f);
  glUniform1i(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "cave_ncrystal"), n);
  if (n > 0) {
    glUniform4fv(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "cave_crystal"), n,
                 fr.crystal_cam[0].data());
  }
}

float actor_shadows_cover(const SharedRenderState* rs, const math::Vector3f& cam_pos) {
  const auto& fr = rs->shadow;
  if (fr.terrain <= 0.f || fr.num_terrain_maps == 0 || fr.terrain_sun.w() <= 0.f) {
    return 0.f;
  }
  Mat4 c2w;
  memcpy(c2w.data(), fr.cam_to_world, sizeof(Mat4));
  const auto w = xform(c2w, math::Vector4f(cam_pos.x(), cam_pos.y(), cam_pos.z(), 1.f));
  const math::Vector3f root(w.x() / kMeter, w.y() / kMeter, w.z() / kMeter);
  // toward the sun, its rise clamped as the canopy's (sun.y >= 0.1): a low sun stays local
  const math::Vector3f s(fr.terrain_sun.x(), fr.terrain_sun.y(), fr.terrain_sun.z());
  const math::Vector3f dir = (s / std::max(s.y(), 0.1f)).normalized();
  math::Vector3f side(-dir.z(), 0.f, dir.x());
  side = side.length() > 1e-3f ? side.normalized() : math::Vector3f(1.f, 0.f, 0.f);
  constexpr float kLength = 40.f, kStep = 1.f, kMargin = 0.3f;
  float y_max = -std::numeric_limits<float>::infinity();
  for (int i = 0; i < fr.num_terrain_maps; i++) {
    y_max = std::max(y_max, fr.terrain_maps[i]->y1);
  }
  // feet, waist, head, and both sides at the waist: a roof edge crosses the body gradually
  const float starts[5][2] = {{0.f, 0.5f}, {0.f, 1.f}, {0.f, 1.6f}, {-0.4f, 1.f}, {0.4f, 1.f}};
  float sum = 0.f;
  for (const auto& st : starts) {
    const math::Vector3f o = root + side * st[0] + math::Vector3f(0.f, st[1], 0.f);
    float blocked = 0.f;
    float y_prev = o.y();
    for (float d = kStep; d <= kLength && blocked < 1.f && y_prev < y_max + kMargin; d += kStep) {
      const math::Vector3f q = o + dir * d;
      for (int i = 0; i < fr.num_terrain_maps; i++) {
        blocked = std::max(blocked, fr.terrain_maps[i]->solid_span(q.x(), q.z(), y_prev, q.y(),
                                                                   kMargin));
      }
      y_prev = q.y();
    }
    sum += blocked;
  }
  const float share = sum / 5.f;
  return fr.terrain * fr.terrain_sun.w() * share * share * (3.f - 2.f * share);
}

void actor_shadows_set_uniforms(const SharedRenderState* rs, GLuint program, bool camera_space) {
  const auto& fr = rs->shadow;
  glUniform1i(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "shadow_on"), fr.active ? 1 : 0);
  if (!fr.active) {
    // the shadow samplers must still see a depth texture, or the driver logs every draw
    static GLuint dummy = 0;
    if (!dummy) {
      glActiveTexture(GL_TEXTURE0 + kSunUnit);
      glGenTextures(1, &dummy);
      glBindTexture(GL_TEXTURE_2D, dummy);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, 1, 1, 0, GL_DEPTH_COMPONENT,
                   GL_UNSIGNED_INT, nullptr);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    }
    for (int i = 0; i <= ActorShadowFrame::kMaxLamps; i++) {
      glActiveTexture(GL_TEXTURE0 + kSunUnit + i);
      glBindTexture(GL_TEXTURE_2D, dummy);
    }
    glActiveTexture(GL_TEXTURE0);
    world_shadows_set_uniforms(rs, program, camera_space);
    return;
  }
  glUniform1i(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "shadow_view"), fr.view);
  glUniform1f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "shadow_strength"), fr.strength);
  glUniform1f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "shadow_sun_gain"), fr.sun_gain);
  glUniform1f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "shadow_share_global"), fr.share_global);
  glUniformMatrix4fv(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "shadow_mtx_sun"), 1, GL_FALSE,
                     camera_space ? fr.sun_cam[0].data() : fr.sun_world[0].data());
  glUniform1i(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "shadow_nlamps"), fr.num_lamps);
  glUniformMatrix4fv(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "shadow_mtx_lamp"),
                     ActorShadowFrame::kMaxLamps, GL_FALSE,
                     camera_space ? fr.lamp_cam[0][0].data() : fr.lamp_world[0][0].data());
  glUniform4fv(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "shadow_lamp_pos"),
               ActorShadowFrame::kMaxLights,
               camera_space ? fr.lamp_pos_cam[0].data() : fr.lamp_pos_world[0].data());
  glUniform4fv(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "shadow_lamp_col"),
               ActorShadowFrame::kMaxLights, fr.lamp_color[0].data());
  glUniform1fv(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "shadow_lamp_added"),
               ActorShadowFrame::kMaxLamps, fr.lamp_added);
  glUniform1fv(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "shadow_light_added"),
               ActorShadowFrame::kMaxLights, fr.light_added);
  glUniform1i(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "shadow_nlights"), fr.num_lights);
  glUniform2f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "shadow_texel"), fr.sun_texel, fr.lamp_texel);
  glUniform1i(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "shadow_taps"), fr.taps);
  glUniform1f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "shadow_reach"), fr.reach);
  glUniform4fv(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "shadow_marker_pos"), 1,
               fr.marker_pos_world.data());
  glUniform1f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "shadow_marker_strength"), fr.marker);
  glUniform1i(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "shadow_skip"), g_occ_tex ? 1 : 0);
  glUniform1f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "shadow_hue_t"), fr.hue_t);
  if (fr.hue_t > 0.f) {
    glUniform3fv(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "shadow_hue_lgt"), 1, fr.hue_lgt.data());
    glUniform3fv(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "shadow_hue_amb"), 1, fr.hue_amb.data());
  }
  if (g_occ_tex) {
    glActiveTexture(GL_TEXTURE0 + kOccUnit);
    glBindTexture(GL_TEXTURE_2D, g_occ_tex);
  }
  glActiveTexture(GL_TEXTURE0 + kSunUnit);
  glBindTexture(GL_TEXTURE_2D, fr.sun_tex);
  for (int i = 0; i < ActorShadowFrame::kMaxLamps; i++) {
    glActiveTexture(GL_TEXTURE0 + kLampUnit0 + i);
    glBindTexture(GL_TEXTURE_2D, fr.lamp_tex[i]);
  }
  // glq tracks units 0-15 only: unit 44 is ours alone and holds the caster map until replaced
  glUniform1i(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "shadow_caster_tex"), kCasterUnit);
  glActiveTexture(GL_TEXTURE0 + kCasterUnit);
  glBindTexture(GL_TEXTURE_2D, fr.caster_tex);
  glActiveTexture(GL_TEXTURE0);
  canopy_set_uniforms(program, camera_space);
  world_shadows_set_uniforms(rs, program, camera_space);
}
