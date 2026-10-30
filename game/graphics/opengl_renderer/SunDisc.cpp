#include "SunDisc.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "common/log/log.h"
#include "common/util/FileUtil.h"

#include "game/graphics/gfx.h"
#include "game/graphics/opengl_renderer/GlQueries.h"
#include "game/graphics/opengl_renderer/LevelHeightfield.h"
#include "game/graphics/opengl_renderer/Neighbours.h"
#include "game/graphics/opengl_renderer/SsaoRenderer.h"
#include "game/graphics/opengl_renderer/background/Unbake.h"
#include "game/graphics/opengl_renderer/background/background_common.h"

#include "third-party/stb_image/stb_image.h"

namespace {
// GL queries off the per-frame path under the bloom area's bit (GlQueries.h)
constexpr int kGlq = PERF_OPT_PRESENT2 + 1;
// texture units (kb/facts/renderer.md texture units): depth as 2D MS, depth as 2D, the visibility
constexpr int kUnitDepthMs = 46;
constexpr int kUnitDepth = 47;
constexpr int kUnitVis = 48;
// sky bodies: Jak 3's moon, sun glow, starburst, soft wash and star dot
constexpr int kUnitMoon = 51;
constexpr int kUnitGlow = 52;
constexpr int kUnitBurst = 53;
constexpr int kUnitSoft = 54;
constexpr int kUnitDot = 55;
constexpr float kPi = 3.14159265f;
// moon texture smoothing, Gaussian sigma in source texels: surface detail, limb
constexpr float kMoonDetail = 1.0f;
constexpr float kMoonLimb = 2.5f;
constexpr int kStars = 928;

float lum(const math::Vector3f& c) {
  return 0.2126f * c.x() + 0.7152f * c.y() + 0.0722f * c.z();
}

struct SunModel {
  math::Vector3f dir = math::Vector3f(0.f, -1.f, 0.f);  // toward the sun, world
  math::Vector3f col = math::Vector3f(1.f, 1.f, 1.f);   // light colour, max 1 (display encoded)
  float day = 0.f;     // palette weight of the day slots
};

/// The day slots of the sidecar blended by the hour: direction weighted by the light's luminance,
/// as unbake_sun_summary does, without the night slots (moon).
bool sun_model(const UnbakeSidecar& sc, const math::Vector<s32, 4> itimes[4], SunModel* out) {
  float w[8][3];
  unbake_slot_weights(itimes, w);
  math::Vector3f d(0, 0, 0), c(0, 0, 0);
  float day = 0.f;
  for (int s = 0; s < 8; s++) {
    if (!(sc.slot_mask & sc.day_mask & (1u << s))) {
      continue;
    }
    const float ws = (w[s][0] + w[s][1] + w[s][2]) / 3.f;
    math::Vector3f l;
    for (int k = 0; k < 3; k++) {
      l[k] = std::max(sc.slots[s].lgt[k], 0.f);
    }
    d += sc.slots[s].dir_fit.normalized() * (ws * lum(l));
    c += l * ws;
    day += ws;
  }
  const float mx = std::max(c.x(), std::max(c.y(), c.z()));
  if (d.length() < 1e-6f || mx <= 0.f) {
    return false;
  }
  out->dir = d.normalized();
  out->col = c / mx;
  out->day = std::clamp(day, 0.f, 1.f);
  return true;
}

/// The night slots of the sidecar blended by the hour (the moon): direction weighted by the
/// light's luminance, the palette's night weight and the blend's direct share of its light.
bool moon_model(const UnbakeSidecar& sc,
                const math::Vector<s32, 4> itimes[4],
                math::Vector3f* dir,
                float* weight,
                float* share) {
  float w[8][3];
  unbake_slot_weights(itimes, w);
  math::Vector3f d(0, 0, 0);
  float night = 0.f, sun = 0.f, total = 0.f;
  for (int s = 0; s < 8; s++) {
    if (!(sc.slot_mask & sc.night_mask & (1u << s))) {
      continue;
    }
    const float ws = (w[s][0] + w[s][1] + w[s][2]) / 3.f;
    math::Vector3f l, a;
    for (int k = 0; k < 3; k++) {
      l[k] = std::max(sc.slots[s].lgt[k], 0.f);
      a[k] = std::max(sc.slots[s].amb[k], 0.f);
    }
    d += sc.slots[s].dir_fit.normalized() * (ws * lum(l));
    night += ws;
    sun += ws * lum(l);
    total += ws * (lum(l) + lum(a));
  }
  if (d.length() < 1e-6f || night <= 0.f) {
    return false;
  }
  *dir = d.normalized();
  *weight = std::clamp(night, 0.f, 1.f);
  *share = total > 0.f ? sun / total : 0.f;
  return true;
}

/// The sidecar the sky passes read: the viewer level's, else the first port's with one.
const UnbakeSidecar* sky_sidecar(const SharedRenderState* rs,
                                 const TfragPcPortData* const ports[2],
                                 const TfragPcPortData** port_out,
                                 std::string* viewer_out) {
  std::string viewer = neighbours::viewer();
  const UnbakeSidecar* sc = nullptr;
  const TfragPcPortData* port = nullptr;
  for (int pass = 0; pass < 2 && !sc; pass++) {
    for (int i = 0; i < 2 && !sc; i++) {
      if (!ports[i] || (pass == 0 && !viewer.empty() && viewer != ports[i]->level_name)) {
        continue;
      }
      const auto* lev = rs->loader->get_tfrag3_level(ports[i]->level_name);
      if (lev && lev->unbake) {
        sc = lev->unbake.get();
        port = ports[i];
      }
    }
  }
  if (sc && viewer.empty()) {
    viewer = port->level_name;
  }
  *port_out = port;
  *viewer_out = viewer;
  return sc;
}

math::Vector3f slerp_ex(const math::Vector3f& a, const math::Vector3f& b, float t) {
  const float c = std::clamp(a.dot(b), -1.f, 1.f);
  const float th = std::acos(c);
  if (th < 1e-4f) {
    return a;
  }
  const float s = std::sin(th);
  return (a * (std::sin((1.f - t) * th) / s) + b * (std::sin(t * th) / s)).normalized();
}

/// The day slots placed at their peak hours, slerped by `hour`; past the first or last slot the
/// arc continues at the neighbouring pair's rate (sunrise, sunset).
bool sun_on_arc(const UnbakeSidecar& sc, const float slot_hour[8], float hour, SunModel* out) {
  int day[8], n = 0;
  for (int s = 0; s < 8; s++) {
    if ((sc.slot_mask & sc.day_mask & (1u << s)) && slot_hour[s] >= 0.f) {
      day[n++] = s;
    }
  }
  std::sort(day, day + n, [&](int a, int b) { return slot_hour[a] < slot_hour[b]; });
  if (n < 2) {
    return false;
  }
  int i = 0;
  while (i < n - 2 && hour > slot_hour[day[i + 1]]) {
    i++;
  }
  const int a = day[i], b = day[i + 1];
  const float span = std::max(slot_hour[b] - slot_hour[a], 1e-3f);
  const float t = std::clamp((hour - slot_hour[a]) / span, -2.f, 3.f);
  out->dir = slerp_ex(sc.slots[a].dir_fit.normalized(), sc.slots[b].dir_fit.normalized(), t);
  const float tc = std::clamp(t, 0.f, 1.f);
  math::Vector3f c;
  for (int k = 0; k < 3; k++) {
    c[k] = std::max(sc.slots[a].lgt[k] * (1.f - tc) + sc.slots[b].lgt[k] * tc, 0.f);
  }
  const float mx = std::max(c.x(), std::max(c.y(), c.z()));
  if (mx <= 0.f) {
    return false;
  }
  out->col = c / mx;
  return true;
}

/// Fog of a distant object at the game's fog end (the unified fog and haze included when this
/// frame draws them), and that end distance in metres.
float far_fog(const SharedRenderState* rs, float* end_m) {
  const auto& gs = Gfx::g_global_settings;
  const float pzw = rs->camera_persp[2][3];
  const float fog_min = rs->camera_fog.y(), fog_max = rs->camera_fog.z();
  const float hw = rs->camera_hvdf_off.w();
  // fog value is hvdf.w + pzw * z: fog_max at the start, fog_min at the end (vista_fog_camera)
  float end = pzw != 0.f ? (fog_min - hw) / pzw : 0.f;
  const float start = pzw != 0.f ? (fog_max - hw) / pzw : 0.f;
  end = std::max(end, start);
  *end_m = std::max(end / 4096.f, 50.f);
  float f = std::clamp((255.f - fog_min) / 255.f, 0.f, 1.f);
  if (rs->vista_frame_fog == VISTA_FOG_UNIFIED) {
    const float h = gs.vista_atmo_height_m;
    const float y = rs->camera_pos.y() / 4096.f;
    const float od = gs.vista_atmo_density_per_km / 1000.f *
                     std::max(*end_m - gs.vista_atmo_start_m, 0.f) *
                     std::exp(h > 0.f ? -std::max(y, 0.f) / h : 0.f);
    const float af = (1.f - std::exp(-od)) * f;
    const float hz =
        gs.vista_neighbours
            ? gs.vista_haze_max * (1.f - std::exp(-gs.vista_haze_density_per_km / 1000.f *
                                                  std::max(*end_m - gs.vista_haze_start_m, 0.f)))
            : 0.f;
    const float te = 1.f - (1.f - af) * (1.f - hz);
    f = gs.vista_fog_floor ? std::max(f, te) : te;
  }
  return std::clamp(f, 0.f, 0.9999f);
}

/// x in [0, 1] whose smoothstep is y (bloom's emissive ramp).
float inv_smoothstep(float y) {
  y = std::clamp(y, 0.f, 1.f);
  return 0.5f - std::sin(std::asin(1.f - 2.f * y) / 3.f);
}
}  // namespace

