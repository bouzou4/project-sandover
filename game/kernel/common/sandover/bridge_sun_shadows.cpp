#include "game/kernel/common/sandover/bridge.h"

#include <algorithm>
#include <cmath>

#include "common/log/log.h"

#include "game/graphics/gfx.h"

u64 pc_get_sky_bodies() {
  const auto& s = Gfx::g_global_settings;
  return s.sun_disc ? (u64)s.drawn_sky_bodies : 0;
}

void pc_set_sun_option(u64 which, s64 value) {
  auto& s = Gfx::g_global_settings;
  const float v = value / 1000000.f;
  const int iv = (int)std::lround(v);
  switch ((SunOption)which) {
    case SunOption::ENABLE:
      s.sun_enable = value != 0;
      break;
    case SunOption::MODE:
      s.sun_mode = iv;
      break;
    case SunOption::OFFSET_DEG:
      s.sun_offset_deg = v;
      break;
    case SunOption::NIGHT:
      s.sun_night = iv;
      break;
    case SunOption::STRENGTH:
      s.sun_strength = v;
      break;
    case SunOption::VIEW:
      s.sun_view = iv;
      break;
    case SunOption::ARC_POS:
      s.sun_arc_pos = v;
      break;
    case SunOption::DISC:
      s.sun_disc = value != 0;
      break;
    case SunOption::DISC_SIZE_DEG:
      s.sun_disc_size_deg = std::clamp(v, 0.05f, 20.f);
      break;
    case SunOption::DISC_GLARE:
      s.sun_disc_glare = std::clamp(v, 0.f, 1.f);
      break;
    case SunOption::DISC_CORE:
      s.sun_disc_core = std::max(v, 0.f);
      break;
    case SunOption::DISC_RIM:
      s.sun_disc_rim = std::max(v, 0.f);
      break;
    case SunOption::DISC_SDR_CORE:
      s.sun_disc_sdr_core = std::max(v, 0.f);
      break;
    case SunOption::DISC_SDR_RIM:
      s.sun_disc_sdr_rim = std::max(v, 0.f);
      break;
    case SunOption::DISC_FADE:
      s.sun_disc_fade = std::clamp(v, 0.01f, 1.f);
      break;
    case SunOption::DISC_HDR:
      s.sun_disc_hdr = std::clamp(iv, 0, 2);
      break;
    case SunOption::DISC_RESET: {
      static const GfxGlobalSettings d{};
      s.sun_disc_size_deg = d.sun_disc_size_deg;
      s.sun_disc_glare = d.sun_disc_glare;
      s.sun_disc_core = d.sun_disc_core;
      s.sun_disc_rim = d.sun_disc_rim;
      s.sun_disc_sdr_core = d.sun_disc_sdr_core;
      s.sun_disc_sdr_rim = d.sun_disc_sdr_rim;
      s.sun_disc_fade = d.sun_disc_fade;
      s.sun_disc_hdr = d.sun_disc_hdr;
      break;
    }
    case SunOption::SKY_STYLE:
      s.sky_style = std::clamp(iv, 0, 1);
      break;
    case SunOption::SKY_BURST:
      s.sky_burst = std::clamp(v, 0.f, 4.f);
      break;
    case SunOption::SKY_WASH:
      s.sky_wash = std::clamp(v, 0.f, 4.f);
      break;
    case SunOption::SKY_GLOW:
      s.sky_glow = std::max(v, 0.f);
      break;
    case SunOption::SKY_MOON:
      s.sky_moon = std::max(v, 0.f);
      break;
    case SunOption::SKY_MOON_SDR:
      s.sky_moon_sdr = std::max(v, 0.f);
      break;
    case SunOption::SKY_STARS:
      s.sky_stars = std::clamp(v, 0.f, 4.f);
      break;
    case SunOption::SKY_MOON_DEG:
      s.sky_moon_deg = std::clamp(v, 0.1f, 20.f);
      break;
    case SunOption::SKY_GLOW_SDR:
      s.sky_glow_sdr = std::max(v, 0.f);
      break;
    case SunOption::SKY_RESET: {
      static const GfxGlobalSettings d{};
      s.sky_style = d.sky_style;
      s.sky_burst = d.sky_burst;
      s.sky_wash = d.sky_wash;
      s.sky_glow = d.sky_glow;
      s.sky_moon = d.sky_moon;
      s.sky_moon_sdr = d.sky_moon_sdr;
      s.sky_stars = d.sky_stars;
      s.sky_moon_deg = d.sky_moon_deg;
      s.sky_glow_sdr = d.sky_glow_sdr;
      break;
    }
    default:
      lg::error("pc-set-sun-option: unknown option {}", which);
  }
}

// world sun map size and cascades per shadow quality tier (option 40): low, medium, high
constexpr int kWorldShadowTier[3][2] = {{1024, 2}, {2048, 3}, {4096, 3}};

