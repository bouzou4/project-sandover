#include "SsaoRenderer.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <optional>
#include <string>

#include "common/log/log.h"

#include "game/graphics/gfx.h"
#include "game/graphics/opengl_renderer/GlQueries.h"
#include "game/graphics/opengl_renderer/PerfTimers.h"
#include "game/graphics/opengl_renderer/Swamp.h"
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

// spiral turns that minimise discrepancy for a given tap count (G3D AmbientOcclusion.cpp)
constexpr int kSpiralTurns[] = {1, 1, 1, 2, 3, 2, 5, 2, 3, 2, 3, 3, 5, 5,  3,  4, 7,
                                5, 5, 7, 9, 8, 5, 5, 7, 7, 7, 8, 5, 8, 11, 12, 7};
constexpr int kMaxTaps = sizeof(kSpiralTurns) / sizeof(kSpiralTurns[0]) - 1;

constexpr float kFarM = 10000.f;

// pc-set-perf-opt bits (PERF_OPT_SSAO + n)
constexpr int kOptDirectDepth = PERF_OPT_SSAO + 0;  // linearize reads render_fb's depth, no copy
constexpr int kOptNoQueries = PERF_OPT_SSAO + 3;  // cached uniform locations, no fb binding query

bool g_normals_active = false;   // normal target attached and cleared this dispatch
bool g_normals_drawing = false;  // draw buffer 1 enabled on the render framebuffer
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
bool g_insp_drawing = false;  // draw buffer 3 (inspect ID target) enabled on the render framebuffer
bool g_sz_live = false;       // sprite depth target attached and cleared this dispatch
bool g_sz_drawing = false;    // draw buffer 4 (sprite depth) goes with the emissive output
float g_lin[3] = {};          // depth linearization of this dispatch (render())

// render framebuffer draw buffers: 0, normals, emissive, inspect (a count of 2 or 3 when inspect is
// off)
void render_fb_draw_buffers(bool emissive) {
  const bool sz = emissive && g_sz_drawing;
  const GLenum bufs[5] = {GL_COLOR_ATTACHMENT0, g_normals_drawing ? GL_COLOR_ATTACHMENT1 : GL_NONE,
                          emissive ? GL_COLOR_ATTACHMENT2 : GL_NONE,
                          g_insp_drawing ? GL_COLOR_ATTACHMENT3 : GL_NONE,
                          sz ? GL_COLOR_ATTACHMENT4 : GL_NONE};
  glDrawBuffers(sz ? 5 : (g_insp_drawing ? 4 : (emissive ? 3 : 2)), bufs);
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
  // only the tile pass reads it, and that pass also needs SSAO
  const bool sz = Gfx::g_global_settings.emissive_lights_enable && Gfx::g_global_settings.ssao_enable;
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
    glUniform1i(GLQ_LOC(kOptNoQueries, program, "ssao_nrm"), mode);
    glUniform1i(GLQ_LOC(kOptNoQueries, program, "ssao_fogw"),
                Gfx::g_global_settings.ssao_fog_weight > 0.f ? 1 : 0);
  }
}

void SsaoRenderer::set_normal_write(bool write) {
  if (g_normals_active) {
    glColorMaski(1, write, write, write, write);
  }
}

void SsaoRenderer::set_bucket_normals(SharedRenderState* rs, int bucket) {
  using jak1::BucketId;
  const bool want =
      g_normals_active &&
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
}

void SsaoRenderer::begin_normals(SharedRenderState* rs) {
  if (!Gfx::g_global_settings.ssao_normal_target) {
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

  const GLenum both[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
  glDrawBuffers(2, both);
  const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
  glDisable(GL_SCISSOR_TEST);
  glColorMaski(1, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  const float zero[4] = {0.f, 0.f, 0.f, 0.f};
  glClearBufferfv(GL_COLOR, 1, zero);
  if (scissor) {
    glEnable(GL_SCISSOR_TEST);
  }
  const GLenum first[2] = {GL_COLOR_ATTACHMENT0, GL_NONE};
  glDrawBuffers(2, first);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev);
  g_normals_active = true;
  g_normals_drawing = false;
}

void SsaoRenderer::end_normals(SharedRenderState* rs) {
  g_normals_active = false;
  g_normals_drawing = false;
  if (!m_nrm_attached) {
    return;
  }
  GLint prev = 0, prog = 0;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev);
  glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, rs->render_fb);
  glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, 0, 0);
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

