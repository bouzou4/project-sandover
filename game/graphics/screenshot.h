#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "common/common_types.h"

#include "game/kernel/common/kscheme.h"

/*!
 * @file screenshot.h
 * This file contains a basic interface for the screen shot system to make it easier to share with
 * the GOAL kernel.
 */

// this must match the structure in capture-pc.gc (if present)
struct ScreenShotSettings {
  s32 width;
  s32 height;
  s32 msaa;
  char name[244];
};

extern ScreenShotSettings* g_screen_shot_settings;
extern bool g_want_screenshot;
/// Frames still to save as a burst (`<name>-fNN.png`, rig resolution); set from GOAL.
extern std::atomic<int> g_screenshot_burst;
/// Render thread: index of the burst frame being rendered, -1 outside a burst.
extern int g_burst_frame;

void register_screen_shot_settings(ScreenShotSettings* settings);
const char* get_screen_shot_name();

/*!
 * Play-session snapshot keys (Jak 1). Active only when the environment variable
 * SANDOVER_RUN_DIR names a directory: F9 saves the window framebuffer (native resolution, HUD
 * included) to <run dir>/NNNN.png, F10 and F8 ask the play script for the next/previous level.
 * GOAL drains the events with (pc-snap-poll) and reports the game state for each snapshot.
 */
namespace snap {
enum Event : u32 { NONE = 0, SHOT = 1, NEXT = 2, PREV = 3 };
bool enabled();
void on_key(Event e);
/// Render thread: the PNG path to write this frame, or "" when no snapshot is pending.
std::string take_pending_path();
/// EE thread: next queued event as (index << 4) | Event, 0 when the queue is empty.
u64 poll();
}  // namespace snap
