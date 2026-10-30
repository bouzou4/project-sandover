#include "MotionVectors.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "common/util/exr.h"

#include "game/graphics/gfx.h"
#include "game/graphics/opengl_renderer/GlQueries.h"
#include "game/graphics/opengl_renderer/PerfTimers.h"
#include "game/graphics/opengl_renderer/SsaoRenderer.h"

#include "fmt/format.h"
#include "third-party/imgui/imgui.h"

namespace motion_vectors {
namespace {

std::atomic<bool> g_cut_pending{false};
std::atomic<bool> g_chain_cut{false};  // latched with the chain the render thread draws next

std::mutex g_dump_mutex;
std::string g_dump_dir;
int g_dump_frames = 0;
u64 g_dump_serial = 0;  // per request: every eye's renderer takes it once

FrameState g_frame;
MercStats g_merc_stats, g_merc_shown;

// pass uniforms, looked up once per program (programs are never relinked)
struct PassLocs {
  GLuint prog = 0;
  GLint depth_ms, depth_ss, obj_ms, obj_ss, samples, reproj, sky, size, wk, valid, w_tol, aux_on,
      rel_on;
};
struct ViewLocs {
  GLuint prog = 0;
  GLint vel, aux, cur, prev, mode, size, out_size, scale, gain;
};

using Mat4d = std::array<std::array<double, 4>, 4>;  // [row][col]
using Mat3d = std::array<std::array<double, 3>, 3>;

constexpr double kScissorAdjust = 512.0 / 448.0;  // Jak 1 SCISSOR_ADJUST * HEIGHT_SCALE

/// Camera space (world minus the camera position, game units) to the GL clip space every
/// background draw uses (make_new_cam_mat and the tfrag shaders), y scissor-adjusted.
Mat4d cam_to_clip(const math::Vector4f rot[4],
                  const math::Vector4f persp[4],
                  float fog,
                  float hvdf_z) {
  const double pzw = persp[2][3];
  const double sc[3] = {fog * (double)persp[0][0] / 256.0, -fog * (double)persp[1][1] / 128.0,
                        fog * (double)persp[2][2] / 8388608.0 + (hvdf_z / 8388608.0 - 1.0) * pzw};
  double res[4][4] = {};  // make_new_cam_mat: res[j] is the shader's column j
  for (int r = 0; r < 3; r++) {
    for (int c = 0; c < 3; c++) {
      res[r][c] = rot[r][c] * sc[c];
    }
    res[r][3] = rot[r][2] * pzw;
  }
  res[3][2] = fog * (double)persp[3][2] / 8388608.0;
  Mat4d m{};
  for (int i = 0; i < 4; i++) {
    for (int j = 0; j < 4; j++) {
      m[i][j] = -res[j][i];
    }
  }
  for (int j = 0; j < 4; j++) {
    m[1][j] *= kScissorAdjust;
  }
  return m;
}

Mat4d mul(const Mat4d& a, const Mat4d& b) {
  Mat4d r{};
  for (int i = 0; i < 4; i++) {
    for (int j = 0; j < 4; j++) {
      for (int k = 0; k < 4; k++) {
        r[i][j] += a[i][k] * b[k][j];
      }
    }
  }
  return r;
}

bool invert(const Mat4d& m, Mat4d* out) {
  // Gauss-Jordan with partial pivoting
  double a[4][8];
  for (int i = 0; i < 4; i++) {
    for (int j = 0; j < 4; j++) {
      a[i][j] = m[i][j];
      a[i][j + 4] = i == j ? 1.0 : 0.0;
    }
  }
  for (int c = 0; c < 4; c++) {
    int p = c;
    for (int r = c + 1; r < 4; r++) {
      if (std::abs(a[r][c]) > std::abs(a[p][c])) {
        p = r;
      }
    }
    if (std::abs(a[p][c]) < 1e-30) {
      return false;
    }
    for (int j = 0; j < 8; j++) {
      std::swap(a[c][j], a[p][j]);
    }
    const double d = a[c][c];
    for (int j = 0; j < 8; j++) {
      a[c][j] /= d;
    }
    for (int r = 0; r < 4; r++) {
      if (r != c) {
        const double f = a[r][c];
        for (int j = 0; j < 8; j++) {
          a[r][j] -= f * a[c][j];
        }
      }
    }
  }
  for (int i = 0; i < 4; i++) {
    for (int j = 0; j < 4; j++) {
      (*out)[i][j] = a[i][j + 4];
    }
  }
  return true;
}

/// Rows x, y, w and columns 0-2 of a cam_to_clip matrix: directions to clip (x, y, w).
Mat3d dir_rows(const Mat4d& m) {
  Mat3d r;
  const int rows[3] = {0, 1, 3};
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) {
      r[i][j] = m[rows[i]][j];
    }
  }
  return r;
}

