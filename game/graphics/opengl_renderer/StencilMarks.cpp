#include "StencilMarks.h"

#include <algorithm>

#include "common/log/log.h"

#include "game/graphics/gfx.h"
#include "game/graphics/opengl_renderer/GlQueries.h"
#include "game/graphics/opengl_renderer/PerfTimers.h"
#include "game/graphics/opengl_renderer/buckets.h"

namespace stencil_marks {
namespace {

constexpr int kOptLate = PERF_OPT_SSAO + 1;
constexpr int kProbeCompare = 16384;  // pc-set-ssao-option 4

enum Slot { SKY, OCEAN_B, OCEAN_A, P0_B, P0_A, P1_B, P1_A, SLOTS };

struct Copy {
  GLuint tex = 0;
  int w = 0, h = 0, samples = 0;
};

Copy g_copy[SLOTS];
GLuint g_ext[SLOTS] = {};  // this dispatch's source per slot (a copy, or the water depth)
bool g_on = false;         // copies and the late pass run this dispatch
bool g_deferred = false;   // buckets 0-30 write no stencil
bool g_compare = false;
int g_w = 0, g_h = 0, g_samples = 1;
GLuint g_vao = 0;
GLuint g_queries[10] = {};
GLuint g_sky_query = 0;  // any sample drawn in buckets 0-4
bool g_sky_query_on = false;
int g_frame = 0;

bool compare_probe() {
  return (Gfx::g_global_settings.ssao_prereq_probe & kProbeCompare) != 0;
}

// `timed`: false inside another group's scope (the merc prop buckets), which then holds the cost
void take(SharedRenderState* rs, Slot s, bool timed = true) {
  perf::Scope ps(timed ? perf::Group::SSAO_MARKS : perf::Group::NONE);
  auto& c = g_copy[s];
  const GLenum target = g_samples > 1 ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D;
  if (!c.tex || c.w != g_w || c.h != g_h || c.samples != g_samples) {
    if (c.tex) {
      glDeleteTextures(1, &c.tex);
    }
    // may run mid-flush (Merc2): keep the active unit's binding
    GLint bound = 0;
    glGetIntegerv(g_samples > 1 ? GL_TEXTURE_BINDING_2D_MULTISAMPLE : GL_TEXTURE_BINDING_2D,
                  &bound);
    glGenTextures(1, &c.tex);
    glBindTexture(target, c.tex);
    if (g_samples > 1) {
      glTexImage2DMultisample(target, g_samples, GL_DEPTH24_STENCIL8, g_w, g_h, GL_TRUE);
    } else {
      glTexImage2D(target, 0, GL_DEPTH24_STENCIL8, g_w, g_h, 0, GL_DEPTH_STENCIL,
                   GL_UNSIGNED_INT_24_8, nullptr);
      glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, 0);
    }
    glBindTexture(target, bound);
    c.w = g_w;
    c.h = g_h;
    c.samples = g_samples;
  }
  glCopyImageSubData(rs->render_fb_depth_tex, target, 0, 0, 0, 0, c.tex, target, 0, 0, 0, 0, g_w,
                     g_h, 1);
  g_ext[s] = c.tex;
}

// one source: where its copy (and, with `before`, a changed depth) is still the depth, write ref
void pass(GLuint prog, Slot after, int before, GLint ref, GLuint mask) {
  if (!g_ext[after] || (before >= 0 && !g_ext[before])) {
    return;
  }
  // the sky marks only where buckets 0-4 drew something (GPU-side test, no stall)
  const bool cond = after == SKY && g_sky_query;
  if (cond) {
    glBeginConditionalRender(g_sky_query, GL_QUERY_WAIT);
  }
  const bool ms = g_samples > 1;
  const GLenum target = ms ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D;
  glActiveTexture(GL_TEXTURE0 + (ms ? 2 : 0));
  glBindTexture(target, g_ext[after]);
  if (before >= 0) {
    glActiveTexture(GL_TEXTURE0 + (ms ? 3 : 1));
    glBindTexture(target, g_ext[before]);
  }
  glUniform1i(GLQ_LOC(kOptLate, prog, "has_before"), before >= 0 ? 1 : 0);
  if (g_compare) {
    // probe: bit 7 -> scratch bit 0, bit 5 -> bit 1
    ref = (ref & 0x80 ? 1 : 0) | (ref & 0x20 ? 2 : 0);
    mask = (mask & 0x80 ? 1 : 0) | (mask & 0x20 ? 2 : 0);
  }
  glStencilFunc(GL_ALWAYS, ref, 0xFF);
  glStencilMask(mask);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  if (cond) {
    glEndConditionalRender();
  }
}

void compare_and_clear(GLuint prog) {
  // stencil value tests against the old marks (bits 7, 5) and the late ones (scratch bits 0, 1)
  struct Test {
    GLint ref;
    GLuint mask;
  };
  const Test tests[10] = {
      {0x80, 0x81}, {0x01, 0x81}, {0x81, 0x81},  // bit 7: old only, late only, both
      {0x20, 0xA3}, {0x21, 0xA3}, {0x23, 0xA3},  // prop old only (late: none, bit 7, both)
      {0x02, 0xA3}, {0x82, 0xA3}, {0xA2, 0xA3},  // prop late only
      {0x22, 0xA3},                              // prop both
  };
  if (!g_queries[0]) {
    glGenQueries(10, g_queries);
  }
  // whole screen: has_before 0 never discards
  glUniform1i(GLQ_LOC(kOptLate, prog, "has_before"), 0);
  glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
  glDisable(GL_DEPTH_TEST);
  glStencilMask(0);
  glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
  for (int i = 0; i < 10; i++) {
    glStencilFunc(GL_EQUAL, tests[i].ref, tests[i].mask);
    glBeginQuery(GL_SAMPLES_PASSED, g_queries[i]);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glEndQuery(GL_SAMPLES_PASSED);
  }
  glStencilFunc(GL_ALWAYS, 0, 0xFF);
  glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
  glStencilMask(0x03);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  GLuint n[10] = {};
  for (int i = 0; i < 10; i++) {
    glGetQueryObjectuiv(g_queries[i], GL_QUERY_RESULT, &n[i]);
  }
  if (g_frame++ % 30 == 0) {
    lg::info(
        "SSAO-MARKS samples={} sky7_old_only={} sky7_late_only={} sky7_both={} prop_old_only={} "
        "prop_late_only={} prop_both={} src={}{}{}{}",
        (long long)g_w * g_h * g_samples, n[0], n[1], n[2], n[3] + n[4] + n[5], n[6] + n[7] + n[8],
        n[9], g_ext[SKY] ? "sky " : "", g_ext[OCEAN_A] ? "ocean " : "", g_ext[P0_A] ? "prop0 " : "",
        g_ext[P1_A] ? "prop1" : "");
  }
}

void end_sky_query() {
  if (g_sky_query_on) {
    glEndQuery(GL_ANY_SAMPLES_PASSED);
    g_sky_query_on = false;
  }
}

}  // namespace

