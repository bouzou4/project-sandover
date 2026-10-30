#include "SsaoRenderer.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <optional>
#include <string>

#include "common/log/log.h"

#include "game/graphics/gfx.h"
#include "game/graphics/opengl_renderer/EmissiveLights.h"
#include "game/graphics/opengl_renderer/GlQueries.h"
#include "game/graphics/opengl_renderer/Materials.h"
#include "game/graphics/opengl_renderer/PerfTimers.h"
#include "game/graphics/opengl_renderer/StencilMarks.h"
#include "game/graphics/opengl_renderer/Swamp.h"
#include "game/graphics/opengl_renderer/TemporalAccum.h"
#include "game/graphics/opengl_renderer/buckets.h"

#include "third-party/imgui/imgui.h"

namespace {
struct PresetEntry {
  const char* name;
  const char* label;
  SsaoParams params;
};
const PresetEntry kPresets[] = {
#include "game/graphics/opengl_renderer/SsaoPresets.inc"
};
constexpr int kPresetCount = sizeof(kPresets) / sizeof(kPresets[0]);

constexpr float kFarM = 10000.f;

// pc-set-perf-opt bits (PERF_OPT_SSAO + n)
constexpr int kOptDirectDepth = PERF_OPT_SSAO + 0;  // linearize reads render_fb's depth, no copy
constexpr int kOptNoQueries = PERF_OPT_SSAO + 3;    // cached uniform locations, no fb binding query

bool g_normals_active = false;   // normal target attached and cleared this dispatch
bool g_normals_drawing = false;  // draw buffer 1 enabled on the render framebuffer
bool g_dn_active = false;        // detail normal target attached; draw buffer 6 goes with 1
// programs whose fragment stage writes the normal target (ssao_normal_frag.glsl)
constexpr ShaderId kNormalShaders[] = {
    ShaderId::TFRAG3,   ShaderId::TFRAG3_SUN, ShaderId::TFRAG3_NO_TEX, ShaderId::SHRUB,
    ShaderId::TIE_WIND, ShaderId::ETIE_BASE,  ShaderId::ETIE,          ShaderId::MERC2,
    ShaderId::EMERC,    ShaderId::GENERIC};
// fixed-slot "quick" mood levels plus darkcave (facts/levels.md)
const char* const kCaveLevels[] = {"darkcave", "maincave", "robocave",
                                   "jungleb",  "lavatube", "citadel"};

GLuint g_em_fb = 0;           // framebuffer the emissive target is attached to, 0 if none
bool g_em_masked = false;     // draw buffer 2 has a partial colour mask (emissive or material only)
bool g_em_mask[2] = {true, true};  // that mask: E (r, g), material (b, a)
bool g_nrm_write = true;            // last set_normal_write
bool g_color_lock = false;          // depth prepass: every colour mask off, left alone
bool g_insp_drawing = false;  // draw buffer 3 (inspect ID target) enabled on the render framebuffer
bool g_sz_live = false;       // sprite depth target attached and cleared this dispatch
bool g_sz_drawing = false;    // draw buffer 4 (sprite depth) goes with the emissive output
float g_lin[3] = {};          // depth linearization of this dispatch (render())

bool g_vel_drawing = false;  // draw buffer 5 (motion vectors object target) on, merc2 draws only

// debug probe (pc-set-ssao-option 4): 1 no stencil, 2 no normal draw buffer, 4 ssao_nrm 0,
// 8 no vertex normals, 16 no emissive output, 32 no normal target, 64 no sprite depth,
// 128 no AO pass, 256 no per-draw normal colour mask, 512 stencil test without writes,
// 1024/2048/4096/8192 only sky/background/character/translucent buckets write the stencil (these
// test the per-bucket writes: perf-opt bit 5 off); 16384 compares the late marks (StencilMarks.h)
bool probe(int bit) {
  return (Gfx::g_global_settings.ssao_prereq_probe & bit) != 0;
}

// render framebuffer draw buffers: 0, normals, emissive, inspect, sprite depth, object velocity,
// detail normals (a count of 2 or 3 when inspect is off)
void render_fb_draw_buffers(bool emissive) {
  const bool sz = emissive && g_sz_drawing;
  const bool dn = g_normals_drawing && g_dn_active;
  const GLenum bufs[7] = {GL_COLOR_ATTACHMENT0,
                          g_normals_drawing ? GL_COLOR_ATTACHMENT1 : GL_NONE,
                          emissive ? GL_COLOR_ATTACHMENT2 : GL_NONE,
                          g_insp_drawing ? GL_COLOR_ATTACHMENT3 : GL_NONE,
                          sz ? GL_COLOR_ATTACHMENT4 : GL_NONE,
                          g_vel_drawing ? GL_COLOR_ATTACHMENT5 : GL_NONE,
                          dn ? GL_COLOR_ATTACHMENT6 : GL_NONE};
  glDrawBuffers(
      dn ? 7 : (g_vel_drawing ? 6 : (sz ? 5 : (g_insp_drawing ? 4 : (emissive ? 3 : 2)))), bufs);
}

void apply_stencil(GLint ref, GLuint write_mask) {
  glEnable(GL_STENCIL_TEST);
  glStencilFunc(GL_ALWAYS, ref, 0xFF);
  glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
  glStencilMask(write_mask);
}

void make_color_target(GLuint& fbo,
                       GLuint& tex,
                       int w,
                       int h,
                       GLint ifmt,
                       GLenum fmt,
                       GLenum type) {
  glGenFramebuffers(1, &fbo);
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexImage2D(GL_TEXTURE_2D, 0, ifmt, w, h, 0, fmt, type, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
  GLenum draw = GL_COLOR_ATTACHMENT0;
  glDrawBuffers(1, &draw);
  auto status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (status != GL_FRAMEBUFFER_COMPLETE) {
    lg::error("SSAO: incomplete framebuffer 0x{:x} ({}x{})", status, w, h);
  }
}
}  // namespace

namespace Gfx {
int SsaoPresetCount() {
  return kPresetCount;
}

const char* SsaoPresetName(int index) {
  return index >= 0 && index < kPresetCount ? kPresets[index].name : "custom";
}

bool SsaoApplyPreset(int index) {
  if (index < 0 || index >= kPresetCount) {
    return false;
  }
  g_global_settings.ssao_params = kPresets[index].params;
  g_global_settings.ssao_preset = index;
  g_global_settings.ssao_enable = true;
  return true;
}
}  // namespace Gfx

/// Saves the GL state the SSAO passes touch and restores it on destruction.
struct SsaoRenderer::SavedState {
  GLboolean depth_test, blend, scissor, cull, depth_mask;
  GLboolean color_mask[4];
  GLint depth_func, blend_src_rgb, blend_dst_rgb, blend_src_a, blend_dst_a, blend_eq_rgb,
      blend_eq_a, viewport[4], program, vao, active_tex, tex2d[3], draw_fbo, read_fbo;
  SavedState() {
    depth_test = glIsEnabled(GL_DEPTH_TEST);
    blend = glIsEnabled(GL_BLEND);
    scissor = glIsEnabled(GL_SCISSOR_TEST);
    cull = glIsEnabled(GL_CULL_FACE);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);
    glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
    glGetIntegerv(GL_DEPTH_FUNC, &depth_func);
    glGetIntegerv(GL_BLEND_SRC_RGB, &blend_src_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &blend_dst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &blend_src_a);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &blend_dst_a);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &blend_eq_rgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &blend_eq_a);
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active_tex);
    for (int i = 0; i < 3; i++) {
      glActiveTexture(GL_TEXTURE0 + i);
      glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex2d[i]);
    }
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
  }
  ~SavedState() {
    auto set = [](GLenum cap, GLboolean on) { on ? glEnable(cap) : glDisable(cap); };
    set(GL_DEPTH_TEST, depth_test);
    set(GL_BLEND, blend);
    set(GL_SCISSOR_TEST, scissor);
    set(GL_CULL_FACE, cull);
    glDepthMask(depth_mask);
    glColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
    glDepthFunc(depth_func);
    glBlendFuncSeparate(blend_src_rgb, blend_dst_rgb, blend_src_a, blend_dst_a);
    glBlendEquationSeparate(blend_eq_rgb, blend_eq_a);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    glUseProgram(program);
    glBindVertexArray(vao);
    for (int i = 0; i < 3; i++) {
      glActiveTexture(GL_TEXTURE0 + i);
      glBindTexture(GL_TEXTURE_2D, tex2d[i]);
    }
    glActiveTexture(active_tex);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
  }
};