bool moon_disc_direction(const SharedRenderState* rs,
                         const TfragPcPortData* const ports[2],
                         math::Vector3f* dir,
                         float* weight,
                         float* share) {
  if (rs->version != GameVersion::Jak1 || !rs->loader) {
    return false;
  }
  const TfragPcPortData* port = nullptr;
  std::string viewer;
  const UnbakeSidecar* sc = sky_sidecar(rs, ports, &port, &viewer);
  return sc && heightfield_level_has_sky(viewer) &&
         moon_model(*sc, port->camera.itimes, dir, weight, share);
}

bool sun_disc_direction(const SharedRenderState* rs,
                        const TfragPcPortData* const ports[2],
                        neighbours::MoodTable* mood,
                        math::Vector3f* dir,
                        float* day,
                        float* hour_out) {
  if (rs->version != GameVersion::Jak1 || !rs->loader) {
    return false;
  }
  std::string viewer = neighbours::viewer();
  const UnbakeSidecar* sc = nullptr;
  const TfragPcPortData* port = nullptr;
  for (int pass = 0; pass < 2 && !sc; pass++) {
    for (int i = 0; i < 2 && !sc; i++) {
      if (!ports[i] || (pass == 0 && !viewer.empty() && viewer != ports[i]->level_name)) {
        continue;
      }
      const auto* lev = rs->loader->get_tfrag3_level(ports[i]->level_name);
      if (lev && lev->unbake) {
        sc = lev->unbake.get();
        port = ports[i];
      }
    }
  }
  if (!sc) {
    return false;
  }
  if (viewer.empty()) {
    viewer = port->level_name;
  }
  SunModel sun;
  if (!heightfield_level_has_sky(viewer) || !sun_model(*sc, port->camera.itimes, &sun)) {
    return false;
  }
  const auto& gs = Gfx::g_global_settings;
  float hour = -1.f;
  if (gs.vista_tod_addr && rs->ee_main_memory) {
    memcpy(&hour, (const u8*)rs->ee_main_memory + gs.vista_tod_addr, sizeof(float));
  }
  float slot_hour[8];
  const bool arc = hour >= 0.f && hour < 48.f && mood->slot_hours(port->level_name, slot_hour) &&
                   sun_on_arc(*sc, slot_hour, std::fmod(hour, 24.f), &sun);
  if (hour_out) {
    *hour_out = arc ? std::fmod(hour, 24.f) : -1.f;
  }
  *dir = sun.dir;
  *day = sun.day;
  return true;
}

SunDisc::~SunDisc() {
  if (m_vao) {
    glDeleteVertexArrays(1, &m_vao);
  }
  if (m_vis_fbo) {
    glDeleteFramebuffers(1, &m_vis_fbo);
    glDeleteTextures(1, &m_vis_tex);
  }
  if (m_body_fbo) {
    glDeleteFramebuffers(1, &m_body_fbo);
    glDeleteTextures(1, &m_body_vis);
  }
  for (GLuint* t : {&m_moon_tex, &m_glow_tex, &m_burst_tex, &m_soft_tex, &m_dot_tex}) {
    if (*t) {
      glDeleteTextures(1, t);
    }
  }
  if (m_star_vao) {
    glDeleteVertexArrays(1, &m_star_vao);
    glDeleteBuffers(1, &m_star_vbo);
  }
}