bool invert3(const Mat3d& m, Mat3d* out) {
  const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
                     m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                     m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
  if (std::abs(det) < 1e-30) {
    return false;
  }
  const double id = 1.0 / det;
  (*out)[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) * id;
  (*out)[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * id;
  (*out)[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * id;
  (*out)[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) * id;
  (*out)[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * id;
  (*out)[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * id;
  (*out)[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) * id;
  (*out)[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * id;
  (*out)[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * id;
  return true;
}

void to_gl(const Mat4d& m, float* out) {
  for (int c = 0; c < 4; c++) {
    for (int r = 0; r < 4; r++) {
      out[c * 4 + r] = (float)m[r][c];
    }
  }
}

/// Angle (degrees) between the camera rotations' forward axes and up axes, the larger.
float turn_deg(const math::Vector4f a[4], const math::Vector4f b[4]) {
  float worst = 0.f;
  for (int axis : {1, 2}) {
    double dot = 0, la = 0, lb = 0;
    for (int r = 0; r < 3; r++) {
      dot += (double)a[r][axis] * b[r][axis];
      la += (double)a[r][axis] * a[r][axis];
      lb += (double)b[r][axis] * b[r][axis];
    }
    const double c = std::clamp(dot / std::sqrt(std::max(la * lb, 1e-30)), -1.0, 1.0);
    worst = std::max(worst, (float)(std::acos(c) * 57.29577951308232));
  }
  return worst;
}

void make_target(GLuint& fbo, GLuint& tex, int w, int h, GLint ifmt, GLenum fmt, GLenum type,
                 GLint filter) {
  glGenFramebuffers(1, &fbo);
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexImage2D(GL_TEXTURE_2D, 0, ifmt, w, h, 0, fmt, type, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
  const GLenum draw = GL_COLOR_ATTACHMENT0;
  glDrawBuffers(1, &draw);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    lg::error("motion vectors: incomplete framebuffer ({}x{})", w, h);
  }
}

}  // namespace

bool wanted() {
  const auto& gs = Gfx::g_global_settings;
  return gs.mv_enable || gs.mb_level > 0;
}

void game_cut() {
  g_cut_pending = true;
}

void latch_cut_for_chain() {
  g_chain_cut = g_cut_pending.exchange(false);
}

void request_dump(const std::string& dir, int frames) {
  std::lock_guard<std::mutex> lk(g_dump_mutex);
  g_dump_dir = dir;
  g_dump_frames = std::clamp(frames, 0, 64);
  g_dump_serial++;
}

const FrameState& frame() {
  return g_frame;
}

void count_merc(bool matched, bool keyed) {
  if (matched) {
    g_merc_stats.matched++;
  } else if (keyed) {
    g_merc_stats.missed++;
  } else {
    g_merc_stats.unkeyed++;
  }
}

Renderer::~Renderer() {
  release();
  if (m_vao) {
    glDeleteVertexArrays(1, &m_vao);
  }
}

bool Renderer::frame_valid() const {
  return g_frame.active && g_frame.valid;
}

void Renderer::release_relative() {
  if (!m_rel.fbo) {
    return;
  }
  if (m_vel.fbo) {
    GLint draw = 0, read = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
    glBindFramebuffer(GL_FRAMEBUFFER, m_vel.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, read);
  }
  glDeleteFramebuffers(1, &m_rel.fbo);
  glDeleteTextures(1, &m_rel.tex);
  m_rel = Target();
  m_rel_wanted = false;
  m_rel_written = false;
}

void Renderer::release() {
  for (Target* t : {&m_vel, &m_aux, &m_rel, &m_colour, &m_history}) {
    if (t->fbo) {
      glDeleteFramebuffers(1, &t->fbo);
      glDeleteTextures(1, &t->tex);
    }
    *t = Target();
  }
  if (m_obj) {
    glDeleteTextures(1, &m_obj);
    m_obj = 0;
  }
  m_obj_w = m_obj_h = m_obj_samples = 0;
  m_w = m_h = 0;
  m_have_history = false;
}

void Renderer::ensure_targets(int w, int h, bool hdr) {
  if (!m_vao) {
    glGenVertexArrays(1, &m_vao);
  }
  if (w == m_w && h == m_h && hdr == m_hdr && m_vel.fbo) {
    return;
  }
  // a resize runs before the frame's buckets: they must not start on these targets
  GLint old_draw = 0, old_read = 0;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_draw);
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read);
  for (Target* t : {&m_vel, &m_aux, &m_rel, &m_colour, &m_history}) {
    if (t->fbo) {
      glDeleteFramebuffers(1, &t->fbo);
      glDeleteTextures(1, &t->tex);
    }
    *t = Target();
  }
  make_target(m_vel.fbo, m_vel.tex, w, h, GL_RG16F, GL_RG, GL_HALF_FLOAT, GL_NEAREST);
  make_target(m_aux.fbo, m_aux.tex, w, h, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, GL_NEAREST);
  // m_vel's framebuffer also writes the aux target (draw buffer 1) when it is wanted
  glBindFramebuffer(GL_FRAMEBUFFER, m_vel.fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, m_aux.tex, 0);
  const GLint cf = hdr ? GL_RGBA16F : GL_RGBA8;
  const GLenum ct = hdr ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE;
  make_target(m_colour.fbo, m_colour.tex, w, h, cf, GL_RGBA, ct, GL_LINEAR);
  make_target(m_history.fbo, m_history.tex, w, h, cf, GL_RGBA, ct, GL_LINEAR);
  glBindTexture(GL_TEXTURE_2D, 0);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, old_draw);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, old_read);
  m_w = w;
  m_h = h;
  m_hdr = hdr;
  m_have_history = false;
  lg::info("motion vectors: allocated targets {}x{}", w, h);
}

void Renderer::ensure_object_target(SharedRenderState* rs, int samples) {
  const int w = rs->render_fb_w, h = rs->render_fb_h;
  const GLenum target = samples > 1 ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D;
  if (m_obj_w != w || m_obj_h != h || m_obj_samples != samples || !m_obj) {
    if (m_obj) {
      glDeleteTextures(1, &m_obj);
    }
    glGenTextures(1, &m_obj);
    glBindTexture(target, m_obj);
    if (samples > 1) {
      glTexImage2DMultisample(target, samples, GL_RGBA16F, w, h, GL_TRUE);
    } else {
      glTexImage2D(target, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
      glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, 0);
    }
    glBindTexture(target, 0);
    m_obj_w = w;
    m_obj_h = h;
    m_obj_samples = samples;
  }
  // the render framebuffer is recreated on resize and ids are reused, so ask it
  GLint have = 0;
  glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT5,
                                        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &have);
  if ((GLuint)have != m_obj) {
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT5, target, m_obj, 0);
    const auto status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
      static bool logged = false;
      if (!logged) {
        lg::error("motion vectors: object target leaves the framebuffer incomplete (0x{:x})", status);
        logged = true;
      }
      glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT5, GL_TEXTURE_2D, 0, 0);
      return;
    }
  }
  m_obj_fb = rs->render_fb;
}

