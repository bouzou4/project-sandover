#include "game/kernel/common/sandover/bridge.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "common/log/log.h"

#include "game/graphics/gfx.h"
#include "game/graphics/opengl_renderer/Neighbours.h"
#include "game/kernel/common/Ptr.h"
#include "game/kernel/common/kernel_types.h"

void pc_set_vista(u32 level_str) {
  auto& s = Gfx::g_global_settings;
  std::string name = Ptr<String>(level_str).c()->data();
  s.vista_enable = !name.empty() && name != "none";
  s.vista_neighbours = name == "neighbours";
  if (s.vista_enable) {
    strncpy(s.vista_level, name.c_str(), sizeof(s.vista_level) - 1);
  }
}

void pc_set_vista_hide(u32 filter_str) {
  auto& s = Gfx::g_global_settings;
  strncpy(s.vista_hide, Ptr<String>(filter_str).c()->data(), sizeof(s.vista_hide) - 1);
}

void pc_set_vista_option(u64 which, u64 value) {
  auto& s = Gfx::g_global_settings;
  switch (which) {
    case 0:
      s.vista_hide_standins = value != 0;
      break;
    case 1:
      s.vista_fog_mode = value ? VISTA_FOG_MAIN : VISTA_FOG_OWN;
      break;
    case 2:
      s.vista_fog_end_m = (float)std::max((s64)value, (s64)1);
      break;
    case 3:
      s.vista_draw_shrub = value != 0;
      break;
    case 4:
      s.vista_gpu_timing = value != 0;
      break;
    case 5:
      s.vista_tod_addr = (u32)value;
      break;
    case 6:
      s.vista_hour_override = (s64)value < 0 ? -1.f : (float)value / 100.f;
      break;
    case 7:
      s.vista_own_mood = value != 0;
      break;
    case 8:
      s.vista_draw_alpha = value != 0;
      break;
    case 9:
      s.vista_hide_medres = value != 0;
      break;
    case 10:
      s.vista_hide_neighbour_lowres = value != 0;
      break;
    case 11:
      s.vista_landmarks = value != 0;
      break;
    case 12:
      s.vista_standin_neighbours = value != 0;
      break;
    case 13:
      s.vista_fog_floor = value != 0;
      break;
    case 14:
      s.vista_probe = (int)value;
      break;
    case 15:
      s.vista_hiz = value != 0;
      break;
    default:
      lg::error("pc-set-vista-option: unknown option {}", which);
      break;
  }
}

/// True while neighbour mode wants landmark actors of undisplayed game levels (vista-landmarks.gc).
u32 pc_vista_landmarks() {
  const auto& s = Gfx::g_global_settings;
  return bool_to_symbol(s.vista_enable && s.vista_neighbours && s.vista_landmarks &&
                        neighbours::is_outdoor(neighbours::viewer()));
}

/// The level the player is in (level-get-target-inside), sent every frame; gates neighbour mode.
void pc_set_vista_viewer(u32 level_str) {
  const std::string name = Ptr<String>(level_str).c()->data();
  neighbours::set_viewer(name == "none" ? "" : name);
}

/// True while the loader keeps `level_str` resident as a neighbour the game has not loaded.
u32 pc_vista_neighbour(u32 level_str) {
  const auto& s = Gfx::g_global_settings;
  if (!(s.vista_enable && s.vista_neighbours)) {
    return bool_to_symbol(false);
  }
  std::vector<std::string> game, adj;
  neighbours::get_resident(&game, &adj);
  const std::string name = Ptr<String>(level_str).c()->data();
  return bool_to_symbol(std::find(adj.begin(), adj.end(), name) != adj.end());
}

/*!
 * Vista fog policy: mode is "vanilla", "push_out", "own" or "unified". push_out and own take
 * a = fog end in m; unified takes a = start in m, b = density in 0.001/km, c = scale height
 * in m (0: no height falloff). Negative values keep the current setting.
 */
