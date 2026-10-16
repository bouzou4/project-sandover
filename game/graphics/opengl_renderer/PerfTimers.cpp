#include "PerfTimers.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/log/log.h"

#include "game/graphics/gfx.h"
#include "game/graphics/opengl_renderer/GlQueries.h"
#include "game/graphics/opengl_renderer/opengl_utils.h"

namespace perf {
namespace {

struct Info {
  const char* name;
  Group parent;
};

constexpr std::array<Info, (int)Group::COUNT> kInfo = {{
    {"none", Group::NONE},
    {"frame", Group::NONE},
    {"setup", Group::NONE},
    {"buckets", Group::NONE},
    {"sky", Group::BUCKETS},
    {"sky-blend", Group::BUCKETS},
    {"tex", Group::BUCKETS},
    {"tfrag", Group::BUCKETS},
    {"tie", Group::BUCKETS},
    {"etie", Group::BUCKETS},
    {"shrub", Group::BUCKETS},
    {"generic", Group::BUCKETS},
    {"merc", Group::BUCKETS},
    {"fur", Group::MERC},
    {"eyes", Group::BUCKETS},
    {"shadow", Group::BUCKETS},
    {"actor-shadows", Group::BUCKETS},
    {"ocean-texture", Group::BUCKETS},
    {"ocean-far", Group::BUCKETS},
    {"ocean-mid", Group::BUCKETS},
    {"ocean-near", Group::BUCKETS},
    {"water-depth", Group::BUCKETS},
    {"depth-cue", Group::BUCKETS},
    {"sprite", Group::BUCKETS},
    {"direct", Group::BUCKETS},
    {"vista", Group::BUCKETS},
    {"vista-alpha", Group::BUCKETS},
    {"ssao-targets", Group::BUCKETS},
    {"ssao-depth", Group::BUCKETS},
    {"ssao", Group::BUCKETS},
    {"ssao-view", Group::BUCKETS},
    {"emissive-lights", Group::BUCKETS},
    {"bloom", Group::BUCKETS},
    {"blit-display", Group::NONE},
    {"pcrtc", Group::NONE},
    {"imgui", Group::NONE},
    {"present", Group::NONE},
    {"ssao-depth-lin", Group::SSAO},
    {"ssao-ao", Group::SSAO},
    {"ssao-blur", Group::SSAO},
    {"ssao-composite", Group::SSAO},
    {"emlights-resolve", Group::EMISSIVE_LIGHTS},
    {"emlights-tiles", Group::EMISSIVE_LIGHTS},
    {"emlights-track", Group::EMISSIVE_LIGHTS},
    {"emlights-light", Group::EMISSIVE_LIGHTS},
    {"ocean-mid-tex", Group::OCEAN_MID},
    {"ocean-mid-envmap", Group::OCEAN_MID},
    {"shadow-clear", Group::ACTOR_SHADOWS},
    {"shadow-merc", Group::ACTOR_SHADOWS},
    {"shadow-generic", Group::ACTOR_SHADOWS},
    {"swamp-mist", Group::BUCKETS},
    {"pcrtc-resolve", Group::PCRTC},
    {"pcrtc-post", Group::PCRTC},
    {"present-encode", Group::PRESENT},
    {"present-flip", Group::PRESENT},
    {"present-swap", Group::PRESENT},
    {"smaa", Group::BUCKETS},
    {"smaa-copy", Group::SMAA},
    {"smaa-edges", Group::SMAA},
    {"smaa-weights", Group::SMAA},
    {"smaa-blend", Group::SMAA},
    {"upscale", Group::BUCKETS},
    {"upscale-copy", Group::UPSCALE},
    {"fsr-easu", Group::UPSCALE},
    {"fsr-rcas", Group::UPSCALE},
    {"upscale-bilinear", Group::UPSCALE},
    {"world-shadows", Group::ACTOR_SHADOWS},
    {"ocean-mid-depth", Group::OCEAN_MID},
}};

constexpr int kCounters = (int)Counter::COUNT;
constexpr std::array<const char*, kCounters> kCounterNames = {
    "shadow-maps",       "vista-levels",      "vista-ktris",      "vista-draws",
    "vista-cpu-ms",      "ocean-mid-patches", "ocean-near-cpu-ms", "ocean-rim-cpu-ms",
    "ocean-rim-rebuilt", "world-cpu-ms",      "world-cascades",   "ocean-mid-mfrags",
    "ocean-near-mfrags"};

constexpr int kGroups = (int)Group::COUNT;
constexpr int kRing = 6;

// scopes named at run time get ids from kGroups up
struct Named {
  std::string name;
  Group parent;
};
std::vector<Named> g_named;
std::unordered_map<std::string, int> g_named_ids;

int num_ids() {
  return kGroups + (int)g_named.size();
}

const char* id_name(int id) {
  return id < kGroups ? kInfo[id].name : g_named[id - kGroups].name.c_str();
}

Group id_parent(int id) {
  return id < kGroups ? kInfo[id].parent : g_named[id - kGroups].parent;
}

struct Mark {
  int id;
  u32 q0, q1;  // indices into the slot's query pool
};

struct Slot {
  std::vector<GLuint> pool;
  u32 used = 0;
  std::vector<Mark> marks;
  std::vector<int> open;  // per id: mark index + 1 of the innermost open scope
  bool pending = false;
};

struct Stat {
  double sum = 0, min = 0, max = 0;
  int n = 0;
  void add(double ms) {
    min = n ? std::min(min, ms) : ms;
    max = n ? std::max(max, ms) : ms;
    sum += ms;
    n++;
  }
};

enum Cpu { CPU_FRAME, CPU_RENDER, CPU_WAIT_DMA, CPU_EE, CPU_EE_WAIT, CPU_PRESENT, CPU_COUNT };
constexpr const char* kCpuNames[CPU_COUNT] = {"frame", "render",  "wait-dma",
                                              "ee",    "ee-wait", "present"};

using Clock = std::chrono::steady_clock;

std::array<Slot, kRing> g_slots;
int g_head = 0;
bool g_open = false;
std::vector<Stat> g_gpu(kGroups);
int g_gpu_frames = 0;
std::array<Stat, CPU_COUNT> g_cpu;
std::array<double, CPU_COUNT> g_cpu_frame = {-1, -1, -1, -1, -1, -1};
std::array<Stat, kCounters> g_cnt;
std::array<double, kCounters> g_cnt_frame = [] {
  std::array<double, kCounters> a;
  a.fill(-1);
  return a;
}();
// the window's slowest frame (CPU frame interval): its CPU times and counters
double g_peak_frame = -1;
std::array<double, CPU_COUNT> g_peak_cpu;
std::array<double, kCounters> g_peak_cnt;
int g_frames = 0;
int g_window_id = 0;
int g_window = 0;
int g_fb_w = 0, g_fb_h = 0, g_msaa = 0;
Clock::time_point g_last_present;
bool g_have_present = false;

// game thread -> render thread, latest values in microseconds
std::atomic<s64> g_ee_us{-1}, g_ee_wait_us{-1};
Clock::time_point g_ee_start, g_ee_work_done;
bool g_ee_started = false, g_ee_have_done = false;

double ms_between(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double, std::milli>(b - a).count();
}

void reset_window() {
  g_gpu.assign(num_ids(), Stat{});
  g_cpu = {};
  g_cnt = {};
  g_gpu_frames = 0;
  g_frames = 0;
  g_peak_frame = -1;
}

void collect(Slot& s) {
  if (!s.pending) {
    return;
  }
  s.pending = false;
  const int n = num_ids();
  std::vector<double> sums(n, 0.0);
  std::vector<char> ran(n, 0);
  if ((int)g_gpu.size() < n) {
    g_gpu.resize(n);
  }
  std::vector<GLuint64> t(s.used, 0);
  for (u32 i = 0; i < s.used; i++) {
    glGetQueryObjectui64v(s.pool[i], GL_QUERY_RESULT, &t[i]);
  }
  for (const auto& m : s.marks) {
    if (m.q1 == UINT32_MAX) {
      continue;  // never closed
    }
    sums[m.id] += double(t[m.q1] - t[m.q0]) * 1e-6;
    ran[m.id] = 1;
  }
  for (int g = 0; g < n; g++) {
    if (ran[g]) {
      g_gpu[g].add(sums[g]);
    }
  }
  g_gpu_frames++;
}

u32 stamp(Slot& s) {
  if (s.used == s.pool.size()) {
    const size_t old = s.pool.size();
    s.pool.resize(old + 64);
    glGenQueries(64, &s.pool[old]);
  }
  glQueryCounter(s.pool[s.used], GL_TIMESTAMP);
  return s.used++;
}

void log_window(const char* path) {
  const int w = g_window_id++;
  for (int g = 1; g < (int)g_gpu.size(); g++) {
    const auto& st = g_gpu[g];
    if (!st.n) {
      continue;
    }
    // avg over every collected frame, so a pass that skips frames still sums correctly
    lg::info("PERF-GPU w={} name={} in={} avg_ms={:.4f} min_ms={:.4f} max_ms={:.4f} n={}/{}", w,
             id_name(g), kInfo[(int)id_parent(g)].name, st.sum / std::max(g_gpu_frames, 1), st.min,
             st.max, st.n, g_gpu_frames);
  }
  for (int c = 0; c < CPU_COUNT; c++) {
    const auto& st = g_cpu[c];
    if (!st.n) {
      continue;
    }
    lg::info("PERF-CPU w={} name={} avg_ms={:.4f} min_ms={:.4f} max_ms={:.4f} n={}", w,
             kCpuNames[c], st.sum / st.n, st.min, st.max, st.n);
  }
  for (int c = 0; c < kCounters; c++) {
    const auto& st = g_cnt[c];
    if (st.n) {
      lg::info("PERF-CNT w={} name={} avg={:.2f} min={:.0f} max={:.0f} n={}", w, kCounterNames[c],
               st.sum / st.n, st.min, st.max, st.n);
    }
  }
  // the slowest frame's split, as counters (n=1), so a hitch can be told apart from the mean
  if (g_peak_frame >= 0) {
    for (int c = 0; c < CPU_COUNT; c++) {
      if (g_peak_cpu[c] >= 0) {
        lg::info("PERF-CNT w={} name=peak-{} avg={:.2f} min={:.2f} max={:.2f} n=1", w, kCpuNames[c],
                 g_peak_cpu[c], g_peak_cpu[c], g_peak_cpu[c]);
      }
    }
    for (int c = 0; c < kCounters; c++) {
      if (g_peak_cnt[c] >= 0) {
        lg::info("PERF-CNT w={} name=peak-{} avg={:.2f} min={:.2f} max={:.2f} n=1", w,
                 kCounterNames[c], g_peak_cnt[c], g_peak_cnt[c], g_peak_cnt[c]);
      }
    }
  }
  lg::info("PERF-END w={} frames={} gpu_frames={} fb={}x{} msaa={} present={}", w, g_frames,
           g_gpu_frames, g_fb_w, g_fb_h, g_msaa, path);
}

}  // namespace

namespace detail {
bool g_frame_on = false;

void begin(int id) {
  auto& s = g_slots[g_head];
  if ((int)s.open.size() <= id) {
    s.open.resize(num_ids(), 0);
  }
  s.marks.push_back({id, stamp(s), UINT32_MAX});
  s.open[id] = (int)s.marks.size();
}

void end(int id) {
  auto& s = g_slots[g_head];
  const int idx = id < (int)s.open.size() ? s.open[id] - 1 : -1;
  if (idx < 0) {
    return;
  }
  s.marks[idx].q1 = stamp(s);
  // re-open an outer scope of the same group, if any (nesting is rare)
  int outer = 0;
  for (int i = idx - 1; i >= 0; i--) {
    if (s.marks[i].id == id && s.marks[i].q1 == UINT32_MAX) {
      outer = i + 1;
      break;
    }
  }
  s.open[id] = outer;
}

int named(Group parent, const char* prefix, const std::string& label) {
  std::string name = std::string(prefix) + ":" + label;
  const auto it = g_named_ids.find(name);
  if (it != g_named_ids.end()) {
    return it->second;
  }
  const int id = num_ids();
  g_named_ids.emplace(name, id);
  g_named.push_back({std::move(name), parent});
  return id;
}
}  // namespace detail

const char* group_name(Group g) {
  return kInfo[(int)g].name;
}

Group group_parent(Group g) {
  return kInfo[(int)g].parent;
}

void frame_begin(int fb_w, int fb_h, int msaa) {
  const int window = Gfx::g_global_settings.perf_timing_window;
  if (window != g_window) {
    g_window = window;
    reset_window();
  }
  detail::g_frame_on = window > 0;
  g_open = detail::g_frame_on;
  if (!g_open) {
    return;
  }
  g_fb_w = fb_w;
  g_fb_h = fb_h;
  g_msaa = msaa;
  g_head = (g_head + 1) % kRing;
  auto& s = g_slots[g_head];
  // a window can hold frames drawn before a settings change; tools keep the second window
  collect(s);
  s.used = 0;
  s.marks.clear();
  s.open.assign(num_ids(), 0);
  detail::begin((int)Group::FRAME);
}

void frame_end(const char* path) {
  const auto now = Clock::now();
  if (g_have_present) {
    g_cpu_frame[CPU_FRAME] = ms_between(g_last_present, now);
  }
  g_last_present = now;
  g_have_present = true;
  if (!g_open) {
    g_cpu_frame.fill(-1);
    g_cnt_frame.fill(-1);
    return;
  }
  detail::end((int)Group::FRAME);
  g_slots[g_head].pending = true;
  g_open = false;
  detail::g_frame_on = false;

  g_cpu_frame[CPU_EE] = g_ee_us.load() * 1e-3;
  g_cpu_frame[CPU_EE_WAIT] = g_ee_wait_us.load() * 1e-3;
  if (g_cpu_frame[CPU_FRAME] > g_peak_frame) {
    g_peak_frame = g_cpu_frame[CPU_FRAME];
    g_peak_cpu = g_cpu_frame;
    g_peak_cnt = g_cnt_frame;
  }
  for (int c = 0; c < CPU_COUNT; c++) {
    if (g_cpu_frame[c] >= 0) {
      g_cpu[c].add(g_cpu_frame[c]);
    }
  }
  g_cpu_frame.fill(-1);
  for (int c = 0; c < kCounters; c++) {
    if (g_cnt_frame[c] >= 0) {
      g_cnt[c].add(g_cnt_frame[c]);
    }
  }
  g_cnt_frame.fill(-1);
  if (++g_frames >= g_window) {
    log_window(path);
    reset_window();
  }
}

void cpu_render(double ms) {
  g_cpu_frame[CPU_RENDER] = ms;
}

void cpu_wait_dma(double ms) {
  g_cpu_frame[CPU_WAIT_DMA] = ms;
}

void cpu_present(double ms) {
  g_cpu_frame[CPU_PRESENT] = ms;
}

void set_opt_uniform(u32 program) {
  const GLint loc = GLQ_LOC(PERF_OPT_CORE + 1, program, "perf_opt");
  if (loc >= 0) {
    glUniform1ui(loc, Gfx::g_global_settings.perf_opt_mask);
  }
}

void count(Counter c, double v) {
  if (detail::g_frame_on) {
    g_cnt_frame[(int)c] = v;
  }
}

void ee_frame_start() {
  const auto now = Clock::now();
  if (g_ee_have_done) {
    // time blocked from the end of the game's work to the next frame start (sync + vsync)
    g_ee_wait_us = (s64)(ms_between(g_ee_work_done, now) * 1e3);
  }
  g_ee_start = now;
  g_ee_started = true;
}

void ee_frame_work_done() {
  if (!g_ee_started) {
    return;
  }
  g_ee_work_done = Clock::now();
  g_ee_have_done = true;
  g_ee_us = (s64)(ms_between(g_ee_start, g_ee_work_done) * 1e3);
}

}  // namespace perf
