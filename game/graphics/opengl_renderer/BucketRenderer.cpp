#include "BucketRenderer.h"

#include <cmath>
#include <utility>

#include "game/graphics/pipelines/opengl.h"

#include "fmt/format.h"
#include "third-party/imgui/imgui.h"

std::string BucketRenderer::name() const {
  return m_name;
}

std::string BucketRenderer::name_and_id() const {
  return fmt::format("[{:2d}] {}", (int)m_my_id, m_name);
}

EmptyBucketRenderer::EmptyBucketRenderer(const std::string& name, int my_id)
    : BucketRenderer(name, my_id) {}

void EmptyBucketRenderer::render(DmaFollower& dma,
                                 SharedRenderState* render_state,
                                 ScopedProfilerNode& /*prof*/) {
  if (render_state->version == GameVersion::Jak1) {
    // an empty bucket should have 4 things:
    // a NEXT in the bucket buffer
    // a CALL that calls the default register buffer chain
    // a CNT then RET to get out of the default register buffer chain
    // a NEXT to get to the next bucket.

    // NEXT
    auto first_tag = dma.current_tag();
    dma.read_and_advance();
    ASSERT(first_tag.kind == DmaTag::Kind::NEXT && first_tag.qwc == 0);

    // CALL
    auto call_tag = dma.current_tag();
    dma.read_and_advance();
    ASSERT_MSG(call_tag.kind == DmaTag::Kind::CALL && call_tag.qwc == 0,
               fmt::format("Bucket renderer {} ({}) was supposed to be empty, but wasn't\n",
                           m_my_id, m_name));

    // in the default reg buffer:
    ASSERT(dma.current_tag_offset() == render_state->default_regs_buffer);
    dma.read_and_advance();
    ASSERT(dma.current_tag().kind == DmaTag::Kind::RET);
    dma.read_and_advance();

    // NEXT to next buffer
    auto to_next_buffer = dma.current_tag();
    ASSERT(to_next_buffer.kind == DmaTag::Kind::NEXT);
    ASSERT(to_next_buffer.qwc == 0);
    dma.read_and_advance();

    // and we should now be in the next bucket!
    ASSERT(dma.current_tag_offset() == render_state->next_bucket);
  } else {
    auto first_tag = dma.current_tag();
    dma.read_and_advance();
    if (first_tag.kind != DmaTag::Kind::CNT || first_tag.qwc != 0) {
      fmt::print("Bucket renderer {} ({}) was supposed to be empty, but wasn't\n", m_my_id, m_name);
      ASSERT(false);
    }
  }
}

SkipRenderer::SkipRenderer(const std::string& name, int my_id) : BucketRenderer(name, my_id) {}

void SkipRenderer::render(DmaFollower& dma,
                          SharedRenderState* render_state,
                          ScopedProfilerNode& /*prof*/) {
  while (dma.current_tag_offset() != render_state->next_bucket) {
    dma.read_and_advance();
  }
}

PrintRenderer::PrintRenderer(const std::string& name, int my_id) : BucketRenderer(name, my_id) {}

void PrintRenderer::render(DmaFollower& dma,
                           SharedRenderState* render_state,
                           ScopedProfilerNode& /*prof*/) {
  // jump to bucket
  dma.read_and_advance();

  auto transfers = 0;
  // print the entire chain
  fmt::print("START {} DMA!!!!!!!\n", m_name);
  while (dma.current_tag_offset() != render_state->next_bucket) {
    auto dmatag = dma.current_tag();
    auto data = dma.read_and_advance();
    printf(
        "dma transfer %d:\n%ssize: %d\nvif0: %s, data: %d\nvif1: %s, data: %d, imm: "
        "%d\n\n",
        transfers, dmatag.print().c_str(), data.size_bytes, data.vifcode0().print().c_str(),
        data.vif0(), data.vifcode1().print().c_str(), data.vifcode1().num,
        data.vifcode1().immediate);
    transfers++;
  }
  printf("transfers: %d\n\n", transfers);
}