SsaoRenderer::~SsaoRenderer() {
  free_targets();
  free_normals();
  if (m_em_tex) {
    glDeleteTextures(1, &m_em_tex);
  }
  if (m_sz_tex) {
    glDeleteTextures(1, &m_sz_tex);
  }
  if (m_vao) {
    glDeleteVertexArrays(1, &m_vao);
  }
}

void SsaoRenderer::set_bucket_stencil(int bucket) {
  using jak1::BucketId;
  if (stencil_marks::deferred() && bucket <= (int)BucketId::SHRUB_GENERIC_LEVEL1) {
    reset_stencil();  // written after bucket 30 (StencilMarks.h)
    return;
  }
  const bool sky = bucket <= (int)BucketId::OCEAN_MID_AND_FAR;
  const bool bg =
      bucket >= (int)BucketId::TFRAG_TEX_LEVEL0 && bucket <= (int)BucketId::SHRUB_GENERIC_LEVEL1;
  const bool chars =
      bucket >= (int)BucketId::PRIS_TEX_LEVEL0 && bucket <= (int)BucketId::GENERIC_PRIS;
  const bool trans =
      bucket > (int)BucketId::SHRUB_GENERIC_LEVEL1 && bucket <= (int)BucketId::GENERIC_ALPHA;
  const bool only = probe(1024 | 2048 | 4096 | 8192);
  if (probe(1) || (only && !((probe(1024) && sky) || (probe(2048) && bg) ||
                             (probe(4096) && chars) || (probe(8192) && trans)))) {
    reset_stencil();
    return;
  }
  if (probe(512)) {
    apply_stencil(0, 0);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    return;
  }
  if (bucket <= (int)BucketId::OCEAN_MID_AND_FAR) {
    apply_stencil(0x80, 0x80);  // sky and far ocean: exclude
  } else if (bucket <= (int)BucketId::SHRUB_GENERIC_LEVEL1) {
    // opaque background: receive. With local lights on (the stencil shadow, which uses bits
    // 0-5, is off) bit 5 marks merc level props: merc gives them the lights the bake lacks
    const auto& gs = Gfx::g_global_settings;
    const bool mark = gs.shadow_mode != 0 && gs.shadow_lights > 0;
    const bool prop = bucket == (int)BucketId::MERC_TFRAG_TEX_LEVEL0 ||
                      bucket == (int)BucketId::MERC_TFRAG_TEX_LEVEL1;
    apply_stencil(mark && prop ? 0x20 : 0x00, mark ? 0xA0 : 0x80);
  } else if (bucket < (int)BucketId::MERC_AFTER_ALPHA) {
    // blended tfrag (trans, dirt, ice) mostly lies on opaque ground or writes its own depth
    apply_stencil(0x00, 0x80);
  } else if (bucket <= (int)BucketId::GENERIC_ALPHA) {
    apply_stencil(0x80, 0x80);  // translucent actors and effects: exclude
  } else if (bucket == (int)BucketId::SHADOW) {
    glDisable(GL_STENCIL_TEST);  // the shadow pass sets its own state, masked to bits 0-5
  } else if (bucket <= (int)BucketId::GENERIC_PRIS) {
    apply_stencil(0x40, 0xC0);  // characters: receive, and mark as dynamic
  } else if (bucket == (int)BucketId::GENERIC_PRIS + 1) {
    reset_stencil();
  }
}

void SsaoRenderer::begin_emissive(SharedRenderState* rs, bool on) {
  m_em_live = false;
  g_sz_drawing = false;
  if (!on && !g_em_fb) {
    return;
  }
  GLint prev_draw = 0, prev_read = 0;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw);
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
  if (!on) {
    {
      glBindFramebuffer(GL_FRAMEBUFFER, g_em_fb);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, 0, 0);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT4, GL_TEXTURE_2D, 0, 0);
      g_em_fb = 0;
      g_sz_live = false;
      free_sprite_depth();
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev_draw);
      glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_read);
    }
    return;
  }
  glBindFramebuffer(GL_FRAMEBUFFER, rs->render_fb);
  GLint samples = 1;
  glGetIntegerv(GL_SAMPLES, &samples);
  samples = std::max(samples, 1);
  const int w = rs->render_fb_w, h = rs->render_fb_h;
  const GLenum target = samples > 1 ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D;
  const bool mat = rs->light_material;
  if (!m_em_tex || m_em_w != w || m_em_h != h || m_em_samples != samples || m_em_mat != mat) {
    perf::AddMs hitch_t(perf::Counter::HITCH_TARGET_MS);
    if (m_em_tex) {
      glDeleteTextures(1, &m_em_tex);
    }
    glGenTextures(1, &m_em_tex);
    glBindTexture(target, m_em_tex);
    const GLint ifmt = mat ? GL_RGBA8 : GL_RG8;
    if (samples > 1) {
      glTexImage2DMultisample(target, samples, ifmt, w, h, GL_TRUE);
    } else {
      glTexImage2D(target, 0, ifmt, w, h, 0, mat ? GL_RGBA : GL_RG, GL_UNSIGNED_BYTE, nullptr);
      glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    glBindTexture(target, 0);
    m_em_w = w;
    m_em_h = h;
    m_em_samples = samples;
    m_em_mat = mat;
    g_em_fb = 0;
    lg::info("SSAO: emissive mask target {}x{}, {} samples{}", w, h, samples,
             mat ? ", highlight material" : "");
  }
  // the render framebuffer is recreated (resize, FP16 toggle) and ids are reused, so ask it
  GLint have = 0;
  glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2,
                                        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &have);
  const bool em_fb_changed = g_em_fb != (GLuint)rs->render_fb;
  if (em_fb_changed || (GLuint)have != m_em_tex) {
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, target, m_em_tex, 0);
    g_em_fb = rs->render_fb;
  }
  // sprite depth (1 / z in metres, max blend) for the tile lights: additive sprites write no depth
  // only the tile pass reads it
  const bool sz = Gfx::g_global_settings.emissive_lights_enable && !probe(64);
  if (sz && (!m_sz_tex || m_sz_w != w || m_sz_h != h || m_sz_samples != samples)) {
    if (m_sz_tex) {
      glDeleteTextures(1, &m_sz_tex);
    }
    glGenTextures(1, &m_sz_tex);
    glBindTexture(target, m_sz_tex);
    if (samples > 1) {
      glTexImage2DMultisample(target, samples, GL_R16F, w, h, GL_TRUE);
    } else {
      glTexImage2D(target, 0, GL_R16F, w, h, 0, GL_RED, GL_HALF_FLOAT, nullptr);
      glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    glBindTexture(target, 0);
    m_sz_w = w;
    m_sz_h = h;
    m_sz_samples = samples;
  }
  glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT4,
                                        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &have);
  const GLuint sz_want = sz ? m_sz_tex : 0;
  if (em_fb_changed || (GLuint)have != sz_want) {
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT4, sz ? target : GL_TEXTURE_2D,
                           sz_want, 0);
  }
  g_sz_live = sz;
  if (!sz) {
    free_sprite_depth();
  }
  const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
  glDisable(GL_SCISSOR_TEST);
  g_sz_drawing = sz;
  emissive_draw_buffers(true);
  // a = 1: no highlight where nothing writes the material
  const float zero[4] = {0, 0, 0, mat ? 1.f : 0.f};
  glClearBufferfv(GL_COLOR, 2, zero);
  if (sz) {
    const float none[4] = {0, 0, 0, 0};
    glColorMaski(4, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearBufferfv(GL_COLOR, 4, none);
  }
  emissive_draw_buffers(false);
  g_sz_drawing = false;
  if (scissor) {
    glEnable(GL_SCISSOR_TEST);
  }
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev_draw);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_read);
  m_em_live = true;
}

