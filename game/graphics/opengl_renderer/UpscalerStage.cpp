#include "UpscalerStage.h"

#include <cstdlib>
#include <string>

#include "game/graphics/gfx.h"
#include "game/graphics/opengl_renderer/D3d12Bridge.h"
#include "game/graphics/opengl_renderer/MotionVectors.h"
#include "game/graphics/opengl_renderer/OpenGLRenderer.h"
#include "game/graphics/opengl_renderer/PerfTimers.h"

void UpscalerStage::init(bool jak1) {
  if (jak1) {
    Gfx::g_fsr3_supported = Fsr3Upscaler::context_supported();
    Gfx::g_dlss_supported = DlssUpscaler::context_supported() && m_dlss.probe();
    // the DirectX container: features acquire it while probing; unused, it is torn down
    auto& d3d12 = D3d12Bridge::get();
    Gfx::g_fsr4_supported = d3d12.init() && m_fsr4_eye[0].probe();
    d3d12.release_if_unused();
  }
  m_fsr3_eye[1].share_programs(&m_fsr3_eye[0]);
  m_fsr4_eye[1].share_runtime(&m_fsr4_eye[0], 1);
}

void UpscalerStage::decide(const RenderOptions& settings, bool stereo) {
  m_upscaled = false;
  m_display_src = nullptr;
  // stereo and XR: one FSR 3.1 context per eye (dispatch_buckets_stereo)
  m_fsr3_frame = settings.temporal_aa && !settings.dlss && !settings.fsr4;
  m_dlss_frame = settings.temporal_aa && settings.dlss && !stereo;
  m_fsr4_frame = settings.temporal_aa && settings.fsr4;
  if (!m_fsr3_frame && !m_dlss_frame && !m_fsr4_frame) {
    Fsr3Upscaler::clear_jitter();
  }
  if (!m_fsr3_frame) {
    m_fsr3_eye[0].release();
  }
  if (!m_fsr3_frame || !stereo) {
    m_fsr3_eye[1].release();
  }
  if (m_dlss_frame && !m_dlss.ready()) {
    // no device or NGX: FSR 3.1 from the next frame (Gfx::fsr3_on)
    Gfx::g_dlss_supported = false;
    m_dlss_frame = false;
  }
  if (!m_dlss_frame) {
    m_dlss.release();
  }
  if (Gfx::g_fsr4_supported && !m_fsr4_eye[0].poll()) {
    // the warm-up context found no FSR 4: FSR 3.1 from the next frame
    Gfx::g_fsr4_supported = false;
    m_fsr4_frame = false;
  }
  if (!m_fsr4_frame) {
    m_fsr4_eye[0].release();
  }
  if (!m_fsr4_frame || !stereo) {
    m_fsr4_eye[1].release();
  }
  // SANDOVER_XR_FSR4_SERIAL=1 (dev A/B): both eyes hand back in turn, as before
  static const bool serial = [] {
    const char* v = std::getenv("SANDOVER_XR_FSR4_SERIAL");
    return v && std::string(v) == "1";
  }();
  m_fsr4_serial = serial;
}

void UpscalerStage::arm_fsr4_defer(bool eligible) {
  m_fsr4_defer = !m_fsr4_serial && m_fsr4_frame && eligible;
  m_fsr4_eye0_deferred = false;
}

void UpscalerStage::begin_frame(const SharedRenderState& rs, int display_w, int display_h) {
  // the display size they reconstruct (the render size at scale 1) and this frame's jitter
  m_dw = display_w > 0 ? display_w : rs.render_fb_w;
  m_dh = display_h > 0 ? display_h : rs.render_fb_h;
  if (m_fsr3_frame) {
    // stereo: both eyes of a display frame take the same jitter phase
    fsr3(rs.eye_slot)
        .begin_frame(rs.render_fb_w, rs.render_fb_h, m_dw,
                     rs.stereo_active ? (s64)rs.display_tick : -1);
  }
  if (m_dlss_frame) {
    m_dlss.begin_frame(rs.render_fb_w, rs.render_fb_h, m_dw);
  }
  if (m_fsr4_frame) {
    fsr4(rs.eye_slot)
        .begin_frame(rs.render_fb_w, rs.render_fb_h, m_dw,
                     rs.stereo_active ? (s64)rs.display_tick : -1);
  }
}

