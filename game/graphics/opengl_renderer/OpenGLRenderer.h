#pragma once

#include <chrono>
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
#include "game/graphics/opengl_renderer/MotionBlur.h"
#include "game/graphics/opengl_renderer/MotionVectors.h"
#include "game/graphics/opengl_renderer/PerfTimers.h"
#include "game/graphics/opengl_renderer/Profiler.h"
#include "game/graphics/opengl_renderer/Shader.h"
#include "game/graphics/opengl_renderer/SunDisc.h"
#include "game/graphics/opengl_renderer/SsaoDepth.h"
#include "game/graphics/opengl_renderer/SsaoRenderer.h"
#include "game/graphics/opengl_renderer/Swamp.h"
#include "game/graphics/opengl_renderer/ContactShadows.h"
#include "game/graphics/opengl_renderer/TextureAnimator.h"
#include "game/graphics/opengl_renderer/UpscalerStage.h"
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

  // FSR 3 runs this frame and the scene colour format; decided once per frame by the caller
  bool temporal_aa = false;
  bool dlss = false;  // the temporal pass is DLSS
  bool fsr4 = false;  // the temporal pass is FSR 4
  bool scene_hdr = false;

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
  /// The scene colour target is FP16 this frame (HDR target or HDR output, Jak 1).
  bool scene_hdr() const;
  /// Builds the FSR 3 programs for this colour format on first use; false if they failed.
  bool temporal_aa_ready(bool hdr) { return m_upscaler.fsr3_ready(hdr); }

 private:
  void setup_frame(const RenderOptions& settings);
  void dispatch_buckets(DmaFollower dma, ScopedProfilerNode& prof, bool sync_after_buckets);
  void dispatch_buckets_stereo(DmaFollower dma,
                               ScopedProfilerNode& prof,
                               bool sync_after_buckets,
                               xr::FrameTargets* xr);
  void copy_eye_to_xr(const xr::EyeTarget& target, GLuint src_fbo, int src_x, int w, int h);
  void save_xr_eyes(const xr::FrameTargets& frame, const std::string& path);
  void xr_seq_capture(const xr::FrameTargets& frame);
  void copy_eye_to_xr_cropped(const xr::FrameTargets& frame,
                              int eye,
                              GLuint src_fbo,
                              int src_x,
                              int w,
                              int h);
  void ui_layer_begin(xr::FrameTargets* xr);
  void ui_layer_finish(xr::FrameTargets& xr);
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
  void tonemap_to_sdr(const Fbo& src, Fbo& dst);
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
  SsaoDepth m_water_depth_near;  // re-taken before the near ocean (actors in it)
  SsaoRenderer m_ssao;
  SwampMist m_swamp_mist;
  ContactShadows m_contact_shadows;
  SunDisc m_sun_disc;
  BloomRenderer m_bloom;
  int m_display_w = 0, m_display_h = 0;  // render scale < 1: the size the upscaler reconstructs
  UpscalerStage m_upscaler;              // FSR 3.1 / DLSS / FSR 4 per eye slot, and its output
  bool m_probe_shot = false;  // SANDOVER_BG_PROBE: this frame saves a screenshot
  int m_probe_eye = 0;
  bool m_stereo_cut = false;             // eye 0's motion vectors cut this display frame
  EmissiveLights m_emissive_lights;
  motion_vectors::Renderer m_motion_vectors_eye[2];
  motion_vectors::Renderer& mvr() { return m_motion_vectors_eye[m_render_state.eye_slot & 1]; }
  motion_blur::Renderer m_motion_blur;
  bool m_mv_blackout = false;  // this frame's PCRTC alpha is 0 (load blackout)
  InspectRenderer m_inspect;
  std::unique_ptr<VistaRenderer> m_vista;
  std::unique_ptr<ActorShadows> m_actor_shadows;  // Jak 1

  float m_last_pmode_alp = 1.;
  bool m_enable_fast_blackout_loads = true;
  // pre-roll: after a blackout that loaded a level the screen stays black while frames still
  // pay first-use costs (driver pipeline builds); preroll_step()
  bool m_preroll_on = false;
  int m_preroll_calm = 0;
  int m_preroll_frames = 0;
  double m_preroll_calm_ms = 0;
  int m_blackout_levels = 0;
  std::chrono::steady_clock::time_point m_preroll_t;
  std::chrono::steady_clock::time_point m_preroll_cap_t;
  std::chrono::steady_clock::time_point m_frame_t;
  std::array<float, 32> m_calm_ring{};
  int m_calm_n = 0;
  int m_calm_i = 0;
  void preroll_step();
  double calm_median_ms() const;
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
  Fbo m_xr_sdr_fbo;     // both eyes tonemapped for the SDR swapchains (FP16 target on)
  Fbo m_ui_canvas[2];   // XR 2D layer drawn on black and on white (ui_canvas_draw)
  bool m_xr_repeat = false;
  float m_xr_logged_tan[2] = {0.f, 0.f};
  float m_xr_game_tan[2] = {0.f, 0.f};  // the drawn chain's projection half-tangents
  // XR frame sequence (pc-set-xr-fake-option 8): eye images copied on the GPU, saved at the end
  int m_xr_seq_serial = 0, m_xr_seq_n = 0, m_xr_seq_done = 0;
  std::vector<GLuint> m_xr_seq_tex;  // frame * 2 + eye
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
