#include "game/kernel/common/sandover/bridge.h"

#include <algorithm>

#include "game/graphics/gfx.h"

void pc_set_inspect(u64 enable) {
  Gfx::g_global_settings.inspect_enable = enable != 0;
}

void pc_inspect_pick(u64 x, u64 y, u64 verdict) {
  auto& s = Gfx::g_global_settings;
  auto& p = s.inspect_picks[s.inspect_pick_seq % 16];
  p[0] = x / 1000000.f;
  p[1] = y / 1000000.f;
  p[2] = (float)verdict;
  s.inspect_pick_seq++;
}

void pc_set_perf_timing(s64 frames) {
  // negative: hitch log, frames of at least -frames ms (PerfTimers.h)
  Gfx::g_global_settings.perf_timing_window = (int)std::clamp<s64>(frames, -10000, 100000);
}

void pc_set_perf_opt(s64 mask) {
  Gfx::g_global_settings.perf_opt_mask = (u32)(mask & 0xffffffff);
}