void SsaoRenderer::free_sprite_depth() {
  if (m_sz_tex) {
    glDeleteTextures(1, &m_sz_tex);
    m_sz_tex = 0;
    m_sz_w = m_sz_h = m_sz_samples = 0;
  }
}

void SsaoRenderer::emissive_draw_buffers(bool on) {
  emissive_draw_buffers(on, true, true);
}

void SsaoRenderer::emissive_draw_buffers(bool on, bool em, bool material) {
  on = on && !probe(16);
  if (g_color_lock) {
    return;
  }
  if (g_em_masked) {
    glColorMaski(2, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    g_em_masked = false;
  }
  if (!g_em_fb) {
    return;
  }
  // callers draw to the render framebuffer during the bucket loop
  if (!Gfx::perf_opt(kOptNoQueries)) {
    GLint fb = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fb);
    if ((GLuint)fb != g_em_fb) {
      return;
    }
  }
  // keeps draw buffers 1 and 3 as the normal and inspect targets left them
  render_fb_draw_buffers(on);
  if (on) {
    glColorMaski(2, em, em, material, material);
    g_em_masked = !em || !material;
    g_em_mask[0] = em;
    g_em_mask[1] = material;
  }
}

bool SsaoRenderer::sprite_depth_drawing(bool on) {
  g_sz_drawing = on && g_sz_live && g_em_fb;
  return g_sz_drawing;
}

const float* SsaoRenderer::depth_lin() {
  return g_lin;
}

GLuint SsaoRenderer::sprite_depth_texture() const {
  return m_em_live && g_sz_live ? m_sz_tex : 0;
}

void SsaoRenderer::material_blend_guard(GLuint program, bool blend, bool em, int& state) {
  const int s = (blend ? 1 : 0) | (em ? 2 : 0);
  if (s == state) {
    return;
  }
  state = s;
  // blended draws: a = 1 as master (it is their E's blend factor) and b, a masked
  glUniform1i(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_hl_on"), blend ? 0 : 1);
  emissive_draw_buffers(true, em, !blend);
}

bool SsaoRenderer::draw_blends(const DrawMode& mode) {
  return mode.get_ab_enable() && mode.get_alpha_blend() != DrawMode::AlphaBlend::DISABLED &&
         mode.get_alpha_blend() != DrawMode::AlphaBlend::SRC_SRC_SRC_SRC;
}

bool SsaoRenderer::blend_replaces_alpha(const DrawMode& mode) {
  return mode.get_alpha_blend() == DrawMode::AlphaBlend::SRC_DST_SRC_DST ||
         mode.get_alpha_blend() == DrawMode::AlphaBlend::SRC_0_FIX_DST;
}

void SsaoRenderer::reset_stencil() {
  glDisable(GL_STENCIL_TEST);
  glStencilFunc(GL_ALWAYS, 0, 0xFF);
  glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
  glStencilMask(0xFF);
}

bool SsaoRenderer::normals_active() {
  return g_normals_active;
}

void SsaoRenderer::set_normal_mode(GLuint program, int mode) {
  if (g_normals_active) {
    mode = probe(4) ? 0 : (probe(8) ? std::min(mode, 1) : mode);
    glUniform1i(GLQ_LOC(kOptNoQueries, program, "ssao_nrm"), mode);
    glUniform1i(GLQ_LOC(kOptNoQueries, program, "ssao_fogw"),
                Gfx::g_global_settings.ssao_fog_weight > 0.f ? 1 : 0);
  }
}

void SsaoRenderer::set_velocity_drawing(bool on) {
  if (on == g_vel_drawing) {
    return;
  }
  g_vel_drawing = on;
  // keeps buffers 0-4 and 6 as they are on the bound framebuffer
  GLenum bufs[7];
  for (int i = 0; i < 7; i++) {
    GLint b = GL_NONE;
    glGetIntegerv(GL_DRAW_BUFFER0 + i, &b);
    bufs[i] = (GLenum)b;
  }
  bufs[5] = on ? GL_COLOR_ATTACHMENT5 : GL_NONE;
  glDrawBuffers(bufs[6] != GL_NONE ? 7 : (on ? 6 : 5), bufs);
}

bool SsaoRenderer::velocity_drawing() {
  return g_vel_drawing;
}

void SsaoRenderer::color_lock(bool on) {
  if (on == g_color_lock) {
    return;
  }
  g_color_lock = on;
  if (on) {
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    return;
  }
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  if (g_em_masked) {
    glColorMaski(2, g_em_mask[0], g_em_mask[0], g_em_mask[1], g_em_mask[1]);
  }
  set_normal_write(g_nrm_write);
}

void SsaoRenderer::set_normal_write(bool write) {
  if (g_color_lock) {
    return;
  }
  g_nrm_write = write;
  if (g_normals_active && !probe(256)) {
    glColorMaski(1, write, write, write, write);
  }
  if (g_dn_active) {
    glColorMaski(6, write, write, write, write);
  }
}

void SsaoRenderer::set_bucket_normals(SharedRenderState* rs, int bucket) {
  using jak1::BucketId;
  const bool want =
      g_normals_active && !probe(2) &&
      ((bucket >= (int)BucketId::TFRAG_TEX_LEVEL0 &&
        bucket <= (int)BucketId::SHRUB_GENERIC_LEVEL1) ||
       (bucket >= (int)BucketId::PRIS_TEX_LEVEL0 && bucket <= (int)BucketId::GENERIC_PRIS));
  if (want == g_normals_drawing) {
    return;
  }
  GLint prev = 0;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, rs->render_fb);
  g_normals_drawing = want;
  render_fb_draw_buffers(false);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev);
}

void SsaoRenderer::set_inspect_drawing(SharedRenderState* rs, bool on) {
  if (on == g_insp_drawing) {
    return;
  }
  GLint prev = 0;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, rs->render_fb);
  g_insp_drawing = on;
  render_fb_draw_buffers(false);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev);
}

void SsaoRenderer::free_normals() {
  if (m_nrm) {
    glDeleteTextures(1, &m_nrm);
  }
  m_nrm = 0;
  m_nrm_w = m_nrm_h = m_nrm_samples = 0;
  if (m_dn) {
    glDeleteTextures(1, &m_dn);
  }
  m_dn = 0;
  m_dn_w = m_dn_h = m_dn_samples = 0;
}

