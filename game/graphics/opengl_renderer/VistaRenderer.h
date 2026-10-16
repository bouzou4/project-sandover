#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/opengl_renderer/Neighbours.h"
#include "game/graphics/opengl_renderer/SsaoDepth.h"
#include "game/graphics/opengl_renderer/background/Shrub.h"
#include "game/graphics/opengl_renderer/background/TFragment.h"
#include "game/graphics/opengl_renderer/background/Tie3.h"

/*!
 * Vista debug mode (Jak 1): draws the static background (tfrag, tie, shrub) of levels that the
 * loader keeps resident but the game does not draw, using the camera packet of a level drawn
 * this frame. One level (vista_level), or in neighbour mode every level adjacent to the game's
 * levels, each with its own palette weights and its trans/dirt/ice/water tfrag trees.
 * Culling is frustum only. Nothing is allocated or drawn unless vista_enable is set.
 */
class VistaRenderer {
 public:
  explicit VistaRenderer(const std::vector<GLuint>* anim_slot_array)
      : m_anim_slot_array(anim_slot_array) {}

  /// Opaque draws (after the last shrub bucket). cams are this frame's level 0/1 tfrag packets.
  void render(SharedRenderState* render_state,
              ScopedProfilerNode& prof,
              const TfragPcPortData* cam0,
              const TfragPcPortData* cam1);
  /// Alpha tfrag trees of the levels drawn by render() this frame (after the level alpha buckets).
  void render_alpha(SharedRenderState* render_state, ScopedProfilerNode& prof);
  void draw_debug_window();

 private:
  void log_status(SharedRenderState* render_state, const std::string& log_line);
  struct LevelRenderers {
    std::unique_ptr<TFragment> tfrag;
    std::unique_ptr<TFragment> alpha;
    std::unique_ptr<Tie3> tie;
    std::unique_ptr<Shrub> shrub;
  };
  LevelRenderers& renderers_for(const std::string& level, SharedRenderState* render_state);
  /// Distance (game units) from `cam` to the level's background bounds; -1 if not loaded.
  float level_distance(const std::string& level,
                       SharedRenderState* render_state,
                       const math::Vector4f& cam);
  struct Bounds {
    u64 load_id = UINT64_MAX;
    math::Vector3f lo, hi;
  };
  std::map<std::string, Bounds> m_bounds;
  bool level_in_view(const std::string& level, const math::Vector4f* planes) const;
  /// Bounds (with a 1 m margin) intersect, or unknown.
  bool levels_overlap(const std::string& a, const std::string& b) const;
  std::vector<std::string> m_offscreen;  // drawn levels skipped this frame as outside the frustum
  /// Re-record the frame lights of the level the unsorted order draws last (see render()).
  void restore_frame_lights(SharedRenderState* render_state);
  int m_lights_level = -1;
  std::vector<std::pair<std::string, TfragRenderSettings>> m_unsorted;
  std::string m_lights_name;
  TfragRenderSettings m_lights_settings;
  float neighbour_hour(const SharedRenderState* render_state) const;

  const std::vector<GLuint>* m_anim_slot_array = nullptr;
  std::map<std::string, LevelRenderers> m_levels;
  std::vector<std::pair<std::string, TfragRenderSettings>> m_drawn;
  neighbours::MoodTable m_mood;
  GpuTimer m_timer{"vista-draw"};
  GpuTimer m_alpha_timer{"vista-alpha"};
  std::string m_status = "off";
  int m_mood_drift = -1;  // viewer level: own-table itimes vs the game's, in 1/64
  float m_hour = -1.f;
  std::string m_last_log;
  u64 m_next_log_frame = 0;
};
