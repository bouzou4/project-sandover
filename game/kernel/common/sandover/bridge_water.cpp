#include "game/kernel/common/sandover/bridge.h"

#include <algorithm>
#include <cmath>

#include "common/log/log.h"

#include "game/graphics/gfx.h"
#include "game/graphics/opengl_renderer/LavaField.h"
#include "game/kernel/common/Ptr.h"
#include "game/kernel/common/kernel_types.h"

void pc_set_water(u64 which, s64 value) {
  auto& s = Gfx::g_global_settings;
  const float v = value / 1000000.f;
  switch ((WaterOption)which) {
    case WaterOption::ENABLE:
      s.water_enable = value != 0;
      break;
    case WaterOption::WAVE:
      s.water_wave = std::max(v, 0.f);
      break;
    case WaterOption::GLINT:
      s.water_glint = std::max(v, 0.f);
      break;
    case WaterOption::GLINT_EXP:
      s.water_glint_exp = std::max(v, 1.f);
      break;
    case WaterOption::FRESNEL:
      s.water_fresnel = std::clamp(v, 0.f, 1.f);
      break;
    case WaterOption::SPARKLE:
      s.water_sparkle = std::max(v, 0.f);
      break;
    case WaterOption::VIEW:
      s.water_view = (int)std::lround(v);
      break;
    case WaterOption::DISPLACE:
      s.water_displace = std::max(v, 0.f);
      break;
    case WaterOption::SUBDIV:
      s.water_subdiv = std::clamp((int)std::lround(v), 1, 32);
      break;
    case WaterOption::RANGE_FULL_M:
      s.water_range_full_m = std::max(v, 0.f);
      break;
    case WaterOption::RANGE_ZERO_M:
      s.water_range_zero_m = std::max(v, 1.f);
      break;
    case WaterOption::CREST_FOAM:
      s.water_crest_foam = std::clamp(v, 0.f, 1.f);
      break;
    case WaterOption::CREST_J:
      s.water_crest_j = v;
      break;
    case WaterOption::SHORE_FOAM:
      s.water_shore_foam = std::clamp(v, 0.f, 1.f);
      break;
    case WaterOption::SHORE_M:
      s.water_shore_m = std::max(v, 0.01f);
      break;
    case WaterOption::SHALLOW:
      s.water_shallow = std::clamp(v, 0.f, 1.f);
      break;
    case WaterOption::SHALLOW_M:
      s.water_shallow_m = std::max(v, 0.01f);
      break;
    case WaterOption::SSS:
      s.water_sss = std::max(v, 0.f);
      break;
    case WaterOption::CREST_LIGHT:
      s.water_crest_light = std::clamp(v, 0.f, 1.f);
      break;
    case WaterOption::ABSORB:
      s.water_absorb = std::max(v, 0.f);
      break;
    case WaterOption::SKY_REFL:
      s.water_sky_refl = std::clamp(v, 0.f, 1.f);
      break;
    case WaterOption::SKY_HAZE_M:
      s.water_sky_haze_m = std::max(v, 1.f);
      break;
    case WaterOption::AMP_NOISE:
      s.water_amp_noise = std::clamp(v, 0.f, 1.f);
      break;
    case WaterOption::PHASE_NOISE:
      s.water_phase_noise = std::max(v, 0.f);
      break;
    case WaterOption::NOISE_M:
      s.water_noise_m = std::max(v, 1.f);
      break;
    case WaterOption::SHALLOW_DEPTH_M:
      s.water_shallow_depth_m = std::max(v, 0.05f);
      break;
    case WaterOption::SHORE_FADE_NEAR_M:
      s.water_shore_fade_near_m = std::max(v, 0.f);
      break;
    case WaterOption::SHORE_FADE_FAR_M:
      s.water_shore_fade_far_m = std::max(v, 1.f);
      break;
    case WaterOption::SHOAL_M:
      s.water_shoal_m = std::max(v, 0.f);
      break;
    case WaterOption::TILE_HEX:
      s.water_tile_hex = std::clamp(v, 0.f, 1.f);
      break;
    case WaterOption::TILE_HEX_M:
      s.water_tile_hex_m = std::max(v, 1.f);
      break;
    case WaterOption::TILE_HEX_ROT_DEG:
      s.water_tile_hex_rot_deg = std::clamp(v, 0.f, 180.f);
      break;
    case WaterOption::TILE_WARP_M:
      s.water_tile_warp_m = std::max(v, 0.f);
      break;
    case WaterOption::TILE_WARP_CELL_M:
      s.water_tile_warp_cell_m = std::max(v, 1.f);
      break;
    case WaterOption::AA_LOD_START:
      s.water_aa_lod_start = v;
      break;
    case WaterOption::AA_LOD_END:
      s.water_aa_lod_end = v;
      break;
    case WaterOption::AA_FOAM_EDGE_PX:
      s.water_aa_foam_edge_px = std::max(v, 0.f);
      break;
    case WaterOption::AA_WAVE_PX_FULL:
      s.water_aa_wave_px_full = std::max(v, 0.f);
      break;
    case WaterOption::AA_WAVE_PX_ZERO:
      s.water_aa_wave_px_zero = std::max(v, 0.f);
      break;
    case WaterOption::CLOCK_S:
      s.water_clock_s = v;
      break;
    case WaterOption::SHOAL_OCC:
      s.water_shoal_occ = std::min(v, 0.f);
      break;
    case WaterOption::DEPTH_BLUR_M:
      s.water_depth_blur_m = std::max(v, 0.f);
      break;
    case WaterOption::TESS_SEG_M:
      s.water_tess_seg_m = std::max(v, 0.01f);
      break;
    case WaterOption::SUN_GLINT:
      s.water_sun_glint = value != 0;
      break;
    case WaterOption::SUN_GLINT_STRENGTH:
      s.water_sun_glint_strength = std::max(v, 0.f);
      break;
    case WaterOption::SUN_GLINT_ROUGH:
      s.water_sun_glint_rough = std::clamp(v, 0.01f, 0.5f);
      break;
    case WaterOption::SUN_GLINT_PEAK:
      s.water_sun_glint_peak = std::max(v, 0.01f);
      break;
    case WaterOption::LOD_START_M:
      s.water_lod_start_m = std::max(v, 0.f);
      break;
    case WaterOption::LOD_END_M:
      s.water_lod_end_m = std::max(v, 1.f);
      break;
    case WaterOption::OPAQUE_M:
      s.water_opaque_m = std::max(v, 0.05f);
      break;
    case WaterOption::ABSORB_R:
    case WaterOption::ABSORB_G:
    case WaterOption::ABSORB_B:
      s.water_absorb_rgb[which - (u64)WaterOption::ABSORB_R] = std::max(v, 0.f);
      break;
    case WaterOption::LOD_PROBE:
      s.water_lod_probe = (int)std::llround(v);
      break;
    case WaterOption::SUN_GLINT_RESET: {
      static const GfxGlobalSettings d{};
      s.water_sun_glint_strength = d.water_sun_glint_strength;
      s.water_sun_glint_rough = d.water_sun_glint_rough;
      s.water_sun_glint_peak = d.water_sun_glint_peak;
      break;
    }
    default: {
      int i = (int)which - (int)WaterOption::WAVE0;
      WaterWave* set = s.water_waves;
      if (i < 0 || i >= kWaterWaves * 5) {
        i = (int)which - (int)WaterOption::LAKE_WAVE0;
        set = s.water_lake_waves;
      }
      if (i < 0 || i >= kWaterWaves * 5) {
        lg::error("pc-set-water: unknown option {}", which);
        break;
      }
      auto& w = set[i / 5];
      switch ((WaterWaveField)(i % 5)) {
        case WaterWaveField::DIR_DEG:
          w.dir_deg = v;
          break;
        case WaterWaveField::AMP_M:
          w.amp_m = std::max(v, 0.f);
          break;
        case WaterWaveField::LENGTH_M:
          w.length_m = std::max(v, 0.1f);
          break;
        case WaterWaveField::SPEED:
          w.speed = v;
          break;
        case WaterWaveField::STEEP:
          w.steep = std::clamp(v, 0.f, 1.f);
          break;
      }
    }
  }
}

