#include "game/kernel/common/sandover/bridge.h"

#include <algorithm>
#include <cmath>

#include "common/log/log.h"

#include "game/graphics/gfx.h"

void pc_set_ssao_debug(u32 symptr) {
  Gfx::g_global_settings.ssao_depth_debug = symbol_to_bool(symptr);
}

void pc_set_ssao_depth_capture(u32 symptr) {
  Gfx::g_global_settings.ssao_depth_capture = symbol_to_bool(symptr);
}

void pc_set_ssao_option(u64 which, u64 value) {
  auto& s = Gfx::g_global_settings;
  switch (which) {
    case 0:
      s.ssao_capture_point = value ? 1 : 0;
      break;
    case 1:
      s.ssao_msaa_mode = value ? 1 : 0;
      break;
    case 2:
      s.ssao_gpu_timing = value != 0;
      break;
    case 3:
      s.ssao_msaa_probe_request = true;
      break;
    case 4:
      s.ssao_prereq_probe = (int)value;
      break;
    case 5:
      s.bg_prepass = (int)value;
      break;
    default:
      lg::error("pc-set-ssao-option: unknown option {}", which);
      break;
  }
}

void pc_set_ssao_preset(s64 index) {
  if (index < 0) {
    Gfx::g_global_settings.ssao_enable = false;
  } else if (!Gfx::SsaoApplyPreset((int)index)) {
    lg::error("pc-set-ssao-preset: no preset {} ({} presets), AO left as it was", index,
              Gfx::SsaoPresetCount());
  }
}

void pc_set_ssao_param(u64 which, s64 value) {
  auto& s = Gfx::g_global_settings;
  auto& p = s.ssao_params;
  const float v = value / 1000000.f;
  const int iv = (int)std::lround(v);
  switch ((SsaoParam)which) {
    case SsaoParam::ENABLE:
      s.ssao_enable = value != 0;
      return;
    case SsaoParam::VIEW:
      s.ssao_view = iv;
      return;
    case SsaoParam::GPU_TIMING:
      s.ssao_gpu_timing = value != 0;
      return;
    case SsaoParam::NORMAL_TARGET:
      s.ssao_normal_target = value != 0;
      return;
    case SsaoParam::EMISSIVE:
      s.ssao_emissive = value != 0;
      return;
    case SsaoParam::FOG_WEIGHT:
      s.ssao_fog_weight = std::max(v, 0.f);
      return;
    case SsaoParam::EMISSIVE_LO:
      s.ssao_emissive_lo = v;
      return;
    case SsaoParam::EMISSIVE_HI:
      s.ssao_emissive_hi = v;
      return;
    case SsaoParam::GTAO_DENOISE:
      s.ssao_gtao.denoise = std::clamp(iv, 1, 3);
      return;
    case SsaoParam::GTAO_HISTORY:
      s.ssao_gtao.history = value != 0;
      return;
    case SsaoParam::GTAO_HISTORY_CLAMP:
      s.ssao_gtao.history_clamp = value != 0;
      return;
    case SsaoParam::GTAO_HISTORY_GAMMA:
      s.ssao_gtao.history_gamma = std::max(v, 0.f);
      return;
    case SsaoParam::RADIUS:
      p.radius_m = v;
      break;
    case SsaoParam::INTENSITY:
      p.intensity = v;
      break;
    case SsaoParam::MERC_WEIGHT:
      p.merc_weight = v;
      break;
    case SsaoParam::LUMA_FLOOR:
      p.luma_floor = v;
      break;
    case SsaoParam::CAVE_SCALE:
      p.cave_scale = v;
      break;
    case SsaoParam::GTAO_SLICES:
      p.gtao_slices = std::clamp(iv, 1, 9);
      break;
    case SsaoParam::GTAO_STEPS:
      p.gtao_steps = std::clamp(iv, 1, 8);
      break;
    case SsaoParam::GTAO_FULL_RES:
      p.gtao_full_res = value != 0;
      break;
    case SsaoParam::GTAO_THIN:
      p.gtao_thin = std::clamp(v, 0.f, 1.f);
      break;
    default:
      lg::error("pc-set-ssao-param: unknown param {}", which);
      return;
  }
  s.ssao_preset = -1;
}
