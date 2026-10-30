#include "VistaHiZ.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "common/util/Timer.h"

#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/opengl_renderer/Shader.h"

namespace {
constexpr int kTile = 8;  // render pixels per level-0 tile
constexpr int kUnitDepth = 0;
constexpr int kUnitDepthMs = 1;
// slack for the jitter, rasterization rules and the depth format
constexpr float kPadPx = 2.f;
constexpr float kDepthEps = 1.f / (1 << 20);
}  // namespace

VistaHiZ::~VistaHiZ() {
  if (m_fbo) {
    glDeleteFramebuffers(1, &m_fbo);
    glDeleteTextures(1, &m_tex);
    glDeleteFramebuffers(1, &m_fbo_mid);
    glDeleteTextures(1, &m_tex_mid);
    glDeleteVertexArrays(1, &m_vao);
    glDeleteBuffers(1, &m_pbo);
  }
  if (m_fence) {
    glDeleteSync(m_fence);
  }
}

void VistaHiZ::begin(SharedRenderState* rs) {
  m_valid = false;
  m_begun_frame = UINT64_MAX;
  if (m_fence) {
    glDeleteSync(m_fence);
    m_fence = nullptr;
  }
  if (!rs->render_fb_depth_tex || rs->render_fb_w <= 0 || rs->render_fb_h <= 0 ||
      rs->draw_offset_x || rs->draw_offset_y) {
    return;
  }
  const int fw = rs->render_fb_w, fh = rs->render_fb_h;
  // two passes: 4x4, then 2x2
  const int mw = (fw + 3) / 4, mh = (fh + 3) / 4;
  const int tw = (mw + 1) / 2, th = (mh + 1) / 2;
  if (!m_fbo) {
    glGenFramebuffers(1, &m_fbo);
    glGenTextures(1, &m_tex);
    glGenFramebuffers(1, &m_fbo_mid);
    glGenTextures(1, &m_tex_mid);
    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_pbo);
  }
  if (tw != m_tw || th != m_th || mw != m_mw || mh != m_mh) {
    m_tw = tw;
    m_th = th;
    m_mw = mw;
    m_mh = mh;
    glBindTexture(GL_TEXTURE_2D, m_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, tw, th, 0, GL_RED, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_tex, 0);
    glBindTexture(GL_TEXTURE_2D, m_tex_mid);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, mw, mh, 0, GL_RED, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo_mid);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_tex_mid, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo);
    glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)tw * th * sizeof(float), nullptr, GL_STREAM_READ);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  }
  m_fbw = fw;
  m_fbh = fh;

  GLint vp[4], vao = 0, samples = 1, active_tex = 0;
  GLboolean cmask[4];
  glGetIntegerv(GL_VIEWPORT, vp);
  glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
  glGetIntegerv(GL_ACTIVE_TEXTURE, &active_tex);
  glGetBooleanv(GL_COLOR_WRITEMASK, cmask);
  const GLboolean depth_on = glIsEnabled(GL_DEPTH_TEST), blend_on = glIsEnabled(GL_BLEND),
                  scissor_on = glIsEnabled(GL_SCISSOR_TEST);
  glBindFramebuffer(GL_FRAMEBUFFER, rs->render_fb);
  glGetIntegerv(GL_SAMPLES, &samples);

  const GLenum buf = GL_COLOR_ATTACHMENT0;
  glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
  glDrawBuffers(1, &buf);
  glBindFramebuffer(GL_FRAMEBUFFER, m_fbo_mid);
  glDrawBuffers(1, &buf);
  glViewport(0, 0, mw, mh);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_SCISSOR_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  auto& sh = rs->shaders[ShaderId::VISTA_HIZ];
  sh.activate();
  const GLuint id = sh.id();
  glUniform1i(glGetUniformLocation(id, "depth_ms"), kUnitDepthMs);
  glUniform1i(glGetUniformLocation(id, "depth_ss"), kUnitDepth);
  glUniform1i(glGetUniformLocation(id, "samples"), samples > 1 ? samples : 0);
  glUniform2i(glGetUniformLocation(id, "size"), fw, fh);
  glUniform1i(glGetUniformLocation(id, "tile"), 4);
  const GLenum target = samples > 1 ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D;
  glActiveTexture(GL_TEXTURE0 + (samples > 1 ? kUnitDepthMs : kUnitDepth));
  glBindTexture(target, rs->render_fb_depth_tex);
  glBindVertexArray(m_vao);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glBindTexture(target, 0);

  glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
  glViewport(0, 0, tw, th);
  glUniform1i(glGetUniformLocation(id, "samples"), 0);
  glUniform2i(glGetUniformLocation(id, "size"), mw, mh);
  glUniform1i(glGetUniformLocation(id, "tile"), 2);
  glActiveTexture(GL_TEXTURE0 + kUnitDepth);
  glBindTexture(GL_TEXTURE_2D, m_tex_mid);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glBindTexture(GL_TEXTURE_2D, 0);

  glPixelStorei(GL_PACK_ALIGNMENT, 4);
  glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo);
  glReadPixels(0, 0, tw, th, GL_RED, GL_FLOAT, nullptr);
  glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  m_fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  glFlush();
  m_begun_frame = rs->frame_idx;

  glBindFramebuffer(GL_FRAMEBUFFER, rs->render_fb);
  glViewport(vp[0], vp[1], vp[2], vp[3]);
  glBindVertexArray(vao);
  glActiveTexture(active_tex);
  glColorMask(cmask[0], cmask[1], cmask[2], cmask[3]);
  depth_on ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST);
  blend_on ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
  scissor_on ? glEnable(GL_SCISSOR_TEST) : glDisable(GL_SCISSOR_TEST);
}