FrameCamera frame_camera(const SharedRenderState& s) {
  if (s.eye.active && s.game_camera.valid) {
    const auto& g = s.game_camera;
    return {g.matrix, g.rot, g.persp, &g.pos, &g.hvdf_off, &g.fog};
  }
  return {s.camera_matrix, s.camera_rot,       s.camera_persp,
          &s.camera_pos,   &s.camera_hvdf_off, &s.camera_fog};
}

void eye_camera_to_clip(math::Vector4f p[4], const EyeView& e) {
  float rt[3];
  for (int j = 0; j < 3; j++) {
    rt[j] = e.rot[j][0] * e.t[0] + e.rot[j][1] * e.t[1] + e.rot[j][2] * e.t[2];
  }
  math::Vector4f out[4];
  for (int i = 0; i < 3; i++) {
    out[i] = p[0] * e.rot[0][i] + p[1] * e.rot[1][i] + p[2] * e.rot[2][i];
  }
  out[3] = p[3] - p[0] * rt[0] - p[1] * rt[1] - p[2] * rt[2];
  for (int i = 0; i < 4; i++) {
    out[i].x() *= e.fov_x;
    out[i].y() *= e.fov_y;
    p[i] = out[i];
  }
}

namespace {
bool invert4(const math::Vector4f m[4], double out[4][4]) {
  double a[4][8];
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      a[r][c] = m[r][c];
      a[r][c + 4] = r == c ? 1. : 0.;
    }
  }
  for (int c = 0; c < 4; c++) {
    int piv = c;
    for (int r = c + 1; r < 4; r++) {
      if (std::abs(a[r][c]) > std::abs(a[piv][c])) {
        piv = r;
      }
    }
    if (std::abs(a[piv][c]) < 1e-30) {
      return false;
    }
    if (piv != c) {
      for (int k = 0; k < 8; k++) {
        std::swap(a[c][k], a[piv][k]);
      }
    }
    const double d = a[c][c];
    for (int k = 0; k < 8; k++) {
      a[c][k] /= d;
    }
    for (int r = 0; r < 4; r++) {
      if (r != c) {
        const double f = a[r][c];
        for (int k = 0; k < 8; k++) {
          a[r][k] -= f * a[c][k];
        }
      }
    }
  }
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      out[r][c] = a[r][c + 4];
    }
  }
  return true;
}
}  // namespace

bool eye_world_to_clip(math::Vector4f m[4], const math::Vector4f persp[4], const EyeView& e) {
  double inv[4][4];
  if (!invert4(persp, inv)) {
    return false;
  }
  math::Vector4f q[4];
  for (int i = 0; i < 4; i++) {
    q[i] = persp[i];
  }
  eye_camera_to_clip(q, e);
  // E = persp^-1 * (D * persp * S); m' = m * E
  double ex[4][4];
  for (int i = 0; i < 4; i++) {
    for (int j = 0; j < 4; j++) {
      double acc = 0;
      for (int k = 0; k < 4; k++) {
        acc += inv[i][k] * q[k][j];
      }
      ex[i][j] = acc;
    }
  }
  math::Vector4f out[4];
  for (int i = 0; i < 4; i++) {
    for (int j = 0; j < 4; j++) {
      double acc = 0;
      for (int k = 0; k < 4; k++) {
        acc += (double)m[i][k] * ex[k][j];
      }
      out[i][j] = (float)acc;
    }
  }
  for (int i = 0; i < 4; i++) {
    m[i] = out[i];
  }
  return true;
}

void eye_world_to_camera(math::Vector4f m[4], const EyeView& e) {
  for (int i = 0; i < 4; i++) {
    float v[3] = {m[i].x(), m[i].y(), m[i].z()};
    if (i == 3) {
      for (int k = 0; k < 3; k++) {
        v[k] -= e.t[k];
      }
    }
    for (int j = 0; j < 3; j++) {
      m[i][j] = e.rot[j][0] * v[0] + e.rot[j][1] * v[1] + e.rot[j][2] * v[2];
    }
  }
}

