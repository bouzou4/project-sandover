#include "EmissiveLights.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>

#include "common/log/log.h"
#include "common/util/Timer.h"

#include "game/graphics/gfx.h"
#include "game/graphics/opengl_renderer/ActorShadows.h"
#include "game/graphics/opengl_renderer/GlQueries.h"
#include "game/graphics/opengl_renderer/PerfTimers.h"
#include "game/graphics/opengl_renderer/SsaoRenderer.h"
#include "game/graphics/opengl_renderer/WorldLights.h"
#include "game/graphics/opengl_renderer/background/Unbake.h"
#include "game/graphics/opengl_renderer/loader/Loader.h"
#include "game/graphics/screenshot.h"

#include "fmt/format.h"
#include "third-party/imgui/imgui.h"

namespace {
// a tile's light sits at most this far behind the tile's nearest surface (metres)
constexpr float kNearSlackM = 1.f;
// share of the added light tinted by the surface colour
constexpr float kAlbedoMix = 0.5f;
// tile fill (glowing share of the glowing taps' bounding box): thin below lo, compact above hi
constexpr float kFillLo = 0.2f, kFillHi = 0.5f;
// added light (luma) approaches this and never passes it
constexpr float kMaxAdd = 0.35f;
constexpr float kMeter = 4096.f;
// a camera move or turn past these in one frame is a cut (warp, continue, cutscene): reset tracking
constexpr float kCutMoveM = 8.f;
constexpr float kCutCos = 0.866f;  // 30 degrees
constexpr int kTexUnits = 12;
// glow props leave these world slots free: room for rows fading out, and the eco shot lights' slots
constexpr int kGlowSpare = 2;
constexpr int kEcoReserve = world_lights::kEcoSlots;
// perf-opt bit 0 also owns this pass's GL query caches
constexpr int kQueryBit = PERF_OPT_TILE_LIGHTS + 0;

void make_texture(GLuint& tex, int w, int h, GLint ifmt, GLenum type, GLint filter) {
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexImage2D(GL_TEXTURE_2D, 0, ifmt, w, h, 0, GL_RGBA, type, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
}

void make_fbo(GLuint& fbo, const GLuint* tex, int n) {
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  GLenum draw[4];
  for (int i = 0; i < n; i++) {
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D, tex[i], 0);
    draw[i] = GL_COLOR_ATTACHMENT0 + i;
  }
  glDrawBuffers(n, draw);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    lg::error("emissive lights: incomplete framebuffer");
  }
}

/// GL state the passes change, restored on destruction.
struct Saved {
  GLboolean depth_test, blend, scissor, cull, stencil;
  GLboolean color_mask[4];
  GLint blend_src_rgb, blend_dst_rgb, blend_src_a, blend_dst_a, blend_eq_rgb, blend_eq_a,
      viewport[4], program, vao, active_tex, tex[kTexUnits], tex_ms[kTexUnits], draw_fbo, read_fbo,
      read_buf, stencil_func, stencil_ref, stencil_mask, stencil_write, stencil_fail, stencil_zfail,
      stencil_zpass;
  Saved() {
    depth_test = glIsEnabled(GL_DEPTH_TEST);
    blend = glIsEnabled(GL_BLEND);
    scissor = glIsEnabled(GL_SCISSOR_TEST);
    cull = glIsEnabled(GL_CULL_FACE);
    stencil = glIsEnabled(GL_STENCIL_TEST);
    glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
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
    for (int i = 0; i < kTexUnits; i++) {
      glActiveTexture(GL_TEXTURE0 + i);
      glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex[i]);
      glGetIntegerv(GL_TEXTURE_BINDING_2D_MULTISAMPLE, &tex_ms[i]);
    }
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
    glGetIntegerv(GL_READ_BUFFER, &read_buf);
    glGetIntegerv(GL_STENCIL_FUNC, &stencil_func);
    glGetIntegerv(GL_STENCIL_REF, &stencil_ref);
    glGetIntegerv(GL_STENCIL_VALUE_MASK, &stencil_mask);
    glGetIntegerv(GL_STENCIL_WRITEMASK, &stencil_write);
    glGetIntegerv(GL_STENCIL_FAIL, &stencil_fail);
    glGetIntegerv(GL_STENCIL_PASS_DEPTH_FAIL, &stencil_zfail);
    glGetIntegerv(GL_STENCIL_PASS_DEPTH_PASS, &stencil_zpass);
  }
  ~Saved() {
    auto set = [](GLenum cap, GLboolean on) { on ? glEnable(cap) : glDisable(cap); };
    set(GL_DEPTH_TEST, depth_test);
    set(GL_BLEND, blend);
    set(GL_SCISSOR_TEST, scissor);
    set(GL_CULL_FACE, cull);
    set(GL_STENCIL_TEST, stencil);
    glStencilFunc(stencil_func, stencil_ref, stencil_mask);
    glStencilMask(stencil_write);
    glStencilOp(stencil_fail, stencil_zfail, stencil_zpass);
    glColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
    glBlendFuncSeparate(blend_src_rgb, blend_dst_rgb, blend_src_a, blend_dst_a);
    glBlendEquationSeparate(blend_eq_rgb, blend_eq_a);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    glUseProgram(program);
    glBindVertexArray(vao);
    for (int i = 0; i < kTexUnits; i++) {
      glActiveTexture(GL_TEXTURE0 + i);
      glBindTexture(GL_TEXTURE_2D, tex[i]);
      glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, tex_ms[i]);
    }
    glActiveTexture(active_tex);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
    glReadBuffer(read_buf);
  }
};

