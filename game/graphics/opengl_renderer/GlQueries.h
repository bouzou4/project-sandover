#pragma once

/*!
 * @file GlQueries.h
 * GL queries off the per-frame path. Every glGet*, glIsEnabled and glGetUniformLocation makes a
 * threaded GL driver wait for its worker thread, so a pass that asks costs a sync. Every use
 * names the perf-opt bit of the area that owns the call site, so each area's saving stays an
 * A/B marginal of its own bit; with that bit off the plain driver call runs.
 *
 *   GLQ_LOC(bit, program, "name")  uniform location, cached at this call site per program
 *   glq::uniform_loc(bit, program, name)  the same for a name built at run time
 *   glq::SavedState  state a pass changes and puts back, re-queried when stale (below)
 *
 * Programs are never deleted or relinked, so a cached location stays valid for the process.
 * A SavedState copy is reused only while the state generation and the perf-opt mask are those it
 * was queried under, and at most 255 frames. invalidate_state() bumps the generation; it is called
 * when frame targets, window, MSAA, HDR, stereo/XR or any GfxGlobalSettings byte change
 * (OpenGLRenderer::setup_frame, dispatch_buckets_stereo) and when level GL objects are deleted
 * (Loader, background tree caches), so a copy never names an FBO, texture or VAO that is gone or
 * a viewport of an old size. SANDOVER_GL_STATE_CHECK=1 makes SavedState query every frame and log
 * where the live state differs from a copy that is not stale. Shared by every area (unified from
 * the present, geometry, SSAO and tile-light caches); do not add another.
 */

#include <string>

#include "common/common_types.h"

#include "game/graphics/gfx.h"
#include "game/graphics/pipelines/opengl.h"

namespace glq {

/// The uniform locations of one call site, per program.
struct UniformSite {
  static constexpr int kSlots = 16;
  GLuint prog[kSlots] = {};
  GLint loc[kSlots] = {};
  int n = 0;

  GLint get(bool on, GLuint program, const char* name) {
    if (!on) {
      return glGetUniformLocation(program, name);
    }
    for (int i = 0; i < n; i++) {
      if (prog[i] == program) {
        return loc[i];
      }
    }
    const GLint l = glGetUniformLocation(program, name);
    if (n < kSlots) {
      prog[n] = program;
      loc[n] = l;
      n++;
    }
    return l;
  }
};

/// Location of `name` in `program`, cached per (program, name) while perf-opt `bit` is on.
GLint uniform_loc(int bit, GLuint program, const std::string& name);

/// Mark every SavedState copy stale: the next capture() queries the driver. Render thread.
void invalidate_state();

/// GL state a pass changes and puts back: capture() before, restore() after.
class SavedState {
 public:
  /// `bit`: the owning area's perf-opt bit. `units_2d` / `units_ms`: bit u = the GL_TEXTURE_2D /
  /// GL_TEXTURE_2D_MULTISAMPLE binding of texture unit u (units 0-15) is saved too. Always saved:
  /// enables, depth/colour masks, blend, stencil func/ref/masks/ops (front face, restored to both),
  /// viewport, program, VAO, active unit, draw and read FBO and the read buffer.
  SavedState(const char* site, int bit, unsigned units_2d = 0, unsigned units_ms = 0)
      : m_site(site), m_bit(bit), m_units_2d(units_2d), m_units_ms(units_ms) {}

  void capture();
  void restore() const;

  bool scissor() const { return m_s.scissor; }
  bool cull() const { return m_s.cull; }
  bool blend() const { return m_s.blend; }
  bool depth_test() const { return m_s.depth_test; }
  bool stencil() const { return m_s.stencil; }

 private:
  static constexpr int kUnits = 16;
  struct State {
    GLboolean depth_test, blend, scissor, cull, stencil, depth_mask;
    GLboolean color_mask[4];
    GLint blend_src_rgb, blend_dst_rgb, blend_src_a, blend_dst_a;
    GLint blend_eq_rgb, blend_eq_a;
    GLint stencil_func, stencil_ref, stencil_value_mask, stencil_write_mask;
    GLint stencil_fail, stencil_zfail, stencil_zpass;
    GLint read_buffer;
    GLint viewport[4];
    GLint program, vao, active_tex, draw_fbo, read_fbo;
    GLint tex_2d[kUnits], tex_ms[kUnits];
  };
  void query(State& s) const;

  const char* m_site;
  int m_bit;
  unsigned m_units_2d, m_units_ms;
  State m_s{};
  bool m_valid = false;
  u64 m_gen = 0;
  u32 m_mask = 0;
  int m_frames = 0;
  int m_logs = 0;
};

}  // namespace glq

/// Location of uniform `name` (a string literal) in `program`, cached at this call site while
/// perf-opt `bit` is on.
#define GLQ_LOC(bit, program, name)                                 \
  ([](int glq_bit_, GLuint glq_prog_) {                             \
    static glq::UniformSite glq_site_;                              \
    return glq_site_.get(Gfx::perf_opt(glq_bit_), glq_prog_, name); \
  }((bit), (GLuint)(program)))
