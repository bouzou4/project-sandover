#pragma once

#include <memory>
#include <string>
#include <vector>

#include "common/dma/dma_chain_read.h"

#include "game/graphics/opengl_renderer/Profiler.h"
#include "game/graphics/opengl_renderer/Shader.h"
#include "game/graphics/opengl_renderer/buckets.h"
#include "game/graphics/opengl_renderer/loader/Loader.h"
#include "game/graphics/texture/TexturePool.h"

struct VistaNeighbourFog;

struct Fbo;

struct LevelVis {
  bool valid = false;
  u8 data[2048];
};

class EyeRenderer;

struct LevelHeightfield;

/*!
 * Actor shadow maps for this frame (Jak 1), filled by ActorShadows before bucket 0 and read by
 * the receiver shaders. World matrices take GOAL world units; camera matrices take merc camera
 * space (the skinned position before the perspective).
 */
struct ActorShadowFrame {
  static constexpr int kMaxLamps = 2;   // local lights with a shadow map
  static constexpr int kMaxLights = 4;  // local lights lighting merc; the first num_lamps cast
  bool active = false;
  int view = 0;
  int num_lamps = 0;
  int num_lights = 0;
  static constexpr int kMaxCrystals = 7;  // darkcave palette slots 1-7
  float actor_dark = 1.f;  // merc level-light multiplier away from lit crystals (darkcave)
  float bg_dark = 1.f;     // darkcave background: base palette slot multiplier
  int num_crystals = 0;    // lit darkcave crystals; merc rises from actor_dark to 1 near them
  math::Vector4f crystal_cam[kMaxCrystals];  // merc camera space; w: glow (palette fade, 0..2)
  float strength = 1.f;
  float sun_gain = 1.f;              // sun map only: the world map's day and elevation fade
  bool moon = false;                 // the sun map's light is the moon (night slots)
  float marker = 0.f;                // landing disc strength
  math::Vector4f marker_pos_world;   // under the player; w: radius, 0 = no disc
  float reach = 0.f;                 // sun map depth units, see shadow_sun in the shaders
  float share_global = 0.f;  // sun share for receivers without sidecar data
  // terrain sun cover (actor_shadows_cover): strength, 0 = off; maps of the levels in use
  float terrain = 0.f;
  static constexpr int kMaxTerrainMaps = 4;
  const LevelHeightfield* terrain_maps[kMaxTerrainMaps] = {};
  int num_terrain_maps = 0;
  math::Vector4f cam_to_world[4];  // merc camera space to world
  math::Vector4f terrain_sun;      // toward the sun (world, unit); w: low-sun fade of the cover
  u32 caster_tex = 0;              // sun map colour: 1 - the caster's cover (receivers fade by it)
  math::Vector4f sun_world[4];
  math::Vector4f sun_cam[4];
  math::Vector4f lamp_world[kMaxLamps][4];
  math::Vector4f lamp_cam[kMaxLamps][4];
  math::Vector4f lamp_pos_world[kMaxLights];  // w: falloff radius r0 (game units)
  math::Vector4f lamp_pos_cam[kMaxLights];
  math::Vector4f lamp_color[kMaxLights];  // power, merc vertex-colour units; w: cut-off radius
  float sun_texel = 0, lamp_texel = 0;
  int taps = 9;  // PCF taps per map lookup: 1, 4 or 9 (shadow quality)
  u32 sun_tex = 0;
  u32 lamp_tex[kMaxLamps] = {0, 0};
  // maps of lights the bake does not hold (glow props, dark eco crystals): the tile pass shadows
  // their own light instead of the receivers removing a share of the baked light
  int lamp_row[kMaxLamps] = {-1, -1};
  float lamp_added[kMaxLamps] = {0.f, 0.f};
  float light_added[kMaxLights] = {0.f, 0.f, 0.f, 0.f};
  // sky-hued shadows: strength (0 = neutral), the live fitted sun and ambient hues (luminance 1)
  float hue_t = 0.f;
  math::Vector4f hue_lgt, hue_amb;
};

