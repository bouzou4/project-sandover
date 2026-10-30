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
  WORLD_SHADOWS,     // world sun map cascades, inside ACTOR_SHADOWS
  OCEAN_MID_DEPTH,   // mid ocean depth-only pass (perf bit 26)
  MOTION_VECTORS,    // velocity pass (MotionVectors.h), object target clear included
  MOTION_VECTORS_VIEW,  // its debug views
  FSR3,              // FSR 3.1 temporal upscale before the text buckets (Fsr3Upscaler.h)
  FSR3_REACTIVE,     // scene copy before the sprite bucket and the autogen reactive mask
  FSR3_PREPARE,      // clears and the prepare-inputs pass
  FSR3_LUMA,         // luma pyramid, shading change pyramid and shading change
  FSR3_REACTIVITY,   // prepare reactivity and luma instability
  FSR3_ACCUMULATE,   // the accumulate pass (reprojection, upsample, sharpening input)
  FSR3_RCAS,         // RCAS to the output, and the debug view when on
  DLSS,              // DLSS before the text buckets (DlssUpscaler.h)
  DLSS_INPUTS,       // the input copy into the shared textures
  DLSS_EVAL,         // unlock, the D3D11 evaluate, lock: GL's view of DLSS plus the interop sync
  DLSS_OUTPUT,       // the output blit into the display target
  FSR4,              // FSR 4 before the text buckets (Fsr4Upscaler.h)
  FSR4_INPUTS,       // the input copy into the shared textures
  FSR4_DISPATCH,     // hand-over to D3D12, the dispatch submission, hand-back: GL's view of it
  FSR4_OUTPUT,       // the output blit into the display target
  CONTACT_SHADOWS,   // screen-space contact shadows after SSAO (ContactShadows.h)
  MOTION_BLUR,       // per-object motion blur (MotionBlur.h): scene copy, tiles, reconstruction
  SSAO_MARKS,        // depth copies and the late stencil marks pass (StencilMarks.h)
  BG_PREPASS,        // background depth prepass (BgPrepass in gfx.h)
  EYE,               // one eye's bucket dispatch (stereo and XR; eye 1 logs as e1:eye)
  COUNT
};

/// Per-frame values logged as PERF-CNT lines (averaged over the frames that set them).
enum class Counter : u8 {
  SHADOW_MAPS,        // actor shadow maps rendered (sun + spot)
  VISTA_LEVELS,       // neighbour levels drawn
  VISTA_KTRIS,        // neighbour triangles submitted, thousands
  VISTA_DRAWS,        // neighbour draw calls
  VISTA_CPU_MS,       // render thread time issuing the neighbour opaque draws
  VISTA_HIZ_PCT,      // neighbour bounds tested against the depth and left out, percent
  VISTA_HIZ_SYNC_MS,  // render thread time in the depth reduction and readback
  VISTA_HIZ_LEAK,     // probe 2: samples passed by the draws the occlusion leaves out (must be 0)
  OCEAN_MID_PATCHES,  // tessellated mid ocean source triangles
  OCEAN_NEAR_CPU_MS,  // render thread time in the near ocean's water setup (rim included)
  OCEAN_RIM_CPU_MS,   // ... of it in near_rim_on_plane
  OCEAN_RIM_REBUILT,  // 1 when near_rim_on_plane rebuilt its rim, 0 when it reused it
  WORLD_CPU_MS,       // render thread time in the world sun map prepass
  WORLD_CASCADES,     // world map cascades redrawn this frame
  OCEAN_MID_MFRAGS,   // fragment shader runs of the mid textured draw, millions (GPU timing switch)
  OCEAN_NEAR_MFRAGS,  // ... of the near textured draw
  TIE_MFRAGS,         // fragment shader runs of the tie buckets (envmap base included), millions
  DLSS_D3D_MS,        // the DLSS evaluate on the D3D11 device (timestamps, a few frames late)
  DLSS_SYNC_CPU_MS,   // render thread time in unlock, evaluate submission and lock
  FSR4_D3D_MS,        // the FSR 4 dispatch on the D3D12 queue (timestamps, a few frames late)
  FSR4_SYNC_CPU_MS,   // render thread time in the hand-over, dispatch submission and hand-back
  BG_MFRAGS,          // background (tfrag, tie, shrub, vista) fragment shader runs, millions (probe)
  BG_TFRAG_MFRAGS,    // ... of it in the tfrag buckets
  BG_TIE_MFRAGS,      // ... tie
  BG_SHRUB_MFRAGS,    // ... shrub
  BG_VISTA_MFRAGS,    // ... vista
  BG_PRE_MFRAGS,      // the background depth prepass (not in BG_MFRAGS)
  BG_PREPASS_CUTS,    // prepass runs ended early by a blended depth writer
  EYE_CPU_MS,         // render thread time in one eye's bucket dispatch
  // hitch attribution: render thread ms (or counts) spent this frame in one-off work
  HITCH_SHADER_MS,     // shader compile and link
  HITCH_SHADERS,       // programs built
  HITCH_RECOMPILES,    // driver "recompiled based on GL state" warnings
  HITCH_PBR_MS,        // PBR map uploads (and a decode left to first use)
  HITCH_PBR_DECODE_MS,  // ... of it decoding on the render thread
  HITCH_TARGET_MS,     // render target allocation (ensure_target paths, main framebuffer)
  HITCH_LOADER_MS,     // Loader::update (level upload stages, unloads)
  HITCH_UPSCALER_MS,   // FSR 3.1 program build, FSR 4 context creation
  BLACKOUT,            // 1 while the screen is black (load), 0 otherwise
  COUNT
};

