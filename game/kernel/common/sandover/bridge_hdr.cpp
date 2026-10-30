#include "game/kernel/common/sandover/bridge.h"

#include <cmath>

#include "common/log/log.h"

#include "game/graphics/gfx.h"

void pc_set_bloom(u64 which, s64 value) {
  auto& s = Gfx::g_global_settings;
  const float v = value / 1000000.f;
  switch (which) {
    case 0:
      s.bloom_enable = value != 0;
      break;
    case 1:
      s.bloom_strength = v;
      break;
    case 2:
      s.bloom_radius = v;
      break;
    case 3:
      s.bloom_threshold = v;
      break;
    case 4:
      s.bloom_hue_clip = value != 0;
      break;
    default:
      lg::error("pc-set-bloom: unknown option {}", which);
  }
}

void pc_set_hdr_target(u32 symptr) {
  Gfx::g_global_settings.hdr_target = symbol_to_bool(symptr);
}

void pc_set_tonemap(u64 which, s64 value) {
  auto& s = Gfx::g_global_settings;
  const float v = value / 1000000.f;
  switch ((TonemapOption)which) {
    case TonemapOption::MODE:
      s.tonemap_mode = (int)std::lround(v);
      break;
    case TonemapOption::EXPOSURE:
      s.tonemap_exposure = v;
      break;
    case TonemapOption::KNEE:
      s.tonemap_knee = v;
      break;
    case TonemapOption::PAPER_WHITE:
      s.paper_white_nits = v;
      break;
    case TonemapOption::TOON_KNEE:
      s.tonemap_toon_knee = v;
      break;
    case TonemapOption::TOON_RANGE:
      s.tonemap_toon_range = v;
      break;
    case TonemapOption::WHITE_START:
      s.tonemap_white_start = v;
      break;
    case TonemapOption::WHITE_FULL:
      s.tonemap_white_full = v;
      break;
    case TonemapOption::HALO_START:
      s.tonemap_halo_start = v;
      break;
    case TonemapOption::HALO_FULL:
      s.tonemap_halo_full = v;
      break;
    default:
      lg::error("pc-set-tonemap: unknown option {}", which);
      break;
  }
}

void pc_set_hdr_output(u64 which, s64 value) {
  auto& s = Gfx::g_global_settings;
  const float v = value / 1000000.f;
  switch ((HdrOutputOption)which) {
    case HdrOutputOption::ENABLE:
      s.hdr_output = v != 0.f;
      break;
    case HdrOutputOption::FORMAT:
      s.hdr_format = (int)std::lround(v);
      break;
    case HdrOutputOption::PAPER_WHITE:
      s.paper_white_nits = v;
      break;
    case HdrOutputOption::PEAK:
      s.hdr_peak_nits = v;
      break;
    case HdrOutputOption::BLACK:
      s.hdr_black_nits = v;
      break;
    case HdrOutputOption::SATURATION:
      s.hdr_saturation = v;
      break;
    case HdrOutputOption::KNEE:
      s.hdr_knee = v;
      break;
    case HdrOutputOption::OS_SWITCH:
      s.hdr_os_switch = v != 0.f;
      break;
    case HdrOutputOption::PEAK_MILLI:
      s.hdr_peak_nits = value / 1000.f;
      break;
    case HdrOutputOption::CAL_PATTERN:
      s.hdr_cal_pattern = (int)std::lround(v);
      break;
    default:
      lg::error("pc-set-hdr-output: unknown option {}", which);
      break;
  }
}

/// pc-get-hdr-output, in thousandths (GOAL's int-to-float is 32-bit, millionths overflow above
/// 2147 nits): 0 HDR output presenting (1000) or not (0), 1 display peak and 2 display black in
/// nits (negative until the display was queried), 3 effective peak, 4 effective black, 5 the peak
/// setting as last sent (negative = from the display), 6 Windows' SDR white (negative if unknown).
s64 pc_get_hdr_output(u64 which) {
  const auto& h = Gfx::g_hdr_present;
  switch (which) {
    case 0:
      return h.active && h.hdr ? 1000 : 0;
    case 1:
      return std::llround(h.display_peak_nits * 1000.0);
    case 2:
      return std::llround(h.display_black_nits * 1000.0);
    case 3:
      return std::llround(h.peak_nits * 1000.0);
    case 4:
      return std::llround(h.black_nits * 1000.0);
    case 5:
      return std::llround(Gfx::g_global_settings.hdr_peak_nits * 1000.0);
    case 6:
      return std::llround(h.desktop_white_nits * 1000.0);
    default:
      lg::error("pc-get-hdr-output: unknown value {}", which);
      return 0;
  }
}