void SsaoRenderer::begin_normals(SharedRenderState* rs) {
  if (!Gfx::g_global_settings.ssao_normal_target || probe(32)) {
    end_normals(rs);
    return;
  }
  GLint prev = 0;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, rs->render_fb);
  GLint samples = 1;
  glGetIntegerv(GL_SAMPLES, &samples);
  samples = std::max(samples, 1);
  const int w = rs->render_fb_w, h = rs->render_fb_h;
  if (m_nrm_w != w || m_nrm_h != h || m_nrm_samples != samples) {
    perf::AddMs hitch_t(perf::Counter::HITCH_TARGET_MS);
    free_normals();
    glGenTextures(1, &m_nrm);
    if (samples > 1) {
      glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, m_nrm);
      glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, samples, GL_RGB10_A2, w, h, GL_TRUE);
      glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);
    } else {
      glBindTexture(GL_TEXTURE_2D, m_nrm);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB10_A2, w, h, 0, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV,
                   nullptr);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
      glBindTexture(GL_TEXTURE_2D, 0);
    }
    m_nrm_w = w;
    m_nrm_h = h;
    m_nrm_samples = samples;
    lg::info("SSAO: allocated normal target {}x{}, {} samples", w, h, samples);
  }
  // the render framebuffer is recreated on resize and ids are reused, so ask it
  GLint have = 0;
  glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT1,
                                        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &have);
  if ((GLuint)have != m_nrm) {
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT1,
                           samples > 1 ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D, m_nrm, 0);
    auto status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
      lg::error("SSAO: normal target leaves the framebuffer incomplete (0x{:x}), disabled", status);
      glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, 0, 0);
      Gfx::g_global_settings.ssao_normal_target = false;
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev);
      g_normals_active = false;
      return;
    }
  }
  m_nrm_attached = true;

  const auto& gs = Gfx::g_global_settings;
  bool dn = gs.emissive_lights_enable && pbr_active(rs) && gs.pbr_background;
  if (dn && (m_dn_w != w || m_dn_h != h || m_dn_samples != samples)) {
    if (m_dn) {
      glDeleteTextures(1, &m_dn);
    }
    glGenTextures(1, &m_dn);
    if (samples > 1) {
      glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, m_dn);
      glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, samples, GL_RG8, w, h, GL_TRUE);
      glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);
    } else {
      glBindTexture(GL_TEXTURE_2D, m_dn);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, w, h, 0, GL_RG, GL_UNSIGNED_BYTE, nullptr);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
      glBindTexture(GL_TEXTURE_2D, 0);
    }
    m_dn_w = w;
    m_dn_h = h;
    m_dn_samples = samples;
    lg::info("SSAO: allocated detail normal target {}x{}, {} samples", w, h, samples);
  }
  glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT6,
                                        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &have);
  const GLuint dn_want = dn ? m_dn : 0;
  if ((GLuint)have != dn_want) {
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT6,
                           dn && samples > 1 ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D, dn_want,
                           0);
    auto status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    if (dn && status != GL_FRAMEBUFFER_COMPLETE) {
      lg::error("SSAO: detail normal target leaves the framebuffer incomplete (0x{:x}), skipped",
                status);
      glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT6, GL_TEXTURE_2D, 0, 0);
      dn = false;
    }
  }
  if (!dn && m_dn) {
    glDeleteTextures(1, &m_dn);
    m_dn = 0;
    m_dn_w = m_dn_h = m_dn_samples = 0;
  }
  m_dn_attached = dn;

  const GLenum both[7] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_NONE, GL_NONE,
                          GL_NONE,              GL_NONE,              GL_COLOR_ATTACHMENT6};
  glDrawBuffers(dn ? 7 : 2, both);
  const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
  glDisable(GL_SCISSOR_TEST);
  glColorMaski(1, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  const float zero[4] = {0.f, 0.f, 0.f, 0.f};
  glClearBufferfv(GL_COLOR, 1, zero);
  if (dn) {
    glColorMaski(6, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    const float flat[4] = {128.f / 255.f, 128.f / 255.f, 0.f, 0.f};
    glClearBufferfv(GL_COLOR, 6, flat);
  }
  if (scissor) {
    glEnable(GL_SCISSOR_TEST);
  }
  const GLenum first[2] = {GL_COLOR_ATTACHMENT0, GL_NONE};
  glDrawBuffers(2, first);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev);
  g_normals_active = true;
  g_normals_drawing = false;
  g_dn_active = dn;
}

void SsaoRenderer::end_normals(SharedRenderState* rs) {
  g_normals_active = false;
  g_normals_drawing = false;
  g_dn_active = false;
  if (!m_nrm_attached) {
    return;
  }
  GLint prev = 0, prog = 0;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev);
  glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, rs->render_fb);
  glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, 0, 0);
  if (m_dn_attached) {
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT6, GL_TEXTURE_2D, 0, 0);
    m_dn_attached = false;
  }
  if (m_dn) {
    glDeleteTextures(1, &m_dn);
    m_dn = 0;
    m_dn_w = m_dn_h = m_dn_samples = 0;
  }
  const GLenum first = GL_COLOR_ATTACHMENT0;
  glDrawBuffers(1, &first);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev);
  for (auto id : kNormalShaders) {
    auto& sh = rs->shaders[id];
    sh.activate();
    glUniform1i(GLQ_LOC(kOptNoQueries, sh.id(), "ssao_nrm"), 0);
  }
  glUseProgram(prog);
  free_normals();
  m_nrm_attached = false;
}

void SsaoRenderer::bind_normals(GLuint prog, int ss_unit, int ms_unit) {
  const int n = g_normals_active ? m_nrm_samples : 0;
  glUniform1i(GLQ_LOC(kOptNoQueries, prog, "nrm_ss"), ss_unit);
  glUniform1i(GLQ_LOC(kOptNoQueries, prog, "nrm_ms"), ms_unit);
  glUniform1i(GLQ_LOC(kOptNoQueries, prog, "nrm_samples"), n);
  glUniform2f(GLQ_LOC(kOptNoQueries, prog, "nrm_scale"), m_nrm_scale[0], m_nrm_scale[1]);
  glActiveTexture(GL_TEXTURE0 + ss_unit);
  glBindTexture(GL_TEXTURE_2D, n == 1 ? m_nrm : 0);
  glActiveTexture(GL_TEXTURE0 + ms_unit);
  glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, n > 1 ? m_nrm : 0);
}

void SsaoRenderer::set_detail_uniforms(SharedRenderState* rs, GLuint program) {
  glUniform1i(GLQ_LOC(kOptNoQueries, program, "ssao_dn"), g_dn_active ? 1 : 0);
  if (!g_dn_active) {
    return;
  }
  float sg[3], m[9];
  emissive_view_signs(rs, sg);
  for (int k = 0; k < 3; k++) {
    for (int i = 0; i < 3; i++) {
      m[k * 3 + i] = sg[k] * rs->camera_rot[i][k];
    }
  }
  glUniformMatrix3fv(GLQ_LOC(kOptNoQueries, program, "ssao_dn_rot"), 1, GL_TRUE, m);
}

void SsaoRenderer::bind_detail(GLuint prog, int ss_unit, int ms_unit) {
  const int n = g_dn_active && m_dn_attached ? m_dn_samples : 0;
  glUniform1i(GLQ_LOC(kOptNoQueries, prog, "dn_ss"), ss_unit);
  glUniform1i(GLQ_LOC(kOptNoQueries, prog, "dn_ms"), ms_unit);
  glUniform1i(GLQ_LOC(kOptNoQueries, prog, "dn_on"), n > 0 && n == m_nrm_samples ? 1 : 0);
  glActiveTexture(GL_TEXTURE0 + ss_unit);
  glBindTexture(GL_TEXTURE_2D, n == 1 ? m_dn : 0);
  glActiveTexture(GL_TEXTURE0 + ms_unit);
  glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, n > 1 ? m_dn : 0);
}

void SsaoRenderer::free_targets() {
  auto del = [](Tex& t) {
    if (t.fbo) {
      glDeleteFramebuffers(1, &t.fbo);
    }
    if (t.tex) {
      glDeleteTextures(1, &t.tex);
    }
    t = Tex();
  };
  free_depth_copy();
  for (auto& t : m_ao) {
    del(t);
  }
  del(m_color);
  del(m_ex);
  m_ex_depth = 0;
  if (m_csz) {
    glDeleteTextures(1, &m_csz);
    glDeleteFramebuffers(kMips, m_csz_fbo);
  }
  m_csz = 0;
  std::fill(std::begin(m_csz_fbo), std::end(m_csz_fbo), 0);
  m_w = m_h = m_samples = 0;
}