void pc_set_swamp(u64 which, s64 value) {
  auto& s = Gfx::g_global_settings;
  const float v = value / 1000000.f;
  const float v01 = std::clamp(v, 0.f, 1.f);
  const float vpos = std::max(v, 0.f);
  switch ((SwampOption)which) {
    case SwampOption::WATER:
      s.swamp_water = value != 0;
      break;
    case SwampOption::WATER_DARK:
      s.swamp_water_dark = v01;
      break;
    case SwampOption::WATER_REFL:
      s.swamp_water_refl = vpos;
      break;
    case SwampOption::WATER_RIPPLE:
      s.swamp_water_ripple = vpos;
      break;
    case SwampOption::WATER_GLINT:
      s.swamp_water_glint = vpos;
      break;
    case SwampOption::WET:
      s.swamp_wet = value != 0;
      break;
    case SwampOption::WET_DARK:
      s.swamp_wet_dark = v01;
      break;
    case SwampOption::WET_COAT:
      s.swamp_wet_coat = vpos;
      break;
    case SwampOption::WET_PUDDLE:
      s.swamp_wet_puddle = v01;
      break;
    case SwampOption::MIST:
      s.swamp_mist = value != 0;
      break;
    case SwampOption::MIST_DENSITY:
      s.swamp_mist_density = vpos;
      break;
    case SwampOption::MIST_HEIGHT_M:
      s.swamp_mist_height_m = std::max(v, 0.05f);
      break;
    case SwampOption::MIST_START_M:
      s.swamp_mist_start_m = vpos;
      break;
    case SwampOption::MIST_MAX:
      s.swamp_mist_max = v01;
      break;
    case SwampOption::MIST_SUN:
      s.swamp_mist_sun = vpos;
      break;
    case SwampOption::MIST_BRIGHT:
      s.swamp_mist_bright = v01;
      break;
    case SwampOption::SHORE:
      s.shore_wet = value != 0;
      break;
    case SwampOption::SHORE_BAND_M:
      s.shore_wet_band_m = std::max(v, 0.05f);
      break;
    case SwampOption::SHORE_DARK:
      s.shore_wet_dark = v01;
      break;
    case SwampOption::SHORE_FILM:
      s.shore_wet_film = vpos;
      break;
    case SwampOption::SHORE_CELL_M:
      s.shore_wet_cell_m = std::max(v, 0.1f);
      break;
    case SwampOption::MISTY_MIST:
      s.misty_mist = value != 0;
      break;
    case SwampOption::MISTY_MIST_DENSITY:
      s.misty_mist_density = vpos;
      break;
    case SwampOption::MISTY_MIST_HEIGHT_M:
      s.misty_mist_height_m = std::max(v, 0.05f);
      break;
    case SwampOption::MISTY_MIST_START_M:
      s.misty_mist_start_m = vpos;
      break;
    case SwampOption::MISTY_MIST_MAX:
      s.misty_mist_max = v01;
      break;
    case SwampOption::MISTY_MIST_BRIGHT:
      s.misty_mist_bright = v01;
      break;
    case SwampOption::MISTY_MIST_STRENGTH:
      s.misty_mist_strength = std::clamp(v, 0.f, 4.f);
      break;
    case SwampOption::WET_PBR:
      s.wet_pbr = value != 0;
      break;
    case SwampOption::WET_PBR_GLOSS:
      s.wet_pbr_gloss = v01;
      break;
    case SwampOption::WET_PBR_ROUGH:
      s.wet_pbr_rough = std::clamp(v, 0.04f, 1.f);
      break;
    case SwampOption::WET_PBR_F0:
      s.wet_pbr_f0 = v01;
      break;
    case SwampOption::WET_PBR_DARK:
      s.wet_pbr_dark = v01;
      break;
    case SwampOption::WET_PBR_SPLASH_FULL_M:
      s.wet_pbr_splash_full_m = vpos;
      break;
    case SwampOption::WET_PBR_SPLASH_TOP_M:
      s.wet_pbr_splash_top_m = vpos;
      break;
    default:
      lg::error("pc-set-swamp: unknown option {}", which);
  }
}