void SunDisc::render(SharedRenderState* rs,
                     ScopedProfilerNode& prof,
                     const TfragPcPortData* ports[2]) {
  const auto& gs = Gfx::g_global_settings;
  Gfx::g_global_settings.drawn_sky_bodies = 0;
  if (!gs.sun_disc || rs->version != GameVersion::Jak1 || !rs->has_pc_data || rs->stereo_active ||
      rs->eye.active || !rs->loader) {
    return;
  }

  // the viewer level's sidecar, else the first loaded one with a sidecar
  std::string viewer = neighbours::viewer();
  const UnbakeSidecar* sc = nullptr;
  const TfragPcPortData* port = nullptr;
  for (int pass = 0; pass < 2 && !sc; pass++) {
    for (int i = 0; i < 2 && !sc; i++) {
      if (!ports[i] || (pass == 0 && !viewer.empty() && viewer != ports[i]->level_name)) {
        continue;
      }
      const auto* lev = rs->loader->get_tfrag3_level(ports[i]->level_name);
      if (lev && lev->unbake) {
        sc = lev->unbake.get();
        port = ports[i];
      }
    }
  }
  if (!sc) {
    return;
  }
  if (viewer.empty()) {
    viewer = port->level_name;
  }
  if (gs.sky_style == 1 && bodies_ready()) {
    render_bodies(rs, prof, *sc, port, viewer);
    return;
  }
  SunModel sun;
  if (!heightfield_level_has_sky(viewer)) {
    return;
  }
  if (!sun_model(*sc, port->camera.itimes, &sun)) {
    return;
  }
  // smooth path by the game's hour; the palette blend above keeps the day weight
  float hour = -1.f;
  if (gs.vista_tod_addr && rs->ee_main_memory) {
    memcpy(&hour, (const u8*)rs->ee_main_memory + gs.vista_tod_addr, sizeof(float));
  }
  float slot_hour[8];
  const bool on_arc = hour >= 0.f && hour < 48.f &&
                      m_mood.slot_hours(port->level_name, slot_hour) &&
                      sun_on_arc(*sc, slot_hour, std::fmod(hour, 24.f), &sun);

  // SSAO camera space (Swamp.cpp mist_ray_uniforms) and its pixel mapping (SsaoRenderer)
  const int w = rs->render_fb_w, h = rs->render_fb_h;
  const auto* persp = rs->camera_persp;
  const float fogx = rs->camera_fog.x();
  const float pzw = std::abs(persp[2][3]);
  const float pxx = std::abs(fogx * persp[0][0] / 256.f);
  const float pyy = std::abs(fogx * persp[1][1] / 128.f * (512.f / 448.f));
  if (w <= 0 || h <= 0 || pxx <= 0.f || pyy <= 0.f || pzw <= 0.f) {
    return;
  }
  const float info[4] = {2.f * pzw / (w * pxx), 2.f * pzw / (h * pyy), -pzw / pxx, -pzw / pyy};
  const float sg[3] = {fogx * persp[0][0] >= 0.f ? -1.f : 1.f,
                       -fogx * persp[1][1] >= 0.f ? -1.f : 1.f, persp[2][3] >= 0.f ? -1.f : 1.f};
  const auto& r = rs->camera_rot;
  math::Vector3f cs;
  for (int k = 0; k < 3; k++) {
    cs[k] = sg[k] * (r[0][k] * sun.dir.x() + r[1][k] * sun.dir.y() + r[2][k] * sun.dir.z());
  }
  const float radius = 0.5f * gs.sun_disc_size_deg * kPi / 180.f;
  const bool ahead = cs.z() > std::sin(radius);
  const float cx = (cs.x() / cs.z() - info[2]) / info[0];
  const float cy = (cs.y() / cs.z() - info[3]) / info[1];
  const float px_per_rad = 1.f / (std::min(info[0], info[1]) * cs.z() * cs.z());
  const float r_px = radius * px_per_rad;
  const float half = 1.3f * r_px + 3.f;
  const bool in_frame =
      ahead && !(cx + half < 0.f || cx - half > w || cy + half < 0.f || cy - half > h);

  // haze along the sun ray: the fog of the fog end, thinned by the layer height over the ray
  float end_m = 0.f;
  const float f_end = far_fog(rs, &end_m);
  const float sin_el = sun.dir.y();
  const float hl = std::max(gs.vista_atmo_height_m, 50.f);
  const float path =
      hl / std::sqrt(std::max(sin_el, 0.f) * std::max(sin_el, 0.f) + (hl / end_m) * (hl / end_m));
  const float trans = std::pow(1.f - f_end, path / end_m);
  // added over the sky, which already carries the haze colour: what the fog pass gives a bright
  // distant object, sun * T + fog * (1 - T)
  const math::Vector3f col_enc = sun.col;
  math::Vector3f col_lin;
  for (int k = 0; k < 3; k++) {
    col_lin[k] = std::pow(std::max(col_enc[k], 0.f), 2.2f);
  }
  col_lin /= std::max(lum(col_lin), 1e-4f);
  auto smooth = [](float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
  };
  const float gain = trans * smooth(0.05f, 0.3f, sun.day) * smooth(-0.02f, 0.01f, sin_el);
  if (gain > 1e-4f) {
    Gfx::g_global_settings.drawn_sky_bodies |= 1;
  }
  const bool hdr = rs->hdr_target &&
                   (gs.sun_disc_hdr == 1 ||
                    (gs.sun_disc_hdr == 0 && Gfx::g_hdr_present.active && Gfx::g_hdr_present.hdr));
  const float core = hdr ? gs.sun_disc_core : gs.sun_disc_sdr_core;
  const float rim = hdr ? gs.sun_disc_rim : gs.sun_disc_sdr_rim;
  const float glare = rs->emissive_mask && gs.bloom_enable && gs.sun_disc_glare > 0.f
                          ? gs.ssao_emissive_lo + (gs.ssao_emissive_hi - gs.ssao_emissive_lo) *
                                                      inv_smoothstep(gs.sun_disc_glare)
                          : 0.f;

  if (rs->frame_idx >= m_log_frame) {
    m_log_frame = rs->frame_idx + 120;
    lg::info(
        "SUN-DISC: {} hour {:.2f} {} dir ({:.3f} {:.3f} {:.3f}) elev {:.1f} px ({:.1f} {:.1f}) of "
        "{}x{} r {:.1f}px "
        "day {:.2f} fog_end {:.0f}m f {:.3f} T {:.3f} gain {:.3f} col ({:.2f} {:.2f} {:.2f}) {} "
        "core {} rim {} mask {:.3f}{}",
        viewer, hour, on_arc ? "arc" : "palette", sun.dir.x(), sun.dir.y(), sun.dir.z(),
        std::asin(std::clamp(sin_el, -1.f, 1.f)) * 180.f / kPi, cx, h - cy, w, h, r_px, sun.day,
        end_m, f_end, trans, gain, col_enc.x(), col_enc.y(), col_enc.z(), hdr ? "hdr" : "sdr", core,
        rim, glare, in_frame ? "" : " (out of frame)");
  }
  if (!in_frame || gain <= 1e-4f) {
    return;
  }

  static glq::SavedState saved("sun-disc", kGlq, 0, 0);
  saved.capture();
  GLint depth_func = GL_GEQUAL;
  glGetIntegerv(GL_DEPTH_FUNC, &depth_func);
  if (!m_vao) {
    glGenVertexArrays(1, &m_vao);
  }
  glBindVertexArray(m_vao);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_STENCIL_TEST);
  glDepthMask(GL_FALSE);

  // visibility: share of sky taps over the disc, eased toward over a few frames
  if (!m_vis_fbo) {
    glGenFramebuffers(1, &m_vis_fbo);
    glGenTextures(1, &m_vis_tex);
    glBindTexture(GL_TEXTURE_2D, m_vis_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, 1, 1, 0, GL_RED, GL_HALF_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, m_vis_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_vis_tex, 0);
    m_last_frame = ~0ull;
  }
  const bool fresh = m_last_frame == ~0ull || rs->frame_idx > m_last_frame + 1;
  m_last_frame = rs->frame_idx;
  glBindFramebuffer(GL_FRAMEBUFFER, m_vis_fbo);
  const GLenum att0 = GL_COLOR_ATTACHMENT0;
  glDrawBuffers(1, &att0);
  glViewport(0, 0, 1, 1);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glDisable(GL_DEPTH_TEST);
  if (rs->render_fb_depth_tex) {
    glBindFramebuffer(GL_FRAMEBUFFER, rs->render_fb);
    GLint samples = 1;
    glGetIntegerv(GL_SAMPLES, &samples);
    glBindFramebuffer(GL_FRAMEBUFFER, m_vis_fbo);
    auto& sh = rs->shaders[ShaderId::SUN_VIS];
    sh.activate();
    const GLuint id = sh.id();
    glUniform1i(GLQ_LOC(kGlq, id, "depth_ms"), kUnitDepthMs);
    glUniform1i(GLQ_LOC(kGlq, id, "depth_ss"), kUnitDepth);
    glUniform1i(GLQ_LOC(kGlq, id, "ms"), samples > 1 ? 1 : 0);
    glUniform2f(GLQ_LOC(kGlq, id, "centre"), cx, cy);
    glUniform1f(GLQ_LOC(kGlq, id, "radius"), std::max(r_px, 1.f));
    glUniform2i(GLQ_LOC(kGlq, id, "size"), w, h);
    glActiveTexture(GL_TEXTURE0 + (samples > 1 ? kUnitDepthMs : kUnitDepth));
    glBindTexture(samples > 1 ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D, rs->render_fb_depth_tex);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    const float a = fresh ? 1.f : gs.sun_disc_fade;
    glBlendColor(0.f, 0.f, 0.f, a);
    glBlendFunc(GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindTexture(samples > 1 ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D, 0);
  } else {
    const float one[4] = {1.f, 0.f, 0.f, 1.f};
    glClearBufferfv(GL_COLOR, 0, one);
  }

  // the disc: sky samples only (depth still the clear value 0 under GEQUAL), additive
  glBindFramebuffer(GL_FRAMEBUFFER, rs->render_fb);
  glViewport(0, 0, w, h);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_GEQUAL);
  glEnable(GL_BLEND);
  glBlendEquation(GL_FUNC_ADD);
  glBlendFunc(GL_ONE, GL_ONE);
  // PS2 blending reads destination alpha: never write it
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
  if (glare > 0.f) {
    SsaoRenderer::emissive_draw_buffers(true, true, false);
  }
  auto& sh = rs->shaders[ShaderId::SUN_DISC];
  sh.activate();
  const GLuint id = sh.id();
  glUniform1i(GLQ_LOC(kGlq, id, "vis_tex"), kUnitVis);
  glUniform4f(GLQ_LOC(kGlq, id, "proj_info"), info[0], info[1], info[2], info[3]);
  glUniform3f(GLQ_LOC(kGlq, id, "sun_cs"), cs.x(), cs.y(), cs.z());
  glUniform1f(GLQ_LOC(kGlq, id, "radius"), radius);
  glUniform1f(GLQ_LOC(kGlq, id, "px_per_rad"), px_per_rad);
  glUniform2f(GLQ_LOC(kGlq, id, "lum"), core, rim);
  glUniform3f(GLQ_LOC(kGlq, id, "col_lin"), col_lin.x(), col_lin.y(), col_lin.z());
  glUniform1f(GLQ_LOC(kGlq, id, "gain"), gain);
  glUniform1f(GLQ_LOC(kGlq, id, "glare_mask"), glare);
  glUniform4f(GLQ_LOC(kGlq, id, "quad"), (cx + 0.f) / w * 2.f - 1.f, cy / h * 2.f - 1.f,
              half / w * 2.f, half / h * 2.f);
  glActiveTexture(GL_TEXTURE0 + kUnitVis);
  glBindTexture(GL_TEXTURE_2D, m_vis_tex);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glBindTexture(GL_TEXTURE_2D, 0);
  if (glare > 0.f) {
    SsaoRenderer::emissive_draw_buffers(false);
  }
  prof.add_draw_call(2);

  glDepthFunc(depth_func);
  saved.restore();
}