void set_xr_gs_uniforms(u32 program, const SharedRenderState& s, int mode) {
  const auto& g = s.game_camera;
  const auto& p = g.persp;
  const bool on = mode != 0 && s.eye.active && g.valid && p[2][3] != 0.f && p[0][0] != 0.f &&
                  p[1][1] != 0.f && p[3][2] != 0.f && g.fog.x() != 0.f;
  glUniform1i(glGetUniformLocation(program, "xr_gs_on"), on ? mode : 0);
  if (!on) {
    return;
  }
  const auto& e = s.eye;
  const float fog = g.fog.x();
  float rt[3];
  for (int j = 0; j < 3; j++) {
    rt[j] = e.rot[j][0] * e.t[0] + e.rot[j][1] * e.t[1] + e.rot[j][2] * e.t[2];
  }
  glUniformMatrix3fv(glGetUniformLocation(program, "xr_gs_rot"), 1, GL_TRUE, &e.rot[0][0]);
  glUniform3f(glGetUniformLocation(program, "xr_gs_rt"), rt[0], rt[1], rt[2]);
  // GS = offset + a * (camera x / z, y / z, 1 / z)
  glUniform3f(glGetUniformLocation(program, "xr_gs_off"), g.hvdf_off.x(), g.hvdf_off.y(),
              g.hvdf_off.z() + fog * p[2][2] / p[2][3]);
  glUniform3f(glGetUniformLocation(program, "xr_gs_a"), fog * p[0][0] / p[2][3],
              fog * p[1][1] / p[2][3], fog * p[3][2] / p[2][3]);
  glUniform2f(glGetUniformLocation(program, "xr_gs_fov"), e.fov_x, e.fov_y);
}

math::Vector2f stereo_gs_parallax(const SharedRenderState& state) {
  const auto& p = state.camera_persp;
  if (state.stereo_eye_x == 0.f || p[3][2] == 0.f || p[2][3] == 0.f) {
    return math::Vector2f(0.f, 0.f);
  }
  // camera-space x shift e moves GS x by -pxx * e * (z_gs - z0) / m32
  const float z0 = state.camera_hvdf_off.z() + state.camera_fog.x() * p[2][2] / p[2][3];
  const float k = -p[0][0] * state.stereo_eye_x * 16777216.f / (p[3][2] * 4096.f);
  return math::Vector2f(k, z0 / 16777216.f);
}

void SharedRenderState::reset() {
  has_pc_data = false;
  for (auto& x : occlusion_vis) {
    x.valid = false;
  }
  load_status_debug.clear();
}

RenderMux::RenderMux(const std::string& name,
                     int my_id,
                     std::vector<std::unique_ptr<BucketRenderer>> renderers)
    : BucketRenderer(name, my_id), m_renderers(std::move(renderers)) {
  for (auto& r : m_renderers) {
    m_name_strs.push_back(r->name_and_id());
  }
  for (auto& n : m_name_strs) {
    m_name_str_ptrs.push_back(n.data());
  }
}

void RenderMux::render(DmaFollower& dma,
                       SharedRenderState* render_state,
                       ScopedProfilerNode& prof) {
  m_renderers[m_render_idx]->enabled() = m_enabled;
  m_renderers[m_render_idx]->render(dma, render_state, prof);
}

void RenderMux::draw_debug_window() {
  ImGui::ListBox("Pick", &m_render_idx, m_name_str_ptrs.data(), m_renderers.size());
  ImGui::Separator();
  m_renderers[m_render_idx]->draw_debug_window();
}

void RenderMux::init_textures(TexturePool& tp, GameVersion version) {
  for (auto& rend : m_renderers) {
    rend->init_textures(tp, version);
  }
}

void RenderMux::init_shaders(ShaderLibrary& sl) {
  for (auto& rend : m_renderers) {
    rend->init_shaders(sl);
  }
}
