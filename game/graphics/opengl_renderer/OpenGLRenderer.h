#pragma once

#include <array>
#include <memory>

#include "common/dma/dma_chain_read.h"

#include "game/graphics/opengl_renderer/BloomRenderer.h"
#include "game/graphics/opengl_renderer/EmissiveLights.h"
#include "game/graphics/opengl_renderer/ActorShadows.h"
#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/opengl_renderer/CollideMeshRenderer.h"
#include "game/graphics/opengl_renderer/Fbo.h"
#include "game/graphics/opengl_renderer/InspectRenderer.h"
#include "game/graphics/opengl_renderer/PerfTimers.h"
#include "game/graphics/opengl_renderer/Profiler.h"
#include "game/graphics/opengl_renderer/Shader.h"
#include "game/graphics/opengl_renderer/Smaa.h"
#include "game/graphics/opengl_renderer/SunDisc.h"
#include "game/graphics/opengl_renderer/Upscaler.h"
#include "game/graphics/opengl_renderer/SsaoDepth.h"
#include "game/graphics/opengl_renderer/SsaoRenderer.h"
#include "game/graphics/opengl_renderer/Swamp.h"
#include "game/graphics/opengl_renderer/TextureAnimator.h"
#include "game/graphics/opengl_renderer/VistaRenderer.h"
#include "game/graphics/opengl_renderer/foreground/Generic2.h"
#include "game/graphics/opengl_renderer/foreground/Merc2.h"
#include "game/graphics/opengl_renderer/opengl_utils.h"
#include "game/tools/filter_menu/filter_menu.h"
#include "game/tools/subtitle_editor/subtitle_editor.h"

namespace xr {
struct FrameTargets;
struct EyeTarget;
}  // namespace xr

struct RenderOptions {
  bool draw_render_debug_window = false;
  bool draw_profiler_window = false;
  bool draw_loader_window = false;
  bool draw_small_profiler_window = false;
  bool draw_subtitle_editor_window = false;
  bool draw_filters_window = false;

  // internal rendering settings - The OpenGLRenderer will internally use this resolution/format.
  int msaa_samples = 1;
  int game_res_w = 640;
  int game_res_h = 480;

  // size of the window's framebuffer (framebuffer 0)
  // The renderer needs to know this to do an optimization to render directly to the window's
  // framebuffer when possible.
  int window_framebuffer_height = 0;
  int window_framebuffer_width = 0;

  // the part of the window that we should draw to. The rest is black. This value is determined by
  // logic inside of the game - it needs to know the desired aspect ratio.
  int draw_region_height = 0;
  int draw_region_width = 0;

  // display size of the scene when it renders below it (render scale < 1), 0 = game_res
  int display_w = 0;
  int display_h = 0;

  bool save_screenshot = false;
  bool quick_screenshot = false;
  bool internal_res_screenshot = false;
  std::string screenshot_path;

  float pmode_alp_register = 1.f;

  int brightness_contrast_color = 0;    // 0 - no change
  int brightness_contrast_alpha = 128;  // 128 - no change

  // when enabled, does a `glFinish()` after each major rendering pass. This blocks until the GPU
  // is done working, making it easier to profile GPU utilization.
  bool gpu_sync = false;

  // OpenXR frame (Jak 1): when set, the stereo path also copies each eye into these images
  xr::FrameTargets* xr = nullptr;
  // the chain was already drawn for an earlier display frame: draw it again for a fresh head
  // pose without advancing anything per game frame (frame_idx, VIF interrupts, wind, shadows)
  bool xr_repeat = false;
};

/*!
 * Main OpenGL renderer.
 * This handles the glClear and all game rendering, but not actual setup, synchronization or imgui
 * stuff.
 *
 * It is simply a collection of bucket renderers, and a few special case ones.
 */
class OpenGLRenderer {
 public:
  OpenGLRenderer(std::shared_ptr<TexturePool> texture_pool,
                 std::shared_ptr<Loader> loader,
                 GameVersion version);
  // TODO delete

  // rendering interface: takes the dma chain from the game, and some size/debug settings from
  // the graphics system.
  void render(DmaFollower dma, const RenderOptions& settings);

 private:
  void setup_frame(const RenderOptions& settings);
  void dispatch_buckets(DmaFollower dma, ScopedProfilerNode& prof, bool sync_after_buckets);
  void dispatch_buckets_stereo(DmaFollower dma,
                               ScopedProfilerNode& prof,
                               bool sync_after_buckets,
                               xr::FrameTargets* xr);
  void copy_eye_to_xr(const xr::EyeTarget& target, int src_x, int w, int h);
  void copy_eye_to_xr_cropped(const xr::FrameTargets& frame, int eye, int src_x, int w, int h);
  void dispatch_buckets_jak1(DmaFollower dma, ScopedProfilerNode& prof, bool sync_after_buckets);
  void dispatch_buckets_jak2(DmaFollower dma, ScopedProfilerNode& prof, bool sync_after_buckets);
  void dispatch_buckets_jak3(DmaFollower dma, ScopedProfilerNode& prof, bool sync_after_buckets);

