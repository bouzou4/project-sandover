#include "game/kernel/common/sandover/bridge.h"

#include <algorithm>
#include <cmath>

#include "common/log/log.h"

#include "game/graphics/gfx.h"
#include "game/graphics/opengl_renderer/MotionBlur.h"
#include "game/graphics/opengl_renderer/MotionVectors.h"
#include "game/kernel/common/Ptr.h"
#include "game/kernel/common/kernel_types.h"
#include "game/runtime.h"

void pc_set_sandover_path(s64 on) {
  Gfx::g_global_settings.sandover_path = on != 0;
}

void pc_set_aa(u64 which, s64 value) {
  auto& s = Gfx::g_global_settings;
  const int n = (int)(value / 1000000);
  switch (which) {
    case 0: {
      const int mode = std::clamp(n, (int)AA_MSAA, (int)AA_FSR4);
      if (mode != s.aa_mode) {
        lg::info("aa: mode {}", mode == AA_FSR4   ? "fsr4"
                                : mode == AA_DLSS ? "dlss"
                                : mode == AA_FSR3 ? "fsr3"
                                                  : "msaa");
      }
      s.aa_mode = mode;
      break;
    }
    case 1: {
      const float rs = std::clamp(value / 1000000.f, 0.25f, 1.f);
      if (rs != s.render_scale) {
        lg::info("aa: render scale {:.3f}", rs);
      }
      s.render_scale = rs;
      break;
    }
    case 2:
      s.fsr3_debug = value != 0;
      break;
    case 3:
      s.fsr3_sharpness = std::clamp(value / 1000000.f, 0.f, 1.f);
      break;
    case 4:
      s.fsr3_jitter = std::clamp(n, 0, 3);
      break;
    case 5: {
      // FSR 3 debug view, sharpness and jitter (2-4) and the DLSS and FSR 4 jitter signs (6, 7)
      // back to the engine defaults
      static const GfxGlobalSettings d{};
      s.fsr3_debug = d.fsr3_debug;
      s.fsr3_sharpness = d.fsr3_sharpness;
      s.fsr3_jitter = d.fsr3_jitter;
      s.dlss_jitter_sign = d.dlss_jitter_sign;
      s.fsr4_jitter_sign = d.fsr4_jitter_sign;
      break;
    }
    case 6:
      s.dlss_jitter_sign = std::clamp((int)value, 0, 3);
      break;
    case 7:
      s.fsr4_jitter_sign = std::clamp((int)value, 0, 3);
      break;
    default:
      lg::error("pc-set-aa: unknown option {}", which);
  }
}

// 0 unavailable (GL below 4.3 or the FSR 3.1 programs failed: vanilla rendering), 1 runs,
// 2 available but stereo or XR is on (MSAA there)
s64 pc_temporal_aa_state() {
  if (!Gfx::g_fsr3_supported && !Gfx::g_dlss_supported && !Gfx::g_fsr4_supported) {
    return 0;
  }
  return g_game_version == GameVersion::Jak1 && Gfx::stereo_requested() ? 2 : 1;
}

// Motion vectors (Jak 1, MotionVectors.h), values in millionths: 0 on (0/1), 1 debug view
// (motion_vectors::View), 2 view scale (px per frame at full brightness), 3 error view gain,
// 4 camera cut in this frame (sent by the game's camera), 5 cut distance (m), 6 cut turn (deg),
// 7 object w tolerance, 8 log cuts (0/1), 9 reset 1-3 and 5-8 to the defaults
void pc_set_motion_vectors(u64 which, s64 value) {
  auto& s = Gfx::g_global_settings;
  const float v = value / 1000000.f;
  switch (which) {
    case 0:
      s.mv_enable = value != 0;
      break;
    case 1:
      s.mv_view = std::clamp((int)std::lround(v), 0, 4);
      break;
    case 2:
      s.mv_view_scale = std::clamp(v, 0.01f, 1000.f);
      break;
    case 3:
      s.mv_error_gain = std::clamp(v, 0.f, 1000.f);
      break;
    case 4:
      motion_vectors::game_cut();
      break;
    case 5:
      s.mv_cut_move_m = std::max(v, 0.f);
      break;
    case 6:
      s.mv_cut_turn_deg = std::max(v, 0.f);
      break;
    case 7:
      s.mv_object_w_tol = std::max(v, 0.f);
      break;
    case 8:
      s.mv_log_cuts = value != 0;
      break;
    case 9: {
      static const GfxGlobalSettings d{};
      s.mv_view = d.mv_view;
      s.mv_view_scale = d.mv_view_scale;
      s.mv_error_gain = d.mv_error_gain;
      s.mv_cut_move_m = d.mv_cut_move_m;
      s.mv_cut_turn_deg = d.mv_cut_turn_deg;
      s.mv_object_w_tol = d.mv_object_w_tol;
      s.mv_log_cuts = d.mv_log_cuts;
      break;
    }
    default:
      lg::error("pc-set-motion-vectors: unknown option {}", which);
  }
}