bool VistaHiZ::finish(u64 frame_idx) {
  m_valid = false;
  if (!m_fence || m_begun_frame != frame_idx) {
    return false;
  }
  Timer timer;
  const GLenum r = glClientWaitSync(m_fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
  glDeleteSync(m_fence);
  m_fence = nullptr;
  if (r != GL_ALREADY_SIGNALED && r != GL_CONDITION_SATISFIED) {
    return false;
  }
  const int tw = m_tw, th = m_th;
  m_mips.resize(1);
  m_mips[0].w = tw;
  m_mips[0].h = th;
  m_mips[0].d.resize((size_t)tw * th);
  glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo);
  const void* p = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)tw * th * sizeof(float),
                                   GL_MAP_READ_BIT);
  if (!p) {
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    return false;
  }
  memcpy(m_mips[0].d.data(), p, (size_t)tw * th * sizeof(float));
  glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
  glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

  while (m_mips.back().w > 1 || m_mips.back().h > 1) {
    const Mip& a = m_mips.back();
    Mip b;
    b.w = (a.w + 1) / 2;
    b.h = (a.h + 1) / 2;
    b.d.resize((size_t)b.w * b.h);
    for (int y = 0; y < b.h; y++) {
      const int y0 = 2 * y, y1 = std::min(2 * y + 1, a.h - 1);
      for (int x = 0; x < b.w; x++) {
        const int x0 = 2 * x, x1 = std::min(2 * x + 1, a.w - 1);
        b.d[(size_t)y * b.w + x] =
            std::min({a.d[(size_t)y0 * a.w + x0], a.d[(size_t)y0 * a.w + x1],
                      a.d[(size_t)y1 * a.w + x0], a.d[(size_t)y1 * a.w + x1]});
      }
    }
    m_mips.push_back(std::move(b));
  }
  sync_ms = timer.getMs();
  m_valid = true;
  return true;
}

void VistaHiZ::set_camera(const GoalBackgroundCameraData& cam) {
  m_mat = make_new_cam_mat(cam.rot, cam.perspective, cam.fog.x(), cam.hvdf_off.z());
  m_trans = cam.trans.xyz();
  m_yscale = 512.f / 448.f;  // SCISSOR_ADJUST * HEIGHT_SCALE (Jak 1)
}