namespace {
struct Rng {
  u32 s;
  float next() {
    s = s * 1664525u + 1013904223u;
    return (s >> 8) * (1.f / 16777216.f);
  }
};

fs::path sky_tex_root() {
  if (const char* d = std::getenv("SANDOVER_DATA"); d && *d) {
    return fs::path(d) / "sky" / "jak3";
  }
  return file_util::get_jak_project_dir() / "sandover" / "data" / "sky" / "jak3";
}

/// Separable Gaussian of `sg` px over a W x H image of 4 floats per px, channels [c0, c1).
void gauss4(std::vector<float>& img, int W, int H, float sg, int c0, int c1) {
  const int rad = (int)std::ceil(3.f * sg);
  std::vector<float> g(2 * rad + 1), tmp(img.size());
  float gs = 0.f;
  for (int i = -rad; i <= rad; i++) {
    gs += g[i + rad] = std::exp(-0.5f * i * i / (sg * sg));
  }
  for (auto& v : g) {
    v /= gs;
  }
  for (int pass = 0; pass < 2; pass++) {
    const auto& from = pass ? tmp : img;
    auto& to = pass ? img : tmp;
    for (int y = 0; y < H; y++) {
      for (int x = 0; x < W; x++) {
        for (int c = c0; c < c1; c++) {
          float acc = 0.f;
          for (int i = -rad; i <= rad; i++) {
            const int xx = pass ? x : std::clamp(x + i, 0, W - 1);
            const int yy = pass ? std::clamp(y + i, 0, H - 1) : y;
            acc += from[(yy * W + xx) * 4 + c] * g[i + rad];
          }
          to[(y * W + x) * 4 + c] = acc;
        }
      }
    }
  }
}

/// RGBA upsampled 4x bilinearly into a transparent margin (the disc fills its square), colour
/// smoothed by `detail` source texels (normalised by the coverage, so no dark fringe) and the
/// coverage by `limb` texels: no texel edges, a soft limb. `pad`: size with margin over size.
std::vector<u8> soften_rgba(const std::vector<u8>& in,
                            int w,
                            int h,
                            float detail,
                            float limb,
                            int* ow,
                            int* oh,
                            float* pad) {
  const int k = 4, m = (int)std::ceil(3.f * limb * k), W = w * k + 2 * m, H = h * k + 2 * m;
  std::vector<float> a(W * H * 4, 0.f);
  auto src = [&](int x, int y, int c) {
    if (x < 0 || y < 0 || x >= w || y >= h) {
      return 0.f;
    }
    const float al = in[(y * w + x) * 4 + 3] / 255.f;
    return c == 3 ? al : in[(y * w + x) * 4 + c] / 255.f * al;
  };
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      const float fx = (x - m + 0.5f) / k - 0.5f, fy = (y - m + 0.5f) / k - 0.5f;
      const int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
      const float tx = fx - x0, ty = fy - y0;
      for (int c = 0; c < 4; c++) {
        a[(y * W + x) * 4 + c] =
            (src(x0, y0, c) * (1 - tx) + src(x0 + 1, y0, c) * tx) * (1 - ty) +
            (src(x0, y0 + 1, c) * (1 - tx) + src(x0 + 1, y0 + 1, c) * tx) * ty;
      }
    }
  }
  std::vector<float> cov = a;
  gauss4(a, W, H, detail * k, 0, 4);
  gauss4(cov, W, H, limb * k, 0, 4);
  std::vector<u8> out(W * H * 4);
  for (int i = 0; i < W * H; i++) {
    // past the detail blur's reach the limb takes the wide blur's colour
    const float al = a[i * 4 + 3], cl = cov[i * 4 + 3];
    const float t = std::clamp(al / 0.2f, 0.f, 1.f);
    for (int c = 0; c < 3; c++) {
      const float wide = cl > 1e-6f ? cov[i * 4 + c] / cl : 0.f;
      const float v = t * (al > 1e-6f ? a[i * 4 + c] / al : 0.f) + (1.f - t) * wide;
      out[i * 4 + c] = (u8)std::lround(255.f * std::clamp(v, 0.f, 1.f));
    }
    out[i * 4 + 3] = (u8)std::lround(255.f * std::clamp(cov[i * 4 + 3], 0.f, 1.f));
  }
  *ow = W;
  *oh = H;
  *pad = (float)W / (w * k);
  return out;
}