void SsaoRenderer::ensure_targets(int w, int h, int samples) {
  if (m_w == w && m_h == h && m_samples == samples) {
    return;
  }
  perf::AddMs hitch_t(perf::Counter::HITCH_TARGET_MS);
  free_targets();
  m_w = w;
  m_h = h;
  m_samples = samples;

  glGenTextures(1, &m_csz);
  glBindTexture(GL_TEXTURE_2D, m_csz);
  for (int i = 0; i < kMips; i++) {
    glTexImage2D(GL_TEXTURE_2D, i, GL_R32F, std::max(1, w >> i), std::max(1, h >> i), 0, GL_RED,
                 GL_FLOAT, nullptr);
  }
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, kMips - 1);
  glGenFramebuffers(kMips, m_csz_fbo);
  for (int i = 0; i < kMips; i++) {
    glBindFramebuffer(GL_FRAMEBUFFER, m_csz_fbo[i]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_csz, i);
    GLenum draw = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &draw);
  }

  const int hw = std::max(1, w >> 1), hh = std::max(1, h >> 1);
  for (auto& t : m_ao) {
    make_color_target(t.fbo, t.tex, hw, hh, GL_R16F, GL_RED, GL_FLOAT);
  }
  m_ao_w = hw;
  m_ao_h = hh;
  make_color_target(m_color.fbo, m_color.tex, w, h, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
  glBindTexture(GL_TEXTURE_2D, 0);
  lg::info("SSAO: allocated targets {}x{}, {} samples", w, h, samples);
}

void SsaoRenderer::free_depth_copy() {
  if (m_depth.fbo) {
    glDeleteFramebuffers(1, &m_depth.fbo);
  }
  if (m_depth.tex) {
    glDeleteTextures(1, &m_depth.tex);
  }
  m_depth = Tex();
}

void SsaoRenderer::ensure_depth_copy(int w, int h, int samples) {
  if (m_depth.fbo) {
    return;
  }
  glGenFramebuffers(1, &m_depth.fbo);
  glGenTextures(1, &m_depth.tex);
  glBindFramebuffer(GL_FRAMEBUFFER, m_depth.fbo);
  if (samples > 1) {
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, m_depth.tex);
    glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, samples, GL_DEPTH24_STENCIL8, w, h, GL_TRUE);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D_MULTISAMPLE,
                           m_depth.tex, 0);
  } else {
    glBindTexture(GL_TEXTURE_2D, m_depth.tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, w, h, 0, GL_DEPTH_STENCIL,
                 GL_UNSIGNED_INT_24_8, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, m_depth.tex,
                           0);
  }
  GLenum none = GL_NONE;
  glDrawBuffers(1, &none);
  glReadBuffer(GL_NONE);
}

void SsaoRenderer::fullscreen() {
  glBindVertexArray(m_vao);
  glDrawArrays(GL_TRIANGLES, 0, 3);
}

void SsaoRenderer::set_proj_uniforms(GLuint prog) {
  glUniform4f(GLQ_LOC(kOptNoQueries, prog, "proj_info"), m_proj_info[0], m_proj_info[1],
              m_proj_info[2], m_proj_info[3]);
  glUniform1f(GLQ_LOC(kOptNoQueries, prog, "far_m"), kFarM);
}

bool SsaoRenderer::cave_level_loaded(SharedRenderState* rs) {
  if (!rs->loader) {
    return false;
  }
  for (auto* lev : rs->loader->get_in_use_levels()) {
    if (!lev || !lev->level) {
      continue;
    }
    for (const char* name : kCaveLevels) {
      if (lev->level->level_name == name) {
        return true;
      }
    }
  }
  return false;
}