void SsaoRenderer::render(SharedRenderState* rs, ScopedProfilerNode& prof) {
  const auto& gs = Gfx::g_global_settings;
  const auto& p = gs.ssao_params;
  const bool timing = gs.ssao_gpu_timing;
  m_have_frame = false;
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
  m_proj_scale = 0.5f * w * pxx / pzw;

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

  const int hw = std::max(1, w >> 1), hh = std::max(1, h >> 1);
  const float radius = std::max(p.radius_m, 0.01f);

  // ao-raw: SAO at half resolution
  ps.emplace(perf::Group::SSAO_AO);
  if (timing) {
    m_t_ao.begin();
  }
  {
    const int taps = std::clamp(p.taps, 1, kMaxTaps);
    glBindFramebuffer(GL_FRAMEBUFFER, m_ao[0].fbo);
    glViewport(0, 0, hw, hh);
    auto& sh = rs->shaders[ShaderId::SSAO_AO];
    sh.activate();
    set_proj_uniforms(sh.id());
    glUniform1i(GLQ_LOC(kOptNoQueries, sh.id(), "csz"), 0);
    glUniform1f(GLQ_LOC(kOptNoQueries, sh.id(), "proj_scale"), m_proj_scale);
    glUniform1f(GLQ_LOC(kOptNoQueries, sh.id(), "radius"), radius);
    glUniform1f(GLQ_LOC(kOptNoQueries, sh.id(), "bias"), p.bias_m);
    const float crease_rad = std::clamp(p.crease_deg, 0.f, 89.f) * 3.14159265f / 180.f;
    glUniform2f(GLQ_LOC(kOptNoQueries, sh.id(), "crease_sin"), std::sin(0.5f * crease_rad),
                std::sin(crease_rad));
    glUniform1f(GLQ_LOC(kOptNoQueries, sh.id(), "crease_floor"),
                std::clamp(p.crease_floor, 0.f, 1.f));
    glUniform1i(GLQ_LOC(kOptNoQueries, sh.id(), "num_samples"), taps);
    glUniform1f(GLQ_LOC(kOptNoQueries, sh.id(), "num_spiral_turns"), (float)kSpiralTurns[taps]);
    // temporal hook: a per-frame spin goes here once history accumulation exists
    glUniform1f(GLQ_LOC(kOptNoQueries, sh.id(), "frame_spin"), 0.f);
    bind_normals(sh.id(), 2, 3);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_csz);
    fullscreen();
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);
  }
  if (timing) {
    m_t_ao.end();
  }

  // A temporal accumulation stage is not implemented; it would sit here, between AO and blur.

  // ao-blur: separable bilateral blur
  ps.emplace(perf::Group::SSAO_BLUR);
  if (timing) {
    m_t_blur.begin();
  }
  m_ao_result = 0;
  if (p.blur_radius > 0) {
    auto& sh = rs->shaders[ShaderId::SSAO_BLUR];
    sh.activate();
    glUniform1i(GLQ_LOC(kOptNoQueries, sh.id(), "source"), 0);
    glUniform1i(GLQ_LOC(kOptNoQueries, sh.id(), "csz"), 1);
    glUniform1i(GLQ_LOC(kOptNoQueries, sh.id(), "R"), std::min(p.blur_radius, 8));
    glUniform1f(GLQ_LOC(kOptNoQueries, sh.id(), "edge_sharpness"), p.blur_sharpness);
    glUniform1f(GLQ_LOC(kOptNoQueries, sh.id(), "radius"), radius);
    glUniform1f(GLQ_LOC(kOptNoQueries, sh.id(), "far_m"), kFarM);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_csz);
    glViewport(0, 0, hw, hh);
    for (int pass = 0; pass < 2; pass++) {
      glUniform2i(GLQ_LOC(kOptNoQueries, sh.id(), "axis"), pass == 0 ? 1 : 0, pass == 0 ? 0 : 1);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, m_ao[pass].tex);
      glBindFramebuffer(GL_FRAMEBUFFER, m_ao[pass + 1].fbo);
      fullscreen();
    }
    m_ao_result = 2;
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
    const float strengths[2] = {std::clamp(p.intensity * scale, 0.f, 1.f),
                                std::clamp(p.intensity * p.merc_weight * scale, 0.f, 1.f)};
    for (int k = 0; k < 2; k++) {
      glStencilFunc(GL_EQUAL, k == 0 ? 0x00 : 0x40, 0xC0);
      glUniform1f(GLQ_LOC(kOptNoQueries, sh.id(), "strength"), strengths[k]);
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
  if (view == NONE || !m_have_frame) {
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
    changed |= ImGui::SliderFloat("bias (m)", &p.bias_m, 0.f, 0.2f);
    changed |= ImGui::SliderInt("blur radius", &p.blur_radius, 0, 8);
    changed |= ImGui::SliderFloat("blur sharpness", &p.blur_sharpness, 0.1f, 8.f);
    changed |= ImGui::SliderFloat("merc weight", &p.merc_weight, 0.f, 4.f);
    changed |= ImGui::SliderFloat("luma floor (u8)", &p.luma_floor, 0.f, 32.f);
    changed |= ImGui::SliderFloat("cave scale", &p.cave_scale, 0.f, 1.f);
    changed |= ImGui::SliderInt("taps", &p.taps, 1, kMaxTaps);
    changed |= ImGui::SliderFloat("crease angle (deg)", &p.crease_deg, 0.f, 45.f);
    changed |= ImGui::SliderFloat("crease floor", &p.crease_floor, 0.f, 1.f);
    if (changed) {
      s.ssao_preset = -1;
    }
    ImGui::TreePop();
  }
  ImGui::Checkbox("GPU timers", &s.ssao_gpu_timing);
  ImGui::Text("total %.3f  copy %.3f  lin+mips %.3f  ao %.3f  blur %.3f  comp %.3f ms",
              m_t_total.last_avg_ms(), m_t_copy.last_avg_ms(), m_t_linearize.last_avg_ms(),
              m_t_ao.last_avg_ms(), m_t_blur.last_avg_ms(), m_t_composite.last_avg_ms());
}