bool begin(SharedRenderState* rs, bool on) {
  for (auto& e : g_ext) {
    e = 0;
  }
  g_compare = on && compare_probe();
  g_on = on && rs->render_fb_depth_tex && (Gfx::perf_opt(kOptLate) || g_compare) &&
         (Gfx::g_global_settings.ssao_prereq_probe & 1) == 0;
  g_deferred = g_on && !g_compare;
  if (!g_on) {
    release();
    return false;
  }
  GLint prev = 0, samples = 1;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, rs->render_fb);
  glGetIntegerv(GL_SAMPLES, &samples);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev);
  g_samples = std::max(samples, 1);
  g_w = rs->render_fb_w;
  g_h = rs->render_fb_h;
  return g_deferred;
}

void before_sky() {
  if (!g_on) {
    return;
  }
  end_sky_query();
  if (!g_sky_query) {
    glGenQueries(1, &g_sky_query);
  }
  glBeginQuery(GL_ANY_SAMPLES_PASSED, g_sky_query);
  g_sky_query_on = true;
}

bool deferred() {
  return g_deferred;
}

void after_sky(SharedRenderState* rs) {
  if (g_on) {
    end_sky_query();
    take(rs, SKY);
  }
}

void ocean(SharedRenderState* rs, bool after, GLuint water_depth) {
  if (!g_on) {
    return;
  }
  if (after) {
    take(rs, OCEAN_A);
  } else if (water_depth && g_samples == 1) {
    g_ext[OCEAN_B] = water_depth;  // the same single-sample copy
  } else {
    take(rs, OCEAN_B);
  }
}

void props(SharedRenderState* rs, int bucket, bool after) {
  const auto& gs = Gfx::g_global_settings;
  if (!g_on || gs.shadow_mode == 0 || gs.shadow_lights <= 0) {
    return;
  }
  const bool l1 = bucket == (int)jak1::BucketId::MERC_TFRAG_TEX_LEVEL1;
  const Slot b = l1 ? P1_B : P0_B, a = l1 ? P1_A : P0_A;
  if (!after && !g_ext[b]) {
    take(rs, b, false);
  } else if (after && g_ext[b]) {
    take(rs, a, false);
  }
}

void write(SharedRenderState* rs) {
  if (!g_on) {
    return;
  }
  end_sky_query();
  perf::Scope ps(perf::Group::SSAO_MARKS);
  glq::SavedState saved("stencil-marks", glq::kLive, 0x3, 0xC);
  saved.capture();
  if (!g_vao) {
    glGenVertexArrays(1, &g_vao);
  }
  glBindFramebuffer(GL_FRAMEBUFFER, rs->render_fb);
  glViewport(0, 0, g_w, g_h);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);
  glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_EQUAL);
  glDepthMask(GL_FALSE);
  glEnable(GL_STENCIL_TEST);
  glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
  auto& sh = rs->shaders[ShaderId::SSAO_MARKS];
  sh.activate();
  const GLuint prog = sh.id();
  glUniform1i(GLQ_LOC(kOptLate, prog, "ms"), g_samples > 1 ? 1 : 0);
  glUniform1i(GLQ_LOC(kOptLate, prog, "after_ss"), 0);
  glUniform1i(GLQ_LOC(kOptLate, prog, "before_ss"), 1);
  glUniform1i(GLQ_LOC(kOptLate, prog, "after_ms"), 2);
  glUniform1i(GLQ_LOC(kOptLate, prog, "before_ms"), 3);
  glBindVertexArray(g_vao);
  // in drawing order, with the masks the per-bucket writes used
  pass(prog, SKY, -1, 0x80, 0x80);
  pass(prog, P0_A, P0_B, 0x20, 0xA0);
  pass(prog, P1_A, P1_B, 0x20, 0xA0);
  pass(prog, OCEAN_A, OCEAN_B, 0x80, 0x80);
  if (g_compare) {
    compare_and_clear(prog);
  }
  saved.restore();
}

void release() {
  end_sky_query();
  if (g_sky_query) {
    glDeleteQueries(1, &g_sky_query);
    g_sky_query = 0;
  }
  for (auto& c : g_copy) {
    if (c.tex) {
      glDeleteTextures(1, &c.tex);
    }
    c = Copy();
  }
  for (auto& e : g_ext) {
    e = 0;
  }
  g_on = g_deferred = g_compare = false;
}

}  // namespace stencil_marks
