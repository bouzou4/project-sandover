#include "PerfTimers.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
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
    {"world-shadows", Group::ACTOR_SHADOWS},
    {"ocean-mid-depth", Group::OCEAN_MID},
    {"motion-vectors", Group::BUCKETS},
    {"motion-vectors-view", Group::BUCKETS},
    {"fsr3", Group::BUCKETS},
    {"fsr3-reactive", Group::BUCKETS},
    {"fsr3-prepare", Group::FSR3},
    {"fsr3-luma", Group::FSR3},
    {"fsr3-reactivity", Group::FSR3},
    {"fsr3-accumulate", Group::FSR3},
    {"fsr3-rcas", Group::FSR3},
    {"dlss", Group::BUCKETS},
    {"dlss-inputs", Group::DLSS},
    {"dlss-eval", Group::DLSS},
    {"dlss-output", Group::DLSS},
    {"fsr4", Group::BUCKETS},
    {"fsr4-inputs", Group::FSR4},
    {"fsr4-dispatch", Group::FSR4},
    {"fsr4-output", Group::FSR4},
    {"contact-shadows", Group::BUCKETS},
    {"motion-blur", Group::BUCKETS},
    {"ssao-marks", Group::BUCKETS},
    {"bg-prepass", Group::BUCKETS},
    {"eye", Group::BUCKETS},
}};

constexpr int kCounters = (int)Counter::COUNT;
constexpr std::array<const char*, kCounters> kCounterNames = {
    "shadow-maps",       "vista-levels",      "vista-ktris",      "vista-draws",
    "vista-cpu-ms",      "vista-hiz-pct",     "vista-hiz-sync-ms", "vista-hiz-leak", "ocean-mid-patches", "ocean-near-cpu-ms", "ocean-rim-cpu-ms",
    "ocean-rim-rebuilt", "world-cpu-ms",      "world-cascades",   "ocean-mid-mfrags",
    "ocean-near-mfrags", "tie-mfrags",        "dlss-d3d-ms",       "dlss-sync-cpu-ms", "fsr4-d3d-ms",
    "fsr4-sync-cpu-ms",  "bg-mfrags",         "bg-tfrag-mfrags",  "bg-tie-mfrags",
    "bg-shrub-mfrags",   "bg-vista-mfrags",   "bg-pre-mfrags",     "bg-prepass-cuts",
    "eye-cpu-ms",        "hitch-shader-ms",   "hitch-shaders",    "hitch-recompiles",
    "hitch-pbr-ms",      "hitch-pbr-decode-ms", "hitch-target-ms", "hitch-loader-ms",
    "hitch-upscaler-ms", "blackout"};
// eye 1's counters follow eye 0's (detail::g_eye)
constexpr int kCounterSlots = 2 * kCounters;

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

enum Cpu {
  CPU_FRAME,
  CPU_RENDER,
  CPU_WAIT_DMA,
  CPU_EE,
  CPU_EE_WAIT,
  CPU_PRESENT,
  CPU_XR_WAIT,
  CPU_XR_ACQUIRE,
  CPU_XR_END,
  CPU_COPY_DMA,
  CPU_COUNT
};
constexpr const char* kCpuNames[CPU_COUNT] = {"frame",   "render",     "wait-dma", "ee",
                                              "ee-wait", "present",    "xr-wait",  "xr-acquire",
                                              "xr-end",  "xr-copy-dma"};

using Clock = std::chrono::steady_clock;