void bind_emissive(GLuint prog, GLuint em_tex, int em_samples, int ss_unit, int ms_unit) {
  const auto& gs = Gfx::g_global_settings;
  glUniform1i(GLQ_LOC(kQueryBit, prog, "em_ss"), ss_unit);
  glUniform1i(GLQ_LOC(kQueryBit, prog, "em_ms"), ms_unit);
  glUniform1i(GLQ_LOC(kQueryBit, prog, "em_samples"), em_samples);
  glUniform2f(GLQ_LOC(kQueryBit, prog, "em_ramp"), gs.ssao_emissive_lo,
              std::max(gs.ssao_emissive_hi, gs.ssao_emissive_lo + 1e-3f));
  glActiveTexture(GL_TEXTURE0 + ss_unit);
  glBindTexture(GL_TEXTURE_2D, em_samples > 1 ? 0 : em_tex);
  glActiveTexture(GL_TEXTURE0 + ms_unit);
  glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, em_samples > 1 ? em_tex : 0);
}
}  // namespace

EmissiveLights::~EmissiveLights() {
  free_targets();
  drop_readbacks();
  for (auto& rb : m_rb) {
    if (rb.pbo) {
      glDeleteBuffers(1, &rb.pbo);
    }
  }
  if (m_vao) {
    glDeleteVertexArrays(1, &m_vao);
  }
}

void EmissiveLights::free_targets() {
  for (Target* t : {&m_resolve, &m_tiles, &m_part, &m_cand, &m_lights, &m_world}) {
    if (t->fbo) {
      glDeleteFramebuffers(1, &t->fbo);
    }
    for (GLuint& tex : t->tex) {
      if (tex) {
        glDeleteTextures(1, &tex);
      }
    }
    *t = Target();
  }
  if (m_cull_tex) {
    glDeleteTextures(1, &m_cull_tex);
    m_cull_tex = 0;
  }
  m_w = m_h = m_tiles_y = 0;
}

