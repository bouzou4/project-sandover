#include "game/graphics/screenshot.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <utility>

#include "common/log/log.h"
#include "common/util/FileUtil.h"

#include "fmt/format.h"

/*!
 * @file screenshot.cpp
 * This file contains a basic interface for the screen shot system to make it easier to share with
 * the GOAL kernel.
 */

ScreenShotSettings s_default_screen_shot_settings = {1920, 1080, 8, "screenshot"};

bool g_want_screenshot = false;
std::atomic<int> g_screenshot_burst{0};
int g_burst_frame = -1;
ScreenShotSettings* g_screen_shot_settings = &s_default_screen_shot_settings;

void register_screen_shot_settings(ScreenShotSettings* settings) {
  g_screen_shot_settings = settings;
}

const char* get_screen_shot_name() {
  return g_screen_shot_settings->name;
}

namespace snap {
namespace {
std::mutex g_mutex;
std::deque<u64> g_events;
std::string g_pending;
int g_next = -1;

const char* run_dir() {
  const char* d = std::getenv("SANDOVER_RUN_DIR");
  return (d && *d) ? d : nullptr;
}

constexpr size_t kMaxQueued = 64;

// continue after the highest NNNN.png/.json in the run dir or its triage deleted/ folder
int first_free_index(const fs::path& dir) {
  int next = 1;
  for (const auto& d : {dir, dir / "deleted"}) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(d, ec)) {
      const auto stem = e.path().stem().string();
      const auto ext = e.path().extension();
      if ((ext == ".png" || ext == ".json") && stem.size() == 4 &&
          std::all_of(stem.begin(), stem.end(), [](unsigned char c) { return std::isdigit(c); })) {
        next = std::max(next, std::stoi(stem) + 1);
      }
    }
  }
  return next;
}
}  // namespace

bool enabled() {
  return run_dir() != nullptr;
}

void on_key(Event e) {
  const char* dir = run_dir();
  if (!dir) {
    return;
  }
  std::lock_guard<std::mutex> lk(g_mutex);
  if (g_events.size() >= kMaxQueued) {
    return;
  }
  u64 index = 0;
  if (e == SHOT) {
    if (!g_pending.empty()) {
      return;
    }
    fs::path p(dir);
    std::error_code ec;
    fs::create_directories(p, ec);
    if (g_next < 0) {
      g_next = first_free_index(p);
    }
    index = g_next++;
    g_pending = (p / fmt::format("{:04d}.png", index)).string();
    lg::info("snapshot {} -> {}", index, g_pending);
  }
  g_events.push_back((index << 4) | e);
}

std::string take_pending_path() {
  std::lock_guard<std::mutex> lk(g_mutex);
  return std::exchange(g_pending, {});
}

u64 poll() {
  std::lock_guard<std::mutex> lk(g_mutex);
  if (g_events.empty()) {
    return 0;
  }
  u64 e = g_events.front();
  g_events.pop_front();
  return e;
}
}  // namespace snap