/*!
 * The world sun map of this frame (WorldShadows.h): up to three cascades of the static world drawn
 * from the sun, read by the background, merc and (optionally) water receivers.
 */
struct WorldShadowFrame {
  static constexpr int kMaxCascades = 3;
  bool active = false;   // a map was drawn this frame
  bool world = false;    // background receivers take it (option 32)
  bool actors = false;   // merc takes it in place of the terrain cover (option 33)
  bool water = false;    // water may read it (option 34)
  int cascades = 0;
  float strength = 0.f;  // day and elevation fade, 0..1
  math::Vector4f mtx_world[kMaxCascades][4];  // world (game units) to cascade clip
  math::Vector4f mtx_cam[kMaxCascades][4];    // merc camera space to cascade clip
  float texel_world[kMaxCascades] = {};       // game units per texel
  float depth_range[kMaxCascades] = {};       // game units per unit of depth
  float uv_scale[kMaxCascades] = {};          // share of the layer side the cascade fills
  math::Vector4f sun_world;                   // toward the sun, world
  math::Vector4f sun_cam;                     // toward the sun, merc camera space
  float penumbra = 0.f;                       // per unit of blocker distance
  float amb_floor = 0.f;                      // shader colour a receiver at the bake's ambient has
  u32 tex = 0;                                // depth array
  int size = 0;                               // texels per side
};

/*!
 * XR head tracking for the eye being drawn (Jak 1). A point c in the game camera's space
 * (+x left, +y up, +z forward, game units) is at rot * (c - t) in the eye's camera space, and
 * the eye projects with the game's perspective scaled by fov_x / fov_y on screen x / y.
 */
struct EyeView {
  bool active = false;
  float rot[3][3] = {{1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}};
  float t[3] = {0.f, 0.f, 0.f};
  float fov_x = 1.f, fov_y = 1.f;
};

/*!
 * The game's camera for the frame, as the background packets carried it (before any eye
 * transform). Kept across frames: buckets drawn before the first tfrag use the previous one.
 */
struct GameCamera {
  bool valid = false;
  math::Vector4f planes[4];
  math::Vector4f matrix[4];
  math::Vector4f persp[4];
  math::Vector4f rot[4];
  math::Vector4f pos;
  math::Vector4f hvdf_off;
  math::Vector4f fog;
};

/*!
 * The main renderer will contain a single SharedRenderState that's passed to all bucket renderers.
 * This allows bucket renders to share textures and shaders.
 */
struct SharedRenderState {
  explicit SharedRenderState(std::shared_ptr<TexturePool> _texture_pool,
                             std::shared_ptr<Loader> _loader,
                             GameVersion _version)
      : shaders(_version), texture_pool(_texture_pool), loader(_loader), version(_version) {}
  ShaderLibrary shaders;
  std::shared_ptr<TexturePool> texture_pool;
  std::shared_ptr<Loader> loader;

  u32 buckets_base = 0;  // address of buckets array.
  u32 next_bucket = 0;   // address of next bucket that we haven't started rendering in buckets
  u32 default_regs_buffer = 0;  // address of the default regs chain.

  void* ee_main_memory = nullptr;
  u32 offset_of_s7;

  bool use_sky_cpu = true;
  bool use_occlusion_culling = true;
  math::Vector<u8, 4> fog_color = math::Vector<u8, 4>{0, 0, 0, 0};
  float fog_intensity = 1.f;
  bool no_multidraw = false;

  void reset();
  bool has_pc_data = false;

  // limit is arbitrary so let's go ham in case we want more levels in the future
  LevelVis occlusion_vis[32];

  math::Vector4f camera_planes[4];