void EmissiveLights::ensure_targets(int w, int h, bool hdr) {
  if (w == m_w && h == m_h && hdr == m_hdr) {
    return;
  }
  perf::AddMs hitch_t(perf::Counter::HITCH_TARGET_MS);
  free_targets();
  make_texture(m_resolve.tex[0], w, h, hdr ? GL_RGBA16F : GL_RGBA8,
               hdr ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE, GL_LINEAR);
  make_fbo(m_resolve.fbo, m_resolve.tex, 1);
  m_tiles_y = std::max(1, (int)std::lround((double)kTilesX * h / w));
  for (int i = 0; i < 2; i++) {
    make_texture(m_tiles.tex[i], kTilesX, m_tiles_y, GL_RGBA32F, GL_FLOAT, GL_NEAREST);
  }
  make_fbo(m_tiles.fbo, m_tiles.tex, 2);
  for (auto& tex : m_part.tex) {
    make_texture(tex, kTilesX * kSplit, m_tiles_y * kSplit, GL_RGBA32F, GL_FLOAT, GL_NEAREST);
  }
  make_fbo(m_part.fbo, m_part.tex, 4);
  for (int i = 0; i < 2; i++) {
    make_texture(m_cand.tex[i], kMaxCand, 1, GL_RGBA32F, GL_FLOAT, GL_NEAREST);
  }
  make_fbo(m_cand.fbo, m_cand.tex, 2);
  for (int i = 0; i < 2; i++) {
    make_texture(m_lights.tex[i], kMaxLights, 1, GL_RGBA32F, GL_FLOAT, GL_NEAREST);
  }
  make_fbo(m_lights.fbo, m_lights.tex, 2);
  for (int i = 0; i < 2; i++) {
    make_texture(m_world.tex[i], kMaxWorld, 1, GL_RGBA32F, GL_FLOAT, GL_NEAREST);
  }
  make_fbo(m_world.fbo, m_world.tex, 2);
  m_cull_px = (w + kCullX - 1) / kCullX;
  m_cull_y = (h + m_cull_px - 1) / m_cull_px;
  glGenTextures(1, &m_cull_tex);
  glBindTexture(GL_TEXTURE_2D, m_cull_tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RG32UI, kCullX, m_cull_y, 0, GL_RG_INTEGER, GL_UNSIGNED_INT,
               nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
  glBindTexture(GL_TEXTURE_2D, 0);
  m_w = w;
  m_h = h;
  m_hdr = hdr;
  lg::info("emissive lights: allocated targets {}x{} {}, {}x{} tiles", w, h,
           hdr ? "RGBA16F" : "RGBA8", kTilesX, m_tiles_y);
}

void EmissiveLights::gather_world(SharedRenderState* rs,
                                  const float* pi,
                                  int w,
                                  int h,
                                  std::vector<LightTracker::World>& out) {
  const auto& gs = Gfx::g_global_settings;
  out.clear();
  if (gs.world_lights_count <= 0 || !rs->loader) {
    return;
  }
  Timer timer;
  std::vector<world_lights::LevelWeights> levels;
  for (auto* lev : rs->loader->get_in_use_levels()) {
    world_lights::LevelWeights lw;
    lw.name = rs->loader->name_of(lev);
    lw.have = lev->unbake && unbake_last_emissive_weights(lev->unbake.get(), lw.w);
    levels.push_back(std::move(lw));
  }
  const auto& t = rs->camera_pos;
  const float cam_m[3] = {t.x() / kMeter, t.y() / kMeter, t.z() / kMeter};
  const float cls[world_lights::NUM_CLASSES] = {gs.world_lights_strong, gs.world_lights_weak,
                                                gs.world_lights_arc, gs.world_lights_particle,
                                                gs.world_lights_lamp, gs.world_lights_glow};
  world_lights::GlowView glow;
  glow.fade_m = gs.world_lights_glow_fade_m;
  glow.slots = gs.world_lights_glow_slots;
  glow.reserved = kGlowSpare + kEcoReserve;
  std::vector<std::array<float, 3>> tiles;
  for (const auto& c : m_cands) {
    tiles.push_back({c.pos[0], c.pos[1], c.pos[2]});
  }
  glow.tiles = &tiles;
  {
    // camera z (depth > 0 in front) is view_signs' z sign times camera_rot's third column
    const float sz = rs->camera_persp[2][3] >= 0.f ? -1.f : 1.f;
    for (int i = 0; i < 3; i++) {
      glow.fwd[i] = sz * rs->camera_rot[i][2];
    }
    const float tx = std::max(std::abs(pi[2]), std::abs(w * pi[0] + pi[2]));
    const float ty = std::max(std::abs(pi[3]), std::abs(h * pi[1] + pi[3]));
    glow.tan_half = std::sqrt(tx * tx + ty * ty);
  }
  const auto sel =
      world_lights::select(levels, cam_m, std::min(gs.world_lights_count, (int)kMaxWorld), cls,
                           gs.world_lights_radius_scale, gs.world_lights_radius_max, glow);
  for (const auto& l : sel) {
    LightTracker::World wl;
    wl.row = l.row;
    std::copy(l.pos_m, l.pos_m + 3, wl.pos);
    std::copy(l.power, l.power + 3, wl.power);
    wl.radius = l.radius_m;
    wl.arc = l.arc;
    wl.suppress = !l.added;
    wl.wide = l.wide;
    wl.snap = l.snap;
    wl.ramp = l.added && !l.snap && glow.fade_m > 0.f;
    out.push_back(wl);
  }
  m_world_cpu_ms = 0.9f * m_world_cpu_ms + 0.1f * (float)timer.getMs();
}

// camera space of the background shaders (make_new_cam_mat) in the SSAO view convention:
// view = -sign(pc_pxx, pc_pyy, pzw) * camera / 4096, matching proj_info and csz
void emissive_view_signs(const SharedRenderState* rs, float* s) {
  const auto* persp = rs->camera_persp;
  const float fog = rs->camera_fog.x();
  s[0] = fog * persp[0][0] >= 0.f ? -1.f : 1.f;
  s[1] = -fog * persp[1][1] >= 0.f ? -1.f : 1.f;
  s[2] = persp[2][3] >= 0.f ? -1.f : 1.f;
}

namespace {
void view_signs(SharedRenderState* rs, float* s) {
  emissive_view_signs(rs, s);
}
}  // namespace

void EmissiveLights::to_cam(SharedRenderState* rs, const float* world_m, float* cam) const {
  float sg[3], v[3];
  view_signs(rs, sg);
  const auto& t = rs->camera_pos;
  const auto& r = rs->camera_rot;
  for (int i = 0; i < 3; i++) {
    v[i] = world_m[i] * kMeter - t[i];
  }
  for (int k = 0; k < 3; k++) {
    cam[k] = sg[k] * (v[0] * r[0][k] + v[1] * r[1][k] + v[2] * r[2][k]) / kMeter;
  }
}

EmissiveLights::CamSnap EmissiveLights::snap(SharedRenderState* rs) {
  CamSnap c;
  view_signs(rs, c.sg);
  for (int i = 0; i < 3; i++) {
    c.t[i] = rs->camera_pos[i];
    for (int k = 0; k < 3; k++) {
      c.r[i][k] = rs->camera_rot[i][k];
    }
  }
  return c;
}

void EmissiveLights::to_world(const CamSnap& cs, const float* cam, float* world_m) {
  float c[3];
  for (int k = 0; k < 3; k++) {
    c[k] = cs.sg[k] * cam[k] * kMeter;
  }
  // camera_rot is a rotation: its inverse is its transpose
  const auto& r = cs.r;
  for (int i = 0; i < 3; i++) {
    world_m[i] = (r[i][0] * c[0] + r[i][1] * c[1] + r[i][2] * c[2] + cs.t[i]) / kMeter;
  }
}

void EmissiveLights::select_tiles(const float* tf,
                                  const float* tp,
                                  int n,
                                  float threshold,
                                  float tile_area,
                                  float (*flux)[4],
                                  float (*pos)[4]) {
  // emissive_select.vert on the CPU: rank by luma (ties: lower index first), keep kMaxCand
  std::memset(flux, 0, sizeof(float) * 4 * kMaxCand);
  std::memset(pos, 0, sizeof(float) * 4 * kMaxCand);
  std::vector<std::pair<float, int>> ranked;
  for (int i = 0; i < n; i++) {
    const float* f = tf + 4 * i;
    const float l = 0.299f * f[0] + 0.587f * f[1] + 0.114f * f[2];
    const float score = l >= threshold && tp[4 * i + 3] > 0.f ? l : 0.f;
    if (score > 0.f) {
      ranked.push_back({score, i});
    }
  }
  const int keep = std::min((int)ranked.size(), (int)kMaxCand);
  std::partial_sort(ranked.begin(), ranked.begin() + keep, ranked.end(),
                    [](const auto& a, const auto& b) {
                      return a.first > b.first || (a.first == b.first && a.second < b.second);
                    });
  for (int r = 0; r < keep; r++) {
    const float* f = tf + 4 * ranked[r].second;
    const float* q = tp + 4 * ranked[r].second;
    for (int k = 0; k < 3; k++) {
      flux[r][k] = f[k] * tile_area * q[2] * q[2];
      pos[r][k] = q[k];
    }
    flux[r][3] = ranked[r].first;
    pos[r][3] = f[3];
  }
}

void EmissiveLights::read_rows(bool tiles, void* dst_flux, void* dst_pos) {
  glBindFramebuffer(GL_READ_FRAMEBUFFER, tiles ? m_tiles.fbo : m_cand.fbo);
  const int w = tiles ? kTilesX : kMaxCand, h = tiles ? m_tiles_y : 1;
  glReadBuffer(GL_COLOR_ATTACHMENT0);
  glReadPixels(0, 0, w, h, GL_RGBA, GL_FLOAT, dst_flux);
  glReadBuffer(GL_COLOR_ATTACHMENT1);
  glReadPixels(0, 0, w, h, GL_RGBA, GL_FLOAT, dst_pos);
}

void EmissiveLights::issue_readback(Readback& rb,
                                    SharedRenderState* rs,
                                    float tile_m,
                                    const SelectIn& sel) {
  const int n = sel.cpu ? kTilesX * m_tiles_y : kMaxCand;
  const GLsizeiptr row = n * 4 * sizeof(float);
  if (!rb.pbo) {
    glGenBuffers(1, &rb.pbo);
  }
  glBindBuffer(GL_PIXEL_PACK_BUFFER, rb.pbo);
  if (rb.size != 2 * row) {
    glBufferData(GL_PIXEL_PACK_BUFFER, 2 * row, nullptr, GL_STREAM_READ);
    rb.size = 2 * row;
  }
  if (rb.fence) {
    glDeleteSync(rb.fence);
  }
  read_rows(sel.cpu, nullptr, (void*)row);
  glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  rb.fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  rb.cam = snap(rs);
  rb.tile_m = tile_m;
  rb.frame = rs->frame_idx;
  rb.sel = sel;
  rb.n = n;
}

bool EmissiveLights::take_readback(Readback& rb, float (*flux)[4], float (*pos)[4]) {
  if (!rb.fence) {
    return false;
  }
  // a frame old, so normally signalled already; never block more than 100 ms
  const GLenum r = glClientWaitSync(rb.fence, GL_SYNC_FLUSH_COMMANDS_BIT, 100000000);
  glDeleteSync(rb.fence);
  rb.fence = nullptr;
  if (r != GL_ALREADY_SIGNALED && r != GL_CONDITION_SATISFIED) {
    return false;
  }
  const GLsizeiptr row = rb.n * 4 * sizeof(float);
  glBindBuffer(GL_PIXEL_PACK_BUFFER, rb.pbo);
  if (rb.sel.cpu) {
    m_tile_rb.resize((size_t)rb.n * 8);
    glGetBufferSubData(GL_PIXEL_PACK_BUFFER, 0, 2 * row, m_tile_rb.data());
    select_tiles(m_tile_rb.data(), m_tile_rb.data() + 4 * rb.n, rb.n, rb.sel.threshold,
                 rb.sel.tile_area, flux, pos);
  } else {
    glGetBufferSubData(GL_PIXEL_PACK_BUFFER, 0, row, flux);
    glGetBufferSubData(GL_PIXEL_PACK_BUFFER, row, row, pos);
  }
  glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  return true;
}

void EmissiveLights::drop_readbacks() {
  for (auto& rb : m_rb) {
    if (rb.fence) {
      glDeleteSync(rb.fence);
      rb.fence = nullptr;
    }
  }
}

void EmissiveLights::log_frame(int burst, int num_cand) const {
  auto luma = [](const float* c) { return 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2]; };
  std::string line = fmt::format("EMLIGHTS f={} raw=", burst);
  for (int i = 0; i < num_cand; i++) {
    const auto& c = m_cands[i];
    line +=
        fmt::format("{:.3f},{:.3f},{:.3f},{:.4g},{:.2f},{:.4g},{:.4g},{:.4g};", c.pos[0], c.pos[1],
                    c.pos[2], luma(c.power), c.fill, c.power[0], c.power[1], c.power[2]);
  }
  for (const auto* list : {&m_tracker.tiles(), &m_tracker.world()}) {
    line += list == &m_tracker.tiles() ? " out=" : " world=";
    for (const auto& o : *list) {
      line += fmt::format("{},{:.3f},{:.3f},{:.3f},{:.4g},{:.4g},{:.4g},{:.4g};", o.id, o.pos[0],
                          o.pos[1], o.pos[2], luma(o.power), o.power[0], o.power[1], o.power[2]);
    }
  }
  lg::info("{}", line);
  int claimed = 0, first = 0, dropped = 0;
  world_lights::last_claims(&claimed, &first, &dropped);
  int calls = 0;
  const int launches = world_lights::launch_count(&calls);
  lg::info("EMCLAIMS f={} claimed={} first={} dropped_near={} launches={} calls={} launch_lit={}",
           burst, claimed, first, dropped, launches, calls, world_lights::launch_lit().size());
}

