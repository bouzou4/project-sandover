#pragma once

#include <array>
#include <vector>

#include "common/common_types.h"
#include "common/math/Vector.h"

#include "game/graphics/opengl_renderer/background/background_common.h"

struct SharedRenderState;

/// Every draw depth-tests GEQUAL or GREATER, so a node behind the depth draws nothing.
template <typename Draws>
bool vista_hiz_applies(const Draws& draws) {
  for (const auto& d : draws) {
    const auto t = d.mode.get_depth_test();
    if (!d.mode.get_zt_enable() || (t != GsTest::ZTest::GEQUAL && t != GsTest::ZTest::GREATER)) {
      return false;
    }
  }
  return true;
}

/*!
 * Occlusion of vista draws by the depth drawn before them (the viewer levels' opaque background).
 * After that pass the depth is reduced to farthest-depth tiles and read back; a vis node, TIE
 * instance or shrub chunk whose bounds lie wholly behind its tiles, or wholly off screen, cannot
 * pass the depth test (GEQUAL, depth only grows) and is left out. Exact: no pixel changes.
 */
class VistaHiZ {
 public:
  ~VistaHiZ();
  /// Reduce the current depth and start its readback (no wait); the GPU keeps working on what
  /// follows while the CPU catches up. Any depth drawn earlier in the frame is a valid occluder.
  void begin(SharedRenderState* rs);
  /// Wait for the readback begun this frame. False: nothing culls this frame.
  bool finish(u64 frame_idx);
  void clear() { m_valid = false; }
  bool valid() const { return m_valid; }
  /// Projection of the draws that follow (the shaders' pc_camera, scissor and depth mapping).
  void set_camera(const GoalBackgroundCameraData& cam);
  /// False only when the sphere cannot pass the depth test anywhere.
  bool visible(const math::Vector4f& bsphere) const;
  /// visible() on every node `vis` still marks visible.
  void cull(const std::vector<tfrag3::VisNode>& nodes, u8* vis) const;
  /// Bounds (world, game units) cannot pass the depth test anywhere.
  bool box_hidden(const math::Vector3f& lo, const math::Vector3f& hi) const;

  mutable u64 tested = 0, culled = 0;
  bool invert = false;  // probe: keep only what would be left out
  double sync_ms = 0;

 private:
  bool rect_hidden(float x0, float y0, float x1, float y1, float zmax) const;
  bool corners_hidden(const math::Vector3f* pts, int n) const;
  bool m_valid = false;
  GLuint m_fbo = 0, m_tex = 0, m_fbo_mid = 0, m_tex_mid = 0, m_vao = 0, m_pbo = 0;
  GLsync m_fence = nullptr;
  u64 m_begun_frame = UINT64_MAX;
  int m_tw = 0, m_th = 0;   // tile grid of level 0
  int m_mw = 0, m_mh = 0;   // first-pass grid
  int m_fbw = 0, m_fbh = 0;  // render size
  struct Mip {
    int w, h;
    std::vector<float> d;  // farthest (smallest) depth per tile
  };
  std::vector<Mip> m_mips;
  std::array<math::Vector4f, 4> m_mat;
  math::Vector3f m_trans;
  float m_yscale = 1.f;
};