  void do_pcrtc_effects(float alp,
                        int brightness_contrast_color,
                        int brightness_contrast_alpha,
                        SharedRenderState* render_state,
                        ScopedProfilerNode& prof,
                        bool direct_ok);
  void blit_display(ScopedProfilerNode& prof);
  void set_tonemap_uniforms(const Shader& shader, bool hdr_out);
  Fbo* hdr_scene_fbo();
  void save_hdr_scene(ScopedProfilerNode& prof, const std::string& path, bool window_png);
  Fbo* hdr_screenshot_source(ScopedProfilerNode& prof);
  void init_bucket_renderers_jak1();
  void init_bucket_renderers_jak2();
  void init_bucket_renderers_jak3();
  void draw_renderer_selection_window();
  void finish_screenshot(const std::string& output_name,
                         int px,
                         int py,
                         int x,
                         int y,
                         GLuint fbo,
                         int read_buffer,
                         bool quick_screenshot);
  template <typename T, typename U, class... Args>
  T* init_bucket_renderer(const std::string& name, BucketCategory cat, U id, Args&&... args) {
    auto renderer = std::make_unique<T>(name, (int)id, std::forward<Args>(args)...);
    T* ret = renderer.get();
    m_bucket_renderers.at((int)id) = std::move(renderer);
    m_bucket_categories.at((int)id) = cat;
    return ret;
  }

  const std::vector<GLuint>* anim_slot_array() {
    return m_texture_animator ? m_texture_animator->slots() : nullptr;
  }

  SharedRenderState m_render_state;
  Profiler m_profiler;
  SmallProfiler m_small_profiler;
  SubtitleEditor* m_subtitle_editor = nullptr;
  FiltersMenu m_filters_menu;

  std::shared_ptr<Merc2> m_merc2;
  std::shared_ptr<Generic2> m_generic2;
  std::shared_ptr<TextureAnimator> m_texture_animator;
  std::vector<std::unique_ptr<BucketRenderer>> m_bucket_renderers;
  std::vector<BucketCategory> m_bucket_categories;
  class BlitDisplays* m_blit_displays = nullptr;

  std::array<float, (int)BucketCategory::MAX_CATEGORIES> m_category_times;
  FullScreenDraw m_blackout_renderer;
  CollideMeshRenderer m_collide_renderer;
  SsaoDepth m_ssao_depth;
  SsaoDepth m_water_depth;  // opaque scene depth for the ocean shading
  SsaoRenderer m_ssao;
  SwampMist m_swamp_mist;
  SunDisc m_sun_disc;
  BloomRenderer m_bloom;
  SmaaRenderer m_smaa;
  Upscaler m_upscaler;
  int m_display_w = 0, m_display_h = 0;  // render scale < 1: the size the scene is upscaled to
  bool m_upscaled = false;               // this frame's scene was upscaled to m_upscaler.display()
  EmissiveLights m_emissive_lights;
  InspectRenderer m_inspect;
  std::unique_ptr<VistaRenderer> m_vista;
  std::unique_ptr<ActorShadows> m_actor_shadows;  // Jak 1

  float m_last_pmode_alp = 1.;
  bool m_enable_fast_blackout_loads = true;
  std::string m_renderer_filter = "";

  struct FboState {
    struct {
      Fbo window;          // provided by glfw
      Fbo render_buffer;   // temporary buffer to render to
      Fbo resolve_buffer;  // temporary buffer to resolve to
    } resources;

    Fbo* render_fbo = nullptr;  // the selected fbo from the three above to use for rendering
    bool hdr = false;           // render and resolve buffers are RGBA16F
  } m_fbo_state;
  Fbo m_hdr_ldr_fbo;                // tonemapped copy for internal-resolution screenshots
  GpuTimer m_pcrtc_timer{"pcrtc"};  // resolve + tonemap, with the SSAO GPU timing switch
  std::vector<perf::Group> m_perf_groups;  // per bucket, Jak 1 only
  Fbo m_stereo_fbo;                 // both eyes side by side, single-sampled
  bool m_stereo_hdr = false;
  GLuint m_xr_fbo = 0;  // draw target for copies into OpenXR swapchain images
  bool m_xr_repeat = false;
  float m_xr_logged_tan[2] = {0.f, 0.f};
  // what glq::SavedState copies were queried under (setup_frame invalidates on a change)
  std::array<s64, 14> m_glq_key{};
  std::array<u8, sizeof(GfxGlobalSettings)> m_glq_settings{};
  int m_glq_settings_logs = 0;

  GLuint screen_vao = 0;  // vertex array object for a screen-space draw
  GLuint screen_vbo = 0;  // vertex buffer object for a screen-space draw

  std::unique_ptr<BucketRenderer> m_jak2_eye_renderer;
  std::unique_ptr<BucketRenderer> m_jak3_eye_renderer;
  GameVersion m_version;
};