/// One sky texture from the data folder: RGBA, or one channel (red, or alpha with `alpha`)
/// scaled so its brightest texel is 1. PS2 alpha (128 = opaque) is doubled. 0 when missing.
GLuint load_sky_tex(const fs::path& dir,
                    const char* name,
                    int unit,
                    bool rgba,
                    bool alpha,
                    float detail = 0.f,
                    float limb = 0.f,
                    float* pad = nullptr) {
  int w = 0, h = 0, n = 0;
  u8* px = stbi_load((dir / name).string().c_str(), &w, &h, &n, 4);
  if (!px) {
    lg::warn("sky bodies: cannot read {}", (dir / name).string());
    return 0;
  }
  std::vector<u8> out;
  if (rgba) {
    out.assign(px, px + w * h * 4);
    for (int i = 0; i < w * h; i++) {
      out[i * 4 + 3] = (u8)std::min(255, out[i * 4 + 3] * 2);
    }
    if (detail > 0.f) {
      float pd = 1.f;
      out = soften_rgba(out, w, h, detail, limb, &w, &h, &pd);
      if (pad) {
        *pad = pd;
      }
    }
  } else {
    int mx = 1;
    for (int i = 0; i < w * h; i++) {
      mx = std::max<int>(mx, px[i * 4 + (alpha ? 3 : 0)]);
    }
    out.resize(w * h);
    for (int i = 0; i < w * h; i++) {
      out[i] = (u8)std::min(255, px[i * 4 + (alpha ? 3 : 0)] * 255 / mx);
    }
  }
  stbi_image_free(px);
  GLuint t = 0;
  glGenTextures(1, &t);
  glActiveTexture(GL_TEXTURE0 + unit);
  glBindTexture(GL_TEXTURE_2D, t);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, rgba ? GL_RGBA8 : GL_R8, w, h, 0, rgba ? GL_RGBA : GL_RED,
               GL_UNSIGNED_BYTE, out.data());
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glGenerateMipmap(GL_TEXTURE_2D);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
  const float zero[4] = {0.f, 0.f, 0.f, 0.f};
  glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, zero);
  glBindTexture(GL_TEXTURE_2D, 0);
  return t;
}
}  // namespace

bool SunDisc::bodies_ready() {
  if (!m_assets_tried) {
    GLint unit = GL_TEXTURE0, vao = 0, buf = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &unit);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &buf);
    m_assets_ok = make_body_assets();
    glActiveTexture(unit);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, buf);
    if (!m_assets_ok) {
      lg::warn("sky bodies: Jak 3 sky sprites missing, drawing the sun disc");
    }
  }
  return m_assets_ok;
}

