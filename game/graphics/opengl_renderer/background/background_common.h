#pragma once

#include "common/math/Vector.h"

#include "game/graphics/opengl_renderer/BucketRenderer.h"

struct GoalBackgroundCameraData {
  math::Vector4f planes[4];
  math::Vector<s32, 4> itimes[4];
  math::Vector4f camera[4];
  math::Vector4f hvdf_off;
  math::Vector4f fog;
  math::Vector4f trans;
  math::Vector4f rot[4];
  math::Vector4f perspective[4];
};

// data passed from game to PC renderers
// the GOAL code assumes this memory layout.
struct TfragPcPortData {
  GoalBackgroundCameraData camera;
  char level_name[32];
};
static_assert(sizeof(TfragPcPortData) == 16 * 25);

// inputs to background renderers.
struct TfragRenderSettings {
  GoalBackgroundCameraData camera;
  int tree_idx;
  bool debug_culling = false;
  const u8* occlusion_culling = nullptr;
};

enum class DoubleDrawKind { NONE, AFAIL_NO_DEPTH_WRITE };

struct DoubleDraw {
  DoubleDrawKind kind = DoubleDrawKind::NONE;
  float aref_first = 0.;
  float aref_second = 0.;
  float color_mult = 1.;
};

DoubleDraw setup_tfrag_shader(SharedRenderState* render_state, DrawMode mode, ShaderId shader);
DoubleDraw setup_opengl_from_draw_mode(DrawMode mode, u32 tex_unit, bool mipmap);

void first_tfrag_draw_setup(const GoalBackgroundCameraData& settings,
                            SharedRenderState* render_state,
                            ShaderId shader);

void interp_time_of_day(const math::Vector<s32, 4> itimes[4],
                        const tfrag3::PackedTimeOfDay& packed_colors,
                        math::Vector<u8, 4>* out);

void cull_check_all_slow(const math::Vector4f* planes,
                         const std::vector<tfrag3::VisNode>& nodes,
                         const u8* level_occlusion_string,
                         u8* out);
bool sphere_in_view_ref(const math::Vector4f& sphere, const math::Vector4f* planes);

void update_render_state_from_pc_settings(SharedRenderState* state, const TfragPcPortData& data);

/*!
 * Darkcave with actor shadows on: scale the base palette slot (0) of the level's time-of-day
 * blend by the cave dark factor. The crystal light is in slots 1-7 (update-mood-darkcave's
 * palette fades), so crystal-lit rock keeps its light. No-op for every other level.
 */
void cave_dark_itimes(const SharedRenderState* state, TfragPcPortData* data);

/*!
 * Moves the camera to the eye being drawn: by render_state->stereo_eye_x game units along its
 * own x axis (stereo), or by the XR eye (render_state->eye: rotation, offset and fov). Updates
 * the world-space translation (pc_camera path), the rotation and perspective, the full GOAL
 * camera matrix (wind, etie) and the frustum planes, so background culling is done per eye.
 * Also records the camera as it arrived in render_state->game_camera.
 */
void apply_stereo_eye(GoalBackgroundCameraData* cam, SharedRenderState* render_state);

void make_all_visible_multidraws(std::pair<int, int>* draw_ptrs_out,
                                 GLsizei* counts_out,
                                 void** index_offsets_out,
                                 const std::vector<tfrag3::ShrubDraw>& draws);

u32 make_all_visible_multidraws(std::pair<int, int>* draw_ptrs_out,
                                GLsizei* counts_out,
                                void** index_offsets_out,
                                const std::vector<tfrag3::StripDraw>& draws);

u32 make_multidraws_from_vis_string(std::pair<int, int>* draw_ptrs_out,
                                    GLsizei* counts_out,
                                    void** index_offsets_out,
                                    const std::vector<tfrag3::StripDraw>& draws,
                                    const std::vector<u8>& vis_data);

u32 make_all_visible_index_list(std::pair<int, int>* group_out,
                                u32* idx_out,
                                const std::vector<tfrag3::StripDraw>& draws,
                                const u32* idx_in,
                                u32* num_tris_out);

u32 make_index_list_from_vis_string(std::pair<int, int>* group_out,
                                    u32* idx_out,
                                    const std::vector<tfrag3::StripDraw>& draws,
                                    const std::vector<u8>& vis_data,
                                    const u32* idx_in,
                                    u32* num_tris_out);

u32 make_all_visible_index_list(std::pair<int, int>* group_out,
                                u32* idx_out,
                                const std::vector<tfrag3::ShrubDraw>& draws,
                                const u32* idx_in);

u32 make_multidraws_from_vis_and_proto_string(std::pair<int, int>* draw_ptrs_out,
                                              GLsizei* counts_out,
                                              void** index_offsets_out,
                                              const std::vector<tfrag3::StripDraw>& draws,
                                              const std::vector<u8>& vis_data,
                                              const std::vector<u8>& proto_vis_data);

u32 make_index_list_from_vis_and_proto_string(std::pair<int, int>* group_out,
                                              u32* idx_out,
                                              const std::vector<tfrag3::StripDraw>& draws,
                                              const std::vector<u8>& vis_data,
                                              const std::vector<u8>& proto_vis_data,
                                              const u32* idx_in,
                                              u32* num_tris_out);

/*!
 * Vista debug mode (Jak 1): true when the main-level renderer at level_id should hide the
 * stand-ins named by the vista hide filter this frame.
 */
bool vista_hide_active(const SharedRenderState* render_state, int level_id);
/// Stand-in filter for the viewer levels: the computed one in neighbour mode, else vista_hide.
const char* active_vista_filter(const SharedRenderState* render_state);

/*!
 * True if name contains any of the comma-separated substrings in filter.
 */
bool vista_name_matches(const std::string& name, const char* filter);

// neighbour-mode haze uniforms (haze, haze_color); scale multiplies the haze density
void vista_haze_uniforms(const SharedRenderState* render_state, GLuint program, float scale);

/*!
 * Unified vista fog for foreground programs (merc, emerc, sprites): uniforms `atmo` (as in
 * tfrag3.vert; x = 0 when the frame's policy is not unified), `atmo_up` (world y of each camera
 * axis) and `atmo_cam` (camera position, game units). The program must be active.
 */
void vista_atmo_foreground_uniforms(const SharedRenderState* render_state, GLuint program);

/*!
 * Copy of a background camera whose fog runs from the same start to end_m metres. Scales the
 * fog slope, depth-to-fog row and camera w column together, so projection and depth stay exact.
 */
GoalBackgroundCameraData vista_fog_camera(const GoalBackgroundCameraData& in, float end_m);
