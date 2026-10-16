#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/opengl_renderer/GlQueries.h"
#include "game/graphics/opengl_renderer/Shader.h"
#include "game/graphics/opengl_renderer/Neighbours.h"
#include "game/graphics/opengl_renderer/WorldLights.h"
#include "game/graphics/opengl_renderer/WorldShadows.h"
#include "game/graphics/opengl_renderer/foreground/Generic2.h"
#include "game/graphics/opengl_renderer/foreground/Merc2.h"

/*!
 * Actor shadow maps (Jak 1): a sun map and up to two local-light maps holding only the merc and
 * generic actors, drawn before bucket 0 so that the background and the actors can receive them.
 * Background receivers remove only the share of their light that comes from the sun (from the
 * .unbake sidecar, or a global estimate) or from the local light (inverse-square estimate), so
 * the bake's own shadows are not darkened twice.
 *
 * Local lights: the up to four world-light-table lights (lamps, fires, vents; see
 * world_lights::select_actor) that bring the most light to Jak add their light to merc actors;
 * the first two also get a spot map aimed at Jak, so he and his neighbours cast from them. In
 * darkcave the merc level light is scaled by actor_dark and rises to full near lit crystals, per
 * fragment, with each crystal's palette fade (actor_shadows_set_cave_uniforms).
 *
 * The pre-pass replays the frame's merc and generic actor buckets (level props 10-11/17-18,
 * translucent merc 45, characters and pickups 49-56) into the depth maps (the frame's DMA can be
 * walked twice, as the stereo mode does), after reading the camera from the first tfrag
 * bucket's PC port data. Sprites, particles and light-emitting models (orbs, power cells, hut
 * lamps; Merc2::casts_actor_shadow) never cast. Setting
 * ShadowMode::OFF skips all of it.
 */
class ActorShadows {
 public:
  ActorShadows();
  ~ActorShadows();
  ActorShadows(const ActorShadows&) = delete;
  ActorShadows& operator=(const ActorShadows&) = delete;

  /// Before bucket 0. `dma` is a copy positioned at bucket 0; fills render_state->shadow.
  void prepass(DmaFollower dma,
               SharedRenderState* render_state,
               Merc2& merc,
               Generic2& generic,
               ScopedProfilerNode& prof);
  void draw_debug_window();

  static constexpr int kSunSize = 2048;  // high shadow quality; medium 3/4, low 1/2
  static constexpr int kLampSize = 512;
  static constexpr int kOccCell = 32;  // sun-map texels per occupancy cell (pass 1)

 private:
  /// (Re)create the maps and the occupancy grid for shadow quality `q` (0..2).
  void make_targets(int q);
  void release_targets();
  int m_quality = -1;
  int m_sun_size = kSunSize;
  int m_lamp_size = kLampSize;

  /// Fade chosen lights in and dropped ones out (kFadeS), so a light never pops on or off.
  void fade_lights(std::vector<world_lights::ActorLight>* lights);
  struct FadedLight {
    world_lights::ActorLight light;
    float weight;
  };
  std::vector<FadedLight> m_faded;
  std::chrono::steady_clock::time_point m_fade_time;
  bool m_have_fade_time = false;
  std::chrono::steady_clock::time_point m_dark_time;
  bool m_have_dark_time = false;

  struct Target {
    GLuint fbo = 0;
    GLuint tex = 0;
    GLuint colour = 0;  // sun map only: R8, 1 - the nearest caster's cover
  };
  static Target make_target(int size, bool caster_colour = false);

  /// Min sun-map depth per kOccCell cell (perf opt geometry+0), sampled by the receivers.
  void build_occupancy();
  std::unique_ptr<Shader> m_occ_shader;
  GLuint m_occ_tex = 0, m_occ_fbo = 0, m_occ_sampler = 0, m_occ_vao = 0;
  GLuint m_occ_mid_tex = 0, m_occ_mid_fbo = 0;  // first level: min per 8x8 texels
  glq::SavedState m_occ_state{"actor-shadow-occ", PERF_OPT_GEOMETRY + 0};

  Target m_sun;
  Target m_lamps[ActorShadowFrame::kMaxLamps];
  WorldShadows m_world;         // static world sun map, drawn after the actor maps
  neighbours::MoodTable m_mood;  // slot hours for the sun arc (sun_disc_direction)
  std::vector<math::Vector3f> m_prev_actors;  // world, from the previous frame
  math::Vector3f m_prev_player{0.f, 0.f, 0.f};  // world, from the previous frame
  bool m_have_prev_player = false;
  std::vector<int> m_prev_lamps;  // world light rows that cast last frame
  math::Vector3f m_fallback_dir{0.f, 1.f, 0.f};
  float m_fallback_share = 0.25f;
  bool m_have_fallback = false;

  GLuint m_query = 0;
  bool m_query_pending = false;
  double m_gpu_ms_sum = 0, m_cpu_ms_sum = 0;
  int m_gpu_n = 0, m_cpu_n = 0;

  // last frame, for the debug window
  std::string m_level;
  math::Vector3f m_sun_dir{0.f, 1.f, 0.f};
  float m_sun_gain = 1.f;    // the shadows' light fade this frame (1 without the arc)
  bool m_moon = false;       // the shadows' light is the moon (night slots)
  float m_moon_share = 0.f;  // the moon blend's direct share, for the log
  math::Vector3f m_last_light{0.f, 1.f, 0.f};  // last sun or moon direction, held at zero gain
  bool m_have_last_light = false;
  bool m_sun_from_sidecar = false;
  int m_actor_count = 0;
  int m_num_lights = 0;
  float m_actor_dark = 1.f;
  float m_player_cover = 0.f;  // actor_shadows_cover at Jak last frame (log)
  std::string m_last_light_ids;
};

/*!
 * Share of the sun an actor at `cam_pos` (merc camera space, its root) loses to the solid world
 * along the sun ray: five rays (feet, waist, head, both sides) marched toward the sun in 1 m
 * steps out to 40 m through the level heightfield (LevelHeightfield::solid_span: below the top and
 * outside the column's open span, so an arch or roof the ray passes under does not count); the
 * share of rays that hit, eased; times the TERRAIN_SUN strength and the canopy's low-sun fade
 * (sun.y 0.25 to 0.1). The rise is clamped as the canopy's (sun.y >= 0.1). 0 when the maps are off
 * or no heightfield covers the path (levels without a sky).
 */
float actor_shadows_cover(const SharedRenderState* render_state, const math::Vector3f& cam_pos);

/*!
 * Set the receiver uniforms (shadow_*) and bind the maps to units 12-14 for `program`, which
 * must be active. camera_space selects the merc matrices. With shadows off only shadow_on = 0 is
 * set.
 */
void actor_shadows_set_uniforms(const SharedRenderState* render_state,
                                GLuint program,
                                bool camera_space);

/*!
 * Set the darkcave merc light uniforms (cave_dark_base, cave_ncrystal, cave_crystal) for
 * `program`, which must be active: the level light is scaled by cave_dark_base and rises to 1
 * near lit crystals (cave_dark_frag.glsl). Base 1 and no crystals with shadows off.
 */
void actor_shadows_set_cave_uniforms(const SharedRenderState* render_state, GLuint program);