bool SunDisc::make_body_assets() {
  m_assets_tried = true;
  // Jak 3's sky sprites (research/jak3-sky), read from the data folder, never from the repo
  const fs::path dir = sky_tex_root();
  m_moon_tex = load_sky_tex(dir, "full-moon.png", kUnitMoon, true, false, kMoonDetail, kMoonLimb,
                            &m_moon_pad);
  m_glow_tex = load_sky_tex(dir, "sky-glow-soft.png", kUnitGlow, false, false);
  m_burst_tex = load_sky_tex(dir, "glow.png", kUnitBurst, false, false);
  m_soft_tex = load_sky_tex(dir, "glow-soft.png", kUnitSoft, false, false);
  m_dot_tex = load_sky_tex(dir, "sky-hotdot.png", kUnitDot, false, true);

  // stars over the whole sphere (the field turns; the shader hides those below the horizon),
  // about 512 above 6 degrees below it; 16 brightness tiers, few bright
  std::vector<float> stars;
  Rng sr{0x0c0ffee5u};
  while ((int)stars.size() < kStars * 4) {
    const float y = sr.next() * 2.f - 1.f;
    const float a = sr.next() * 2.f * kPi;
    const float rxz = std::sqrt(std::max(1.f - y * y, 0.f));
    const int tier = std::min((int)(16.f * std::pow(sr.next(), 3.f)), 15);
    stars.insert(stars.end(),
                 {rxz * std::cos(a), y, rxz * std::sin(a), 0.04f * std::pow(1.32f, (float)tier)});
  }
  m_star_count = kStars;
  glGenVertexArrays(1, &m_star_vao);
  glGenBuffers(1, &m_star_vbo);
  glBindVertexArray(m_star_vao);
  glBindBuffer(GL_ARRAY_BUFFER, m_star_vbo);
  glBufferData(GL_ARRAY_BUFFER, stars.size() * sizeof(float), stars.data(), GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  return m_moon_tex && m_glow_tex && m_burst_tex && m_soft_tex && m_dot_tex;
}

void SunDisc::render_bodies(SharedRenderState* rs,
                            ScopedProfilerNode& prof,
                            const UnbakeSidecar& sc,
                            const TfragPcPortData* port,
                            const std::string& viewer) {
  const auto& gs = Gfx::g_global_settings;
  if (!heightfield_level_has_sky(viewer)) {
    return;
  }
  auto smooth = [](float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
  };

  // sun on the arc by the game hour (as the disc; at night below the horizon, no day weight),
  // moon at the night slots' fitted light
  SunModel sun;
  const bool have_sun = sun_model(sc, port->camera.itimes, &sun);
  float hour = -1.f;
  if (gs.vista_tod_addr && rs->ee_main_memory) {
    memcpy(&hour, (const u8*)rs->ee_main_memory + gs.vista_tod_addr, sizeof(float));
  }
  float slot_hour[8];
  const bool on_arc = hour >= 0.f && hour < 48.f &&
                      m_mood.slot_hours(port->level_name, slot_hour) &&
                      sun_on_arc(sc, slot_hour, std::fmod(hour, 24.f), &sun);
  math::Vector3f moon_dir(0, 1, 0);
  float night = 0.f, moon_share = 0.f;
  const bool have_moon = moon_model(sc, port->camera.itimes, &moon_dir, &night, &moon_share);

  const int w = rs->render_fb_w, h = rs->render_fb_h;
  const auto* persp = rs->camera_persp;
  const float fogx = rs->camera_fog.x();
  const float pzw = std::abs(persp[2][3]);
  const float pxx = std::abs(fogx * persp[0][0] / 256.f);
  const float pyy = std::abs(fogx * persp[1][1] / 128.f * (512.f / 448.f));
  if (w <= 0 || h <= 0 || pxx <= 0.f || pyy <= 0.f || pzw <= 0.f) {
    return;
  }
  const float info[4] = {2.f * pzw / (w * pxx), 2.f * pzw / (h * pyy), -pzw / pxx, -pzw / pyy};
  const float sg[3] = {fogx * persp[0][0] >= 0.f ? -1.f : 1.f,
                       -fogx * persp[1][1] >= 0.f ? -1.f : 1.f, persp[2][3] >= 0.f ? -1.f : 1.f};
  const auto& r = rs->camera_rot;
  auto to_cs = [&](const math::Vector3f& d) {
    math::Vector3f c;
    for (int k = 0; k < 3; k++) {
      c[k] = sg[k] * (r[0][k] * d.x() + r[1][k] * d.y() + r[2][k] * d.z());
    }
    return c;
  };
  struct Screen {
    math::Vector3f cs;
    float cx = 0.f, cy = 0.f, r_px = 0.f;
    bool in_frame = false;
  };
  auto place = [&](const math::Vector3f& dir, float radius) {
    Screen s;
    s.cs = to_cs(dir);
    if (s.cs.z() > std::sin(radius)) {
      s.cx = (s.cs.x() / s.cs.z() - info[2]) / info[0];
      s.cy = (s.cs.y() / s.cs.z() - info[3]) / info[1];
      s.r_px = radius / (std::min(info[0], info[1]) * s.cs.z() * s.cs.z());
      s.in_frame = !(s.cx + s.r_px < 0.f || s.cx - s.r_px > w || s.cy + s.r_px < 0.f ||
                     s.cy - s.r_px > h);
    }
    return s;
  };

  // haze: the disc's transmittance along a ray by its elevation
  float end_m = 0.f;
  const float f_end = far_fog(rs, &end_m);
  const float hl = std::max(gs.vista_atmo_height_m, 50.f);
  auto trans = [&](float sin_el) {
    const float s = std::max(sin_el, 0.f);
    const float path = hl / std::sqrt(s * s + (hl / end_m) * (hl / end_m));
    return std::pow(1.f - f_end, path / end_m);
  };
  const bool hdr = rs->hdr_target &&
                   (gs.sun_disc_hdr == 1 ||
                    (gs.sun_disc_hdr == 0 && Gfx::g_hdr_present.active && Gfx::g_hdr_present.hdr));

  const float sun_rad = 0.5f * gs.sun_disc_size_deg * kPi / 180.f;
  const float sun_el = sun.dir.y();
  const float sun_gain =
      have_sun ? smooth(0.05f, 0.3f, sun.day) * smooth(-0.02f, 0.01f, sun_el) : 0.f;
  const float sun_t = trans(sun_el);
  // colour by elevation: deep orange at the horizon, pale yellow toward the zenith (display units)
  const float ramp_t = 1.f - std::pow(1.f - std::clamp(sun_el / 0.94f, 0.f, 1.f), 2.f);
  math::Vector3f ramp;
  const float lo[3] = {1.f, 0.38f, 0.14f}, hi[3] = {1.f, 1.f, 0.5f};
  for (int k = 0; k < 3; k++) {
    ramp[k] = lo[k] + (hi[k] - lo[k]) * ramp_t;
  }
  math::Vector3f glow_lin;
  for (int k = 0; k < 3; k++) {
    const float e = 0.5f * ramp[k] + 0.5f * (have_sun ? sun.col[k] : 1.f);
    glow_lin[k] = std::pow(std::max(e, 0.f), 2.2f);
  }
  glow_lin /= std::max(lum(glow_lin), 1e-4f);
  const math::Vector3f burst_col(0.3f * ramp.x(), 0.3f * ramp.y(), 0.15f * ramp.z());
  const math::Vector3f wash_col(0.31f, 0.16f, 0.f);
  const float glare = rs->emissive_mask && gs.bloom_enable && gs.sun_disc_glare > 0.f
                          ? (gs.ssao_emissive_lo + (gs.ssao_emissive_hi - gs.ssao_emissive_lo) *
                                                       inv_smoothstep(gs.sun_disc_glare)) *
                                sun_t
                          : 0.f;

  const float moon_rad = 0.5f * gs.sky_moon_deg * kPi / 180.f;
  const float moon_el = moon_dir.y();
  const float moon_gain =
      have_moon ? smooth(0.05f, 0.3f, night) * smooth(-0.02f, 0.01f, moon_el) : 0.f;
  const float moon_t = trans(moon_el);
  const float star_gain = have_moon ? gs.sky_stars * smooth(0.4f, 0.9f, night) *
                                          (1.f - smooth(0.05f, 0.3f, have_sun ? sun.day : 0.f)) *
                                          (hdr ? 1.f : 0.6f)
                                    : 0.f;

  const Screen ss = place(sun.dir, 3.f * sun_rad), ms = place(moon_dir, moon_rad);
  // the game's own sun and stars give way only while ours draw (time-of-day.gc)
  Gfx::g_global_settings.drawn_sky_bodies = (sun_gain > 1e-4f ? 1 : 0) | (star_gain > 1e-4f ? 2 : 0);
  if (rs->frame_idx >= m_body_log) {
    m_body_log = rs->frame_idx + 120;
    lg::info(
        "SKY-BODIES: {} hour {:.2f} {} sun dir ({:.3f} {:.3f} {:.3f}) elev {:.1f} px ({:.1f} "
        "{:.1f}) gain {:.3f} T {:.3f} | moon dir ({:.3f} {:.3f} {:.3f}) elev {:.1f} px ({:.1f} "
        "{:.1f}) r {:.1f}px night {:.2f} gain {:.3f} T {:.3f} | stars {:.3f} {} {}x{}",
        viewer, hour, on_arc ? "arc" : "palette", sun.dir.x(), sun.dir.y(), sun.dir.z(),
        std::asin(std::clamp(sun_el, -1.f, 1.f)) * 180.f / kPi, ss.cx, h - ss.cy, sun_gain, sun_t,
        moon_dir.x(), moon_dir.y(), moon_dir.z(),
        std::asin(std::clamp(moon_el, -1.f, 1.f)) * 180.f / kPi, ms.cx, h - ms.cy, ms.r_px, night,
        moon_gain, moon_t, star_gain, hdr ? "hdr" : "sdr", w, h);
  }
  if (sun_gain <= 1e-4f && moon_gain <= 1e-4f && star_gain <= 1e-4f) {
    return;
  }

  static glq::SavedState saved("sky-bodies", kGlq, 0, 0);
  saved.capture();
  GLint depth_func = GL_GEQUAL;
  glGetIntegerv(GL_DEPTH_FUNC, &depth_func);
  if (!m_vao) {
    glGenVertexArrays(1, &m_vao);
  }
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_STENCIL_TEST);
  glDepthMask(GL_FALSE);

  // visibility of the sun and the moon, each eased as the disc's; held while off screen
  if (!m_body_fbo) {
    glGenFramebuffers(1, &m_body_fbo);
    glGenTextures(1, &m_body_vis);
    glBindTexture(GL_TEXTURE_2D, m_body_vis);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, 2, 1, 0, GL_RED, GL_HALF_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, m_body_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_body_vis, 0);
    const GLenum att = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &att);
    const float one[4] = {1.f, 0.f, 0.f, 1.f};
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearBufferfv(GL_COLOR, 0, one);
    m_body_last[0] = m_body_last[1] = ~0ull;
  }
  glBindFramebuffer(GL_FRAMEBUFFER, rs->render_fb);
  GLint samples = 1;
  glGetIntegerv(GL_SAMPLES, &samples);
  glBindFramebuffer(GL_FRAMEBUFFER, m_body_fbo);
  const GLenum att0 = GL_COLOR_ATTACHMENT0;
  glDrawBuffers(1, &att0);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glDisable(GL_DEPTH_TEST);
  glBindVertexArray(m_vao);
  const Screen* bodies[2] = {&ss, &ms};
  const float body_r[2] = {sun_rad, moon_rad};
  const float body_gain[2] = {sun_gain, moon_gain};
  for (int b = 0; b < 2; b++) {
    if (body_gain[b] <= 1e-4f || !bodies[b]->in_frame || !rs->render_fb_depth_tex) {
      continue;
    }
    const bool fresh = m_body_last[b] == ~0ull || rs->frame_idx > m_body_last[b] + 1;
    m_body_last[b] = rs->frame_idx;
    glViewport(b, 0, 1, 1);
    auto& sh = rs->shaders[ShaderId::SUN_VIS];
    sh.activate();
    const GLuint id = sh.id();
    glUniform1i(GLQ_LOC(kGlq, id, "depth_ms"), kUnitDepthMs);
    glUniform1i(GLQ_LOC(kGlq, id, "depth_ss"), kUnitDepth);
    glUniform1i(GLQ_LOC(kGlq, id, "ms"), samples > 1 ? 1 : 0);
    glUniform2f(GLQ_LOC(kGlq, id, "centre"), bodies[b]->cx, bodies[b]->cy);
    const float zz = bodies[b]->cs.z() * bodies[b]->cs.z();
    const float rp = body_r[b] / (std::min(info[0], info[1]) * zz);
    glUniform1f(GLQ_LOC(kGlq, id, "radius"), std::max(rp, 1.f));
    glUniform2i(GLQ_LOC(kGlq, id, "size"), w, h);
    glActiveTexture(GL_TEXTURE0 + (samples > 1 ? kUnitDepthMs : kUnitDepth));
    glBindTexture(samples > 1 ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D, rs->render_fb_depth_tex);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendColor(0.f, 0.f, 0.f, fresh ? 1.f : gs.sun_disc_fade);
    glBlendFunc(GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindTexture(samples > 1 ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D, 0);
  }

  // sky samples only (depth still the clear value 0 under GEQUAL), additive
  glBindFramebuffer(GL_FRAMEBUFFER, rs->render_fb);
  glViewport(0, 0, w, h);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_GEQUAL);
  glEnable(GL_BLEND);
  glBlendEquation(GL_FUNC_ADD);
  glBlendFunc(GL_ONE, GL_ONE);
  // PS2 blending reads destination alpha: never write it
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
  const float log_t = std::log(std::max(1.f - f_end, 1e-4f));
  const math::Vector3f up_cs = to_cs(math::Vector3f(0, 1, 0));

  if (star_gain > 1e-4f) {
    // the sky turns 15 degrees an hour about a tilted pole
    const float ang = (hour >= 0.f ? std::fmod(hour, 24.f) : 0.f) * kPi / 12.f;
    const math::Vector3f ax = math::Vector3f(0.3f, 0.9f, 0.3f).normalized();
    const float c = std::cos(ang), s = std::sin(ang), t = 1.f - c;
    const float x = ax.x(), y = ax.y(), z = ax.z();
    const float m[3][3] = {{t * x * x + c, t * x * y - s * z, t * x * z + s * y},
                           {t * x * y + s * z, t * y * y + c, t * y * z - s * x},
                           {t * x * z - s * y, t * y * z + s * x, t * z * z + c}};
    float rot[9], wcs[9];  // column major
    for (int i = 0; i < 3; i++) {
      for (int j = 0; j < 3; j++) {
        rot[j * 3 + i] = m[i][j];
        wcs[j * 3 + i] = sg[i] * r[j][i];
      }
    }
    auto& sh = rs->shaders[ShaderId::SKY_STARS];
    sh.activate();
    const GLuint id = sh.id();
    glUniformMatrix3fv(GLQ_LOC(kGlq, id, "star_rot"), 1, GL_FALSE, rot);
    glUniformMatrix3fv(GLQ_LOC(kGlq, id, "world_cs"), 1, GL_FALSE, wcs);
    glUniform4f(GLQ_LOC(kGlq, id, "proj_info"), info[0], info[1], info[2], info[3]);
    glUniform2f(GLQ_LOC(kGlq, id, "size"), (float)w, (float)h);
    glUniform1f(GLQ_LOC(kGlq, id, "point_px"), std::max(3.f, 3.f * h / 1080.f));
    glUniform1i(GLQ_LOC(kGlq, id, "dot_tex"), kUnitDot);
    glActiveTexture(GL_TEXTURE0 + kUnitDot);
    glBindTexture(GL_TEXTURE_2D, m_dot_tex);
    glUniform1f(GLQ_LOC(kGlq, id, "gain"), star_gain);
    glUniform3f(GLQ_LOC(kGlq, id, "haze"), log_t, hl, end_m);
    glEnable(GL_PROGRAM_POINT_SIZE);
    glBindVertexArray(m_star_vao);
    glDrawArrays(GL_POINTS, 0, m_star_count);
    glDisable(GL_PROGRAM_POINT_SIZE);
    glBindTexture(GL_TEXTURE_2D, 0);
    prof.add_draw_call(1);
  }

  if (sun_gain > 1e-4f || moon_gain > 1e-4f) {
    if (glare > 0.f) {
      SsaoRenderer::emissive_draw_buffers(true, true, false);
    }
    // camera yaw turns the starburst, as Jak 3's
    const float yaw = std::atan2(r[0][2], r[2][2]);
    auto& sh = rs->shaders[ShaderId::SKY_BODY];
    sh.activate();
    const GLuint id = sh.id();
    const float core = hdr ? gs.sun_disc_core : gs.sun_disc_sdr_core;
    const float glow = hdr ? gs.sky_glow : gs.sky_glow_sdr;
    const float moon_l = hdr ? gs.sky_moon : gs.sky_moon_sdr;
    const float bs = gs.sky_burst, ws = gs.sky_wash;
    glUniform1i(GLQ_LOC(kGlq, id, "vis_tex"), kUnitVis);
    glUniform1i(GLQ_LOC(kGlq, id, "moon_tex"), kUnitMoon);
    glUniform1i(GLQ_LOC(kGlq, id, "glow_tex"), kUnitGlow);
    glUniform1i(GLQ_LOC(kGlq, id, "burst_tex"), kUnitBurst);
    glUniform1i(GLQ_LOC(kGlq, id, "soft_tex"), kUnitSoft);
    glUniform4f(GLQ_LOC(kGlq, id, "proj_info"), info[0], info[1], info[2], info[3]);
    glUniform3f(GLQ_LOC(kGlq, id, "up_cs"), up_cs.x(), up_cs.y(), up_cs.z());
    glUniform3f(GLQ_LOC(kGlq, id, "haze"), log_t, hl, end_m);
    glUniform3f(GLQ_LOC(kGlq, id, "sun_cs"), ss.cs.x(), ss.cs.y(), ss.cs.z());
    glUniform1f(GLQ_LOC(kGlq, id, "sun_r"), sun_rad);
    glUniform4f(GLQ_LOC(kGlq, id, "sun_lum"), core, glow, sun_t, sun_gain);
    glUniform3f(GLQ_LOC(kGlq, id, "sun_lin"), glow_lin.x(), glow_lin.y(), glow_lin.z());
    glUniform3f(GLQ_LOC(kGlq, id, "burst_col"), burst_col.x() * bs, burst_col.y() * bs,
                burst_col.z() * bs);
    glUniform3f(GLQ_LOC(kGlq, id, "wash_col"), wash_col.x() * ws, wash_col.y() * ws,
                wash_col.z() * ws);
    glUniform1f(GLQ_LOC(kGlq, id, "burst_rot"), yaw);
    glUniform1f(GLQ_LOC(kGlq, id, "glare_mask"), glare);
    glUniform3f(GLQ_LOC(kGlq, id, "moon_cs"), ms.cs.x(), ms.cs.y(), ms.cs.z());
    glUniform1f(GLQ_LOC(kGlq, id, "moon_r"), moon_rad);
    glUniform1f(GLQ_LOC(kGlq, id, "moon_pad"), m_moon_pad);
    glUniform3f(GLQ_LOC(kGlq, id, "moon_lum"), moon_l, moon_t, moon_gain);
    glUniform3f(GLQ_LOC(kGlq, id, "halo_col"), 0.1f, 0.2f, 0.4f);
    glUniform3f(GLQ_LOC(kGlq, id, "moon_wash_col"), 0.03f * ws, 0.05f * ws, 0.1f * ws);
    glUniform4f(GLQ_LOC(kGlq, id, "quad"), 0.f, 0.f, 1.f, 1.f);
    glUniform1i(GLQ_LOC(kGlq, id, "flare_pass"), 0);
    glActiveTexture(GL_TEXTURE0 + kUnitVis);
    glBindTexture(GL_TEXTURE_2D, m_body_vis);
    const std::pair<int, GLuint> texs[4] = {
        {kUnitMoon, m_moon_tex}, {kUnitGlow, m_glow_tex}, {kUnitBurst, m_burst_tex}, {kUnitSoft, m_soft_tex}};
    for (const auto& [unit, tex] : texs) {
      glActiveTexture(GL_TEXTURE0 + unit);
      glBindTexture(GL_TEXTURE_2D, tex);
    }
    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    for (const auto& [unit, tex] : texs) {
      glActiveTexture(GL_TEXTURE0 + unit);
      glBindTexture(GL_TEXTURE_2D, 0);
    }
    glActiveTexture(GL_TEXTURE0 + kUnitVis);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (glare > 0.f) {
      SsaoRenderer::emissive_draw_buffers(false);
    }
    prof.add_draw_call(1);
  }

  if (sun_gain > 1e-4f && (gs.sky_burst > 0.f || gs.sky_wash > 0.f)) {
    // the flare pass's state: the sprites keep the sun ray's haze, not each pixel's
    auto& f = m_flare;
    f.frame = rs->frame_idx;
    std::copy(info, info + 4, f.info);
    for (int k = 0; k < 3; k++) {
      f.up_cs[k] = up_cs[k];
      f.sun_cs[k] = ss.cs[k];
      f.burst[k] = burst_col[k] * gs.sky_burst;
      f.wash[k] = wash_col[k] * gs.sky_wash;
    }
    f.rot = std::atan2(r[0][2], r[2][2]);
    // an off-screen sun's visibility is stale: fade the flare out over the starburst's radius
    float on = 0.f;
    if (ss.cs.z() > 0.05f) {
      const float dx = std::max({-ss.cx, ss.cx - w, 0.f}), dy = std::max({-ss.cy, ss.cy - h, 0.f});
      const float burst_px = std::tan(7.5f * kPi / 180.f) / std::min(info[0], info[1]);
      on = 1.f - smooth(0.f, burst_px, std::sqrt(dx * dx + dy * dy));
    }
    f.gain = sun_gain * std::pow(sun_t, 0.35f) * on;
  }

  glBindVertexArray(0);
  glDepthFunc(depth_func);
  saved.restore();
}

