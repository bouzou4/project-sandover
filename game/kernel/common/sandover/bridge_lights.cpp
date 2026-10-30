#include "game/kernel/common/sandover/bridge.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "common/log/log.h"

#include "game/graphics/gfx.h"
#include "game/graphics/opengl_renderer/WorldLights.h"
#include "game/graphics/screenshot.h"
#include "game/kernel/common/Ptr.h"
#include "game/kernel/common/kernel_types.h"

void pc_set_emissive_lights(u64 which, s64 value) {
  auto& s = Gfx::g_global_settings;
  const float v = value / 1000000.f;
  switch (which) {
    case 0:
      s.emissive_lights_enable = value != 0;
      break;
    case 1:
      s.emissive_lights_intensity = v;
      break;
    case 2:
      s.emissive_lights_radius = v;
      break;
    case 3:
      s.emissive_lights_falloff = v;
      break;
    case 4:
      s.emissive_lights_threshold = v;
      break;
    case 5:
      s.world_lights_count = (int)std::lround(v);
      break;
    case 6:
      s.world_lights_strong = v;
      break;
    case 7:
      s.world_lights_weak = v;
      break;
    case 8:
      s.world_lights_arc = v;
      break;
    case 9:
      s.world_lights_particle = v;
      break;
    case 10:
      s.world_lights_radius_scale = v;
      break;
    case 11:
      s.world_lights_radius_max = v;
      break;
    case 12:
      s.world_lights_lamp = v;
      break;
    case 13:
      s.emissive_lights_blend = v;
      break;
    case 14:
      s.emissive_lights_frames = std::max(1, (int)std::lround(v));
      break;
    case 15:
      s.emissive_lights_fade = std::max(1, (int)std::lround(v));
      break;
    case 16:
      s.world_lights_suppress = v;
      break;
    case 17:
      // stability burst: the next N frames are saved as <name>-fNN.png, lights logged
      g_screenshot_burst = std::max(0, (int)std::lround(v));
      break;
    case 18:
      s.world_lights_glow = v;
      break;
    case 19:
      s.world_lights_props = value != 0;
      break;
    case 27:
      s.eco_lights = value != 0;
      break;
    case 28:
      s.eco_lights_strength = std::max(v, 0.f);
      break;
    case 20:
      s.light_highlights = value != 0;
      break;
    case 21:
      s.light_highlights_strength = std::max(v, 0.f);
      break;
    case 22:
      s.light_highlights_max = std::max(v, 0.01f);
      break;
    case 23:
      s.light_highlights_damp = std::clamp(v, 0.f, 1.f);
      break;
    case 35:
      s.world_lights_glow_fade_m = std::max(v, 0.f);
      break;
    case 36:
      s.world_lights_glow_slots = std::max(0, (int)std::lround(v));
      break;
    case 37: {
      // glow prop ranking (35-36) and eco shot lights (27-28) back to the engine defaults
      static const GfxGlobalSettings d{};
      s.world_lights_glow_fade_m = d.world_lights_glow_fade_m;
      s.world_lights_glow_slots = d.world_lights_glow_slots;
      s.eco_lights = d.eco_lights;
      s.eco_lights_strength = d.eco_lights_strength;
      s.world_lights_launch = d.world_lights_launch;
      break;
    }
    case 38:
      s.world_lights_launch = value != 0;
      break;
    default:
      lg::error("pc-set-emissive-lights: unknown option {}", which);
  }
}

static bool world_lights_running() {
  const auto& s = Gfx::g_global_settings;
  return (s.ssao_enable && s.emissive_lights_enable && s.world_lights_count > 0) ||
         (s.shadow_mode != 0 && (s.shadow_lights > 0 || s.shadow_cave_dark < 1.f));
}

u64 pc_world_lights_begin(s64 hour) {
  return world_lights_running() ? world_lights::goal_begin((int)hour) : 0;
}

u64 pc_world_lights_query(s64 i) {
  return world_lights::goal_query((int)i);
}

void pc_world_lights_set(s64 i, s64 level_milli) {
  world_lights::goal_set((int)i, (int)level_milli);
}

u64 pc_eco_lights_begin() {
  const auto& s = Gfx::g_global_settings;
  const bool tiles = s.ssao_enable && s.emissive_lights_enable && s.world_lights_count > 0;
  const bool actors = s.shadow_mode != 0 && s.shadow_lights > 0;
  return world_lights::eco_begin() && (tiles || actors) ? 1 : 0;
}

// pos: a GOAL vector (game units); size_level = collision radius (game units) x 1024 + level
// 0..1000. Four arguments: on Windows the GOAL to C call passes no more.
void pc_eco_lights_push(u32 group, u32 pos, s64 size_level, s64 id) {
  constexpr float kM = 4096.f;
  float v[3];
  memcpy(v, Ptr<u8>(pos).c(), sizeof(v));
  const float pos_m[3] = {v[0] / kM, v[1] / kM, v[2] / kM};
  world_lights::eco_push(Ptr<String>(group).c()->data(), pos_m, (float)(size_level >> 10) / kM,
                         (float)(size_level & 1023) / 1000.f, (u32)id);
}

// one sparticle-launch-control spawn: group name, origin (GOAL vector, game units), entity aid
// (0: none), launcher quaternion (0: none)
void pc_launch_lights_push(u32 group, u32 pos, s64 aid, u32 quat) {
  constexpr float kM = 4096.f;
  if (!group || !pos) {
    return;
  }
  float v[3], q[4];
  memcpy(v, Ptr<u8>(pos).c(), sizeof(v));
  if (quat) {
    memcpy(q, Ptr<u8>(quat).c(), sizeof(q));
  }
  const float pos_m[3] = {v[0] / kM, v[1] / kM, v[2] / kM};
  world_lights::launch_push(Ptr<String>(group).c()->data(), pos_m, (u32)aid, quat ? q : nullptr);
}

void pc_launch_lights_end(s64 paused) {
  world_lights::launch_end(paused != 0);
}

u64 pc_eco_lights_known(u32 group) {
  return world_lights::eco_known(Ptr<String>(group).c()->data()) ? 1 : 0;
}

void pc_eco_lights_end() {
  world_lights::eco_end();
}
