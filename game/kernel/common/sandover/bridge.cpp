#include "game/kernel/common/sandover/bridge.h"

namespace {
struct BridgeEntry {
  const char* name;
  void* fn;
  BridgeScope scope;
};

// Registration order is the symbol-table intern order; keep it.
const BridgeEntry kBridgeTable[] = {
    // The game clock's rate while an XR session paces the game (60), else 0.
    {"pc-get-xr-game-rate", (void*)pc_get_xr_game_rate, BridgeScope::AllGames},
    {"pc-set-ssao-debug", (void*)pc_set_ssao_debug, BridgeScope::Jak1},
    {"pc-set-ssao-depth-capture", (void*)pc_set_ssao_depth_capture, BridgeScope::Jak1},
    {"pc-set-ssao-option", (void*)pc_set_ssao_option, BridgeScope::Jak1},
    {"pc-set-ssao-preset", (void*)pc_set_ssao_preset, BridgeScope::Jak1},
    {"pc-set-ssao-param", (void*)pc_set_ssao_param, BridgeScope::Jak1},
    {"pc-set-hdr-target", (void*)pc_set_hdr_target, BridgeScope::Jak1},
    {"pc-set-tonemap", (void*)pc_set_tonemap, BridgeScope::Jak1},
    {"pc-set-hdr-output", (void*)pc_set_hdr_output, BridgeScope::Jak1},
    {"pc-get-hdr-output", (void*)pc_get_hdr_output, BridgeScope::Jak1},
    {"pc-get-aa-available", (void*)pc_get_aa_available, BridgeScope::Jak1},
    {"pc-set-bloom", (void*)pc_set_bloom, BridgeScope::Jak1},
    {"pc-set-aa", (void*)pc_set_aa, BridgeScope::Jak1},
    {"pc-set-sandover-path", (void*)pc_set_sandover_path, BridgeScope::Jak1},
    {"pc-temporal-aa-state", (void*)pc_temporal_aa_state, BridgeScope::Jak1},
    {"pc-get-xr-state", (void*)pc_get_xr_state, BridgeScope::Jak1},
    {"pc-get-xr-cull", (void*)pc_get_xr_cull, BridgeScope::Jak1},
    {"pc-set-xr-game-tan", (void*)pc_set_xr_game_tan, BridgeScope::Jak1},
    {"pc-set-motion-vectors", (void*)pc_set_motion_vectors, BridgeScope::Jak1},
    {"pc-motion-vectors-dump", (void*)pc_motion_vectors_dump, BridgeScope::Jak1},
    {"pc-set-motion-blur", (void*)pc_set_motion_blur, BridgeScope::Jak1},
    {"pc-set-perf-timing", (void*)pc_set_perf_timing, BridgeScope::Jak1},
    {"pc-set-inland-water", (void*)pc_set_inland_water, BridgeScope::Jak1},
    {"pc-set-perf-opt", (void*)pc_set_perf_opt, BridgeScope::Jak1},
    {"pc-set-fur", (void*)pc_set_fur, BridgeScope::Jak1},
    {"pc-snap-poll", (void*)pc_snap_poll, BridgeScope::Jak1},
    {"pc-bug-report-poll", (void*)pc_bug_report_poll, BridgeScope::Jak1},
    {"pc-bug-report-request", (void*)pc_bug_report_request, BridgeScope::Jak1},
    {"pc-bug-report-save", (void*)pc_bug_report_save, BridgeScope::Jak1},
    {"pc-set-water", (void*)pc_set_water, BridgeScope::Jak1},
    {"pc-set-swamp", (void*)pc_set_swamp, BridgeScope::Jak1},
    {"pc-set-lava", (void*)pc_set_lava, BridgeScope::Jak1},
    {"pc-lava-light", (void*)pc_lava_light, BridgeScope::Jak1},
    {"pc-set-caustics", (void*)pc_set_caustics, BridgeScope::Jak1},
    {"pc-set-emissive-lights", (void*)pc_set_emissive_lights, BridgeScope::Jak1},
    {"pc-world-lights-begin", (void*)pc_world_lights_begin, BridgeScope::Jak1},
    {"pc-world-lights-query", (void*)pc_world_lights_query, BridgeScope::Jak1},
    {"pc-world-lights-set", (void*)pc_world_lights_set, BridgeScope::Jak1},
    {"pc-eco-lights-begin", (void*)pc_eco_lights_begin, BridgeScope::Jak1},
    {"pc-eco-lights-push", (void*)pc_eco_lights_push, BridgeScope::Jak1},
    {"pc-eco-lights-known", (void*)pc_eco_lights_known, BridgeScope::Jak1},
    {"pc-eco-lights-end", (void*)pc_eco_lights_end, BridgeScope::Jak1},
    {"pc-launch-lights-push", (void*)pc_launch_lights_push, BridgeScope::Jak1},
    {"pc-launch-lights-end", (void*)pc_launch_lights_end, BridgeScope::Jak1},
    {"pc-set-inspect", (void*)pc_set_inspect, BridgeScope::Jak1},
    {"pc-inspect-pick", (void*)pc_inspect_pick, BridgeScope::Jak1},
    {"pc-set-sun-option", (void*)pc_set_sun_option, BridgeScope::Jak1},
    {"pc-get-sky-bodies", (void*)pc_get_sky_bodies, BridgeScope::Jak1},
    {"pc-set-pbr-param", (void*)pc_set_pbr_param, BridgeScope::Jak1},
    {"pc-set-sun-dir", (void*)pc_set_sun_dir, BridgeScope::Jak1},
    {"pc-set-shadow-option", (void*)pc_set_shadow_option, BridgeScope::Jak1},
    {"pc-set-stereo", (void*)pc_set_stereo, BridgeScope::Jak1},
    {"pc-set-stereo-option", (void*)pc_set_stereo_option, BridgeScope::Jak1},
    {"pc-set-xr", (void*)pc_set_xr, BridgeScope::Jak1},
    {"pc-set-xr-recentre", (void*)pc_set_xr_recentre, BridgeScope::Jak1},
    {"pc-set-xr-fake", (void*)pc_set_xr_fake, BridgeScope::Jak1},
    {"pc-set-xr-fake-option", (void*)pc_set_xr_fake_option, BridgeScope::Jak1},
    {"pc-set-audio-spatial", (void*)pc_set_audio_spatial, BridgeScope::Jak1},
    {"pc-set-audio-output", (void*)pc_set_audio_output, BridgeScope::Jak1},
    {"pc-get-audio-state", (void*)pc_get_audio_state, BridgeScope::Jak1},
    {"pc-get-audio-string", (void*)pc_get_audio_string, BridgeScope::Jak1},
    {"pc-audio-speaker-test", (void*)pc_audio_speaker_test, BridgeScope::Jak1},
    {"pc-set-vista", (void*)pc_set_vista, BridgeScope::Jak1},
    {"pc-set-vista-hide", (void*)pc_set_vista_hide, BridgeScope::Jak1},
    {"pc-set-vista-option", (void*)pc_set_vista_option, BridgeScope::Jak1},
    {"pc-set-vista-fog", (void*)pc_set_vista_fog, BridgeScope::Jak1},
    {"pc-vista-landmarks?", (void*)pc_vista_landmarks, BridgeScope::Jak1},
    {"pc-set-vista-neighbour-fog", (void*)pc_set_vista_neighbour_fog, BridgeScope::Jak1},
    {"pc-set-vista-haze", (void*)pc_set_vista_haze, BridgeScope::Jak1},
    {"pc-set-vista-neighbour-haze", (void*)pc_set_vista_neighbour_haze, BridgeScope::Jak1},
    {"pc-vista-neighbour?", (void*)pc_vista_neighbour, BridgeScope::Jak1},
    {"pc-set-vista-viewer", (void*)pc_set_vista_viewer, BridgeScope::Jak1},
};
}  // namespace

void register_sandover_bridge(
    const std::function<Ptr<Function>(const char*, void*)>& make_func_symbol_func,
    BridgeScope scope) {
  for (const auto& e : kBridgeTable) {
    if (e.scope == scope) {
      make_func_symbol_func(e.name, e.fn);
    }
  }
}
