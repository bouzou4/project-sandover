#pragma once

#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/opengl_renderer/LightTracker.h"
#include "game/graphics/opengl_renderer/PerfTimers.h"
#include "game/graphics/opengl_renderer/SsaoDepth.h"

class SsaoRenderer;

/*!
 * Screen-space emissive tile lights (Jak 1). After the sprite bucket, before bloom:
 * 1. tiles: the frame is cut into a 64-wide grid of tiles; each keeps its emissive flux
 *    (ramp(E) x colour, averaged over the tile) and the E-weighted camera-space position of that
 *    emission (depth from SSAO's copy, or where an emissive sprite covers the pixel its own depth
 *    from the sprite depth target, since particles write none; pulled to the tile's nearest
 *    surface).
 * 2. select: one point per tile ranks it by flux and lands in a row of the kMaxCand brightest,
 *    read back and tracked (below). A thin tile (arcs, sparks: low fill of its box) keeps 15%.
 * 3. light: every opaque pixel (stencil bit 7 clear) adds, for each light within the radius,
 *    power x wrapped N.L x (1 - d / radius)^falloff, half of it tinted by the surface colour.
 *    The sum is soft-limited (x kMaxAdd / (kMaxAdd + luma)) so stacked lights never go white.
 * Emissive pixels do not light themselves. Needs SSAO on this dispatch (depth copy, stencil, normal
 * target) and the emissive mask live. Nothing here runs while emissive_lights_enable is off.
 *
 * World lights (WorldLights.h): the nearest world_lights_count census emitters that are on are
 * added as a second list, so emitters off screen or small on screen still light.
 *
 * Temporal stability (LightTracker.h): the select pass ranks kMaxCand candidate tiles; they are
 * read back and matched to fixed world-space light slots, smoothed, shown and hidden with
 * hysteresis and faded. A world light takes over the tiles within its radius (world priority),
 * so orbs, lava, lamps and crystals light from their fixed position. Tracking runs once per
 * frame (the second stereo eye only re-projects the slots) and restarts after a camera cut.
 */
class EmissiveLights {
 public:
  static constexpr int kTilesX = 64;
  static constexpr int kMaxLights = LightTracker::kSlots;
  static constexpr int kMaxWorld = LightTracker::kWorldSlots;  // >= world_lights::kMaxLights
  static constexpr int kMaxCand = 64;  // candidate tiles read back per frame
  static constexpr int kCullX = 32;    // perf-opt bit 3: light-list cells across the screen
  static constexpr int kSplit = 4;     // perf-opt bit 2: tile taps split kSplit x kSplit ways

  EmissiveLights() = default;
  ~EmissiveLights();
  EmissiveLights(const EmissiveLights&) = delete;
  EmissiveLights& operator=(const EmissiveLights&) = delete;

  /// Run the three passes; `em_tex` is the emissive mask, `em_samples` its sample count.
  void render(SharedRenderState* render_state,
              ScopedProfilerNode& prof,
              SsaoRenderer& ssao,
              GLuint em_tex,
              int em_samples);
  void draw_debug_window();

 private:
  struct Target {
    GLuint fbo = 0;
    GLuint tex[4] = {0, 0, 0, 0};
  };
  void ensure_targets(int w, int h, bool hdr);
  void free_targets();
  /// This frame's world lights (world space, metres). `pi` (SSAO proj_info) and the `w` x `h`
  /// frame give the view the glow props are ranked in.
  void gather_world(SharedRenderState* rs, const float* pi, int w, int h,
                    std::vector<LightTracker::World>& out);
  /// The frame's camera as the SSAO camera space sees it (view signs, rotation, position).
  struct CamSnap {
    float sg[3];
    float t[3];
    float r[3][3];
  };
  static CamSnap snap(SharedRenderState* rs);
  /// World metres to the SSAO camera space, and back.
  void to_cam(SharedRenderState* rs, const float* world_m, float* cam) const;
  static void to_world(const CamSnap& c, const float* cam, float* world_m);
  /// Select-pass inputs; `cpu` (perf-opt bit 2): read the tiles back and rank them on the CPU.
  struct SelectIn {
    bool cpu = false;
    float threshold = 0.f;
    float tile_area = 0.f;
  };
  /// A candidate row (or the tiles) read into a pixel buffer and taken a frame later (bit 0).
  struct Readback {
    GLuint pbo = 0;
    GLsizeiptr size = 0;
    GLsync fence = nullptr;
    CamSnap cam{};
    float tile_m = 0.f;  // tile width per metre of depth
    u64 frame = 0;
    SelectIn sel{};
    int n = 0;  // texels per row read
  };
  static void select_tiles(const float* tf,
                           const float* tp,
                           int n,
                           float threshold,
                           float tile_area,
                           float (*flux)[4],
                           float (*pos)[4]);
  void read_rows(bool tiles, void* dst_flux, void* dst_pos);
  void issue_readback(Readback& rb, SharedRenderState* rs, float tile_m, const SelectIn& sel);
  bool take_readback(Readback& rb, float (*flux)[4], float (*pos)[4]);
  void drop_readbacks();
  /// Per screen cell, the lights whose range reaches it (perf-opt bit 3), uploaded to m_cull_tex.
  void build_cull(const float* pi,
                  const float (*lpos)[4],
                  int num_tiles,
                  float tile_r,
                  const float (*wpos)[4],
                  const float (*wpow)[4],
                  int num_world);
  void log_frame(int burst, int num_cand) const;
  /// True when the camera jumped or turned too far since the last tracked frame.
  bool camera_cut(const float* cam_m, const float* fwd);

  int m_w = 0, m_h = 0, m_tiles_y = 0;
  bool m_hdr = false;
  Target m_resolve;  // full-res single-sample copy of the frame
  Target m_tiles;    // kTilesX x m_tiles_y: flux (rgb, E fill), position (xyz, weight)
  Target m_part;     // kSplit x the tiles: partial sums (emissive_tiles.frag stage 1)
  Target m_cand;     // kMaxCand x 1: flux (rgb, score), position (xyz, fill)
  Target m_lights;   // kMaxLights x 1: power (rgb, 1), position (xyz, 1)
  Target m_world;    // kMaxWorld x 1: power (rgb, 1), position (xyz, radius)
  LightTracker m_tracker;
  std::vector<LightTracker::Candidate> m_cands;
  std::vector<LightTracker::World> m_world_in;
  int m_num_world = 0, m_num_tiles = 0;
  float m_world_cpu_ms = 0.f, m_track_cpu_ms = 0.f;
  float m_last_cam[3] = {}, m_last_fwd[3] = {};
  bool m_have_last = false;
  GLuint m_vao = 0;
  GLuint m_cull_tex = 0;
  int m_cull_px = 0, m_cull_y = 0;
  std::vector<u32> m_cull;
  Readback m_rb[2];
  std::vector<float> m_tile_rb;
  int m_rb_head = 0;
  GpuTimer m_timer{"emissive-lights-total"};
};