void pc_set_shadow_option(u64 which, s64 value) {
  auto& s = Gfx::g_global_settings;
  const float v = value / 1000000.f;
  const int iv = (int)std::lround(v);
  switch ((ShadowOption)which) {
    case ShadowOption::MODE:
      s.shadow_mode = std::clamp(iv, 0, 1);
      break;
    case ShadowOption::STRENGTH:
      s.shadow_strength = v;
      break;
    case ShadowOption::VIEW:
      s.shadow_view = iv;
      break;
    case ShadowOption::RANGE_M:
      s.shadow_range_m = std::max(v, 1.f);
      break;
    case ShadowOption::LAMP_SCALE:
      s.shadow_lamp_scale = v;
      break;
    case ShadowOption::MARKER:
      s.shadow_marker = std::max(v, 0.f);
      break;
    case ShadowOption::REACH_M:
      s.shadow_reach_m = std::max(v, 0.1f);
      break;
    case ShadowOption::LIGHTS:
      s.shadow_lights = std::clamp(iv, 0, 4);
      break;
    case ShadowOption::LIGHT_R0_M:
      s.shadow_light_r0_m = std::max(v, 0.1f);
      break;
    case ShadowOption::LIGHT_REACH_M:
      s.shadow_light_reach_m = std::max(v, 1.f);
      break;
    case ShadowOption::CAVE_DARK:
      s.shadow_cave_dark = std::clamp(v, 0.f, 1.f);
      break;
    case ShadowOption::TERRAIN_SUN:
      s.shadow_terrain_sun = std::clamp(v, 0.f, 1.f);
      break;
    case ShadowOption::SKY_HUE:
      s.shadow_sky_hue = v != 0.f;
      break;
    case ShadowOption::SKY_HUE_STRENGTH:
      s.shadow_sky_hue_strength = std::clamp(v, 0.f, 2.f);
      break;
    case ShadowOption::WORLD:
      s.shadow_world = v != 0.f;
      break;
    case ShadowOption::WORLD_ACTORS:
      s.shadow_world_actors = v != 0.f;
      break;
    case ShadowOption::WORLD_WATER:
      s.shadow_world_water = v != 0.f;
      break;
    case ShadowOption::WORLD_SIZE:
      s.shadow_world_size = iv >= 4096 ? 4096 : (iv >= 2048 ? 2048 : 1024);
      break;
    case ShadowOption::WORLD_CASCADES:
      s.shadow_world_cascades = std::clamp(iv, 1, 3);
      break;
    case ShadowOption::WORLD_PENUMBRA:
      s.shadow_world_penumbra = std::clamp(v, 0.f, 0.1f);
      break;
    case ShadowOption::WORLD_RANGE_M:
      s.shadow_world_range_m = std::clamp(v, 20.f, 400.f);
      break;
    case ShadowOption::WORLD_RESET: {
      static const GfxGlobalSettings d{};
      s.shadow_world = d.shadow_world;
      s.shadow_world_actors = d.shadow_world_actors;
      s.shadow_world_water = d.shadow_world_water;
      s.shadow_world_size = kWorldShadowTier[std::clamp(s.shadow_quality, 0, 2)][0];
      s.shadow_world_cascades = kWorldShadowTier[std::clamp(s.shadow_quality, 0, 2)][1];
      s.shadow_world_penumbra = d.shadow_world_penumbra;
      s.shadow_world_range_m = d.shadow_world_range_m;
      break;
    }
    case ShadowOption::CONTACT:
      s.shadow_contact = std::clamp(iv, 0, 2);
      break;
    case ShadowOption::CONTACT_VIEW:
      s.shadow_contact_view = std::clamp(iv, 0, 4);
      break;
    case ShadowOption::PARALLAX_SHADOW:
      s.pbr_px_shadow = std::clamp(iv, 0, 2);
      break;
    case ShadowOption::PROBE:
      s.shadow_probe = std::max(iv, 0);
      break;
    case ShadowOption::QUALITY:
      s.shadow_quality = std::clamp(iv, 0, 2);
      s.shadow_world_size = kWorldShadowTier[s.shadow_quality][0];
      s.shadow_world_cascades = kWorldShadowTier[s.shadow_quality][1];
      break;
    case ShadowOption::CANOPY:
      s.shadow_canopy = std::clamp(v, 0.f, 1.f);
      break;
    case ShadowOption::CANOPY_AMBIENT:
      s.shadow_canopy_ambient = std::clamp(v, 0.f, 0.8f);
      break;
    case ShadowOption::CANOPY_PATCH:
      s.shadow_canopy_patch = std::clamp(v, 0.f, 0.5f);
      break;
    case ShadowOption::CANOPY_SHARP:
      s.shadow_canopy_sharp = std::clamp(v, 0.f, 1.f);
      break;
    case ShadowOption::CANOPY_PENUMBRA:
      s.shadow_canopy_penumbra = std::clamp(v, 0.f, 0.2f);
      break;
    case ShadowOption::CANOPY_TINT:
      s.shadow_canopy_tint = std::clamp(v, 0.f, 1.f);
      break;
    default:
      lg::error("pc-set-shadow-option: unknown option {}", which);
  }
}

void pc_set_sun_dir(s64 x, s64 y, s64 z) {
  auto& d = Gfx::g_global_settings.sun_dir;
  d[0] = x / 1000000.f;
  d[1] = y / 1000000.f;
  d[2] = z / 1000000.f;
}
