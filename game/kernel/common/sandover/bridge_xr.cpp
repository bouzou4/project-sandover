#include "game/kernel/common/sandover/bridge.h"

#include <algorithm>
#include <cstring>

#include "common/log/log.h"

#include "game/graphics/gfx.h"
#include "game/kernel/common/Ptr.h"

// the XR session's game tick (Hz) while it paces the game's vsync, else 0
s64 pc_get_xr_game_rate() {
  return Gfx::g_xr_game_hz;
}

// OpenXR session facts for the settings profile: 0 a session runs (1) or not (0); 1, 2 the eye
// frame's width and height before the render scale (0 until a frame was rendered)
s64 pc_get_xr_state(u64 which) {
  switch (which) {
    case 0:
      return Gfx::g_xr_session_running ? 1 : 0;
    case 1:
      return Gfx::g_xr_eye_w;
    case 2:
      return Gfx::g_xr_eye_h;
    default:
      lg::error("pc-get-xr-state: unknown value {}", which);
      return 0;
  }
}

// The XR head view the game renders and culls from: 0 without a session, else 1 with 14 floats
// at dst: head rotation rows (game camera axes), head offset (game units), half-tangents x and y
s64 pc_get_xr_cull(u32 dst) {
  auto v = Gfx::get_xr_cull_view();
  auto* out = dst ? Ptr<float>(dst).c() : nullptr;
  if (!out) {
    v.valid = false;
  }
  Gfx::note_xr_cull_handed(v);
  if (!v.valid) {
    return 0;
  }
  for (int r = 0; r < 3; r++) {
    for (int c = 0; c < 3; c++) {
      out[r * 3 + c] = v.rot[r][c];
    }
    out[9 + r] = v.t[r];
  }
  out[12] = v.tan_x;
  out[13] = v.tan_y;
  return 1;
}

// The game's own projection half-tangents (x-ratio, y-ratio) for the view pc-get-xr-cull handed
// it, before it widened to that view; carried on the chain for 2D consumers
void pc_set_xr_game_tan(u32 tan_x, u32 tan_y) {
  float x, y;
  memcpy(&x, &tan_x, 4);
  memcpy(&y, &tan_y, 4);
  Gfx::note_xr_game_tan(x, y);
}

void pc_set_stereo(u32 symptr) {
  Gfx::g_global_settings.stereo_enable = symbol_to_bool(symptr);
}

void pc_set_stereo_option(u64 which, u64 value) {
  auto& s = Gfx::g_global_settings;
  switch (which) {
    case 0:
      s.stereo_ipd_m = (s64)value / 1000000.f;  // micrometres
      break;
    case 1:
      s.stereo_world_scale = std::max((s64)value, (s64)1) / 10000.f;  // 1/10000ths
      break;
    case 2:
      s.stereo_swap_eyes = value != 0;
      break;
    default:
      lg::error("pc-set-stereo-option: unknown option {}", which);
      break;
  }
}

void pc_set_xr(u32 symptr) {
  Gfx::g_global_settings.xr_enable = symbol_to_bool(symptr);
}

void pc_set_xr_recentre() {
  Gfx::g_global_settings.xr_recentre_requests++;
}

void pc_set_xr_fake(u32 symptr) {
  Gfx::g_global_settings.xr_fake = symbol_to_bool(symptr);
}

void pc_set_xr_fake_option(s64 which, s64 value) {
  auto& s = Gfx::g_global_settings;
  if (which >= 0 && which < 3) {
    s.xr_fake_pose[which] = value / 1000.f;  // millidegrees: yaw, pitch, roll
  } else if (which >= 3 && which < 6) {
    s.xr_fake_pose[which] = value / 1000000.f;  // micrometres: x, y, z
  } else if (which == 6) {
    s.xr_fake_sweep_dps = value / 1000.f;  // millidegrees per second, 0 = held
  } else if (which == 7) {
    s.xr_fake_sweep_frames = (int)value;  // > 0: per display frame, a new value restarts
  } else if (which == 8) {
    s.xr_seq_frames =
        (int)std::clamp(value, (s64)0, (s64)16);  // frames in a row, 2 x ~47 MB VRAM each
    s.xr_seq_serial++;
  } else {
    lg::error("pc-set-xr-fake-option: unknown option {}", which);
  }
}