bool EmissiveLights::camera_cut(const float* cam_m, const float* fwd) {
  float d2 = 0.f, dot = 0.f;
  for (int i = 0; i < 3; i++) {
    d2 += (cam_m[i] - m_last_cam[i]) * (cam_m[i] - m_last_cam[i]);
    dot += fwd[i] * m_last_fwd[i];
  }
  const bool cut = !m_have_last || d2 > kCutMoveM * kCutMoveM || dot < kCutCos;
  std::copy(cam_m, cam_m + 3, m_last_cam);
  std::copy(fwd, fwd + 3, m_last_fwd);
  m_have_last = true;
  return cut;
}

void EmissiveLights::build_cull(const float* pi,
                                const float (*lpos)[4],
                                int num_tiles,
                                float tile_r,
                                const float (*wpos)[4],
                                const float (*wpow)[4],
                                int num_world) {
  m_cull.assign((size_t)kCullX * m_cull_y * 2, 0u);
  // screen ratio x/z (or y/z) range over the cell's pixel centres, one pixel wider each side
  auto ratio = [](int a, int b, float scale, float off, float& lo, float& hi) {
    const float u0 = (a + 0.5f) * scale + off, u1 = (b - 0.5f) * scale + off;
    lo = std::min(u0, u1) - std::abs(scale);
    hi = std::max(u0, u1) + std::abs(scale);
  };
  // false when no point of the sphere is inside the wedge lo <= c / z <= hi, z >= 0
  auto slab = [](float c, float z, float r, float lo, float hi) {
    return c - hi * z <= r * std::sqrt(1.f + hi * hi) && lo * z - c <= r * std::sqrt(1.f + lo * lo);
  };
  for (int cy = 0; cy < m_cull_y; cy++) {
    float ylo, yhi;
    ratio(cy * m_cull_px, std::min((cy + 1) * m_cull_px, m_h), pi[1], pi[3], ylo, yhi);
    for (int cx = 0; cx < kCullX; cx++) {
      float xlo, xhi;
      ratio(cx * m_cull_px, std::min((cx + 1) * m_cull_px, m_w), pi[0], pi[2], xlo, xhi);
      u32* m = &m_cull[((size_t)cy * kCullX + cx) * 2];
      auto hit = [&](const float* c, float r) {
        r = r * 1.001f + 1e-3f;
        return c[2] + r >= 0.f && slab(c[0], c[2], r, xlo, xhi) && slab(c[1], c[2], r, ylo, yhi);
      };
      for (int i = 0; i < num_tiles; i++) {
        if (hit(lpos[i], tile_r)) {
          m[0] |= 1u << i;
        }
      }
      for (int i = 0; i < num_world; i++) {
        if (wpow[i][3] > 0.f && hit(wpos[i], wpos[i][3])) {
          m[1] |= 1u << i;
        }
      }
    }
  }
  glBindTexture(GL_TEXTURE_2D, m_cull_tex);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kCullX, m_cull_y, GL_RG_INTEGER, GL_UNSIGNED_INT,
                  m_cull.data());
}