void SunDisc::render_flare(SharedRenderState* rs, ScopedProfilerNode& prof) {
  const auto& f = m_flare;
  if (f.frame != rs->frame_idx || f.gain <= 1e-4f || !m_assets_ok || !m_body_fbo) {
    return;
  }
  static glq::SavedState saved("sky-flare", kGlq, 0, 0);
  saved.capture();
  glBindFramebuffer(GL_FRAMEBUFFER, rs->render_fb);
  glViewport(0, 0, rs->render_fb_w, rs->render_fb_h);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  glEnable(GL_BLEND);
  glBlendEquation(GL_FUNC_ADD);
  glBlendFunc(GL_ONE, GL_ONE);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
  auto& sh = rs->shaders[ShaderId::SKY_BODY];
  sh.activate();
  const GLuint id = sh.id();
  glUniform1i(GLQ_LOC(kGlq, id, "flare_pass"), 1);
  glUniform1i(GLQ_LOC(kGlq, id, "vis_tex"), kUnitVis);
  glUniform1i(GLQ_LOC(kGlq, id, "burst_tex"), kUnitBurst);
  glUniform1i(GLQ_LOC(kGlq, id, "soft_tex"), kUnitSoft);
  glUniform4f(GLQ_LOC(kGlq, id, "proj_info"), f.info[0], f.info[1], f.info[2], f.info[3]);
  glUniform3f(GLQ_LOC(kGlq, id, "up_cs"), f.up_cs[0], f.up_cs[1], f.up_cs[2]);
  glUniform3f(GLQ_LOC(kGlq, id, "sun_cs"), f.sun_cs[0], f.sun_cs[1], f.sun_cs[2]);
  glUniform4f(GLQ_LOC(kGlq, id, "sun_lum"), 0.f, 0.f, 1.f, f.gain);
  glUniform3f(GLQ_LOC(kGlq, id, "burst_col"), f.burst[0], f.burst[1], f.burst[2]);
  glUniform3f(GLQ_LOC(kGlq, id, "wash_col"), f.wash[0], f.wash[1], f.wash[2]);
  glUniform1f(GLQ_LOC(kGlq, id, "burst_rot"), f.rot);
  glUniform3f(GLQ_LOC(kGlq, id, "moon_lum"), 0.f, 0.f, 0.f);
  glUniform4f(GLQ_LOC(kGlq, id, "quad"), 0.f, 0.f, 1.f, 1.f);
  const std::pair<int, GLuint> texs[3] = {
      {kUnitVis, m_body_vis}, {kUnitBurst, m_burst_tex}, {kUnitSoft, m_soft_tex}};
  for (const auto& [unit, tex] : texs) {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, tex);
  }
  if (!m_vao) {
    glGenVertexArrays(1, &m_vao);
  }
  glBindVertexArray(m_vao);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glBindVertexArray(0);
  for (const auto& [unit, tex] : texs) {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, 0);
  }
  prof.add_draw_call(1);
  saved.restore();
}