std::array<Slot, kRing> g_slots;
int g_head = 0;
bool g_open = false;
std::vector<Stat> g_gpu(kGroups);
int g_gpu_frames = 0;
std::array<Stat, CPU_COUNT> g_cpu;
std::array<double, CPU_COUNT> g_cpu_frame = [] {
  std::array<double, CPU_COUNT> a;
  a.fill(-1);
  return a;
}();
std::array<Stat, kCounterSlots> g_cnt;
std::array<double, kCounterSlots> g_cnt_frame = [] {
  std::array<double, kCounterSlots> a;
  a.fill(-1);
  return a;
}();
// the window's slowest frame (CPU frame interval): its CPU times and counters
double g_peak_frame = -1;
std::array<double, CPU_COUNT> g_peak_cpu;
std::array<double, kCounterSlots> g_peak_cnt;
// render thread ms inside each scope this frame (outermost of a group), and the peak frame's
std::vector<double> g_grp_ms, g_peak_grp;
std::vector<Clock::time_point> g_grp_t0;
std::vector<int> g_grp_depth;
constexpr double kGrpLogMs = 0.5;  // peak-cpu-<group> lines at or over this
// hitch log (negative window): no GPU queries, a PERF-FRAME line per frame, PERF-HITCH over it
bool g_gpu_on = false;
double g_hitch_ms = 0;
u64 g_frame_no = 0;
std::atomic<int> g_recompiles{0};
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
  g_peak_grp.clear();
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
  auto cnt_name = [](int c) {
    return c < kCounters ? std::string(kCounterNames[c])
                         : std::string("e1-") + kCounterNames[c - kCounters];
  };
  for (int c = 0; c < kCounterSlots; c++) {
    const auto& st = g_cnt[c];
    if (st.n) {
      lg::info("PERF-CNT w={} name={} avg={:.2f} min={:.0f} max={:.0f} n={}", w, cnt_name(c),
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
    for (int c = 0; c < kCounterSlots; c++) {
      if (g_peak_cnt[c] >= 0) {
        lg::info("PERF-CNT w={} name=peak-{} avg={:.2f} min={:.2f} max={:.2f} n=1", w,
                 cnt_name(c), g_peak_cnt[c], g_peak_cnt[c], g_peak_cnt[c]);
      }
    }
    for (int g = 1; g < (int)g_peak_grp.size(); g++) {
      if (g_peak_grp[g] >= kGrpLogMs) {
        lg::info("PERF-CNT w={} name=peak-cpu-{} avg={:.2f} min={:.2f} max={:.2f} n=1", w,
                 id_name(g), g_peak_grp[g], g_peak_grp[g], g_peak_grp[g]);
      }
    }
  }
  lg::info("PERF-END w={} frames={} gpu_frames={} fb={}x{} msaa={} present={}", w, g_frames,
           g_gpu_frames, g_fb_w, g_fb_h, g_msaa, path);
}

/// Hitch log: one PERF-FRAME line per frame; a frame of at least g_hitch_ms (interval or render
/// thread) also gets PERF-HITCH lines with its CPU split, counters and scopes.
void log_hitch_frame() {
  const auto& c = g_cpu_frame;
  const auto& n = g_cnt_frame;
  lg::info("PERF-FRAME f={} frame_ms={:.2f} render_ms={:.2f} blackout={:.0f}", g_frame_no,
           c[CPU_FRAME], c[CPU_RENDER], n[(int)Counter::BLACKOUT]);
  if (std::max(c[CPU_FRAME], c[CPU_RENDER]) < g_hitch_ms) {
    return;
  }
  for (int i = 0; i < CPU_COUNT; i++) {
    if (c[i] >= 0) {
      lg::info("PERF-HITCH f={} name={} ms={:.2f}", g_frame_no, kCpuNames[i], c[i]);
    }
  }
  for (int i = 0; i < kCounters; i++) {
    if (n[i] > 0) {
      lg::info("PERF-HITCH f={} name={} ms={:.2f}", g_frame_no, kCounterNames[i], n[i]);
    }
  }
  for (int g = 1; g < (int)g_grp_ms.size(); g++) {
    if (g_grp_ms[g] >= kGrpLogMs) {
      lg::info("PERF-HITCH f={} name=cpu-{} ms={:.2f}", g_frame_no, id_name(g), g_grp_ms[g]);
    }
  }
}

}  // namespace

