#pragma once

#include <string>
#include <vector>

#include "common/common_types.h"

/*!
 * Player bug reports (Jak 1). F7, the Sandover page's SAVE BUG REPORT row or (sandover-bug-report)
 * ask GOAL for the game state; GOAL answers with pc-bug-report-save, the next rendered frame is
 * kept, and a worker writes <bug-reports>/<id>/screenshot.png, preview.png, report.json and
 * <bug-reports>/sandover-bug-<id>.zip. <bug-reports> is <run dir>/bug-reports with
 * SANDOVER_RUN_DIR set, else <--config-path>/bug-reports, else <user config>/jak1/bug-reports.
 * report.json follows sandover/tools/sandover/bug_report.schema.json; absolute paths and the
 * user and machine names are replaced by placeholders.
 */
namespace bug_report {

/// Must match sandover-bug-state in goal_src/jak1/pc/features/bug-report.gc.
struct GoalState {
  // 0-2 Jak position, 3-6 Jak quaternion (xyzw), 7-9 camera position, 10-18 camera inv-rot rows,
  // 19 fov (GOAL angle units), 20 hour, 21-28 mood slot weights
  float data[32];
  u32 level;  // GOAL string
  u32 cont;   // GOAL string
};

enum Source : int { KEY = 0, MENU = 1, REPL = 2 };

struct GpuInfo {
  std::string vendor, renderer, version;
};

/// Any thread: ask for a report (ignored while one is being written).
void request(Source source);
/// EE thread: bit 0 = capture the state now (call save), bit 1 = a report was saved, bit 2 = one
/// failed. Bits 1 and 2 are reported once.
u64 poll();
/// EE thread: the state for the requested report; the next rendered frame becomes its picture.
void save(const GoalState& state, const std::string& level, const std::string& cont);
/// Render thread: the PNG path to write this frame, or "" when no report is waiting for one.
std::string take_pending_path();
/// True if `path` is the picture of the report in progress (no EXR, handed to deliver).
bool owns(const std::string& path);
/// Render thread: the frame (top row first, RGBA8) for the report in progress; files are
/// written on a worker thread.
void deliver(std::vector<u32>&& rgba, int width, int height, const GpuInfo& gpu);
/// `text` with absolute paths, user and machine names replaced by placeholders.
std::string sanitize(const std::string& text);

}  // namespace bug_report
