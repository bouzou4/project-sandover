#pragma once

#include <functional>

#include "common/common_types.h"
#include "common/symbols.h"

#include "game/kernel/common/Ptr.h"
#include "game/kernel/common/kernel_types.h"
#include "game/kernel/common/kscheme.h"
#include "game/runtime.h"

inline bool symbol_to_bool(const u32 symptr) {
  return symptr != s7.offset;
}

inline u64 bool_to_symbol(const bool val) {
  return val ? static_cast<u64>(s7.offset) + true_symbol_offset(g_game_version) : s7.offset;
}

// Entry points the Sandover GOAL code calls (registered by register_sandover_bridge).
s64 pc_get_xr_game_rate();
void pc_set_ssao_debug(u32 symptr);
void pc_set_ssao_depth_capture(u32 symptr);
void pc_set_ssao_option(u64 which, u64 value);
void pc_set_ssao_preset(s64 index);
void pc_set_inspect(u64 enable);
void pc_inspect_pick(u64 x, u64 y, u64 verdict);
void pc_set_perf_timing(s64 frames);
void pc_set_perf_opt(s64 mask);
void pc_set_sandover_path(s64 on);
void pc_set_aa(u64 which, s64 value);
s64 pc_temporal_aa_state();
s64 pc_get_xr_state(u64 which);
s64 pc_get_xr_cull(u32 dst);
void pc_set_xr_game_tan(u32 tan_x, u32 tan_y);
void pc_set_motion_vectors(u64 which, s64 value);
void pc_motion_vectors_dump(u32 dir, s64 frames);
void pc_set_motion_blur(u64 which, s64 value);
void pc_set_bloom(u64 which, s64 value);
void pc_set_water(u64 which, s64 value);
void pc_set_swamp(u64 which, s64 value);
void pc_set_lava(u64 which, s64 value);
void pc_lava_light(u32 level_str, s64 level, s64 clock, u64 rgb);
void pc_set_caustics(u64 which, s64 value);
void pc_set_fur(u64 which, s64 value);
void pc_set_inland_water(u64 which, s64 value);
void pc_set_emissive_lights(u64 which, s64 value);
u64 pc_world_lights_begin(s64 hour);
u64 pc_world_lights_query(s64 i);
void pc_world_lights_set(s64 i, s64 level_milli);
u64 pc_eco_lights_begin();
void pc_eco_lights_push(u32 group, u32 pos, s64 size_level, s64 id);
void pc_launch_lights_push(u32 group, u32 pos, s64 aid, u32 quat);
void pc_launch_lights_end(s64 paused);
u64 pc_eco_lights_known(u32 group);
void pc_eco_lights_end();
void pc_set_ssao_param(u64 which, s64 value);
void pc_set_hdr_target(u32 symptr);
void pc_set_tonemap(u64 which, s64 value);
void pc_set_hdr_output(u64 which, s64 value);
s64 pc_get_hdr_output(u64 which);
s64 pc_get_aa_available(u64 which);
u64 pc_get_sky_bodies();
void pc_set_sun_option(u64 which, s64 value);
void pc_set_shadow_option(u64 which, s64 value);
void pc_set_pbr_param(u64 which, s64 value);
void pc_set_sun_dir(s64 x, s64 y, s64 z);
void pc_set_stereo(u32 symptr);
void pc_set_stereo_option(u64 which, u64 value);
void pc_set_xr(u32 symptr);
void pc_set_xr_recentre();
void pc_set_xr_fake(u32 symptr);
void pc_set_xr_fake_option(s64 which, s64 value);
void pc_set_audio_spatial(u32 mode_str);
void pc_set_audio_output(s64 output, u32 device_str);
s64 pc_get_audio_state(u64 which);
u32 pc_get_audio_string(u64 which, u64 idx, u32 dst);
void pc_audio_speaker_test();
void pc_set_vista(u32 level_str);
void pc_set_vista_hide(u32 filter_str);
void pc_set_vista_option(u64 which, u64 value);
u32 pc_vista_landmarks();
void pc_set_vista_viewer(u32 level_str);
u32 pc_vista_neighbour(u32 level_str);
void pc_set_vista_fog(u32 mode_str, u64 a, u64 b, u64 c);
void pc_set_vista_neighbour_fog(u32 level_str, u64 density, u64 tint, u64 cap);
void pc_set_vista_haze(u64 density, u64 start, u64 tint, u64 cap);
void pc_set_vista_neighbour_haze(u32 level_str, u64 scale);
u64 pc_snap_poll();
u64 pc_bug_report_poll();
void pc_bug_report_request(u32 source);
void pc_bug_report_save(u32 state);

enum class BridgeScope { AllGames, Jak1 };

// Registers the table entries of one scope, in table order.
void register_sandover_bridge(
    const std::function<Ptr<Function>(const char*, void*)>& make_func_symbol_func,
    BridgeScope scope);