void SsaoRenderer::render(SharedRenderState* rs, ScopedProfilerNode& prof, bool ao) {
  const auto& gs = Gfx::g_global_settings;
  const auto& p = gs.ssao_params;
  // the per-frame noise converges in the AO history (motion vectors); without them (stereo, XR,
  // upstream rendering) a fixed pattern and more denoise passes
  const bool accum = TemporalAccum::available(rs);
  m_accum_on = accum;
  if (accum == m_fixed_noise_logged) {
    if (!accum) {
      lg::warn("SSAO: no motion vectors; GTAO runs a fixed noise pattern");
    }
    m_fixed_noise_logged = !accum;
  }
  const bool timing = gs.ssao_gpu_timing;
  m_have_frame = false;
  m_ao_frame = false;
  // camera_persp comes from the frame's first tfrag packet; without one (menus) skip SSAO
  if (!rs->has_pc_data) {
    reset_stencil();
    return;
  }
  const auto* persp = rs->camera_persp;
  const float fog = rs->camera_fog.x();
  const float pzw = std::abs(persp[2][3]);
  // projection: see make_new_cam_mat and the SCISSOR_ADJUST scale of the background shaders
  const float pxx = std::abs(fog * persp[0][0] / 256.f);
  const float pyy = std::abs(fog * persp[1][1] / 128.f * (512.f / 448.f));
  if (pxx <= 0.f || pyy <= 0.f || pzw <= 0.f) {
    reset_stencil();
    return;
  }
  const float lin[3] = {
      fog * persp[3][2] / 8388608.f, persp[2][3],
      fog * persp[2][2] / 8388608.f + (rs->camera_hvdf_off.z() / 8388608.f - 1.f) * persp[2][3]};
  std::copy(lin, lin + 3, g_lin);

  // AO reads the normal target from here on; nothing after bucket 56 writes it
  set_bucket_normals(rs, (int)jak1::BucketId::GENERIC_PRIS + 1);
  m_nrm_scale[0] = pxx / pzw;
  m_nrm_scale[1] = pyy / pzw;

  SavedState saved;
  if (timing) {
    m_t_total.begin();
  }
  const int w = rs->render_fb_w;
  const int h = rs->render_fb_h;
  GLint samples = 1;
  glBindFramebuffer(GL_FRAMEBUFFER, rs->render_fb);
  glGetIntegerv(GL_SAMPLES, &samples);
  samples = std::max(samples, 1);
  ensure_targets(w, h, samples);
  if (!m_vao) {
    glGenVertexArrays(1, &m_vao);
  }
  m_proj_info[0] = 2.f * pzw / (w * pxx);
  m_proj_info[1] = 2.f * pzw / (h * pyy);
  m_proj_info[2] = -pzw / pxx;
  m_proj_info[3] = -pzw / pyy;

  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);
  glDisable(GL_STENCIL_TEST);
  glDepthMask(GL_FALSE);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

  std::optional<perf::Scope> ps;
  ps.emplace(perf::Group::SSAO_DEPTH_LIN);
  // depth copy after the character buckets
  if (timing) {
    m_t_copy.begin();
  }
  // the render framebuffer is not bound while linearize samples its depth, so no feedback loop
  GLuint depth_tex = Gfx::perf_opt(kOptDirectDepth) ? rs->render_fb_depth_tex : 0;
  if (depth_tex) {
    free_depth_copy();
  } else {
    ensure_depth_copy(w, h, samples);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, rs->render_fb);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_depth.fbo);
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    depth_tex = m_depth.tex;
  }
  if (timing) {
    m_t_copy.end();
  }

  // linearize + minify: linear z in metres (farthest sample), then the z mip chain
  if (timing) {
    m_t_linearize.begin();
  }
  {
    glBindFramebuffer(GL_FRAMEBUFFER, m_csz_fbo[0]);
    glViewport(0, 0, w, h);
    auto& sh = rs->shaders[ShaderId::SSAO_LINEARIZE];
    sh.activate();
    glUniform1i(GLQ_LOC(kOptNoQueries, sh.id(), "depth_ss"), 0);
    glUniform1i(GLQ_LOC(kOptNoQueries, sh.id(), "depth_ms"), 1);
    glUniform1i(GLQ_LOC(kOptNoQueries, sh.id(), "samples"), samples);
    glUniform3f(GLQ_LOC(kOptNoQueries, sh.id(), "lin"), lin[0], lin[1], lin[2]);
    glUniform1f(GLQ_LOC(kOptNoQueries, sh.id(), "far_m"), kFarM);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, samples > 1 ? 0 : depth_tex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, samples > 1 ? depth_tex : 0);
    fullscreen();
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);

    auto& mn = rs->shaders[ShaderId::SSAO_MINIFY];
    mn.activate();
    glUniform1i(GLQ_LOC(kOptNoQueries, mn.id(), "src"), 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_csz);
    for (int i = 1; i < kMips; i++) {
      // sample only level i-1 while writing level i
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, i - 1);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, i - 1);
      glBindFramebuffer(GL_FRAMEBUFFER, m_csz_fbo[i]);
      glViewport(0, 0, std::max(1, w >> i), std::max(1, h >> i));
      fullscreen();
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, kMips - 1);
  }
  if (timing) {
    m_t_linearize.end();
  }
  if (!ao) {
    // inputs only (tile lights): no AO, so no history to keep
    m_accum[rs->eye_slot & 1].reset();
    if (timing) {
      m_t_total.end();
    }
    reset_stencil();
    m_have_frame = true;
    return;
  }

  const int hw = std::max(1, w >> 1), hh = std::max(1, h >> 1);
  const float radius = std::max(p.radius_m, 0.01f);

  const auto& gt = gs.ssao_gtao;
  m_noise_frame++;
  m_ao_full = p.gtao_full_res != 0;
  const int aw = m_ao_full ? w : hw, ah = m_ao_full ? h : hh;
  if (aw != m_ao_w || ah != m_ao_h) {
    for (auto& t : m_ao) {
      glDeleteFramebuffers(1, &t.fbo);
      glDeleteTextures(1, &t.tex);
      make_color_target(t.fbo, t.tex, aw, ah, GL_R16F, GL_RED, GL_FLOAT);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    m_ao_w = aw;
    m_ao_h = ah;
  }

  // exclude mask: stencil bit 7 (sky, far ocean, translucent actors) per sample, so the AO pass
  // skips texels the composite discards
  const bool ex = make_exclude_mask(rs, w, h, samples);

  // ao-raw: XeGTAO at half or full resolution
  ps.emplace(perf::Group::SSAO_AO);
  if (timing) {
    m_t_ao.begin();
  }
  {
    glBindFramebuffer(GL_FRAMEBUFFER, m_ao[0].fbo);
    glViewport(0, 0, aw, ah);
    auto& sh = rs->shaders[ShaderId::SSAO_AO];
    sh.activate();
    const GLuint id = sh.id();
    set_proj_uniforms(id);
    glUniform1i(GLQ_LOC(kOptNoQueries, id, "csz"), 0);
    glUniform1f(GLQ_LOC(kOptNoQueries, id, "radius"), radius);
    glUniform1f(GLQ_LOC(kOptNoQueries, id, "radius_mul"), gt.radius_mul);
    glUniform1f(GLQ_LOC(kOptNoQueries, id, "falloff_range"),
                std::clamp(gt.falloff_range, 0.01f, 1.f));
    glUniform1f(GLQ_LOC(kOptNoQueries, id, "sample_power"), gt.sample_power);
    glUniform1f(GLQ_LOC(kOptNoQueries, id, "thin_comp"), std::clamp(p.gtao_thin, 0.f, 1.f));
    glUniform1f(GLQ_LOC(kOptNoQueries, id, "mip_offset"), gt.mip_offset);
    glUniform1i(GLQ_LOC(kOptNoQueries, id, "slices"), std::clamp(p.gtao_slices, 1, 9));
    glUniform1i(GLQ_LOC(kOptNoQueries, id, "steps"), std::clamp(p.gtao_steps, 1, 8));
    glUniform1i(GLQ_LOC(kOptNoQueries, id, "full_res"), m_ao_full ? 1 : 0);
    glUniform1i(GLQ_LOC(kOptNoQueries, id, "ex_ss"), 4);
    glUniform1i(GLQ_LOC(kOptNoQueries, id, "ex_ms"), 5);
    glUniform1i(GLQ_LOC(kOptNoQueries, id, "ex_samples"), ex ? samples : 0);
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, ex && samples == 1 ? m_ex.tex : 0);
    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, ex && samples > 1 ? m_ex.tex : 0);
    // stereo: the display frame's index, the same pattern in both eyes
    const u64 noise = rs->stereo_active ? rs->display_tick : m_noise_frame;
    glUniform1i(GLQ_LOC(kOptNoQueries, id, "noise_index"), accum ? (int)(noise % 64) : 0);
    bind_normals(id, 2, 3);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_csz);
    fullscreen();
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);
  }
  if (timing) {
    m_t_ao.end();
  }

  // ao-denoise: the AO history, then XeGTAO's edge-aware passes
  ps.emplace(perf::Group::SSAO_BLUR);
  if (timing) {
    m_t_blur.begin();
  }
  m_ao_result = 0;
  GLuint denoise_src = m_ao[0].tex;
  if (accum && gt.history) {
    TemporalAccum::Inputs in;
    in.signal = m_ao[0].tex;
    in.full_w = w;
    in.full_h = h;
    in.half_res = !m_ao_full;
    in.depth = depth_tex;
    in.samples = samples;
    in.dyn_mask = ex ? m_ex.tex : 0;
    in.blend = gt.history_blend;
    in.blend_dynamic = gt.history_blend_dynamic;
    in.clamp = gt.history_clamp;
    in.gamma = gt.history_gamma;
    const int slot = rs->eye_slot & 1;
    if (m_accum_full[slot] != m_ao_full) {
      m_accum[slot].reset();
      m_accum_full[slot] = m_ao_full;
    }
    if (const GLuint acc = m_accum[slot].run(rs, in)) {
      denoise_src = acc;
    }
  } else {
    m_accum[rs->eye_slot & 1].reset();
  }
  const int passes = accum ? std::clamp(gt.denoise, 1, 3) : 3;
  {
    auto& sh = rs->shaders[ShaderId::SSAO_BLUR];
    sh.activate();
    glUniform1i(GLQ_LOC(kOptNoQueries, sh.id(), "source"), 0);
    glUniform1i(GLQ_LOC(kOptNoQueries, sh.id(), "csz"), 1);
    glUniform1f(GLQ_LOC(kOptNoQueries, sh.id(), "far_m"), kFarM);
    glUniform1i(GLQ_LOC(kOptNoQueries, sh.id(), "z_mip"), m_ao_full ? 0 : 1);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_csz);
    glViewport(0, 0, aw, ah);
    int src = 0;
    for (int pass = 0; pass < passes; pass++) {
      const int dst = src == 1 ? 2 : 1;
      const bool last = pass == passes - 1;
      glUniform1f(GLQ_LOC(kOptNoQueries, sh.id(), "blur_beta"),
                  last ? gt.blur_beta : gt.blur_beta / 5.f);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, pass == 0 ? denoise_src : m_ao[src].tex);
      glBindFramebuffer(GL_FRAMEBUFFER, m_ao[dst].fbo);
      fullscreen();
      src = dst;
    }
    m_ao_result = src;
  }
  if (timing) {
    m_t_blur.end();
  }

  // composite: upsample and multiply; background and characters each get their own strength
  ps.emplace(perf::Group::SSAO_COMPOSITE);
  if (timing) {
    m_t_composite.begin();
  }
  {
    const bool cave = cave_level_loaded(rs);
    const float scale = cave ? p.cave_scale : 1.f;
    const bool use_floor = cave && p.luma_floor > 0.f;
    if (use_floor) {
      glBindFramebuffer(GL_READ_FRAMEBUFFER, rs->render_fb);
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_color.fbo);
      glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, rs->render_fb);
    glViewport(0, 0, w, h);
    auto& sh = rs->shaders[ShaderId::SSAO_COMPOSITE];
    sh.activate();
    glUniform1i(GLQ_LOC(kOptNoQueries, sh.id(), "ao"), 0);
    glUniform1i(GLQ_LOC(kOptNoQueries, sh.id(), "csz"), 1);
    glUniform1i(GLQ_LOC(kOptNoQueries, sh.id(), "color_copy"), 2);
    glUniform1i(GLQ_LOC(kOptNoQueries, sh.id(), "use_floor"), use_floor ? 1 : 0);
    glUniform1i(GLQ_LOC(kOptNoQueries, sh.id(), "ao_full"), m_ao_full ? 1 : 0);
    glUniform1f(GLQ_LOC(kOptNoQueries, sh.id(), "luma_floor"), p.luma_floor / 255.f);
    const bool fog_w = gs.ssao_fog_weight > 0.f;
    const bool mist = fog_w && swamp_mist_uniforms(rs, sh.id());
    glUniform1i(GLQ_LOC(kOptNoQueries, sh.id(), "fog_w"), fog_w ? 1 : 0);
    glUniform1f(GLQ_LOC(kOptNoQueries, sh.id(), "fog_k"), gs.ssao_fog_weight);
    glUniform1i(GLQ_LOC(kOptNoQueries, sh.id(), "mist_on"), mist ? 1 : 0);
    if (mist) {
      set_proj_uniforms(sh.id());
    }
    bind_normals(sh.id(), 3, 4);
    bind_emissive(sh.id());
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_ao[m_ao_result].tex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_csz);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, use_floor ? m_color.tex : 0);

    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFuncSeparate(GL_DST_COLOR, GL_ZERO, GL_ZERO, GL_ONE);
    // PS2 blending reads destination alpha, so AO never writes it
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    const float cap = std::clamp(gs.ssao_gtao.max_darkening, 0.f, 1.f);
    const float strengths[2] = {std::clamp(p.intensity * scale, 0.f, cap),
                                std::clamp(p.intensity * p.merc_weight * scale, 0.f, cap)};
    for (int k = 0; k < 2; k++) {
      glStencilFunc(GL_EQUAL, k == 0 ? 0x00 : 0x40, 0xC0);
      glUniform1f(GLQ_LOC(kOptNoQueries, sh.id(), "strength"), strengths[k]);
      glUniform1f(GLQ_LOC(kOptNoQueries, sh.id(), "response"), gs.ssao_gtao.response);
      // characters draw over the mask without clearing it
      glUniform1i(GLQ_LOC(kOptNoQueries, sh.id(), "em_on"),
                  k == 0 && m_em_live && Gfx::g_global_settings.ssao_emissive ? 1 : 0);
      fullscreen();
    }
    prof.add_draw_call(2);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);
    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE6);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);
  }
  if (timing) {
    m_t_composite.end();
    m_t_total.end();
  }
  reset_stencil();
  m_have_frame = true;
  m_ao_frame = true;
}