namespace detail {
bool g_frame_on = false;
int g_eye = 0;

void begin(int id) {
  if ((int)g_grp_depth.size() <= id) {
    g_grp_depth.resize(num_ids(), 0);
    g_grp_t0.resize(num_ids());
    g_grp_ms.resize(num_ids(), 0.0);
  }
  if (g_grp_depth[id]++ == 0) {
    g_grp_t0[id] = Clock::now();
  }
  if (!g_gpu_on) {
    return;
  }
  auto& s = g_slots[g_head];
  if ((int)s.open.size() <= id) {
    s.open.resize(num_ids(), 0);
  }
  s.marks.push_back({id, stamp(s), UINT32_MAX});
  s.open[id] = (int)s.marks.size();
}

void end(int id) {
  // SANDOVER_HITCH_FINISH=1 (dev): a glFinish per scope end, so GPU and driver waits land in it
  static const bool finish = [] {
    const char* v = std::getenv("SANDOVER_HITCH_FINISH");
    return v && v[0] == '1';
  }();
  if (finish && !g_gpu_on && id < (int)g_grp_depth.size() && g_grp_depth[id] == 1) {
    glFinish();
  }
  if (id < (int)g_grp_depth.size() && g_grp_depth[id] > 0 && --g_grp_depth[id] == 0) {
    g_grp_ms[id] += ms_between(g_grp_t0[id], Clock::now());
  }
  if (!g_gpu_on) {
    return;
  }
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

int intern(Group parent, std::string name) {
  const auto it = g_named_ids.find(name);
  if (it != g_named_ids.end()) {
    return it->second;
  }
  const int id = num_ids();
  g_named_ids.emplace(name, id);
  g_named.push_back({std::move(name), parent});
  return id;
}

int named(Group parent, const char* prefix, const std::string& label) {
  return intern(parent, (g_eye ? "e1:" : "") + std::string(prefix) + ":" + label);
}

int eye_id(Group g) {
  return intern(group_parent(g), std::string("e1:") + group_name(g));
}
}  // namespace detail

void set_eye(int eye) {
  detail::g_eye = eye == 1 ? 1 : 0;
}

const char* group_name(Group g) {
  return kInfo[(int)g].name;
}

Group group_parent(Group g) {
  return kInfo[(int)g].parent;
}

void frame_begin(int fb_w, int fb_h, int msaa) {
  // SANDOVER_HITCH_LOG=<ms>: the hitch log from the first frame, before any nREPL form
  static const bool env_once = [] {
    const char* v = std::getenv("SANDOVER_HITCH_LOG");
    if (v && std::atoi(v) > 0) {
      Gfx::g_global_settings.perf_timing_window = -std::atoi(v);
    }
    return true;
  }();
  (void)env_once;
  const int window = Gfx::g_global_settings.perf_timing_window;
  if (window != g_window) {
    g_window = window;
    reset_window();
  }
  detail::g_frame_on = window != 0;
  g_gpu_on = window > 0;
  g_hitch_ms = window < 0 ? -window : 0.0;
  g_open = detail::g_frame_on;
  if (!g_open) {
    return;
  }
  g_fb_w = fb_w;
  g_fb_h = fb_h;
  g_msaa = msaa;
  g_grp_ms.assign(num_ids(), 0.0);
  g_grp_depth.assign(num_ids(), 0);
  g_grp_t0.resize(num_ids());
  if (g_gpu_on) {
    g_head = (g_head + 1) % kRing;
    auto& s = g_slots[g_head];
    // a window can hold frames drawn before a settings change; tools keep the second window
    collect(s);
    s.used = 0;
    s.marks.clear();
    s.open.assign(num_ids(), 0);
  }
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
    g_recompiles = 0;
    return;
  }
  detail::end((int)Group::FRAME);
  if (g_gpu_on) {
    g_slots[g_head].pending = true;
  }
  g_open = false;
  detail::g_frame_on = false;

  g_cpu_frame[CPU_EE] = g_ee_us.load() * 1e-3;
  g_cpu_frame[CPU_EE_WAIT] = g_ee_wait_us.load() * 1e-3;
  g_cnt_frame[(int)Counter::HITCH_RECOMPILES] = g_recompiles.exchange(0);
  g_frame_no++;
  if (!g_gpu_on) {
    log_hitch_frame();
    g_cpu_frame.fill(-1);
    g_cnt_frame.fill(-1);
    return;
  }
  if (g_cpu_frame[CPU_FRAME] > g_peak_frame) {
    g_peak_frame = g_cpu_frame[CPU_FRAME];
    g_peak_cpu = g_cpu_frame;
    g_peak_cnt = g_cnt_frame;
    g_peak_grp = g_grp_ms;
  }
  for (int c = 0; c < CPU_COUNT; c++) {
    if (g_cpu_frame[c] >= 0) {
      g_cpu[c].add(g_cpu_frame[c]);
    }
  }
  g_cpu_frame.fill(-1);
  for (int c = 0; c < kCounterSlots; c++) {
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

void cpu_xr(double wait_ms, double acquire_ms) {
  g_cpu_frame[CPU_XR_WAIT] = wait_ms;
  g_cpu_frame[CPU_XR_ACQUIRE] = acquire_ms;
}

void cpu_xr_end(double ms) {
  g_cpu_frame[CPU_XR_END] = ms;
}

void cpu_copy_dma(double ms) {
  g_cpu_frame[CPU_COPY_DMA] = ms;
}

void set_opt_uniform(u32 program) {
  const GLint loc = GLQ_LOC(PERF_OPT_CORE + 1, program, "perf_opt");
  if (loc >= 0) {
    glUniform1ui(loc, Gfx::g_global_settings.perf_opt_mask);
  }
}

void count(Counter c, double v) {
  if (detail::g_frame_on) {
    g_cnt_frame[(int)c + detail::g_eye * kCounters] = v;
  }
}

void count_add(Counter c, double v) {
  if (detail::g_frame_on) {
    double& f = g_cnt_frame[(int)c + detail::g_eye * kCounters];
    f = f < 0 ? v : f + v;
  }
}

void add(Counter c, double v) {
  if (detail::g_frame_on) {
    auto& x = g_cnt_frame[(int)c];
    x = x < 0 ? v : x + v;
  }
}

void note_recompile() {
  g_recompiles++;
}

AddMs::AddMs(Counter c)
    : m_c(c), m_t0(on() ? Clock::now().time_since_epoch().count() : -1) {}

AddMs::~AddMs() {
  if (m_t0 >= 0) {
    add(m_c, ms_between(Clock::time_point(Clock::duration(m_t0)), Clock::now()));
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