  // including transformation, rotation, perspective
  math::Vector4f camera_matrix[4];
  math::Vector4f camera_hvdf_off;
  math::Vector4f camera_fog;
  math::Vector4f camera_pos;
  math::Vector4f camera_persp[4];
  math::Vector4f camera_rot[4];  // row-vector rotation; column j is camera axis j in world space

  EyeRenderer* eye_renderer = nullptr;

  std::string load_status_debug;

  // Information for renderers that need to read framebuffers:
  // Most renderers can just use the framebuffer/glViewport set up by OpenGLRenderer, but special
  // effects like sprite distort that read the framebuffer will need to know the details of the
  // framebuffer setup.

  // the framebuffer that bucket renderers should render to.
  int render_fb_x = 0;
  int render_fb_y = 0;
  int render_fb_w = 0;
  int render_fb_h = 0;
  GLuint render_fb = -1;
  // render_fb's depth-stencil texture (multisampled when the frame is), 0 if not a texture
  GLuint render_fb_depth_tex = 0;

  // the region within that framebuffer to draw to.
  int draw_region_w = 0;
  int draw_region_h = 0;
  int draw_offset_x = 0;
  int draw_offset_y = 0;

  int bucket_for_vis_copy = 0;
  int num_vis_to_copy = 0;
  GameVersion version;
  u64 frame_idx = 0;

  bool stencil_dirty = false;
  bool vista_drawn = false;  // vista debug mode drew a level last frame
  // neighbour mode: viewer-level stand-in filter and levels whose med-res actors are hidden
  std::string vista_hide_filter;
  std::vector<std::string> vista_medres_hidden;
  // vista fog policy for this frame's background draws (VistaFogMode; MAIN = vanilla)
  int vista_frame_fog = 1;
  const VistaNeighbourFog* vista_nb_fog = nullptr;  // override of the neighbour being drawn
  u64 atmo_set_shaders = 0;    // shaders whose atmo uniform is non-zero (bit per ShaderId)
  bool ssao_stencil = false;   // SSAO owns stencil bits 6-7 this frame (Jak 1)
  bool emissive_mask = false;  // opaque tfrag/tie write the emissive mask to draw buffer 2
  // the mask is RGBA8 and carries the highlight material in b (roughness), a (1 - sqrt(F0 x lobe))
  bool light_material = false;
  bool merc_props = false;  // Merc2 is drawing a merc level-prop bucket (MERC_TFRAG_TEX)
  bool hdr_target = false;     // main colour target is RGBA16F (Jak 1)
  float render_scale = 1.f;    // scene height over display height (render scale, Upscaler.h)
  GLuint water_depth_tex = 0;  // opaque scene depth for the ocean shading, 0 when not captured

  // stereo debug: camera-space x offset of the eye being drawn (game units), 0 when mono
  bool stereo_active = false;
  float stereo_eye_x = 0.f;
  bool stereo_repeat_pass = false;  // second eye of a frame: skip per-frame simulation
  // XR head tracking: replaces stereo_eye_x while active (stereo_eye_x is then 0)
  EyeView eye;
  GameCamera game_camera;

  ActorShadowFrame shadow;
  WorldShadowFrame world_shadow;
};

/*!
 * The camera the frame's GS-space geometry and camera-space lighting were built with: the game
 * camera while an XR eye is drawn, otherwise the camera_* fields.
 */
struct FrameCamera {
  const math::Vector4f* matrix;
  const math::Vector4f* rot;
  const math::Vector4f* persp;
  const math::Vector4f* pos;
  const math::Vector4f* hvdf_off;
  const math::Vector4f* fog;
};
FrameCamera frame_camera(const SharedRenderState& state);

/*!
 * Applies the eye to a camera-space-to-clip matrix in GOAL row form (clip = sum_i c_i * p[i]):
 * p becomes D * p * S, D the eye's rigid transform and S the fov scale on clip x and y.
 */
void eye_camera_to_clip(math::Vector4f p[4], const EyeView& eye);