void Renderer::detach_object_target(SharedRenderState* rs) {
  if (!m_obj_fb) {
    return;
  }
  GLint prev = 0;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, rs->render_fb);
  if ((GLuint)rs->render_fb == m_obj_fb) {
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT5, GL_TEXTURE_2D, 0, 0);
  }
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev);
  m_obj_fb = 0;
}

void Renderer::begin_frame(SharedRenderState* rs,
                           bool camera_primed,
                           bool blackout,
                           bool wanted,
                           bool force_cut) {
  const auto& gs = Gfx::g_global_settings;
  // stereo: only for a per-eye temporal pass (motion blur stays off there)
  const bool asked = rs->stereo_active ? (gs.mv_enable || wanted)
                                       : (gs.mv_enable || wanted || motion_vectors::wanted());
  const bool on = asked && rs->version == GameVersion::Jak1 && rs->render_fb != 0 &&
                  rs->render_fb_depth_tex != 0;
  m_resolved = false;
  m_dumped_stem.clear();
  m_rel_written = false;
  g_frame.active = false;
  g_frame.valid = false;
  if (!on) {
    if (m_obj_fb || m_vel.fbo) {
      detach_object_target(rs);
      release();
      lg::info("motion vectors: off, targets freed");
    }
    m_have_prev = false;
    return;
  }
  {
    std::lock_guard<std::mutex> lk(g_dump_mutex);
    // stereo: the first eye takes the request, the second starts on the same display frame
    static int taken_frames = 0;
    static u64 taken_serial = 0;
    static u64 taken_tick = 0;
    const bool first = !rs->stereo_active || rs->eye_slot == 0;
    if (first && g_dump_frames > 0) {
      taken_frames = g_dump_frames;
      taken_serial = g_dump_serial;
      taken_tick = rs->display_tick;
      g_dump_frames = 0;
    }
    if (taken_frames > 0 && taken_serial != m_dump_serial &&
        (first || taken_tick == rs->display_tick)) {
      m_dump_serial = taken_serial;
      m_dump_left = taken_frames;
      m_dump_dir = g_dump_dir;
      m_dump_index = 0;
      file_util::create_dir_if_needed(m_dump_dir);
      lg::info("motion vectors: dumping {} frames to {}", m_dump_left, m_dump_dir);
    }
  }
  g_merc_shown = g_merc_stats;
  g_merc_stats = MercStats();

  const int w = rs->render_fb_w, h = rs->render_fb_h;
  const char* cut = nullptr;
  if (!camera_primed) {
    cut = "no-camera";
  } else if (!m_have_prev) {
    cut = "first";
  } else if (m_prev_frame + 1 != rs->display_tick) {
    cut = "gap";
  } else if (w != m_prev_w || h != m_prev_h) {
    cut = "resize";
  } else if (rs->stereo_active != m_prev_stereo) {
    cut = "stereo";  // same-size toggle: never reproject across the eye offset
  } else if (blackout || m_prev_blackout) {
    cut = "blackout";
  } else if (g_chain_cut && rs->game_frame_new) {
    cut = "game";
  } else if (force_cut) {
    cut = "other-eye";
  }
  m_last_move_m = 0.f;
  m_last_turn_deg = 0.f;
  if (camera_primed && m_have_prev) {
    const auto d = rs->camera_pos - m_prev_trans;
    m_last_move_m = std::sqrt(d.x() * d.x() + d.y() * d.y() + d.z() * d.z()) / 4096.f;
    m_last_turn_deg = turn_deg(rs->camera_rot, m_prev_rot);
    const float fov = std::abs(rs->camera_persp[0][0] / std::max(std::abs(m_prev_persp[0][0]),
                                                               1e-6f));
    if (!cut && m_last_move_m > gs.mv_cut_move_m) {
      cut = "jump";
    } else if (!cut && m_last_turn_deg > gs.mv_cut_turn_deg) {
      cut = "turn";
    } else if (!cut && (fov > 1.15f || fov < 1.f / 1.15f)) {
      cut = "fov";
    }
  }
  bool valid = cut == nullptr;
  if (camera_primed) {
    const Mat4d cur =
        cam_to_clip(rs->camera_rot, rs->camera_persp, rs->camera_fog.x(), rs->camera_hvdf_off.z());
    // w from device depth: clip z and w are proportional on v, plus a constant on z
    int j = 0;
    for (int k = 1; k < 3; k++) {
      if (std::abs(cur[3][k]) > std::abs(cur[3][j])) {
        j = k;
      }
    }
    m_w_k1 = (float)(cur[2][j] / cur[3][j]);
    m_w_k0 = (float)cur[2][3];
    g_frame.w_k0 = m_w_k0;
    g_frame.w_k1 = m_w_k1;
    // half field of view from the clip rows' direction parts (x, y against w)
    auto len3 = [&](int r) {
      return std::sqrt(cur[r][0] * cur[r][0] + cur[r][1] * cur[r][1] + cur[r][2] * cur[r][2]);
    };
    if (len3(0) > 0.0 && len3(1) > 0.0) {
      g_frame.tan_half_x = (float)(len3(3) / len3(0));
      g_frame.tan_half_y = (float)(len3(3) / len3(1));
      m_w_per_unit = len3(3);
    }
    Mat4d inv;
    if (m_have_prev && invert(cur, &inv)) {
      const Mat4d prev = cam_to_clip(m_prev_rot, m_prev_persp, m_prev_fog.x(), m_prev_hvdf.z());
      Mat4d shift{};
      for (int i = 0; i < 4; i++) {
        shift[i][i] = 1.0;
      }
      for (int i = 0; i < 3; i++) {
        shift[i][3] = (double)rs->camera_pos[i] - (double)m_prev_trans[i];
      }
      to_gl(mul(mul(prev, shift), inv), m_reproj);
      Mat3d kinv, h3{};
      const Mat3d kp = dir_rows(prev);
      if (invert3(dir_rows(cur), &kinv)) {
        for (int r = 0; r < 3; r++) {
          for (int c = 0; c < 3; c++) {
            for (int k = 0; k < 3; k++) {
              h3[r][c] += kp[r][k] * kinv[k][c];
            }
          }
        }
      }
      for (int c = 0; c < 3; c++) {
        for (int r = 0; r < 3; r++) {
          m_sky[c * 3 + r] = (float)h3[r][c];
        }
      }
      // SANDOVER_MV_STATIC=1 (dev): no camera motion in the vectors, a negative control
      static const bool no_camera = std::getenv("SANDOVER_MV_STATIC") != nullptr;
      if (no_camera) {
        for (int i = 0; i < 16; i++) {
          m_reproj[i] = (i % 5 == 0) ? 1.f : 0.f;
        }
        for (int i = 0; i < 9; i++) {
          m_sky[i] = (i % 4 == 0) ? 1.f : 0.f;
        }
      }
    } else {
      valid = false;
      cut = cut ? cut : "no-inverse";
    }
    for (int i = 0; i < 4; i++) {
      m_prev_rot[i] = rs->camera_rot[i];
      m_prev_persp[i] = rs->camera_persp[i];
    }
    m_prev_trans = rs->camera_pos;
    m_prev_hvdf = rs->camera_hvdf_off;
    m_prev_fog = rs->camera_fog;
    m_have_prev = true;
  } else {
    m_have_prev = false;
  }
  if (!valid) {
    for (int i = 0; i < 16; i++) {
      m_reproj[i] = (i % 5 == 0) ? 1.f : 0.f;
    }
    m_cuts++;
    m_cut_reason = cut;
    if (gs.mv_log_cuts && (m_invalid_run == 0 || std::strcmp(cut, m_run_reason) != 0)) {
      lg::info("MV cut f={} reason={} move={:.2f}m turn={:.1f}deg", rs->frame_idx, cut,
               m_last_move_m, m_last_turn_deg);
    }
    m_run_reason = cut;
    m_invalid_run++;
  } else if (m_invalid_run) {
    if (gs.mv_log_cuts) {
      lg::info("MV valid f={} after {} invalid frames", rs->frame_idx, m_invalid_run);
    }
    m_invalid_run = 0;
  }
  m_prev_frame = rs->display_tick;
  m_prev_w = w;
  m_prev_h = h;
  m_prev_blackout = blackout;
  m_prev_stereo = rs->stereo_active;

  // object velocity target at colour attachment 5, cleared (z = 0: no object)
  GLint prev_draw = 0;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, rs->render_fb);
  GLint samples = 1;
  glGetIntegerv(GL_SAMPLES, &samples);
  ensure_object_target(rs, std::max(samples, 1));
  if (m_obj_fb) {
    const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);
    SsaoRenderer::set_velocity_drawing(true);
    glColorMaski(5, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    const float zero[4] = {0.f, 0.f, 0.f, 0.f};
    glClearBufferfv(GL_COLOR, 5, zero);
    SsaoRenderer::set_velocity_drawing(false);
    if (scissor) {
      glEnable(GL_SCISSOR_TEST);
    }
  }
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev_draw);
  ensure_targets(w, h, rs->hdr_target);

  g_frame.active = m_obj_fb != 0;
  g_frame.valid = valid;
  g_frame.obj_tex = g_frame.active ? m_obj : 0;
  g_frame.obj_samples = m_obj_samples;
  std::copy(std::begin(m_reproj), std::end(m_reproj), g_frame.cam_reproj);
  g_frame.fb_w = w;
  g_frame.fb_h = h;
}