void EmissiveLights::render(SharedRenderState* rs,
                            ScopedProfilerNode& prof,
                            SsaoRenderer& ssao,
                            GLuint em_tex,
                            int em_samples) {
  const auto& gs = Gfx::g_global_settings;
  if (!em_tex || !ssao.have_frame()) {
    if (g_burst_frame >= 0) {
      lg::info("EMLIGHTS f={} off mask={} ssao={} ssao_frame={}", g_burst_frame, em_tex != 0,
               gs.ssao_enable, ssao.have_frame());
    }
    return;
  }
  if (rs->render_fb_w <= 0 || rs->render_fb_h <= 0) {
    return;
  }
  const bool timing = gs.ssao_gpu_timing;
  // bit 0: the shared cached save/restore puts back the entry state, stencil included (units
  // 0-7); our own units 8-10 stay bound
  static glq::SavedState cached_state("emissive-lights", kQueryBit, 0xff, 0xff);
  std::optional<Saved> saved;
  struct Put {
    glq::SavedState* s;
    ~Put() {
      if (s) {
        s->restore();
      }
    }
  } put{nullptr};
  if (Gfx::perf_opt(kQueryBit)) {
    cached_state.capture();
    put.s = &cached_state;
  } else {
    saved.emplace();
  }
  if (timing) {
    m_timer.begin();
  }
  const int w = rs->render_fb_w, h = rs->render_fb_h;
  ensure_targets(w, h, rs->hdr_target);
  const int tile_w = (w + kTilesX - 1) / kTilesX;
  const int tile_h = (h + m_tiles_y - 1) / m_tiles_y;
  if (!m_vao) {
    glGenVertexArrays(1, &m_vao);
  }
  glBindVertexArray(m_vao);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_BLEND);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

  {
    perf::Scope ps(perf::Group::EMLIGHTS_RESOLVE);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, rs->render_fb);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_resolve.fbo);
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  }

  const float* pi = ssao.proj_info();
  // the second eye reuses the first eye's tracked lights (world space): one readback per frame
  const bool track = !rs->stereo_repeat_pass;
  // tiles: flux and emission position per tile
  if (track) {
    perf::Scope ps(perf::Group::EMLIGHTS_TILES);
    auto& sh = rs->shaders[ShaderId::EMISSIVE_TILES];
    sh.activate();
    ssao.bind_view_inputs(sh.id(), 1, 2, 3);
    bind_emissive(sh.id(), em_tex, em_samples, 4, 5);
    const GLuint sz_tex = ssao.sprite_depth_texture();
    glUniform1i(GLQ_LOC(kQueryBit, sh.id(), "sz_ss"), 10);
    glUniform1i(GLQ_LOC(kQueryBit, sh.id(), "sz_ms"), 11);
    glUniform1i(GLQ_LOC(kQueryBit, sh.id(), "sz_on"), sz_tex ? 1 : 0);
    glActiveTexture(GL_TEXTURE10);
    glBindTexture(GL_TEXTURE_2D, em_samples > 1 ? 0 : sz_tex);
    glActiveTexture(GL_TEXTURE11);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, em_samples > 1 ? sz_tex : 0);
    glUniform1i(GLQ_LOC(kQueryBit, sh.id(), "color_tex"), 0);
    glUniform2i(GLQ_LOC(kQueryBit, sh.id(), "tile_px"), tile_w, tile_h);
    glUniform1f(GLQ_LOC(kQueryBit, sh.id(), "near_slack"), kNearSlackM);
    glUniform1i(GLQ_LOC(kQueryBit, sh.id(), "split"), kSplit);
    const GLint part_units[4] = {6, 7, 8, 9};
    glUniform1iv(GLQ_LOC(kQueryBit, sh.id(), "part_tex[0]"), 4, part_units);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_resolve.tex[0]);
    const GLint stage_loc = GLQ_LOC(kQueryBit, sh.id(), "stage");
    if (Gfx::perf_opt(PERF_OPT_TILE_LIGHTS + 2)) {
      // kSplit^2 fragments per tile write partial sums; a second draw combines them
      glBindFramebuffer(GL_FRAMEBUFFER, m_part.fbo);
      glViewport(0, 0, kTilesX * kSplit, m_tiles_y * kSplit);
      glUniform1i(stage_loc, 1);
      glDrawArrays(GL_TRIANGLES, 0, 3);
      for (int i = 0; i < 4; i++) {
        glActiveTexture(GL_TEXTURE0 + part_units[i]);
        glBindTexture(GL_TEXTURE_2D, m_part.tex[i]);
      }
      glUniform1i(stage_loc, 2);
    } else {
      glUniform1i(stage_loc, 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, m_tiles.fbo);
    glViewport(0, 0, kTilesX, m_tiles_y);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glActiveTexture(GL_TEXTURE10);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE11);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);
    glActiveTexture(GL_TEXTURE0);
  }

  // select: the brightest tiles, in rank order (bit 2: on the CPU, from the tiles read back)
  SelectIn sel;
  sel.cpu = Gfx::perf_opt(PERF_OPT_TILE_LIGHTS + 2);
  sel.threshold = gs.emissive_lights_threshold;
  // tile footprint in m^2 per m^2 of depth
  sel.tile_area = std::abs(pi[0] * tile_w * pi[1] * tile_h);
  if (track && !sel.cpu) {
    perf::Scope ps(perf::Group::EMLIGHTS_TILES);
    glBindFramebuffer(GL_FRAMEBUFFER, m_cand.fbo);
    glViewport(0, 0, kMaxCand, 1);
    const float zero[4] = {0, 0, 0, 0};
    glClearBufferfv(GL_COLOR, 0, zero);
    glClearBufferfv(GL_COLOR, 1, zero);
    auto& sh = rs->shaders[ShaderId::EMISSIVE_SELECT];
    sh.activate();
    glUniform1i(GLQ_LOC(kQueryBit, sh.id(), "tile_flux"), 0);
    glUniform1i(GLQ_LOC(kQueryBit, sh.id(), "tile_pos"), 1);
    glUniform1i(GLQ_LOC(kQueryBit, sh.id(), "max_lights"), kMaxCand);
    glUniform1f(GLQ_LOC(kQueryBit, sh.id(), "threshold"), sel.threshold);
    glUniform1f(GLQ_LOC(kQueryBit, sh.id(), "tile_area"), sel.tile_area);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_tiles.tex[0]);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_tiles.tex[1]);
    glDrawArrays(GL_POINTS, 0, kTilesX * m_tiles_y);
  }

  // track: read the candidates back, match them to the light slots, upload the slots
  const bool async = Gfx::perf_opt(PERF_OPT_TILE_LIGHTS + 0);
  if (!async) {
    drop_readbacks();
  }
  float lpow[kMaxLights][4] = {}, lpos[kMaxLights][4] = {};
  float wpow[kMaxWorld][4] = {}, wpos[kMaxWorld][4] = {};
  {
    perf::Scope ps(perf::Group::EMLIGHTS_TRACK);
    Timer timer;
    if (track) {
      float flux[kMaxCand][4], pos[kMaxCand][4];
      CamSnap cam{};
      float tile_m = std::abs(pi[0]) * tile_w;
      bool have = false;
      if (async) {
        // this frame's row goes to a buffer; last frame's row is used, with last frame's camera
        Readback& prev = m_rb[1 - m_rb_head];
        const bool fresh = prev.fence && prev.frame + 1 == rs->frame_idx;
        issue_readback(m_rb[m_rb_head], rs, tile_m, sel);
        m_rb_head = 1 - m_rb_head;
        if (fresh && take_readback(prev, flux, pos)) {
          have = true;
          cam = prev.cam;
          tile_m = prev.tile_m;
        } else if (prev.fence) {
          glDeleteSync(prev.fence);
          prev.fence = nullptr;
        }
      } else {
        if (sel.cpu) {
          const int n = kTilesX * m_tiles_y;
          m_tile_rb.resize((size_t)n * 8);
          read_rows(true, m_tile_rb.data(), m_tile_rb.data() + 4 * n);
          select_tiles(m_tile_rb.data(), m_tile_rb.data() + 4 * n, n, sel.threshold,
                       sel.tile_area, flux, pos);
        } else {
          read_rows(false, flux, pos);
        }
        have = true;
        cam = snap(rs);
      }
      const auto& t = rs->camera_pos;
      const float cam_m[3] = {t.x() / kMeter, t.y() / kMeter, t.z() / kMeter};
      const float fwd[3] = {rs->camera_rot[0][2], rs->camera_rot[1][2], rs->camera_rot[2][2]};
      const bool cut = camera_cut(cam_m, fwd);
      if (cut) {
        m_tracker.reset();
        world_lights::reset_claims();
      }
      // a row read before a cut shows the old place: the world lights still update
      if (async && cut) {
        have = false;
        m_cands.clear();
      }
      if (have) {
        m_cands.clear();
        for (int i = 0; i < kMaxCand && flux[i][3] > 0.f; i++) {
          bool ok = true;
          for (int k = 0; k < 4; k++) {
            ok = ok && std::isfinite(flux[i][k]) && std::isfinite(pos[i][k]);
          }
          if (!ok) {
            continue;
          }
          LightTracker::Candidate c;
          to_world(cam, pos[i], c.pos);
          std::copy(flux[i], flux[i] + 3, c.power);
          c.fill = pos[i][3];
          m_cands.push_back(c);
        }
      }
      // world lights step every frame; only the tile slots wait for a readback
      {
        gather_world(rs, pi, w, h, m_world_in);
        LightTracker::Params tp;
        tp.blend = gs.emissive_lights_blend;
        tp.frames_in = tp.frames_out = std::max(gs.emissive_lights_frames, 1);
        tp.fade_frames = std::max(gs.emissive_lights_fade, 1);
        tp.suppress = gs.world_lights_suppress;
        tp.fill_lo = kFillLo;
        tp.fill_hi = kFillHi;
        std::vector<LightTracker::Emitter> emitters;
        for (const auto& e : world_lights::launch_lit()) {
          emitters.push_back({{e.pos_m[0], e.pos_m[1], e.pos_m[2]},
                              e.radius_m,
                              {e.colour[0], e.colour[1], e.colour[2]}});
        }
        tp.emitters = &emitters;
        m_tracker.update(m_cands, m_world_in, tile_m, cam_m, tp, have || cut);
      }
    }

    m_num_tiles = std::min((int)m_tracker.tiles().size(), (int)kMaxLights);
    for (int i = 0; i < m_num_tiles; i++) {
      const auto& o = m_tracker.tiles()[i];
      std::copy(o.power, o.power + 3, lpow[i]);
      lpow[i][3] = 1.f;
      to_cam(rs, o.pos, lpos[i]);
      lpos[i][3] = 1.f;
    }
    m_num_world = std::min((int)m_tracker.world().size(), (int)kMaxWorld);
    for (int i = 0; i < m_num_world; i++) {
      const auto& o = m_tracker.world()[i];
      std::copy(o.power, o.power + 3, wpow[i]);
      wpow[i][3] = o.added ? 1.5f : 1.f;
      const float wide = o.wide ? 0.25f : 0.f;  // x.25 / x.75: falloff r0 kWideR0M
      wpow[i][3] += wide;
      for (int m = 0; rs->shadow.active && m < rs->shadow.num_lamps; m++) {
        if (rs->shadow.lamp_added[m] > 0.5f && rs->shadow.lamp_row[m] == o.id) {
          wpow[i][3] = 2.f + (float)m + wide;
          if (g_burst_frame >= 0) {
            lg::info("EMLIGHTS f={} row {} shadowed by map {} in the tile pass", g_burst_frame,
                     o.id, m);
          }
        }
      }
      to_cam(rs, o.pos, wpos[i]);
      wpos[i][3] = o.radius;
    }
    auto upload = [](GLuint tex, int n, const float* data) {
      glBindTexture(GL_TEXTURE_2D, tex);
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, n, 1, GL_RGBA, GL_FLOAT, data);
    };
    glActiveTexture(GL_TEXTURE0);
    upload(m_lights.tex[0], kMaxLights, &lpow[0][0]);
    upload(m_lights.tex[1], kMaxLights, &lpos[0][0]);
    upload(m_world.tex[0], kMaxWorld, &wpow[0][0]);
    upload(m_world.tex[1], kMaxWorld, &wpos[0][0]);
    if (Gfx::perf_opt(PERF_OPT_TILE_LIGHTS + 1) && Gfx::perf_opt(PERF_OPT_TILE_LIGHTS + 3)) {
      build_cull(pi, lpos, m_num_tiles, std::max(gs.emissive_lights_radius, 0.01f), wpos, wpow,
                 m_num_world);
    }
    if (track) {
      m_track_cpu_ms = 0.9f * m_track_cpu_ms + 0.1f * (float)timer.getMs();
    }
    if (track && g_burst_frame >= 0) {
      log_frame(g_burst_frame, (int)m_cands.size());
    }
  }

  // light: additive over opaque pixels (stencil bit 7 clear), RGB only
  {
    perf::Scope ps(perf::Group::EMLIGHTS_LIGHT);
    glBindFramebuffer(GL_FRAMEBUFFER, rs->render_fb);
    glViewport(0, 0, w, h);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_ONE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glStencilFunc(GL_EQUAL, 0x00, 0x80);
    auto& sh = rs->shaders[ShaderId::EMISSIVE_LIGHT];
    sh.activate();
    ssao.bind_view_inputs(sh.id(), 1, 2, 3);
    ssao.bind_detail(sh.id(), 51, 52);  // kb/facts/renderer.md texture units: 51, 52
    glUniform1f(GLQ_LOC(kQueryBit, sh.id(), "relief"), 1.f + 2.f * gs.pbr_ambient);
    bind_emissive(sh.id(), em_tex, em_samples, 4, 5);
    glUniform1i(GLQ_LOC(kQueryBit, sh.id(), "color_tex"), 0);
    glUniform1i(GLQ_LOC(kQueryBit, sh.id(), "light_power"), 6);
    glUniform1i(GLQ_LOC(kQueryBit, sh.id(), "light_pos"), 7);
    glUniform1i(GLQ_LOC(kQueryBit, sh.id(), "num_lights"), kMaxLights);
    glUniform1i(GLQ_LOC(kQueryBit, sh.id(), "world_power"), 8);
    glUniform1i(GLQ_LOC(kQueryBit, sh.id(), "world_pos"), 9);
    glUniform1i(GLQ_LOC(kQueryBit, sh.id(), "num_world"), m_num_world);
    glUniform1f(GLQ_LOC(kQueryBit, sh.id(), "intensity"), gs.emissive_lights_intensity);
    glUniform1f(GLQ_LOC(kQueryBit, sh.id(), "radius"),
                std::max(gs.emissive_lights_radius, 0.01f));
    glUniform1f(GLQ_LOC(kQueryBit, sh.id(), "falloff"),
                std::max(gs.emissive_lights_falloff, 0.f));
    glUniform1f(GLQ_LOC(kQueryBit, sh.id(), "albedo_mix"), kAlbedoMix);
    glUniform1f(GLQ_LOC(kQueryBit, sh.id(), "max_add"), kMaxAdd);
    glUniform2f(GLQ_LOC(kQueryBit, sh.id(), "llh"),
                rs->light_material ? gs.light_highlights_strength : 0.f,
                std::max(gs.light_highlights_max, 0.01f));
    perf::set_opt_uniform(sh.id());
    static_assert(kMaxLights == 32 && kMaxWorld == 32, "emissive_light.frag list sizes");
    if (Gfx::perf_opt(PERF_OPT_TILE_LIGHTS + 1)) {
      glUniform4fv(GLQ_LOC(kQueryBit, sh.id(), "l_power"), kMaxLights, &lpow[0][0]);
      glUniform4fv(GLQ_LOC(kQueryBit, sh.id(), "l_pos"), kMaxLights, &lpos[0][0]);
      glUniform4fv(GLQ_LOC(kQueryBit, sh.id(), "w_power"), kMaxWorld, &wpow[0][0]);
      glUniform4fv(GLQ_LOC(kQueryBit, sh.id(), "w_pos"), kMaxWorld, &wpos[0][0]);
    }
    {
      float sg[3], rot[9];
      view_signs(rs, sg);
      for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
          rot[i * 3 + j] = rs->camera_rot[i][j];
        }
      }
      glUniformMatrix3fv(GLQ_LOC(kQueryBit, sh.id(), "cam_to_world"), 1, GL_TRUE, rot);
      glUniform3f(GLQ_LOC(kQueryBit, sh.id(), "cam_trans"), rs->camera_pos.x(), rs->camera_pos.y(),
                  rs->camera_pos.z());
      glUniform3f(GLQ_LOC(kQueryBit, sh.id(), "view_sign"), sg[0], sg[1], sg[2]);
      actor_shadows_set_uniforms(rs, sh.id(), false);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_resolve.tex[0]);
    glActiveTexture(GL_TEXTURE6);
    glBindTexture(GL_TEXTURE_2D, m_lights.tex[0]);
    glActiveTexture(GL_TEXTURE7);
    glBindTexture(GL_TEXTURE_2D, m_lights.tex[1]);
    glActiveTexture(GL_TEXTURE8);
    glBindTexture(GL_TEXTURE_2D, m_world.tex[0]);
    glActiveTexture(GL_TEXTURE9);
    glBindTexture(GL_TEXTURE_2D, m_world.tex[1]);
    glUniform1i(GLQ_LOC(kQueryBit, sh.id(), "cull_tex"), 10);
    glUniform1i(GLQ_LOC(kQueryBit, sh.id(), "cull_px"), m_cull_px);
    glActiveTexture(GL_TEXTURE10);
    glBindTexture(GL_TEXTURE_2D, m_cull_tex);
    if (rs->shadow.active && gs.shadow_lights > 0) {
      // characters (stencil bit 6) take the world lights from ActorShadows' local lights instead
      glUniform1i(GLQ_LOC(kQueryBit, sh.id(), "skip_added"), 0);
      glStencilFunc(GL_EQUAL, 0x00, 0xE0);
      glDrawArrays(GL_TRIANGLES, 0, 3);
      // merc level props (bit 5) take the lights the bake lacks from merc, albedo-modulated
      glUniform1i(GLQ_LOC(kQueryBit, sh.id(), "skip_added"), 1);
      glStencilFunc(GL_EQUAL, 0x20, 0xE0);
      glDrawArrays(GL_TRIANGLES, 0, 3);
      glUniform1i(GLQ_LOC(kQueryBit, sh.id(), "skip_added"), 0);
      glStencilFunc(GL_EQUAL, 0x40, 0xC0);
      glUniform1i(GLQ_LOC(kQueryBit, sh.id(), "num_world"), 0);
    }
    glDrawArrays(GL_TRIANGLES, 0, 3);
  }
  prof.add_draw_call(3);
  if (timing) {
    m_timer.end();
  }
}