bool SsaoRenderer::make_exclude_mask(SharedRenderState* rs, int w, int h, int samples) {
  const GLuint ds = rs->render_fb_depth_tex;
  if (!ds) {
    return false;
  }
  const GLenum target = samples > 1 ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D;
  if (!m_ex.fbo) {
    glGenTextures(1, &m_ex.tex);
    glBindTexture(target, m_ex.tex);
    if (samples > 1) {
      glTexImage2DMultisample(target, samples, GL_R8, w, h, GL_TRUE);
    } else {
      glTexImage2D(target, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
      glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    glBindTexture(target, 0);
    glGenFramebuffers(1, &m_ex.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_ex.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, target, m_ex.tex, 0);
    GLenum draw = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &draw);
    m_ex_depth = 0;
  }
  glBindFramebuffer(GL_FRAMEBUFFER, m_ex.fbo);
  {
    // re-attach every frame: GL reuses texture names, so a cached name can be a dead texture
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, target, ds, 0);
    m_ex_depth = ds;
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      lg::warn("SSAO: exclude mask framebuffer incomplete; AO runs on every texel");
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, target, 0, 0);
      m_ex_depth = 0;
      return false;
    }
  }
  glViewport(0, 0, w, h);
  const float zero[4] = {0, 0, 0, 0};
  glClearBufferfv(GL_COLOR, 0, zero);
  glEnable(GL_STENCIL_TEST);
  glStencilMask(0);
  glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
  auto& sh = rs->shaders[ShaderId::SSAO_VIEW];
  sh.activate();
  glUniform1i(GLQ_LOC(kOptNoQueries, sh.id(), "mode"), 5);
  // characters (bit 6) 0.5 for the AO history, excluded (bit 7) 1
  glUniform4f(GLQ_LOC(kOptNoQueries, sh.id(), "flat_color"), 0.5f, 0.5f, 0.5f, 1.f);
  glStencilFunc(GL_EQUAL, 0x40, 0xC0);
  fullscreen();
  glUniform4f(GLQ_LOC(kOptNoQueries, sh.id(), "flat_color"), 1.f, 1.f, 1.f, 1.f);
  glStencilFunc(GL_EQUAL, 0x80, 0x80);
  fullscreen();
  glDisable(GL_STENCIL_TEST);
  return true;
}

void SsaoRenderer::bind_view_inputs(GLuint prog, int csz_unit, int nrm_ss_unit, int nrm_ms_unit) {
  set_proj_uniforms(prog);
  glUniform1i(GLQ_LOC(kOptNoQueries, prog, "csz"), csz_unit);
  bind_normals(prog, nrm_ss_unit, nrm_ms_unit);
  glActiveTexture(GL_TEXTURE0 + csz_unit);
  glBindTexture(GL_TEXTURE_2D, m_csz);
}

void SsaoRenderer::bind_emissive(GLuint prog) {
  const auto& gs = Gfx::g_global_settings;
  glUniform1i(GLQ_LOC(kOptNoQueries, prog, "em_ss"), 5);
  glUniform1i(GLQ_LOC(kOptNoQueries, prog, "em_ms"), 6);
  glUniform1i(GLQ_LOC(kOptNoQueries, prog, "em_samples"), m_em_samples);
  const bool on = m_em_live && gs.ssao_emissive;
  glUniform1i(GLQ_LOC(kOptNoQueries, prog, "em_on"), on ? 1 : 0);
  glUniform2f(GLQ_LOC(kOptNoQueries, prog, "em_ramp"), gs.ssao_emissive_lo,
              std::max(gs.ssao_emissive_hi, gs.ssao_emissive_lo + 1e-3f));
  glActiveTexture(GL_TEXTURE5);
  glBindTexture(GL_TEXTURE_2D, m_em_live && m_em_samples <= 1 ? m_em_tex : 0);
  glActiveTexture(GL_TEXTURE6);
  glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, m_em_live && m_em_samples > 1 ? m_em_tex : 0);
  glActiveTexture(GL_TEXTURE0);
}

