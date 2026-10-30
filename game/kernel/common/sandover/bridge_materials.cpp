#include "game/kernel/common/sandover/bridge.h"

#include <algorithm>
#include <cmath>

#include "common/log/log.h"

#include "game/graphics/gfx.h"
#include "game/graphics/opengl_renderer/Materials.h"

void pc_set_fur(u64 which, s64 value) {
  auto& s = Gfx::g_global_settings;
  const float v = value / 1000000.f;
  switch ((FurOption)which) {
    case FurOption::ENABLE:
      s.fur_enable = value != 0;
      break;
    case FurOption::SHELLS:
      s.fur_shells = std::clamp((int)std::lround(v), 1, 64);
      break;
    case FurOption::LENGTH_M:
      s.fur_length_m = v;
      break;
    case FurOption::DENSITY:
      s.fur_density = v;
      break;
    case FurOption::GRAVITY:
      s.fur_gravity = v;
      break;
    case FurOption::CURL:
      s.fur_curl = v;
      break;
    case FurOption::ROOT_DARK:
      s.fur_root_dark = v;
      break;
    case FurOption::VIEW:
      s.fur_view = (int)std::lround(v);
      break;
    case FurOption::SIZE_EXP:
      s.fur_size_exp = std::max(v, 0.f);
      break;
    case FurOption::SIZE_REF_M:
      s.fur_size_ref_m = std::max(v, 1e-3f);
      break;
    default:
      lg::error("pc-set-fur: unknown option {}", which);
  }
}

void pc_set_pbr_param(u64 which, s64 value) {
  auto& s = Gfx::g_global_settings;
  const float v = value / 1000000.f;
  switch ((PbrParam)which) {
    case PbrParam::ENABLE:
      s.pbr_enable = value != 0;
      break;
    case PbrParam::STRENGTH:
      s.pbr_strength = v;
      break;
    case PbrParam::DETAIL:
      s.pbr_detail = v;
      break;
    case PbrParam::SPEC:
      s.pbr_spec = v;
      break;
    case PbrParam::VIEW:
      s.pbr_view = (int)std::lround(v);
      break;
    case PbrParam::MERC:
      s.pbr_merc = value != 0;
      break;
    case PbrParam::MERC_VSIGN:
      s.pbr_merc_vsign = v < 0 ? -1.f : 1.f;
      break;
    case PbrParam::BACKGROUND:
      s.pbr_background = value != 0;
      break;
    case PbrParam::KEY:
      s.pbr_key = v;
      break;
    case PbrParam::SOURCE:
      s.pbr_source = value != 0 ? 1 : 0;
      break;
    case PbrParam::TIMING:
      s.pbr_timing = value != 0;
      break;
    case PbrParam::LOOK:
      s.pbr_look = (int)std::lround(v);
      pbr_load_preset();
      break;
    case PbrParam::METALLIC:
      s.pbr_metallic = v;
      break;
    case PbrParam::ENV:
      s.pbr_env = v;
      break;
    case PbrParam::FRESNEL:
      s.pbr_fresnel = v;
      break;
    case PbrParam::AMBIENT:
      s.pbr_ambient = v;
      break;
    case PbrParam::ROUGH_CONTRAST:
      s.pbr_rough_contrast = v;
      break;
    case PbrParam::BAND:
      s.pbr_band = v;
      break;
    case PbrParam::LOBE:
      s.pbr_lobe = v;
      break;
    case PbrParam::METAL_DARKEN:
      s.pbr_metal_darken = v;
      break;
    case PbrParam::MERC_UP:
      s.pbr_merc_up = v < 0 ? -1.f : 1.f;
      break;
    case PbrParam::SPECULAR:
      s.pbr_specular = v;
      break;
    case PbrParam::PARALLAX:
      s.pbr_parallax = value != 0;
      break;
    case PbrParam::PARALLAX_DEPTH:
      s.pbr_px_depth = v;
      break;
    case PbrParam::PARALLAX_BASIS:
      s.pbr_px_basis = value != 0;
      break;
    case PbrParam::METAL_HUE:
      s.pbr_metal_hue = std::clamp(v, 0.f, 1.f);
      break;
    case PbrParam::TRANSMIT:
      s.pbr_transmit = std::max(v, 0.f);
      break;
    case PbrParam::WRAP:
      s.pbr_wrap = std::max(v, 0.f);
      break;
    case PbrParam::WIND_TIE:
      s.pbr_wind_tie = value != 0;
      break;
    case PbrParam::BAKE_WRAP:
      s.pbr_bake_wrap = std::max(v, 0.f);
      break;
    case PbrParam::SUN_OCCLUSION:
      s.pbr_transmit_occl = value != 0;
      break;
    case PbrParam::LEAF_DENSITY:
      s.pbr_transmit_density = std::clamp(v, 0.f, 4.f);
      break;
    case PbrParam::ICE:
      s.pbr_ice = std::max(v, 0.f);
      break;
    case PbrParam::GLINT:
      s.pbr_glint = std::max(v, 0.f);
      break;
    case PbrParam::PROBE:
      s.pbr_probe = (int)value;
      break;
    default:
      lg::error("pc-set-pbr-param: unknown param {}", which);
  }
}