void EmissiveLights::draw_debug_window() {
  auto& s = Gfx::g_global_settings;
  ImGui::Checkbox("Enable emissive tile lights (needs SSAO)", &s.emissive_lights_enable);
  ImGui::SliderFloat("Intensity", &s.emissive_lights_intensity, 0.f, 20.f);
  ImGui::SliderFloat("Radius (m)", &s.emissive_lights_radius, 0.5f, 20.f);
  ImGui::SliderFloat("Falloff exponent", &s.emissive_lights_falloff, 0.f, 6.f);
  ImGui::SliderFloat("Tile threshold", &s.emissive_lights_threshold, 0.f, 0.5f);
  ImGui::Text("total %.3f ms (SSAO GPU timers)", m_timer.last_avg_ms());
  ImGui::SliderFloat("Temporal blend (1 = none)", &s.emissive_lights_blend, 0.01f, 1.f);
  ImGui::SliderInt("Frames to show / hide", &s.emissive_lights_frames, 1, 10);
  ImGui::SliderInt("Fade frames", &s.emissive_lights_fade, 1, 30);
  ImGui::SliderFloat("World suppress (x radius)", &s.world_lights_suppress, 0.f, 2.f);
  ImGui::Checkbox("Highlights (needs material shading)", &s.light_highlights);
  ImGui::SliderFloat("Highlight strength", &s.light_highlights_strength, 0.f, 4.f);
  ImGui::SliderFloat("Highlight soft limit", &s.light_highlights_max, 0.1f, 4.f);
  ImGui::SliderFloat("Damp (enclosed levels)", &s.light_highlights_damp, 0.f, 1.f);
  ImGui::Text("tracked: %d candidates, %d slots live, %d drawn; readback + track %.3f ms",
              (int)m_cands.size(), m_tracker.live_slots(), m_num_tiles, m_track_cpu_ms);
  if (ImGui::Button("Reset tracked lights")) {
    m_tracker.reset();
    world_lights::reset_claims();
  }
  ImGui::Separator();
  ImGui::SliderInt("World lights (0 = off)", &s.world_lights_count, 0, kMaxWorld);
  ImGui::SliderFloat("Strong (vents, cells, crystals)", &s.world_lights_strong, 0.f, 0.5f);
  ImGui::SliderFloat("Lamps and torches", &s.world_lights_lamp, 0.f, 0.5f);
  ImGui::SliderFloat("Glow props (ice crystals)", &s.world_lights_glow, 0.f, 1.f);
  ImGui::Checkbox("Glow props and dark eco crystals (A/B)", &s.world_lights_props);
  ImGui::SliderFloat("Glow prop fade (m, 0 = 8 nearest)", &s.world_lights_glow_fade_m, 0.f, 150.f);
  ImGui::SliderInt("Glow prop slots (0 = all free)", &s.world_lights_glow_slots, 0, kMaxWorld);
  ImGui::SliderFloat("Weak (orbs, eco, crates)", &s.world_lights_weak, 0.f, 0.1f);
  ImGui::SliderFloat("Arcs", &s.world_lights_arc, 0.f, 0.05f);
  ImGui::SliderFloat("Particles", &s.world_lights_particle, 0.f, 0.5f);
  ImGui::SliderFloat("Radius scale", &s.world_lights_radius_scale, 0.1f, 3.f);
  ImGui::SliderFloat("Radius max (m)", &s.world_lights_radius_max, 0.5f, 20.f);
  ImGui::Checkbox("Eco shot lights", &s.eco_lights);
  ImGui::SliderFloat("Eco strength", &s.eco_lights_strength, 0.f, 3.f);
  ImGui::Text("eco shots and blasts listed: %d", world_lights::eco_count());
  ImGui::Text("table %d lights (compiled)", world_lights::size());
  ImGui::Text("this frame: %d drawn, %d on in loaded levels, %d asked of GOAL, %.3f ms",
              m_num_world, world_lights::last_lit(), world_lights::last_queries(), m_world_cpu_ms);
  if (ImGui::Button("Reset world light state")) {
    world_lights::reload();
  }
}