void pc_set_vista_fog(u32 mode_str, u64 a, u64 b, u64 c) {
  auto& s = Gfx::g_global_settings;
  const std::string mode = Ptr<String>(mode_str).c()->data();
  const s64 pa = (s64)a, pb = (s64)b, pc = (s64)c;
  if (mode == "vanilla") {
    s.vista_fog_mode = VISTA_FOG_MAIN;
  } else if (mode == "push_out" || mode == "own") {
    s.vista_fog_mode = mode == "own" ? VISTA_FOG_OWN : VISTA_FOG_PUSH_OUT;
    if (pa > 0) {
      s.vista_fog_end_m = (float)pa;
    }
  } else if (mode == "unified") {
    s.vista_fog_mode = VISTA_FOG_UNIFIED;
    if (pa >= 0) {
      s.vista_atmo_start_m = (float)pa;
    }
    if (pb >= 0) {
      s.vista_atmo_density_per_km = pb / 1000.f;
    }
    if (pc >= 0) {
      s.vista_atmo_height_m = (float)pc;
    }
  } else {
    lg::error("pc-set-vista-fog: unknown mode {}", mode);
  }
}

namespace {
VistaNeighbourFog* vista_nb_fog_entry(const std::string& level, const char* who) {
  auto& s = Gfx::g_global_settings;
  for (int i = 0; i < s.vista_nb_fog_count; i++) {
    if (level == s.vista_nb_fog[i].level) {
      return &s.vista_nb_fog[i];
    }
  }
  if (s.vista_nb_fog_count >= (int)s.vista_nb_fog.size() ||
      level.size() >= sizeof(VistaNeighbourFog::level)) {
    lg::error("{}: cannot add {}", who, level);
    return nullptr;
  }
  VistaNeighbourFog* f = &s.vista_nb_fog[s.vista_nb_fog_count++];
  *f = VistaNeighbourFog();
  strncpy(f->level, level.c_str(), sizeof(f->level) - 1);
  return f;
}
}  // namespace

/*!
 * Per-neighbour unified-fog override: density scale in thousandths, fog colour tint packed as
 * 0xRRGGBB (128 per channel = 1.0) and fog cap (0-1) in thousandths. A negative value keeps the
 * default. Level "clear" drops every override. Kernel functions take at most four arguments.
 */
void pc_set_vista_neighbour_fog(u32 level_str, u64 density, u64 tint, u64 cap) {
  auto& s = Gfx::g_global_settings;
  const std::string level = Ptr<String>(level_str).c()->data();
  if (level == "clear") {
    s.vista_nb_fog_count = 0;
    return;
  }
  VistaNeighbourFog* f = vista_nb_fog_entry(level, "pc-set-vista-neighbour-fog");
  if (!f) {
    return;
  }
  auto milli = [](u64 v, float dflt) { return (s64)v < 0 ? dflt : (s64)v / 1000.f; };
  f->density_scale = milli(density, 1.f);
  for (int c = 0; c < 3; c++) {
    f->tint[c] = (s64)tint < 0 ? 1.f : ((tint >> (16 - 8 * c)) & 0xff) / 128.f;
  }
  f->max_fog = milli(cap, -1.f);
}

/*!
 * Haze in vista neighbour mode (unified fog), on every background draw past a start distance:
 * density in thousandths of 1/km (0: off), start distance in m, the haze colour as a tint of the
 * viewer's fog colour packed as 0xRRGGBB (128 per channel = 1.0), and the cap on the haze amount
 * in thousandths. A negative value keeps the current setting.
 */
void pc_set_vista_haze(u64 density, u64 start, u64 tint, u64 cap) {
  auto& s = Gfx::g_global_settings;
  if ((s64)density >= 0) {
    s.vista_haze_density_per_km = (s64)density / 1000.f;
  }
  if ((s64)start >= 0) {
    s.vista_haze_start_m = (float)(s64)start;
  }
  if ((s64)tint >= 0) {
    for (int c = 0; c < 3; c++) {
      s.vista_haze_tint[c] = ((tint >> (16 - 8 * c)) & 0xff) / 128.f;
    }
  }
  if ((s64)cap >= 0) {
    s.vista_haze_max = std::min((s64)cap / 1000.f, 1.f);
  }
}

/*!
 * Per-neighbour haze multiplier in thousandths (1000 = the global haze density).
 */
void pc_set_vista_neighbour_haze(u32 level_str, u64 scale) {
  const std::string level = Ptr<String>(level_str).c()->data();
  VistaNeighbourFog* f = vista_nb_fog_entry(level, "pc-set-vista-neighbour-haze");
  if (f) {
    f->haze_scale = (s64)scale < 0 ? 1.f : (s64)scale / 1000.f;
  }
}