bool VistaHiZ::rect_hidden(float x0, float y0, float x1, float y1, float zmax) const {
  x0 -= kPadPx;
  y0 -= kPadPx;
  x1 += kPadPx;
  y1 += kPadPx;
  if (x1 < 0.f || y1 < 0.f || x0 > (float)m_fbw || y0 > (float)m_fbh) {
    return true;  // off screen
  }
  int tx0 = std::max(0, (int)std::floor(x0 / kTile));
  int ty0 = std::max(0, (int)std::floor(y0 / kTile));
  int tx1 = std::min(m_tw - 1, (int)std::floor(x1 / kTile));
  int ty1 = std::min(m_th - 1, (int)std::floor(y1 / kTile));
  size_t level = 0;
  while (level + 1 < m_mips.size() && (tx1 - tx0 > 3 || ty1 - ty0 > 3)) {
    tx0 >>= 1;
    ty0 >>= 1;
    tx1 >>= 1;
    ty1 >>= 1;
    level++;
  }
  const Mip& m = m_mips[level];
  for (int y = ty0; y <= ty1; y++) {
    for (int x = tx0; x <= tx1; x++) {
      if (zmax >= m.d[(size_t)y * m.w + x] - kDepthEps) {
        return false;
      }
    }
  }
  return true;
}

bool VistaHiZ::corners_hidden(const math::Vector3f* pts, int n) const {
  float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f, zmax = -1e30f;
  for (int i = 0; i < n; i++) {
    const math::Vector3f v = pts[i] - m_trans;
    math::Vector4f t = m_mat[3] * -1.f;
    t.w() = 0.f;
    t -= m_mat[0] * v.x();
    t -= m_mat[1] * v.y();
    t -= m_mat[2] * v.z();
    if (!(t.w() > 1e-6f)) {
      return false;  // at or behind the eye
    }
    const float iw = 1.f / t.w();
    const float nx = t.x() * iw, ny = t.y() * m_yscale * iw, nz = t.z() * iw;
    x0 = std::min(x0, nx);
    x1 = std::max(x1, nx);
    y0 = std::min(y0, ny);
    y1 = std::max(y1, ny);
    zmax = std::max(zmax, nz);
  }
  const float zw = std::min(zmax, 1.f) * 0.5f + 0.5f;
  return rect_hidden((x0 * 0.5f + 0.5f) * m_fbw, (y0 * 0.5f + 0.5f) * m_fbh,
                     (x1 * 0.5f + 0.5f) * m_fbw, (y1 * 0.5f + 0.5f) * m_fbh, zw);
}

bool VistaHiZ::box_hidden(const math::Vector3f& lo, const math::Vector3f& hi) const {
  if (!m_valid) {
    return false;
  }
  math::Vector3f p[8];
  for (int i = 0; i < 8; i++) {
    p[i] = math::Vector3f((i & 1) ? hi.x() : lo.x(), (i & 2) ? hi.y() : lo.y(),
                          (i & 4) ? hi.z() : lo.z());
  }
  return corners_hidden(p, 8);
}

bool VistaHiZ::visible(const math::Vector4f& s) const {
  if (!m_valid) {
    return true;
  }
  tested++;
  const math::Vector3f r(s.w(), s.w(), s.w());
  const bool hidden = box_hidden(s.xyz() - r, s.xyz() + r);
  culled += hidden;
  return invert ? hidden : !hidden;
}

void VistaHiZ::cull(const std::vector<tfrag3::VisNode>& nodes, u8* vis) const {
  if (!m_valid) {
    return;
  }
  for (size_t i = 0; i < nodes.size(); i++) {
    if (vis[i] && !visible(nodes[i].bsphere)) {
      vis[i] = 0;
    }
  }
}