void pc_set_lava(u64 which, s64 value) {
  auto& s = Gfx::g_global_settings;
  const float v = value / 1000000.f;
  const float vpos = std::max(v, 0.f);
  switch ((LavaOption)which) {
    case LavaOption::GLOW:
      s.lava_glow = value != 0;
      break;
    case LavaOption::GLOW_GAIN:
      s.lava_glow_gain = vpos;
      break;
    case LavaOption::GLOW_MASK:
      s.lava_glow_mask = std::clamp(v, 0.f, 1.f);
      break;
    case LavaOption::GLOW_KEY_LO:
      s.lava_glow_key_lo = v;
      break;
    case LavaOption::GLOW_KEY_HI:
      s.lava_glow_key_hi = v;
      break;
    case LavaOption::UNDER:
      s.lava_under = value != 0;
      break;
    case LavaOption::UNDER_GAIN:
      s.lava_under_gain = vpos;
      break;
    case LavaOption::UNDER_REACH_M:
      s.lava_under_reach_m = std::max(v, 1.f);
      break;
    case LavaOption::UNDER_REFL:
      s.lava_under_refl = vpos;
      break;
    case LavaOption::UNDER_BAKE:
      s.lava_under_bake = std::clamp(v, 0.f, 1.f);
      break;
    case LavaOption::GLOW_CORE:
      s.lava_glow_core = vpos;
      break;
    case LavaOption::GLOW_CRUST:
      s.lava_glow_crust = std::clamp(v, 0.f, 1.f);
      break;
    case LavaOption::UNDER_CHAR:
      s.lava_under_char = vpos;
      break;
    case LavaOption::PIN:
      s.lava_pin = value != 0;
      break;
    case LavaOption::RESET: {
      static const GfxGlobalSettings d{};
      s.lava_glow = d.lava_glow;
      s.lava_glow_gain = d.lava_glow_gain;
      s.lava_glow_mask = d.lava_glow_mask;
      s.lava_glow_key_lo = d.lava_glow_key_lo;
      s.lava_glow_key_hi = d.lava_glow_key_hi;
      s.lava_glow_core = d.lava_glow_core;
      s.lava_glow_crust = d.lava_glow_crust;
      s.lava_under = d.lava_under;
      s.lava_under_gain = d.lava_under_gain;
      s.lava_under_reach_m = d.lava_under_reach_m;
      s.lava_under_refl = d.lava_under_refl;
      s.lava_under_bake = d.lava_under_bake;
      s.lava_under_char = d.lava_under_char;
      s.lava_view = d.lava_view;
      s.lava_pin = d.lava_pin;
    } break;
    case LavaOption::VIEW:
      s.lava_view = (int)std::clamp<s64>(value / 1000000, 0, 2);
      break;
    default:
      lg::error("pc-set-lava: unknown option {}", which);
  }
}

