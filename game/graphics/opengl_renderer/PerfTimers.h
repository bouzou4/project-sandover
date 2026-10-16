#pragma once

#include <string>

#include "common/common_types.h"

/*!
 * Frame profiler for the perf pass: GL_TIMESTAMP scopes per renderer group plus CPU frame, render
 * thread and game (EE) times, logged as PERF-GPU / PERF-CPU / PERF-END lines every N frames.
 * Off by default (`pc-set-perf-timing 0`); while off a scope costs one branch and no GL calls.
 * Results are read back a few frames late, so the CPU never waits on the GPU.
 */
namespace perf {

enum class Group : u8 {
  NONE,
  FRAME,  // GPU span from the frame's first command to the end of present
  SETUP,
  BUCKETS,  // the whole bucket dispatch, Sandover passes included
  SKY,
  SKY_BLEND,
  TEX,
  TFRAG,
  TIE,
  ETIE,
  SHRUB,
  GENERIC,
  MERC,
  FUR,  // nested in MERC
  EYES,
  SHADOW,  // vanilla stencil shadow bucket
  ACTOR_SHADOWS,
  OCEAN_TEXTURE,
  OCEAN_FAR,
  OCEAN_MID,
  OCEAN_NEAR,
  WATER_DEPTH,
  DEPTH_CUE,
  SPRITE,
  DIRECT,  // debug, HUD text and subtitle buckets
  VISTA,
  VISTA_ALPHA,
  SSAO_TARGETS,  // normal and emissive target clears
  SSAO_DEPTH,
  SSAO,
  SSAO_VIEW,
  EMISSIVE_LIGHTS,
  BLOOM,
  BLIT_DISPLAY,
  PCRTC,  // resolve, tonemap and post
  IMGUI,
  PRESENT,  // HDR DXGI present or GL swap
  // later groups, mostly sub-scopes; each one's parent is in PerfTimers.cpp
  SSAO_DEPTH_LIN,  // depth copy, linear z and its mips
  SSAO_AO,
  SSAO_BLUR,
  SSAO_COMPOSITE,
  EMLIGHTS_RESOLVE,  // colour resolve
  EMLIGHTS_TILES,    // tile and select passes
  EMLIGHTS_TRACK,    // readback, tracking, uploads (the sync wait shows here)
  EMLIGHTS_LIGHT,    // the full-screen light pass
  OCEAN_MID_TEX,     // textured mid ocean draw (tessellated when water shading is on)
  OCEAN_MID_ENVMAP,  // envmap mid ocean draw, same tessellation
  SHADOW_CLEAR,      // actor shadow map clears
  SHADOW_MERC,       // merc casters into every map
  SHADOW_GENERIC,    // generic casters into every map
  SWAMP_MIST,
  PCRTC_RESOLVE,  // MSAA resolve blit
  PCRTC_POST,     // tonemap, PQ and brightness pass to the window buffer
  PRESENT_ENCODE,  // HDR: interop lock, encode draw, unlock
  PRESENT_FLIP,    // HDR: D3D copy and Present
  PRESENT_SWAP,    // GL swap
  SMAA,            // SMAA 1x before the HUD
  SMAA_COPY,       // copy or 2-sample resolve of the frame
  SMAA_EDGES,
  SMAA_WEIGHTS,
  SMAA_BLEND,
  UPSCALE,           // render scale: scene upscale before the text buckets
  UPSCALE_COPY,      // copy or resolve of the render-size scene
  UPSCALE_EASU,      // FSR 1 EASU
  UPSCALE_RCAS,      // FSR 1 RCAS
  UPSCALE_BILINEAR,  // bilinear blit
  WORLD_SHADOWS,     // world sun map cascades, inside ACTOR_SHADOWS
  OCEAN_MID_DEPTH,   // mid ocean depth-only pass (perf bit 26)
  COUNT
};

/// Per-frame values logged as PERF-CNT lines (averaged over the frames that set them).
enum class Counter : u8 {
  SHADOW_MAPS,        // actor shadow maps rendered (sun + spot)
  VISTA_LEVELS,       // neighbour levels drawn
  VISTA_KTRIS,        // neighbour triangles submitted, thousands
  VISTA_DRAWS,        // neighbour draw calls
  VISTA_CPU_MS,       // render thread time issuing the neighbour opaque draws
  OCEAN_MID_PATCHES,  // tessellated mid ocean source triangles
  OCEAN_NEAR_CPU_MS,  // render thread time in the near ocean's water setup (rim included)
  OCEAN_RIM_CPU_MS,   // ... of it in near_rim_on_plane
  OCEAN_RIM_REBUILT,  // 1 when near_rim_on_plane rebuilt its rim, 0 when it reused it
  WORLD_CPU_MS,       // render thread time in the world sun map prepass
  WORLD_CASCADES,     // world map cascades redrawn this frame
  OCEAN_MID_MFRAGS,   // fragment shader runs of the mid textured draw, millions (GPU timing switch)
  OCEAN_NEAR_MFRAGS,  // ... of the near textured draw
  COUNT
};

const char* group_name(Group g);
/// The group this one is nested in (NONE for top-level groups).
Group group_parent(Group g);

namespace detail {
extern bool g_frame_on;
void begin(int id);
void end(int id);
int named(Group parent, const char* prefix, const std::string& label);
}  // namespace detail

/// True while this frame is being profiled.
inline bool on() {
  return detail::g_frame_on;
}

/// Render thread, before any GL work of the frame. Latches the on/off state for the frame.
void frame_begin(int fb_w, int fb_h, int msaa);
/// Render thread, after present. `path` names the present path ("hdr dxgi", "gl swap", "xr").
void frame_end(const char* path);

/// CPU samples for the current frame, in milliseconds.
void cpu_render(double ms);    // OpenGLRenderer::render
void cpu_wait_dma(double ms);  // render thread waiting for the game's DMA chain
void cpu_present(double ms);   // the present call (HDR DXGI or GL swap)

/// Sets the `perf_opt` uint uniform (pc-set-perf-opt mask) of a bound program that declares it.
void set_opt_uniform(u32 program);

/// Set a counter for the current frame (no-op while off).
void count(Counter c, double v);

/// Game (EE) thread: call when the vsync wait returns and when sync_path is entered.
void ee_frame_start();
void ee_frame_work_done();

struct Scope {
  explicit Scope(Group g) : m_id(on() ? (int)g : 0) {
    if (m_id) {
      detail::begin(m_id);
    }
  }
  /// A scope named at run time, logged as `<prefix>:<label>` inside `parent` (one per vista
  /// level, say). The name is interned on first use.
  Scope(Group parent, const char* prefix, const std::string& label)
      : m_id(on() ? detail::named(parent, prefix, label) : 0) {
    if (m_id) {
      detail::begin(m_id);
    }
  }
  ~Scope() {
    if (m_id) {
      detail::end(m_id);
    }
  }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

 private:
  int m_id;
};

}  // namespace perf