void Renderer::copy_colour(SharedRenderState* rs, Target& dst) {
  glBindFramebuffer(GL_READ_FRAMEBUFFER, rs->render_fb);
  glReadBuffer(GL_COLOR_ATTACHMENT0);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst.fbo);
  glBlitFramebuffer(0, 0, m_w, m_h, 0, 0, m_w, m_h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
}

void Renderer::resolve(SharedRenderState* rs, ScopedProfilerNode& prof) {
  if (!g_frame.active) {
    return;
  }
  const auto& gs = Gfx::g_global_settings;
  // the program, VAO, active unit and textures here change every frame: never cached
  static glq::SavedState saved("motion-vectors", glq::kLive, 0b11, 0b1100);
  saved.capture();
  const bool views = gs.mv_view != VIEW_NONE;
  const bool want_aux = views || m_dump_left > 0;
  const bool want_colour = (views && gs.mv_view != VIEW_VELOCITY) || m_dump_left > 0;

  glBindVertexArray(m_vao);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_BLEND);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glViewport(0, 0, m_w, m_h);

  if (want_colour) {
    copy_colour(rs, m_colour);
  }

  const bool want_rel = m_rel_wanted && g_frame.valid;
  if (want_rel && !m_rel.fbo) {
    // the motion blur input, made on first use: m_vel's draw buffer 2
    make_target(m_rel.fbo, m_rel.tex, m_w, m_h, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, m_vel.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, m_rel.tex, 0);
  }
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_vel.fbo);
  const GLenum bufs[3] = {GL_COLOR_ATTACHMENT0, want_aux ? GL_COLOR_ATTACHMENT1 : GL_NONE,
                          want_rel ? GL_COLOR_ATTACHMENT2 : GL_NONE};
  glDrawBuffers(3, bufs);

  auto& sh = rs->shaders[ShaderId::MOTION_VECTORS];
  sh.activate();
  static PassLocs L;
  if (L.prog != sh.id()) {
    const GLuint p = sh.id();
    L.prog = p;
    L.depth_ms = glGetUniformLocation(p, "depth_ms");
    L.depth_ss = glGetUniformLocation(p, "depth_ss");
    L.obj_ms = glGetUniformLocation(p, "obj_ms");
    L.obj_ss = glGetUniformLocation(p, "obj_ss");
    L.samples = glGetUniformLocation(p, "samples");
    L.reproj = glGetUniformLocation(p, "reproj");
    L.sky = glGetUniformLocation(p, "sky");
    L.size = glGetUniformLocation(p, "size");
    L.wk = glGetUniformLocation(p, "wk");
    L.valid = glGetUniformLocation(p, "valid");
    L.w_tol = glGetUniformLocation(p, "w_tol");
    L.aux_on = glGetUniformLocation(p, "aux_on");
    L.rel_on = glGetUniformLocation(p, "rel_on");
  }
  const bool ms = m_obj_samples > 1;
  glUniform1i(L.depth_ss, 0);
  glUniform1i(L.obj_ss, 1);
  glUniform1i(L.depth_ms, 2);
  glUniform1i(L.obj_ms, 3);
  glUniform1i(L.samples, ms ? m_obj_samples : 1);
  glUniformMatrix4fv(L.reproj, 1, GL_FALSE, m_reproj);
  glUniformMatrix3fv(L.sky, 1, GL_FALSE, m_sky);
  glUniform2f(L.size, (float)m_w, (float)m_h);
  glUniform2f(L.wk, m_w_k0, m_w_k1);
  glUniform1i(L.valid, g_frame.valid ? 1 : 0);
  glUniform1f(L.w_tol, gs.mv_object_w_tol);
  glUniform1i(L.aux_on, want_aux ? 1 : 0);
  glUniform1i(L.rel_on, want_rel ? (m_rel_camera ? 2 : 1) : 0);
  glActiveTexture(GL_TEXTURE0 + (ms ? 2 : 0));
  glBindTexture(ms ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D, rs->render_fb_depth_tex);
  glActiveTexture(GL_TEXTURE0 + (ms ? 3 : 1));
  glBindTexture(ms ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D, m_obj);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  prof.add_draw_call();
  glActiveTexture(GL_TEXTURE0 + (ms ? 2 : 0));
  glBindTexture(ms ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D, 0);
  glActiveTexture(GL_TEXTURE0 + (ms ? 3 : 1));
  glBindTexture(ms ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D, 0);
  const GLenum one = GL_COLOR_ATTACHMENT0;
  glDrawBuffers(1, &one);
  m_resolved = true;
  m_aux_written = want_aux;
  m_rel_written = want_rel;

  if (m_dump_left > 0) {
    dump(rs);
  }
  saved.restore();
}