/// A level's lava light this frame (mood light-group 0 dir2 while it points down; level 0 when
/// it does not) and the lava clock's factor in millionths; colour packed 10 bits per channel,
/// bit 30: vanilla lights the characters with it.
void pc_lava_light(u32 level_str, s64 level, s64 clock, u64 rgb) {
  lava_field::goal_light(Ptr<String>(level_str).c()->data(), level / 1000000.f,
                         clock / 1000000.f, (rgb & 1023) / 1000.f, ((rgb >> 10) & 1023) / 1000.f,
                         ((rgb >> 20) & 1023) / 1000.f, (rgb >> 30) & 1);
}

void pc_set_caustics(u64 which, s64 value) {
  auto& s = Gfx::g_global_settings;
  const float v = value / 1000000.f;
  const float vpos = std::max(v, 0.f);
  switch ((CausticOption)which) {
    case CausticOption::MODE:
      s.caustic_mode = value != 0 ? 1 : 0;
      break;
    case CausticOption::CELL_SCALE:
      s.caustic_cell_scale = std::max(v, 0.01f);
      break;
    case CausticOption::LOOP_HZ:
      s.caustic_loop_hz = vpos;
      break;
    case CausticOption::DRIFT_MPS:
      s.caustic_drift_mps = vpos;
      break;
    case CausticOption::AMP:
      s.caustic_amp = vpos;
      break;
    case CausticOption::WEB:
      s.caustic_web = vpos;
      break;
    case CausticOption::WEB_MEAN:
      s.caustic_web_mean = std::clamp(v, 0.f, 1.f);
      break;
    case CausticOption::CLOCK_S:
      s.caustic_clock_hold_s = v;
      break;
    default:
      lg::error("pc-set-caustics: unknown option {}", which);
  }
}

void pc_set_inland_water(u64 which, s64 value) {
  auto& s = Gfx::g_global_settings;
  const float v = value / 1000000.f;
  const float v01 = std::clamp(v, 0.f, 1.f);
  const float vpos = std::max(v, 0.f);
  switch ((InlandWaterOption)which) {
    case InlandWaterOption::WATER:
      s.inland_water = value != 0;
      break;
    case InlandWaterOption::DEPTH_M:
      s.inland_water_depth_m = std::max(v, 0.01f);
      break;
    case InlandWaterOption::REFL:
      s.inland_water_refl = vpos;
      break;
    case InlandWaterOption::REFL_CAP:
      s.inland_water_refl_cap = v01;
      break;
    case InlandWaterOption::GLINT:
      s.inland_water_glint = vpos;
      break;
    case InlandWaterOption::MUD:
      s.inland_mud = value != 0;
      break;
    case InlandWaterOption::MUD_DARK:
      s.inland_mud_dark = v01;
      break;
    case InlandWaterOption::MUD_REFL_CAP:
      s.inland_mud_refl_cap = v01;
      break;
    case InlandWaterOption::MUD_ROUGH:
      s.inland_mud_rough = std::clamp(v, 0.05f, 1.f);
      break;
    case InlandWaterOption::MUD_GLINT:
      s.inland_mud_glint = vpos;
      break;
    case InlandWaterOption::VIEW:
      s.inland_water_view = (int)std::lround(v);
      break;
    case InlandWaterOption::PICKED:
      s.inland_water_picked = value != 0;
      break;
    default:
      lg::error("pc-set-inland-water: unknown option {}", which);
  }
}