void UpscalerStage::pre_sprite(const SharedRenderState& rs, bool hdr) {
  if (m_fsr3_frame) {
    fsr3(rs.eye_slot).capture_opaque(rs.render_fb, rs.render_fb_w, rs.render_fb_h, m_dw, m_dh, hdr);
  }
}

void UpscalerStage::post_sprite(const SharedRenderState& rs, unsigned colour_tex) {
  if (m_fsr3_frame) {
    fsr3(rs.eye_slot).build_reactive(colour_tex, rs.render_fb_w, rs.render_fb_h);
  }
}

UpscaleInputs UpscalerStage::inputs(const SharedRenderState& rs,
                                    const motion_vectors::Renderer& mv,
                                    unsigned colour_tex,
                                    bool hdr) {
  const auto& mvf = motion_vectors::frame();
  UpscaleInputs in;
  in.colour = colour_tex;
  in.depth = rs.render_fb_depth_tex;
  in.velocity = mv.velocity_texture();
  in.reset = !mv.frame_valid();
  in.hdr = hdr;
  auto& clock = m_clock_eye[rs.eye_slot & 1];
  in.frame_ms = (float)clock.getMs();
  clock.start();
  in.w_k0 = mvf.w_k0;
  in.w_k1 = mvf.w_k1;
  in.tan_half_x = mvf.tan_half_x;
  in.tan_half_y = mvf.tan_half_y;
  in.sharpness = Gfx::g_global_settings.fsr3_sharpness;
  return in;
}

void UpscalerStage::run(SharedRenderState* rs,
                        ScopedProfilerNode& prof,
                        motion_vectors::Renderer& mv,
                        unsigned colour_tex,
                        bool hdr) {
  const int slot = rs->eye_slot;
  // FSR 3.1: the temporal upscale (or native antialiasing); the text buckets draw after it
  if (m_fsr3_frame) {
    auto p = prof.make_scoped_child("fsr3");
    perf::Scope ps(perf::Group::FSR3);
    fsr3(slot).run(rs, p, inputs(*rs, mv, colour_tex, hdr), m_dw, m_dh);
    if (!mv.dumped_stem().empty()) {
      fsr3(slot).dump_output(mv.dumped_stem(), rs->frame_idx);
    }
    m_upscaled = true;
    m_display_src = fsr3(slot).display();
  }
  // DLSS: the same place, inputs and hand-over
  if (m_dlss_frame) {
    auto p = prof.make_scoped_child("dlss");
    perf::Scope ps(perf::Group::DLSS);
    if (!m_dlss.run(rs, p, inputs(*rs, mv, colour_tex, hdr), m_dw, m_dh)) {
      Gfx::g_dlss_supported = false;  // FSR 3.1 from the next frame
    }
    if (!mv.dumped_stem().empty()) {
      m_dlss.dump_output(mv.dumped_stem(), rs->frame_idx);
    }
    if (m_dlss.display()->valid) {
      m_upscaled = true;
      m_display_src = m_dlss.display();
    }
  }
  // FSR 4: the same again, with eye 0's hand-back deferred to eye 1's in XR
  if (m_fsr4_frame) {
    auto p = prof.make_scoped_child("fsr4");
    perf::Scope ps(perf::Group::FSR4);
    const auto in = inputs(*rs, mv, colour_tex, hdr);
    const int eye = rs->stereo_active ? rs->eye_slot : 0;
    const bool defer = m_fsr4_defer && eye == 0 && mv.dumped_stem().empty();
    Fsr4Upscaler* pending = eye == 1 && m_fsr4_eye0_deferred ? &m_fsr4_eye[0] : nullptr;
    if (!fsr4(slot).run(rs, p, in, m_dw, m_dh, defer, pending)) {
      Gfx::g_fsr4_supported = false;  // FSR 3.1 from the next frame
    }
    if (eye == 0) {
      m_fsr4_eye0_deferred = fsr4(slot).deferred();
    }
    if (!mv.dumped_stem().empty()) {
      fsr4(slot).dump_output(mv.dumped_stem(), rs->frame_idx);
    }
    if (fsr4(slot).display()->valid) {
      m_upscaled = true;
      m_display_src = fsr4(slot).display();
    }
  }
}

Fbo* UpscalerStage::finish_eye0_deferred() {
  m_fsr4_eye0_deferred = false;
  m_fsr4_eye[0].finish_deferred();
  return m_fsr4_eye[0].display();
}