void Renderer::draw_view(SharedRenderState* rs, ScopedProfilerNode& prof) {
  const auto& gs = Gfx::g_global_settings;
  const bool error = gs.mv_view == VIEW_ERROR || gs.mv_view == VIEW_ERROR_CAMERA;
  const bool keep_history = error || m_dump_left > 0;
  if (!m_resolved) {
    m_have_history = false;
    return;
  }
  if (gs.mv_view != VIEW_NONE && (!error || m_have_history)) {
    static glq::SavedState saved("motion-vectors-view", glq::kLive, 0b1111, 0);
    saved.capture();
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, rs->render_fb);
    GLint db[7];
    for (int i = 0; i < 7; i++) {
      glGetIntegerv(GL_DRAW_BUFFER0 + i, &db[i]);
    }
    const GLenum one = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &one);
    glBindVertexArray(m_vao);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    // after an upscale render_fb is the display framebuffer
    glViewport(0, 0, rs->render_fb_w, rs->render_fb_h);
    auto& sh = rs->shaders[ShaderId::MOTION_VECTORS_VIEW];
    sh.activate();
    static ViewLocs L;
    if (L.prog != sh.id()) {
      const GLuint p = sh.id();
      L.prog = p;
      L.vel = glGetUniformLocation(p, "vel");
      L.aux = glGetUniformLocation(p, "aux");
      L.cur = glGetUniformLocation(p, "cur");
      L.prev = glGetUniformLocation(p, "prev");
      L.mode = glGetUniformLocation(p, "mode");
      L.size = glGetUniformLocation(p, "size");
      L.out_size = glGetUniformLocation(p, "out_size");
      L.scale = glGetUniformLocation(p, "scale");
      L.gain = glGetUniformLocation(p, "gain");
    }
    glUniform1i(L.vel, 0);
    glUniform1i(L.aux, 1);
    glUniform1i(L.cur, 2);
    glUniform1i(L.prev, 3);
    glUniform1i(L.mode, gs.mv_view);
    glUniform2f(L.size, (float)m_w, (float)m_h);
    glUniform2f(L.out_size, (float)rs->render_fb_w, (float)rs->render_fb_h);
    glUniform1f(L.scale, std::max(gs.mv_view_scale, 0.01f));
    glUniform1f(L.gain, gs.mv_error_gain);
    const GLuint tex[4] = {m_vel.tex, m_aux.tex, m_colour.tex, m_history.tex};
    for (int i = 0; i < 4; i++) {
      glActiveTexture(GL_TEXTURE0 + i);
      glBindTexture(GL_TEXTURE_2D, tex[i]);
    }
    glDrawArrays(GL_TRIANGLES, 0, 3);
    prof.add_draw_call();
    GLenum restore[7];
    for (int i = 0; i < 7; i++) {
      restore[i] = (GLenum)db[i];
    }
    glDrawBuffers(7, restore);
    saved.restore();
  }
  if (keep_history) {
    std::swap(m_colour, m_history);
    m_have_history = true;
  } else {
    m_have_history = false;
  }
}