/*!
 * Applies the eye to a world-to-clip matrix m = X * persp in GOAL row form: m becomes
 * X * D * persp * S. Returns false (m unchanged) if persp cannot be inverted.
 */
bool eye_world_to_clip(math::Vector4f m[4], const math::Vector4f persp[4], const EyeView& eye);

/*!
 * Applies the eye to an affine world-to-camera matrix in GOAL row form (xyz lanes only): the
 * axis rows become R * row and the translation row R * (row - t).
 */
void eye_world_to_camera(math::Vector4f m[4], const EyeView& eye);

/*!
 * Sets the uniforms of xr_gs.glsl for geometry that arrives in GS coordinates. mode 1
 * reprojects with the vertex depth, mode 2 treats every vertex as at infinity (sky). Off
 * (identity) unless an XR eye is drawn.
 */
void set_xr_gs_uniforms(u32 program, const SharedRenderState& state, int mode);

/*!
 * Per-eye parallax for geometry that arrives already projected to GS coordinates (ocean, shadow).
 * Returns (k, z0) such that the eye's x = x + k * (z - z0), with x in GS units / 4096 and z in
 * GS depth / 2^24, using the frame's perspective from the background data. (0, 0) when mono.
 */
math::Vector2f stereo_gs_parallax(const SharedRenderState& state);

/*!
 * Interface for bucket renders. Each bucket will have its own BucketRenderer.
 */
class BucketRenderer {
 public:
  BucketRenderer(const std::string& name, int my_id) : m_name(name), m_my_id(my_id) {}
  virtual void render(DmaFollower& dma,
                      SharedRenderState* render_state,
                      ScopedProfilerNode& prof) = 0;
  std::string name() const;
  std::string name_and_id() const;
  virtual ~BucketRenderer() = default;
  bool& enabled() { return m_enabled; }
  virtual bool empty() const { return false; }
  virtual void draw_debug_window() = 0;
  virtual void init_shaders(ShaderLibrary&) {}
  virtual void init_textures(TexturePool&, GameVersion) {}

 protected:
  std::string m_name;
  int m_my_id;
  bool m_enabled = true;
};

class RenderMux : public BucketRenderer {
 public:
  RenderMux(const std::string& name,
            int my_id,
            std::vector<std::unique_ptr<BucketRenderer>> renderers);
  void render(DmaFollower& dma, SharedRenderState* render_state, ScopedProfilerNode& prof) override;
  void draw_debug_window() override;
  void init_shaders(ShaderLibrary&) override;
  void init_textures(TexturePool&, GameVersion) override;
  void set_idx(u32 i) { m_render_idx = i; };

 private:
  std::vector<std::unique_ptr<BucketRenderer>> m_renderers;
  int m_render_idx = 0;
  std::vector<std::string> m_name_strs;
  std::vector<const char*> m_name_str_ptrs;
};

/*!
 * Renderer that makes sure the bucket is empty and ignores it.
 */
class EmptyBucketRenderer : public BucketRenderer {
 public:
  EmptyBucketRenderer(const std::string& name, int my_id);
  void render(DmaFollower& dma, SharedRenderState* render_state, ScopedProfilerNode& prof) override;
  bool empty() const override { return true; }
  void draw_debug_window() override {}
};

class SkipRenderer : public BucketRenderer {
 public:
  SkipRenderer(const std::string& name, int my_id);
  void render(DmaFollower& dma, SharedRenderState* render_state, ScopedProfilerNode& prof) override;
  bool empty() const override { return true; }
  void draw_debug_window() override {}
};

/*!
 * Renderer that ignores and prints all DMA transfers.
 */
class PrintRenderer : public BucketRenderer {
 public:
  PrintRenderer(const std::string& name, int my_id);
  void render(DmaFollower& dma, SharedRenderState* render_state, ScopedProfilerNode& prof) override;
  bool empty() const override { return true; }
  void draw_debug_window() override {}
};