void SsaoRenderer::draw_view(SharedRenderState* rs, ScopedProfilerNode& prof) {
  const int view = Gfx::g_global_settings.ssao_view;
  if (view == NONE || !m_have_frame || (view != EMISSIVE && !m_ao_frame)) {
    return;
  }
  if (view == EMISSIVE) {
    SavedState saved;
    glBindFramebuffer(GL_FRAMEBUFFER, rs->render_fb);
    glViewport(0, 0, rs->render_fb_w, rs->render_fb_h);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    // E as grey level over the scene dimmed to 25%
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_SRC_ALPHA);
    auto& sh = rs->shaders[ShaderId::SSAO_VIEW];
    sh.activate();
    glUniform1i(GLQ_LOC(kOptNoQueries, sh.id(), "mode"), (int)EMISSIVE);
    bind_emissive(sh.id());
    fullscreen();
    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE6);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);
    glActiveTexture(GL_TEXTURE0);
    prof.add_draw_call();
    return;
  }
  SavedState saved;
  glBindFramebuffer(GL_FRAMEBUFFER, rs->render_fb);
  glViewport(0, 0, rs->render_fb_w, rs->render_fb_h);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);
  glDisable(GL_STENCIL_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  const bool nrm_view = view == NORMALS || view == NORMAL_SOURCE || view == FOG_T;
  if (nrm_view && m_nrm_attached) {
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, 0, 0);
  }
  auto& sh = rs->shaders[ShaderId::SSAO_VIEW];
  sh.activate();
  set_proj_uniforms(sh.id());
  const bool mist = view == FOG_T && swamp_mist_uniforms(rs, sh.id());
  glUniform1i(GLQ_LOC(kOptNoQueries, sh.id(), "mist_on"), mist ? 1 : 0);
  bind_normals(sh.id(), 2, 3);
  glUniform1i(GLQ_LOC(kOptNoQueries, sh.id(), "ao"), 0);
  glUniform1i(GLQ_LOC(kOptNoQueries, sh.id(), "csz"), 1);
  glUniform1f(GLQ_LOC(kOptNoQueries, sh.id(), "view_far_m"), m_view_far_m);
  glUniform1i(GLQ_LOC(kOptNoQueries, sh.id(), "mode"), view == STENCIL ? (int)DEPTH_56 : view);
  glUniform1i(GLQ_LOC(kOptNoQueries, sh.id(), "ao_full"), m_ao_full ? 1 : 0);
  glUniform1f(GLQ_LOC(kOptNoQueries, sh.id(), "response"),
              Gfx::g_global_settings.ssao_gtao.response);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, m_ao[view == RAW_AO ? 0 : m_ao_result].tex);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, m_csz);
  fullscreen();
  glActiveTexture(GL_TEXTURE3);
  glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);
  if (nrm_view && m_nrm_attached) {
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT1,
                           m_nrm_samples > 1 ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D, m_nrm, 0);
  }
  if (view == STENCIL) {
    // magenta: excluded (bit 7), green: characters (bit 6)
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glUniform1i(GLQ_LOC(kOptNoQueries, sh.id(), "mode"), 5);
    glUniform4f(GLQ_LOC(kOptNoQueries, sh.id(), "flat_color"), 1.f, 0.f, 1.f, 1.f);
    glStencilFunc(GL_EQUAL, 0x80, 0x80);
    fullscreen();
    glUniform4f(GLQ_LOC(kOptNoQueries, sh.id(), "flat_color"), 0.f, 1.f, 0.f, 1.f);
    glStencilFunc(GL_EQUAL, 0x40, 0xC0);
    fullscreen();
    reset_stencil();
  }
  prof.add_draw_call();
}

void SsaoRenderer::draw_debug_window() {
  auto& s = Gfx::g_global_settings;
  auto& p = s.ssao_params;
  ImGui::Checkbox("Enable SSAO", &s.ssao_enable);
  if (ImGui::BeginCombo("Preset", Gfx::SsaoPresetName(s.ssao_preset))) {
    for (int i = 0; i < kPresetCount; i++) {
      if (ImGui::Selectable(kPresets[i].name, s.ssao_preset == i)) {
        Gfx::SsaoApplyPreset(i);
      }
    }
    ImGui::EndCombo();
  }
  if (ImGui::TreeNode("XeGTAO constants (dev)")) {
    auto& g = s.ssao_gtao;
    ImGui::Text("%d slices x %d steps, %s resolution, noise %s", p.gtao_slices, p.gtao_steps,
                p.gtao_full_res ? "full" : "half", m_accum_on ? "per frame + history" : "fixed");
    ImGui::SliderInt("denoise passes after the history", &g.denoise, 1, 3);
    ImGui::Checkbox("AO history (off: the temporal AA alone)", &g.history);
    ImGui::Checkbox("history clamped to this frame's 3x3 range", &g.history_clamp);
    ImGui::SliderFloat("clamp box std devs (0: min/max)", &g.history_gamma, 0.f, 3.f);
    ImGui::SliderFloat("history blend", &g.history_blend, 0.f, 0.98f);
    ImGui::SliderFloat("history blend, characters", &g.history_blend_dynamic, 0.f, 0.98f);
    ImGui::SliderFloat("radius multiplier", &g.radius_mul, 0.3f, 3.f);
    ImGui::SliderFloat("falloff range", &g.falloff_range, 0.01f, 1.f);
    ImGui::SliderFloat("sample distribution power", &g.sample_power, 1.f, 3.f);
    ImGui::SliderFloat("occlusion response power", &g.response, 0.25f, 2.f);
    ImGui::SliderFloat("max darkening", &g.max_darkening, 0.f, 1.f);
    ImGui::SliderFloat("depth mip offset", &g.mip_offset, 0.f, 6.f);
    ImGui::SliderFloat("denoise blur beta", &g.blur_beta, 0.1f, 8.f);
    ImGui::TreePop();
  }
  ImGui::Text("View");
  ImGui::RadioButton("Final", &s.ssao_view, NONE);
  ImGui::SameLine();
  ImGui::RadioButton("Raw AO", &s.ssao_view, RAW_AO);
  ImGui::SameLine();
  ImGui::RadioButton("Blurred AO", &s.ssao_view, BLURRED_AO);
  ImGui::RadioButton("Normals", &s.ssao_view, NORMALS);
  ImGui::SameLine();
  ImGui::RadioButton("Depth after 56", &s.ssao_view, DEPTH_56);
  ImGui::SameLine();
  ImGui::RadioButton("Stencil mask", &s.ssao_view, STENCIL);
  ImGui::RadioButton("Depth normals", &s.ssao_view, DEPTH_NORMALS);
  ImGui::SameLine();
  ImGui::RadioButton("Normal source", &s.ssao_view, NORMAL_SOURCE);
  ImGui::Checkbox("Normal target (off: normals from depth)", &s.ssao_normal_target);
  ImGui::RadioButton("Emissive E", &s.ssao_view, EMISSIVE);
  ImGui::SameLine();
  ImGui::RadioButton("Fog transmission", &s.ssao_view, FOG_T);
  ImGui::SliderFloat("AO fade with fog and mist (0 off, exponent)", &s.ssao_fog_weight, 0.f, 3.f);
  ImGui::Checkbox("Emissive mask (needs .unbake v2)", &s.ssao_emissive);
  ImGui::SliderFloat("E ramp start", &s.ssao_emissive_lo, 0.f, 1.f);
  ImGui::SliderFloat("E ramp end", &s.ssao_emissive_hi, 0.f, 1.f);
  if (s.ssao_view == DEPTH_56 || s.ssao_view == STENCIL) {
    ImGui::SliderFloat("Depth view far (m)", &m_view_far_m, 5.f, 2000.f, "%.0f",
                       ImGuiSliderFlags_Logarithmic);
  }
  if (ImGui::TreeNode("Parameters (dev)")) {
    bool changed = false;
    changed |= ImGui::SliderFloat("radius (m)", &p.radius_m, 0.05f, 3.f);
    changed |= ImGui::SliderFloat("intensity", &p.intensity, 0.f, 1.f);
    changed |= ImGui::SliderFloat("merc weight", &p.merc_weight, 0.f, 4.f);
    changed |= ImGui::SliderFloat("luma floor (u8)", &p.luma_floor, 0.f, 32.f);
    changed |= ImGui::SliderFloat("cave scale", &p.cave_scale, 0.f, 1.f);
    changed |= ImGui::SliderInt("GTAO slices", &p.gtao_slices, 1, 9);
    changed |= ImGui::SliderInt("GTAO steps", &p.gtao_steps, 1, 8);
    changed |= ImGui::SliderInt("GTAO full resolution", &p.gtao_full_res, 0, 1);
    changed |= ImGui::SliderFloat("GTAO thin occluder compensation", &p.gtao_thin, 0.f, 1.f);
    if (changed) {
      s.ssao_preset = -1;
    }
    ImGui::TreePop();
  }
  ImGui::Checkbox("GPU timers", &s.ssao_gpu_timing);
  ImGui::Text("total %.3f  copy %.3f  lin+mips %.3f  ao %.3f  denoise %.3f  comp %.3f ms",
              m_t_total.last_avg_ms(), m_t_copy.last_avg_ms(), m_t_linearize.last_avg_ms(),
              m_t_ao.last_avg_ms(), m_t_blur.last_avg_ms(), m_t_composite.last_avg_ms());
}