void Renderer::dump(SharedRenderState* rs) {
  const int w = m_w, h = m_h;
  std::vector<float> px((size_t)w * h * 4);
  auto read = [&](GLuint fbo, GLenum att) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glReadBuffer(att);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_FLOAT, px.data());
  };
  const std::string base =
      rs->stereo_active ? fmt::format("{}/mv_{:03d}_e{}", m_dump_dir, m_dump_index, rs->eye_slot)
                        : fmt::format("{}/mv_{:03d}", m_dump_dir, m_dump_index);
  exr::StringAttrs attrs;
  attrs["frame"] = std::to_string(rs->frame_idx);
  attrs["valid"] = g_frame.valid ? "1" : "0";
  attrs["cut"] = g_frame.valid ? "" : m_cut_reason;
  attrs["samples"] = std::to_string(m_obj_samples);
  attrs["convention"] = "v = p_prev - p_cur, render-target pixels, GL texel grid (+y up)";
  read(m_colour.fbo, GL_COLOR_ATTACHMENT0);
  exr::write_rgb_half(base + "_colour.exr", px.data(), w, h, 4, true, attrs);
  // RG16F reads back as (r, g, 0, 1); b = the per-pixel source from aux
  std::vector<float> aux((size_t)w * h * 4);
  read(m_aux.fbo, GL_COLOR_ATTACHMENT0);
  aux = px;
  read(m_vel.fbo, GL_COLOR_ATTACHMENT0);
  for (size_t i = 0; i < (size_t)w * h; i++) {
    px[i * 4 + 2] = aux[i * 4 + 3];
  }
  attrs["content"] = "r vx, g vy (px), b source (0 camera, 1 object, 2 sky, 3 invalid)";
  exr::write_rgb_half(base + "_vel.exr", px.data(), w, h, 4, true, attrs);
  attrs["content"] = "r vx, g vy camera-only (px), b clip w / 4096";
  exr::write_rgb_half(base + "_aux.exr", aux.data(), w, h, 4, true, attrs);
  file_util::write_text_file(
      base + ".json",
      fmt::format("{{\"frame\": {}, \"valid\": {}, \"cut\": \"{}\", \"w\": {}, \"h\": {}, "
                  "\"samples\": {}, \"move_m\": {:.4f}, \"turn_deg\": {:.3f}, "
                  "\"merc_matched\": {}, \"merc_missed\": {}, \"merc_unkeyed\": {}, "
                  "\"tick\": {}, \"eye\": {}, \"game_frame_new\": {}, \"w_per_unit\": {:.9g}}}\n",
                  rs->frame_idx, g_frame.valid ? "true" : "false",
                  g_frame.valid ? "" : m_cut_reason, w, h, m_obj_samples, m_last_move_m,
                  m_last_turn_deg, g_merc_shown.matched, g_merc_shown.missed,
                  g_merc_shown.unkeyed, rs->display_tick, rs->stereo_active ? rs->eye_slot : -1,
                  rs->game_frame_new ? "true" : "false", m_w_per_unit));
  lg::info("motion vectors: dumped frame {} as {}", rs->frame_idx, base);
  m_dumped_stem = base;
  m_dump_index++;
  m_dump_left--;
}