// dump the next `frames` rendered frames (colour, velocity, camera-only velocity; EXR) into `dir`
void pc_motion_vectors_dump(u32 dir, s64 frames) {
  motion_vectors::request_dump(Ptr<String>(dir).c()->data(), (int)frames);
}

// Per-object motion blur (Jak 1, MotionBlur.h), values in millionths: 0 level (0 off, 1 weak,
// 2 medium, 3 strong), 1 the frame's game step (s, sent every frame), 2 debug view (0 none,
// 1 object velocity, 2 neighbour max tiles), 3-5 weak/medium/strong shutter (fraction of the frame
// interval), 6 radius cap (px at 2160 lines), 7 reset 2-6 to the defaults, 8 log the next N
// blurred frames' step and scale, 9 screenshot bursts cycle the levels (frame N: N % 4),
// 10 camera motion blur (0 off, 1 while riding a vehicle, 2 always), 11 the player rides a vehicle
// (sent on change)
void pc_set_motion_blur(u64 which, s64 value) {
  auto& s = Gfx::g_global_settings;
  const float v = value / 1000000.f;
  switch (which) {
    case 0:
      s.mb_level = std::clamp((int)std::lround(v), 0, 3);
      break;
    case 1:
      motion_blur::set_frame_dt(v);
      break;
    case 2:
      s.mb_view = std::clamp((int)std::lround(v), 0, 2);
      break;
    case 3:
    case 4:
    case 5:
      s.mb_shutter[which - 3] = std::clamp(v, 0.f, 4.f);
      break;
    case 6:
      s.mb_radius_px = std::clamp(v, 1.f, 256.f);
      break;
    case 7: {
      static const GfxGlobalSettings d{};
      s.mb_view = d.mb_view;
      for (int i = 0; i < 3; i++) {
        s.mb_shutter[i] = d.mb_shutter[i];
      }
      s.mb_radius_px = d.mb_radius_px;
      s.mb_burst_cycle = d.mb_burst_cycle;
      break;
    }
    case 8:
      motion_blur::log_frames((int)std::lround(v));
      break;
    case 9:
      s.mb_burst_cycle = value != 0;
      break;
    case 10:
      s.mb_camera = std::clamp((int)std::lround(v), 0, 2);
      break;
    case 11:
      motion_blur::set_vehicle(value != 0);
      break;
    default:
      lg::error("pc-set-motion-blur: unknown option {}", which);
  }
}

/// pc-get-aa-available: 0 FSR 3, 1 DLSS, 2 FSR 4; 1 when the mode can run on this machine, else 0
/// (DLSS and FSR 4 then fall back to FSR 3.1, FSR 3.1 to the MSAA path).
s64 pc_get_aa_available(u64 which) {
  switch (which) {
    case 0:
      return Gfx::g_fsr3_supported ? 1 : 0;
    case 1:
      // DLSS does not run per eye: not offered in stereo or XR
      return Gfx::g_dlss_supported && !(g_game_version == GameVersion::Jak1 && Gfx::stereo_requested())
                 ? 1
                 : 0;
    case 2:
      return Gfx::g_fsr4_supported ? 1 : 0;
    default:
      lg::error("pc-get-aa-available: unknown mode {}", which);
      return 0;
  }
}
