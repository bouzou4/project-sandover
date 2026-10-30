#include "game/kernel/common/sandover/bridge.h"

#include <algorithm>
#include <string>

#include "game/graphics/bug_report.h"
#include "game/graphics/screenshot.h"
#include "game/kernel/common/Ptr.h"
#include "game/kernel/common/kernel_types.h"

u64 pc_snap_poll() {
  return snap::poll();
}

u64 pc_bug_report_poll() {
  return bug_report::poll();
}

void pc_bug_report_request(u32 source) {
  bug_report::request((bug_report::Source)std::clamp<int>((s32)source, 0, 2));
}

void pc_bug_report_save(u32 state) {
  const auto* s = Ptr<bug_report::GoalState>(state).c();
  auto str = [](u32 p) { return p ? std::string(Ptr<String>(p).c()->data()) : std::string(); };
  bug_report::save(*s, str(s->level), str(s->cont));
}