const char* group_name(Group g);
/// The group this one is nested in (NONE for top-level groups).
Group group_parent(Group g);

namespace detail {
extern bool g_frame_on;
extern int g_eye;
int eye_id(Group g);
void begin(int id);
void end(int id);
int named(Group parent, const char* prefix, const std::string& label);
}  // namespace detail

/// True while this frame is being profiled.
inline bool on() {
  return detail::g_frame_on;
}

/// Stereo: the eye whose work follows. Eye 1's scopes and counters log as `e1:<name>` and
/// `e1-<name>`, so the plain names are eye 0's (or mono's).
void set_eye(int eye);

/// Render thread, before any GL work of the frame. Latches the on/off state for the frame.
void frame_begin(int fb_w, int fb_h, int msaa);
/// Render thread, after present. `path` names the present path ("hdr dxgi", "gl swap", "xr").
void frame_end(const char* path);

/// CPU samples for the current frame, in milliseconds.
void cpu_render(double ms);    // OpenGLRenderer::render
void cpu_wait_dma(double ms);  // render thread waiting for the game's DMA chain
void cpu_present(double ms);   // the present call (HDR DXGI or GL swap)
void cpu_xr(double wait_ms, double acquire_ms);  // xrWaitFrame + xrBeginFrame, swapchain acquire
void cpu_xr_end(double ms);    // swapchain release and xrEndFrame
void cpu_copy_dma(double ms);  // XR: copying a new chain from the game

/// Sets the `perf_opt` uint uniform (pc-set-perf-opt mask) of a bound program that declares it.
void set_opt_uniform(u32 program);

/// Set a counter for the current frame (no-op while off).
void count(Counter c, double v);
/// Adds `v` to this frame's value of `c` (several sources per frame).
void count_add(Counter c, double v);
/// Adds to eye 0's slot whatever eye is current (hitch counters).
void add(Counter c, double v);
/// Any thread: one driver shader recompile, counted into the frame that ends next.
void note_recompile();

/// Adds the render thread time of its lifetime to a counter (no-op while off).
struct AddMs {
  explicit AddMs(Counter c);
  ~AddMs();
  AddMs(const AddMs&) = delete;
  AddMs& operator=(const AddMs&) = delete;

 private:
  Counter m_c;
  s64 m_t0;
};

/// Game (EE) thread: call when the vsync wait returns and when sync_path is entered.
void ee_frame_start();
void ee_frame_work_done();

struct Scope {
  explicit Scope(Group g)
      : m_id(on() && g != Group::NONE ? (detail::g_eye ? detail::eye_id(g) : (int)g) : 0) {
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