void Renderer::draw_debug_window() {
  auto& gs = Gfx::g_global_settings;
  ImGui::Checkbox("Enable", &gs.mv_enable);
  ImGui::RadioButton("No view", &gs.mv_view, VIEW_NONE);
  ImGui::RadioButton("Velocity", &gs.mv_view, VIEW_VELOCITY);
  ImGui::RadioButton("Reprojection error", &gs.mv_view, VIEW_ERROR);
  ImGui::RadioButton("Reprojection error, camera only", &gs.mv_view, VIEW_ERROR_CAMERA);
  ImGui::RadioButton("Source", &gs.mv_view, VIEW_SOURCE);
  ImGui::SliderFloat("Full brightness (px/frame)", &gs.mv_view_scale, 0.5f, 64.f);
  ImGui::SliderFloat("Error gain", &gs.mv_error_gain, 1.f, 32.f);
  ImGui::Text("frame %s, cuts %llu (last: %s)", frame_valid() ? "valid" : "INVALID",
              (unsigned long long)m_cuts, m_cut_reason);
  ImGui::Text("camera move %.3f m, turn %.2f deg", m_last_move_m, m_last_turn_deg);
  ImGui::Text("merc: %d matched, %d missed, %d unkeyed", g_merc_shown.matched,
              g_merc_shown.missed, g_merc_shown.unkeyed);
}

}  // namespace motion_vectors
