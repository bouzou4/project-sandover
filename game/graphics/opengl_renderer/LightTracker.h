#pragma once

#include <vector>

/*!
 * Temporal stability for the emissive lights (lava and orb light flickered while
 * the camera moved). The screen-space tiles are anchored to the screen
 * grid, so a moving camera moves each emitter across tile edges and the brightest tiles' position,
 * colour and thin/compact class jump from frame to frame. This keeps a fixed set of light slots
 * in world space (metres) instead:
 * - each frame's candidate tiles are matched to the slots by distance (greedy, nearest first,
 *   within a gate of a few tiles' width at the candidate's distance);
 * - a matched slot moves its position and power towards the candidate by `blend` per frame;
 * - thin/compact is decided with hysteresis on the tile's fill;
 * - a slot shows after `frames_in` frames in a row with a match and hides after `frames_out`
 *   without one, fading over `fade_frames` either way (no pops);
 * - world lights (census emitters) fade the same way by table row, and a slot within
 *   `suppress` x a world light's radius gives no light while that world light is on: the fixed
 *   world position lights that emitter instead of the tiles.
 * No GL here; EmissiveLights reads the candidates back and uploads the result.
 */
class LightTracker {
 public:
  static constexpr int kSlots = 32;
  static constexpr int kWorldSlots = 32;

  struct Emitter {
    float pos[3];  // world, metres
    float radius;
    float colour[3];
  };

  struct Params {
    float blend = 0.25f;  // share of the new value taken per frame (1 = no smoothing)
    int frames_in = 3;
    int frames_out = 3;
    int fade_frames = 8;
    float suppress = 1.f;  // x world radius; 0 = world lights never suppress tiles
    float fill_lo = 0.2f, fill_hi = 0.5f;
    float thin_cover = 0.15f;  // power kept by a thin tile
    float gate_tiles = 2.f;    // match gate in tile widths at the candidate's distance
    float gate_min_m = 0.5f;
    float ramp = 0.06f;  // World::ramp rows: power change per frame, x each channel's power
    // a tile toward a launch-placed emitter (within `emitter_tiles` tile widths), at its depth
    // or up to 10% nearer (a tile takes its nearest surface), in its hue, stands for it: no light
    const std::vector<Emitter>* emitters = nullptr;
    float emitter_tiles = 1.5f;
  };

  struct Candidate {
    float pos[3];    // world, metres
    float power[3];  // before the thin cover
    float fill;
  };

  struct World {
    int row;
    float pos[3];
    float power[3];
    float radius;
    bool arc;
    bool suppress = true;  // takes over the screen tiles near it (false: a light the bake lacks)
    bool wide = false;     // falloff r0 kWideR0M (dark eco crystal)
    bool snap = false;     // no fade: full while listed, gone the frame it is not
    bool ramp = false;     // no fade in: power slews from 0 by at most Params::ramp a frame
  };

  struct Out {
    float pos[3];
    float power[3];
    float radius;  // world lights only
    bool arc;
    bool added = false;  // world light the bake lacks (World::suppress false)
    bool wide = false;
    int id;  // slot (tiles) or table row (world)
  };

  /*!
   * One frame. `cands` rank ordered (brightest first); `tile_m_per_m` is a tile's width in metres
   * per metre of distance; `cam` the camera position (world, metres). With `tiles` false (no tile
   * readback this frame) only the world lights step; the tile slots and tiles() are left as they
   * were and `cands` is ignored.
   */
  void update(const std::vector<Candidate>& cands,
              const std::vector<World>& world,
              float tile_m_per_m,
              const float cam[3],
              const Params& p,
              bool tiles = true);
  void reset();

  const std::vector<Out>& tiles() const { return m_tiles_out; }
  const std::vector<Out>& world() const { return m_world_out; }
  int live_slots() const;

 private:
  struct Slot {
    bool used = false;
    float pos[3] = {};
    float power[3] = {};
    bool thin = false;
    int seen = 0, missed = 0;
    bool visible = false;
    bool evict = false;  // fading out to make room for a brighter light
    float fade = 0.f;
  };
  struct WSlot {
    int row = -1;
    World w{};
    float shown[3] = {};  // ramp rows: the power drawn
    int seen = 0, missed = 0;
    bool visible = false;
    float fade = 0.f;
  };
  Slot m_slots[kSlots];
  WSlot m_world[kWorldSlots];
  std::vector<Out> m_tiles_out, m_world_out;
};
