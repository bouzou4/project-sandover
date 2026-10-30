/*!
 * @file opengl.cpp
 * Lower-level OpenGL interface. No actual rendering is performed here!
 */

#include "opengl.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <sstream>

#include "common/dma/dma_copy.h"
#include "common/global_profiler/GlobalProfiler.h"
#include "common/goal_constants.h"
#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "common/util/FrameLimiter.h"
#include "common/util/Timer.h"
#include "common/util/compress.h"

#include "game/graphics/bug_report.h"
#include "game/graphics/display.h"
#include "game/graphics/gfx.h"
#include "game/graphics/hdr/HdrPresent.h"
#include "game/graphics/opengl_renderer/D3d12Bridge.h"
#include "game/graphics/opengl_renderer/MotionBlur.h"
#include "game/graphics/opengl_renderer/MotionVectors.h"
#include "game/graphics/opengl_renderer/Neighbours.h"
#include "game/graphics/opengl_renderer/OpenGLRenderer.h"
#include "game/graphics/opengl_renderer/PerfTimers.h"
#include "game/graphics/opengl_renderer/debug_gui.h"
#include "game/graphics/screenshot.h"
#include "game/graphics/texture/TexturePool.h"
#include "game/graphics/xr/OpenXr.h"
#include "game/runtime.h"
#include "game/sce/libscf.h"
#include "game/system/hid/input_manager.h"
#include "game/system/hid/sdl_util.h"

#include "fmt/format.h"
#include "third-party/SDL/include/SDL3/SDL.h"
#include "third-party/SDL/include/SDL3/SDL_hints.h"
#include "third-party/SDL/include/SDL3/SDL_version.h"
#include "third-party/imgui/imgui.h"
#include "third-party/imgui/imgui_freetype.h"
#include "third-party/imgui/imgui_impl_opengl3.h"
#include "third-party/imgui/imgui_impl_sdl3.h"
#include "third-party/imgui/imgui_style.h"
#define STBI_WINDOWS_UTF8
#include "common/util/dialogs.h"
#include "common/util/string_util.h"

#include "third-party/stb_image/stb_image.h"

constexpr bool run_dma_copy = false;

constexpr PerGameVersion<int> fr3_level_count(jak1::LEVEL_TOTAL,
                                              jak2::LEVEL_TOTAL,
                                              jak3::LEVEL_TOTAL,
                                              jakx::LEVEL_TOTAL);

struct GraphicsData {
  // vsync
  std::mutex sync_mutex;
  std::condition_variable sync_cv;

  // dma chain transfer
  std::mutex dma_mutex;
  std::condition_variable dma_cv;
  u64 frame_idx = 0;
  u64 frame_idx_of_input_data = 0;
  bool has_data_to_render = false;
  FixedChunkDmaCopier dma_copier;
  // XR: the copier holds the last game frame's chain, drawn again every display frame
  bool xr_chain_valid = false;
  // the head view the game drew the pending / the held chain from
  Gfx::XrCullView xr_view_pending, xr_view_chain;

  // texture pool
  std::shared_ptr<TexturePool> texture_pool;

  std::shared_ptr<Loader> loader;

  // temporary opengl renderer
  OpenGLRenderer ogl_renderer;

  OpenGlDebugGui debug_gui;

  FrameLimiter frame_limiter;
  Timer engine_timer;
  // present-to-present intervals, logged every ~5 s
  Timer present_timer;
  double present_sum = 0, present_min = 1e9, present_max = 0;
  int present_count = 0;
  bool present_started = false;
  double last_engine_time = 1. / 60.;
  float pmode_alp = 1.f;

  std::string imgui_log_filename, imgui_filename;
  GameVersion version;

  GraphicsData(GameVersion version)
      : dma_copier(EE_MAIN_MEM_SIZE),
        texture_pool(std::make_shared<TexturePool>(version)),
        loader(std::make_shared<Loader>(
            file_util::get_jak_project_dir() / "out" / game_version_names[version] / "fr3",
            fr3_level_count[version])),
        ogl_renderer(texture_pool, loader, version),
        debug_gui(),
        version(version) {
    loader->set_load_unbake(version == GameVersion::Jak1);
  }
};

std::unique_ptr<GraphicsData> g_gfx_data;

static bool gl_inited = false;
static int gl_init(GfxGlobalSettings& settings) {
  prof().instant_event("ROOT");
  Timer gl_init_timer;
  // Initialize SDL
  {
    auto p = scoped_prof("startup::sdl::init_sdl");
    // remove SDL garbage from hooking signal handler.
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
      sdl_util::log_error("Could not initialize SDL, exiting");
      dialogs::create_error_message_dialog("Critical Error Encountered",
                                           "Could not initialize SDL, exiting");
      return 1;
    }
  }

  {
    auto p = scoped_prof("startup::sdl::get_version_info");

    auto compiled_sdl_version = SDL_VERSION;
    auto linked_sdl_version = SDL_GetVersion();
    lg::info("SDL Initialized, compiled with version - {} | linked with version - {}",
             compiled_sdl_version, linked_sdl_version);
  }

  {
    auto p = scoped_prof("startup::sdl::set_gl_attributes");

    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    if (settings.debug) {
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);
    } else {
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
#ifndef __APPLE__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
#endif
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
  }
  lg::info("gl init took {:.3f}s\n", gl_init_timer.getSeconds());
  return 0;
}

static void gl_exit() {
  g_gfx_data.reset();
  gl_inited = false;
}

static void init_imgui(SDL_Window* window,
                       SDL_GLContext gl_context,
                       const std::string& glsl_version) {
  // check that version of the library is okay
  IMGUI_CHECKVERSION();

  // this does initialization for stuff like the font data
  ImGui::CreateContext();

  // Init ImGui settings
  g_gfx_data->imgui_filename = file_util::get_file_path({"imgui.ini"});
  g_gfx_data->imgui_log_filename = file_util::get_file_path({"imgui_log.txt"});
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
  io.IniFilename = g_gfx_data->imgui_filename.c_str();
  io.LogFilename = g_gfx_data->imgui_log_filename.c_str();

  if (Gfx::g_debug_settings.alternate_style) {
    ImGui::applyAlternateStyle();
  }

  ImGui::applyFontStyle();

  // set up to get inputs for this window
  ImGui_ImplSDL3_InitForOpenGL(window, gl_context);

  // NOTE: imgui's setup calls functions that may fail intentionally, and attempts to disable error
  // reporting so these errors are invisible. But it does not work, and some weird X11 default
  // cursor error is set here that we clear.
  SDL_ClearError();

  // set up the renderer
  ImGui_ImplOpenGL3_Init(glsl_version.c_str());
}

static std::shared_ptr<GfxDisplay> gl_make_display(int width,
                                                   int height,
                                                   const char* title,
                                                   GfxGlobalSettings& /*settings*/,
                                                   GameVersion game_version,
                                                   bool is_main) {
  // Setup the window
  prof().instant_event("ROOT");
  prof().begin_event("startup::sdl::create_window");
  SDL_Window* window =
      SDL_CreateWindow(title, width, height,
                       SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  prof().end_event();
  if (!window) {
    sdl_util::log_error("gl_make_display failed - Could not create display window");
    dialogs::create_error_message_dialog(
        "Critical Error Encountered",
        "Unable to create OpenGL window.\nOpenGOAL requires OpenGL 4.3.\nEnsure your GPU "
        "supports this and your drivers are up to date.");
    return NULL;
  }

  // Make an OpenGL Context
  prof().begin_event("startup::sdl::create_context");
  SDL_GLContext gl_context = SDL_GL_CreateContext(window);
  prof().end_event();
  if (!gl_context) {
    sdl_util::log_error("gl_make_display failed - Could not create OpenGL Context");
    dialogs::create_error_message_dialog(
        "Critical Error Encountered",
        "Unable to create OpenGL context.\nOpenGOAL requires OpenGL 4.3.\nEnsure your GPU "
        "supports this and your drivers are up to date.");
    return NULL;
  }

  {
    auto p = scoped_prof("startup::sdl::assign_context");
    if (!SDL_GL_MakeCurrent(window, gl_context)) {
      sdl_util::log_error("gl_make_display failed - Could not associated context with window");
      dialogs::create_error_message_dialog("Critical Error Encountered",
                                           "Unable to create OpenGL window with context.\nOpenGOAL "
                                           "requires OpenGL 4.3.\nEnsure your GPU "
                                           "supports this and your drivers are up to date.");
      return NULL;
    }
  }

  if (!gl_inited) {
    {
      auto p = scoped_prof("startup::sdl::glad_init");
      gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress);
      if (!gladLoadGL()) {
        lg::error("GL init fail");
        dialogs::create_error_message_dialog("Critical Error Encountered",
                                             "Unable to initialize OpenGL API.\nOpenGOAL requires "
                                             "OpenGL 4.3.\nEnsure your GPU "
                                             "supports this and your drivers are up to date.");
        return NULL;
      }
    }
    {
      auto p = scoped_prof("startup::sdl::gfx_data_init");
      g_gfx_data = std::make_unique<GraphicsData>(game_version);
    }
    gl_inited = true;
    const char* gl_version = (const char*)glGetString(GL_VERSION);
    lg::info("OpenGL initialized - v{}.{} | Renderer: {}", GLVersion.major, GLVersion.minor,
             gl_version);
  }

  {
    auto p = scoped_prof("startup::sdl::window_extras");
    float dpi = 1.0f;
    float display_scale = SDL_GetWindowDisplayScale(window);
    if (display_scale > 0.0) {
      dpi = display_scale * 96.0f;
    }

    // Setup Window Icon
    const auto image_path = file_util::get_jak_project_dir() / "game" / "assets" /
                            version_to_game_name(game_version) /
                            (dpi == 1.0f ? "app64.png" : "app256.png");
    if (fs::exists(image_path)) {
      int icon_width;
      int icon_height;

      auto icon_data = stbi_load(image_path.string().c_str(), &icon_width, &icon_height, nullptr,
                                 STBI_rgb_alpha);
      if (icon_data) {
        SDL_Surface* icon_surf = SDL_CreateSurfaceFrom(
            icon_width, icon_height, SDL_PIXELFORMAT_RGBA32, (void*)icon_data, 4 * icon_width);
        if (!icon_surf) {
          sdl_util::log_error("unable to generate surface from app icon data");
        } else {
          SDL_SetWindowIcon(window, icon_surf);
          SDL_DestroySurface(icon_surf);
        }
        stbi_image_free(icon_data);
      } else {
        lg::error("Could not load icon for OpenGL window, couldn't load image data");
      }
    } else {
      lg::error("Could not load icon for OpenGL window, {} does not exist", image_path.string());
    }
  }

  prof().begin_event("startup::sdl::create_GLDisplay");
  auto display = std::make_shared<GLDisplay>(window, gl_context, is_main);
  display->set_imgui_visible(Gfx::g_debug_settings.show_imgui);
  prof().end_event();

  {
    auto p = scoped_prof("startup::sdl::init_imgui");
    // setup imgui
#ifdef __APPLE__
    init_imgui(window, gl_context, "#version 410");
#else
    init_imgui(window, gl_context, "#version 430");
#endif
  }

  return std::static_pointer_cast<GfxDisplay>(display);
}

GLDisplay::GLDisplay(SDL_Window* window, SDL_GLContext gl_context, bool is_main)
    : m_window(window),
      m_gl_context(gl_context),
      m_display_manager(std::make_shared<DisplayManager>(window)),
      m_input_manager(std::make_shared<InputManager>(window)) {
  m_main = is_main;
  m_display_manager->set_input_manager(m_input_manager);
  // Register commands
  m_input_manager->register_command(
      CommandBinding::Source::KEYBOARD,
      CommandBinding(Gfx::g_debug_settings.hide_imgui_key, [&](const SDL_Event& event) {
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.repeat == 0) {
          if (!Gfx::g_debug_settings.ignore_hide_imgui) {
            set_imgui_visible(!is_imgui_visible());
          }
        }
      }));
  ;
  m_input_manager->register_command(
      CommandBinding::Source::KEYBOARD,
      CommandBinding(SDLK_F2, [&]() { m_take_screenshot_next_frame = true; }));

  if (g_game_version == GameVersion::Jak1) {
    // XR recentre; the default game bindings use no F-keys
    m_input_manager->register_command(
        CommandBinding::Source::KEYBOARD, CommandBinding(SDLK_F6, [](const SDL_Event& event) {
          if (event.type == SDL_EVENT_KEY_DOWN && event.key.repeat == 0) {
            Gfx::g_global_settings.xr_recentre_requests++;
          }
        }));
  }

  if (g_game_version == GameVersion::Jak1) {
    m_input_manager->register_command(
        CommandBinding::Source::KEYBOARD, CommandBinding(SDLK_F7, [](const SDL_Event& event) {
          if (event.type == SDL_EVENT_KEY_DOWN && event.key.repeat == 0) {
            bug_report::request(bug_report::KEY);
          }
        }));
  }

  if (g_game_version == GameVersion::Jak1 && snap::enabled()) {
    for (auto [key, ev] : {std::pair{SDLK_F9, snap::SHOT}, std::pair{SDLK_F10, snap::NEXT},
                           std::pair{SDLK_F8, snap::PREV}}) {
      m_input_manager->register_command(
          CommandBinding::Source::KEYBOARD, CommandBinding(key, [ev](const SDL_Event& event) {
            if (event.type == SDL_EVENT_KEY_DOWN && event.key.repeat == 0) {
              snap::on_key(ev);
            }
          }));
    }
  }

  const auto& bind = Gfx::g_debug_settings.toggle_fullscreen_key;

  m_input_manager->register_command(
      CommandBinding::Source::KEYBOARD,
      CommandBinding(bind.key, bind.modifiers, [&](const SDL_Event& event) {
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.repeat == 0 &&
            bind.modifiers.has_necessary_modifiers(SDL_GetModState())) {
          m_display_manager->toggle_display_mode();
        }
      }));
}

void GLDisplay::init_splash() {
  if (m_splash_program)
    return;
  if (!Gfx::g_splash.ready.load())
    return;

  auto shader_folder = "game/graphics/opengl_renderer/shaders";
  auto vert_src =
      file_util::read_text_file(file_util::get_file_path({shader_folder, "splash.vert"}));
  auto frag_src =
      file_util::read_text_file(file_util::get_file_path({shader_folder, "splash.frag"}));

  constexpr int len = 1024;
  GLint compile_ok;
  char err[len];

  auto compile_shader = [](GLenum type, const char* src, GLint& compile_ok, char* err) -> GLuint {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &compile_ok);
    if (!compile_ok) {
      glGetShaderInfoLog(s, len, nullptr, err);
      lg::error("splash shader compile failed: {}\n", err);
      glDeleteShader(s);
      return 0;
    }
    return s;
  };

  GLuint vs = compile_shader(GL_VERTEX_SHADER, vert_src.c_str(), compile_ok, err);
  GLuint fs = compile_shader(GL_FRAGMENT_SHADER, frag_src.c_str(), compile_ok, err);
  if (!vs || !fs) {
    glDeleteShader(vs);
    glDeleteShader(fs);
    return;
  }

  m_splash_program = glCreateProgram();
  glAttachShader(m_splash_program, vs);
  glAttachShader(m_splash_program, fs);
  glLinkProgram(m_splash_program);
  glDeleteShader(vs);
  glDeleteShader(fs);

  glGetProgramiv(m_splash_program, GL_LINK_STATUS, &compile_ok);
  if (!compile_ok) {
    glGetProgramInfoLog(m_splash_program, len, nullptr, err);
    lg::error("Failed to link splash shader:\n{}", err);
    glDeleteProgram(m_splash_program);
    m_splash_program = 0;
    return;
  }

  if (!Gfx::g_splash.data.empty()) {
    glGenTextures(1, &m_splash_texture);
    glBindTexture(GL_TEXTURE_2D, m_splash_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, Gfx::g_splash.width, Gfx::g_splash.height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, Gfx::g_splash.data.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);
    Gfx::g_splash.data.clear();
    Gfx::g_splash.data.shrink_to_fit();
  }

  glGenVertexArrays(1, &m_splash_vao);
}

void GLDisplay::draw_splash(int fb_w, int fb_h) {
  if (!m_splash_program || !m_splash_texture)
    return;
  glBindFramebuffer(GL_FRAMEBUFFER, Gfx::window_fbo());
  glClearColor(0.f, 0.f, 0.f, 1.f);
  glClear(GL_COLOR_BUFFER_BIT);
  glViewport(0, 0, fb_w, fb_h);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glUseProgram(m_splash_program);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, m_splash_texture);
  glUniform1i(glGetUniformLocation(m_splash_program, "splash_tex"), 0);
  glUniform2f(glGetUniformLocation(m_splash_program, "u_res"), fb_w, fb_h);
  glUniform2f(glGetUniformLocation(m_splash_program, "u_tex"), Gfx::g_splash.width,
              Gfx::g_splash.height);
  glBindVertexArray(m_splash_vao);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glBindVertexArray(0);
}

GLDisplay::~GLDisplay() {
  if (m_splash_texture)
    glDeleteTextures(1, &m_splash_texture);
  if (m_splash_program)
    glDeleteProgram(m_splash_program);
  if (m_splash_vao)
    glDeleteVertexArrays(1, &m_splash_vao);
  // Cleanup ImGUI
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  xr::shutdown();
  hdr::shutdown();
  // the renderer and the D3D12 bridge go while the GL context is current
  if (m_main) {
    gl_exit();
    D3d12Bridge::get().shutdown();
  }
  // Cleanup SDL
  SDL_GL_DestroyContext(m_gl_context);
  SDL_DestroyWindow(m_window);
  // cleanup SDL related sub-systems before we quit SDL
  if (m_display_manager) {
    m_display_manager.reset();
  }
  if (m_input_manager) {
    m_input_manager.reset();
  }
  // now quit SDL
  SDL_Quit();
}

namespace {
// the game's view covers the head turn possible while its pose ages: a brisk turn rate times the
// age (pose located -> last display of the chain drawn from it), within these bounds
constexpr float kXrTurnDegPerS = 300.f;
constexpr float kXrMarginMinDeg = 10.f, kXrMarginMaxDeg = 40.f;
constexpr float kXrMaxHalfAngle = 1.396f;  // 80 degrees
constexpr float kXrPoseAgeMaxMs = 200.f;   // longer: a stall (load, screenshot), skipped
constexpr double kXrEmulatedPeriodS = 1. / 90.;  // no runtime: the PSVR2's rate
// the margin is held: it sets the game's projection, which must not move between frames. It
// rises at once to a larger want; it falls only after 2 s at 6 deg (20 ms) too wide, 5 deg a step
constexpr float kXrMarginStartDeg = 25.f;
constexpr float kXrMarginUpDeg = 1.5f;
constexpr double kXrMarginUpWaitS = 0.5;
constexpr float kXrMarginBandDeg = 6.f;
constexpr double kXrMarginSustainS = 2.;
constexpr float kXrMarginStepDeg = 5.f;
constexpr double kXrMarginCooldownS = 5.;  // down steps of 5 deg: at most 1 deg/s
constexpr double kXrMarginMaxDtS = 0.1;    // a stall counts as one short frame
float g_xr_pose_age_ms = 50.f;             // running mean of each chain's oldest display
float g_xr_margin_deg = kXrMarginStartDeg;
double g_xr_margin_up_s = 0., g_xr_margin_down_s = 0.;  // time the want has been above / below
double g_xr_margin_last_s = -1., g_xr_margin_down_at = -1e9;
bool g_xr_margin_logged = false;
double g_xr_age_stamp = -1.;               // the chain being aged and its latest display's age
float g_xr_age_last_ms = -1.f;
// pose age split, summed over chains for the 5 s log: sample -> game read -> chain sent -> taken
// by the render thread -> its last display began; then the display period
struct XrAgeSplit {
  double read = 0., send = 0., take = 0., shown = 0., age = 0.;
  int n = 0;
  double last_log_s = -1.;
} g_xr_age_split;
double g_xr_age_taken_s = 0., g_xr_age_last_begin_s = 0., g_xr_age_period_s = 0.;
Gfx::XrCullView g_xr_age_view;

double steady_s() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void note_xr_pose_age(const Gfx::XrCullView& chain_view, double period_s) {
  if (!chain_view.valid) {
    return;
  }
  const double now = steady_s();
  if (chain_view.stamp_s != g_xr_age_stamp) {
    // a new chain: the previous one's last display was its worst case
    if (g_xr_age_last_ms >= 0.f && g_xr_age_last_ms <= kXrPoseAgeMaxMs) {
      g_xr_pose_age_ms += 0.05f * (g_xr_age_last_ms - g_xr_pose_age_ms);
      const auto& v = g_xr_age_view;
      auto& a = g_xr_age_split;
      if (v.handed_s > 0. && v.sent_s > 0.) {
        a.read += v.handed_s - v.stamp_s;
        a.send += v.sent_s - v.handed_s;
        a.take += g_xr_age_taken_s - v.sent_s;
        a.shown += g_xr_age_last_begin_s - g_xr_age_taken_s;
        a.age += g_xr_age_last_ms;
        a.n++;
      }
      if (a.last_log_s < 0.) {
        a.last_log_s = now;
      } else if (now - a.last_log_s >= 5. && a.n > 0) {
        const double k = 1000. / a.n;
        lg::info(
            "[xr] pose age {:.1f} ms over {} chains: sample -> game read {:.1f}, -> chain sent "
            "{:.1f}, -> taken {:.1f}, -> last display {:.1f}, + period {:.1f}",
            a.age / a.n, a.n, a.read * k, a.send * k, a.take * k, a.shown * k,
            g_xr_age_period_s * 1000.);
        a = XrAgeSplit{};
        a.last_log_s = now;
      }
    }
    g_xr_age_stamp = chain_view.stamp_s;
    g_xr_age_view = chain_view;
    g_xr_age_taken_s = now;
  }
  if (period_s <= 0.) {
    period_s = kXrEmulatedPeriodS;
  }
  g_xr_age_last_begin_s = now;
  g_xr_age_period_s = period_s;
  g_xr_age_last_ms = (float)((now - chain_view.stamp_s + period_s) * 1000.);
}

float held_xr_margin_deg(double now) {
  // SANDOVER_XR_MARGIN_DEG=N (dev): a fixed margin, for A/B
  static const char* fixed = std::getenv("SANDOVER_XR_MARGIN_DEG");
  if (fixed) {
    if (!g_xr_margin_logged) {
      g_xr_margin_logged = true;
      lg::info("[xr] view margin {:.1f} deg, fixed (SANDOVER_XR_MARGIN_DEG)", std::atof(fixed));
    }
    return (float)std::atof(fixed);
  }
  const double dt =
      g_xr_margin_last_s < 0. ? 0. : std::clamp(now - g_xr_margin_last_s, 0., kXrMarginMaxDtS);
  g_xr_margin_last_s = now;
  const float want =
      std::clamp(kXrTurnDegPerS * g_xr_pose_age_ms * 0.001f, kXrMarginMinDeg, kXrMarginMaxDeg);
  const float off = want - g_xr_margin_deg;
  g_xr_margin_up_s = off > kXrMarginUpDeg ? g_xr_margin_up_s + dt : 0.;
  g_xr_margin_down_s = off < -kXrMarginBandDeg ? g_xr_margin_down_s + dt : 0.;
  if (g_xr_margin_up_s >= kXrMarginUpWaitS) {
    g_xr_margin_deg = std::ceil(want);
    g_xr_margin_up_s = 0.;
    g_xr_margin_logged = false;
  } else if (g_xr_margin_down_s >= kXrMarginSustainS &&
             now - g_xr_margin_down_at >= kXrMarginCooldownS) {
    g_xr_margin_deg += std::max(std::round(off), -kXrMarginStepDeg);
    g_xr_margin_down_at = now;
    g_xr_margin_down_s = 0.;
    g_xr_margin_logged = false;
  }
  if (!g_xr_margin_logged) {
    g_xr_margin_logged = true;
    lg::info("[xr] view margin {:.1f} deg, held (pose age {:.1f} ms)", g_xr_margin_deg,
             g_xr_pose_age_ms);
  }
  return g_xr_margin_deg;
}

void publish_xr_cull_view(const xr::FrameTargets& t, float world_scale) {
  Gfx::XrCullView v;
  v.valid = true;
  v.stamp_s = steady_s();
  const float units_per_m = 4096.f / std::max(world_scale, 0.001f);
  std::memcpy(v.rot, t.eye[0].rot, sizeof(v.rot));
  for (int k = 0; k < 3; k++) {
    v.t[k] = 0.5f * (t.eye[0].pos_m[k] + t.eye[1].pos_m[k]) * units_per_m;
  }
  const float margin = held_xr_margin_deg(v.stamp_s) * 0.01745329252f;
  v.tan_x = std::tan(std::min(std::atan(t.render_tan_x) + margin, kXrMaxHalfAngle));
  v.tan_y = std::tan(std::min(std::atan(t.render_tan_y) + margin, kXrMaxHalfAngle));
  Gfx::set_xr_cull_view(v);
}
}  // namespace

void render_game_frame(int game_width,
                       int game_height,
                       int window_fb_width,
                       int window_fb_height,
                       int draw_region_width,
                       int draw_region_height,
                       int msaa_samples,
                       int brightness_contrast_color,
                       int brightness_contrast_alpha,
                       bool take_screenshot,
                       bool xr_frame,
                       xr::FrameTargets* xr_targets) {
  // wait for a copied chain.
  bool got_chain = false;
  bool xr_fresh = false;
  if (xr_frame) {
    // XR display frame: take a new chain if the game sent one, never wait for it. The copy
    // frees the game's buffer at once; the last copy is drawn again until the next arrives.
    std::unique_lock<std::mutex> lock(g_gfx_data->dma_mutex);
    // SANDOVER_XR_REDRAWS=N (dev): redraw each chain N more times before taking the next
    static const int redraws = std::getenv("SANDOVER_XR_REDRAWS")
                                   ? std::atoi(std::getenv("SANDOVER_XR_REDRAWS"))
                                   : 0;
    static int redraws_left = 0;
    const bool hold = redraws > 0 && redraws_left-- > 0;
    if (g_gfx_data->has_data_to_render && !hold) {
      redraws_left = redraws;
      auto p = scoped_prof("xr-copy-dma");
      Timer copy_timer;
      g_gfx_data->dma_copier.run(g_gfx_data->dma_copier.get_last_input_data(),
                                 g_gfx_data->dma_copier.get_last_input_offset());
      perf::cpu_copy_dma(copy_timer.getMs());
      g_gfx_data->frame_idx_of_input_data = g_gfx_data->frame_idx;
      g_gfx_data->xr_chain_valid = true;
      g_gfx_data->xr_view_chain = g_gfx_data->xr_view_pending;
      g_gfx_data->engine_timer.start();
      g_gfx_data->has_data_to_render = false;
      g_gfx_data->sync_cv.notify_all();
      xr_fresh = true;
    }
    got_chain = xr_targets && g_gfx_data->xr_chain_valid;
  } else {
    g_gfx_data->xr_chain_valid = false;
    auto p = scoped_prof("wait-for-dma");
    Timer wait_timer;
    std::unique_lock<std::mutex> lock(g_gfx_data->dma_mutex);
    // there's a timeout here, so imgui can still be responsive even if we don't render anything
    got_chain = g_gfx_data->dma_cv.wait_for(lock, std::chrono::milliseconds(40),
                                            [=] { return g_gfx_data->has_data_to_render; });
    perf::cpu_wait_dma(wait_timer.getMs());
  }
  // render that chain.
  if (got_chain) {
    if (!xr_frame) {
      g_gfx_data->frame_idx_of_input_data = g_gfx_data->frame_idx;
    }
    RenderOptions options;
    options.game_res_w = game_width;
    options.game_res_h = game_height;
    options.window_framebuffer_width = window_fb_width;
    options.window_framebuffer_height = window_fb_height;
    options.draw_region_width = draw_region_width;
    options.draw_region_height = draw_region_height;
    options.msaa_samples = msaa_samples;
    options.brightness_contrast_color = brightness_contrast_color;
    options.brightness_contrast_alpha = brightness_contrast_alpha;
    options.draw_render_debug_window = g_gfx_data->debug_gui.should_draw_render_debug();
    options.draw_profiler_window = g_gfx_data->debug_gui.should_draw_profiler();
    options.draw_loader_window = g_gfx_data->debug_gui.should_draw_loader_menu();
    options.draw_subtitle_editor_window = g_gfx_data->debug_gui.should_draw_subtitle_editor();
    options.draw_filters_window = g_gfx_data->debug_gui.should_draw_filters_menu();
    options.save_screenshot = false;
    options.quick_screenshot = false;
    options.internal_res_screenshot = false;
    options.gpu_sync = g_gfx_data->debug_gui.should_gl_finish();
    // the eye frame: the runtime's recommended size times the render scale (VR settings profile)
    auto set_xr_eye_size = [&]() {
      const float rs = Gfx::g_global_settings.render_scale;
      options.game_res_w = xr_targets->render_w;
      options.game_res_h = xr_targets->render_h;
      if (rs < 0.999f) {
        options.display_w = xr_targets->render_w;
        options.display_h = xr_targets->render_h;
        options.game_res_w = std::max(64, (int)std::lround(xr_targets->render_w * rs));
        options.game_res_h = std::max(64, (int)std::lround(xr_targets->render_h * rs));
      }
    };
    if (xr_targets) {
      // eyes render at the runtime's recommended size (tracked: one frame that covers both
      // eye frustums); the window gets the side-by-side mirror
      options.xr = xr_targets;
      const auto& hv = g_gfx_data->xr_view_chain;
      note_xr_pose_age(hv, xr_targets->display_period_s);
      xr_targets->chain_head = hv.valid;
      std::memcpy(xr_targets->head_rot, hv.rot, sizeof(hv.rot));
      std::memcpy(xr_targets->head_t, hv.t, sizeof(hv.t));
      xr_targets->view_tan_x = hv.valid ? hv.tan_x : 0.f;
      xr_targets->view_tan_y = hv.valid ? hv.tan_y : 0.f;
      xr_targets->game_tan_x = hv.valid ? hv.game_tan_x : 0.f;
      xr_targets->game_tan_y = hv.valid ? hv.game_tan_y : 0.f;
      options.xr_repeat = !xr_fresh;
      set_xr_eye_size();
    }

    if (take_screenshot) {
      options.save_screenshot = true;
      options.quick_screenshot = true;
      options.screenshot_path = file_util::make_screenshot_filepath(g_game_version);
    }
    // note : it's important we call get_screenshot_flag first because it modifies state
    if (g_gfx_data->debug_gui.get_screenshot_flag() || g_want_screenshot) {
      g_want_screenshot = false;
      options.save_screenshot = true;
      options.internal_res_screenshot = true;
      options.game_res_w = g_screen_shot_settings->width;
      options.game_res_h = g_screen_shot_settings->height;
      options.window_framebuffer_width = options.game_res_w;
      options.window_framebuffer_height = options.game_res_h;
      options.draw_region_width = options.game_res_w;
      options.draw_region_height = options.game_res_h;
      options.msaa_samples = g_screen_shot_settings->msaa;
      options.screenshot_path =
          file_util::make_screenshot_filepath(g_game_version, get_screen_shot_name());
    }

    if (xr_targets) {
      // a screenshot keeps the session's eye size (scaled): no target re-creation
      set_xr_eye_size();
      options.msaa_samples = msaa_samples;
    }

    g_burst_frame = -1;
    if (!options.save_screenshot && g_screenshot_burst.load() > 0) {
      static int burst_idx = 0;
      static std::string burst_base;
      if (burst_idx == 0) {
        burst_base = get_screen_shot_name();
      }
      options.save_screenshot = true;
      options.internal_res_screenshot = true;
      options.game_res_w = g_screen_shot_settings->width;
      options.game_res_h = g_screen_shot_settings->height;
      options.window_framebuffer_width = options.game_res_w;
      options.window_framebuffer_height = options.game_res_h;
      options.draw_region_width = options.game_res_w;
      options.draw_region_height = options.game_res_h;
      options.msaa_samples = g_screen_shot_settings->msaa;
      options.screenshot_path = file_util::make_screenshot_filepath(
          g_game_version, fmt::format("{}-f{:02d}", burst_base, burst_idx));
      g_burst_frame = burst_idx++;
      if (g_screenshot_burst.fetch_sub(1) <= 1) {
        burst_idx = 0;
      }
    }

    if (auto snap_path = snap::take_pending_path(); !snap_path.empty()) {
      if (options.save_screenshot) {
        lg::warn("snapshot {} skipped: another screenshot this frame", snap_path);
      } else {
        options.save_screenshot = true;
        options.screenshot_path = snap_path;
      }
    }

    if (!options.save_screenshot) {
      if (auto bug_path = bug_report::take_pending_path(); !bug_path.empty()) {
        options.save_screenshot = true;
        options.screenshot_path = bug_path;
      }
    }

    options.draw_small_profiler_window =
        g_gfx_data->debug_gui.master_enable && g_gfx_data->debug_gui.small_profiler;
    options.pmode_alp_register = g_gfx_data->pmode_alp;

    // same test as OpenGLRenderer::render's stereo_active: FSR 3.1 and FSR 4 run per eye there
    const bool stereo = g_game_version == GameVersion::Jak1 &&
                        (Gfx::g_global_settings.stereo_enable || options.xr);
    auto& renderer = g_gfx_data->ogl_renderer;
    options.scene_hdr = renderer.scene_hdr();
    // decided once: GOAL's pc-set-aa may change the mode while this frame renders
    options.dlss = g_game_version == GameVersion::Jak1 && !stereo && Gfx::dlss_on();
    options.fsr4 = g_game_version == GameVersion::Jak1 && Gfx::fsr4_on();
    // stereo: DLSS does not run per eye; FSR 3.1 takes its place there
    const bool fsr3 = Gfx::fsr3_on() || (stereo && Gfx::dlss_on());
    options.temporal_aa =
        g_game_version == GameVersion::Jak1 && (options.dlss || options.fsr4 || fsr3);
    if (options.temporal_aa && !options.dlss && !options.fsr4 &&
        !renderer.temporal_aa_ready(options.scene_hdr)) {
      // the programs did not build: vanilla rendering (pc-temporal-aa-state reports 0)
      Gfx::g_fsr3_supported = false;
      options.temporal_aa = false;
    }
    if (options.temporal_aa) {
      options.msaa_samples = 1;
      // render scale: the scene at a fraction of the size FSR 3 reconstructs (XR: set_xr_eye_size)
      const float rs = Gfx::g_global_settings.render_scale;
      if (rs < 0.999f && !options.xr) {
        options.display_w = options.game_res_w;
        options.display_h = options.game_res_h;
        options.game_res_w = std::max(64, (int)std::lround(options.game_res_w * rs));
        options.game_res_h = std::max(64, (int)std::lround(options.game_res_h * rs));
      }
    }
    GLint msaa_max;
    glGetIntegerv(GL_MAX_SAMPLES, &msaa_max);
    if (options.msaa_samples > msaa_max) {
      options.msaa_samples = msaa_max;
    }

    Timer render_timer;
    if (xr_frame) {
      auto p = scoped_prof("ogl-render");
      auto& chain = g_gfx_data->dma_copier.get_last_result();
      g_gfx_data->ogl_renderer.render(DmaFollower(chain.data.data(), chain.start_offset), options);
    } else if constexpr (run_dma_copy) {
      auto& chain = g_gfx_data->dma_copier.get_last_result();
      g_gfx_data->ogl_renderer.render(DmaFollower(chain.data.data(), chain.start_offset), options);
    } else {
      auto p = scoped_prof("ogl-render");
      g_gfx_data->ogl_renderer.render(DmaFollower(g_gfx_data->dma_copier.get_last_input_data(),
                                                  g_gfx_data->dma_copier.get_last_input_offset()),
                                      options);
    }
    perf::cpu_render(render_timer.getMs());
  }

  // before vsync, mark the chain as rendered (an XR frame released it when it copied it).
  if (!xr_frame) {
    // should be fine to remove this mutex if the game actually waits for vsync to call
    // send_chain again. but let's be safe for now.
    std::unique_lock<std::mutex> lock(g_gfx_data->dma_mutex);
    g_gfx_data->engine_timer.start();
    g_gfx_data->has_data_to_render = false;
    g_gfx_data->sync_cv.notify_all();
  }
}

void GLDisplay::process_sdl_events() {
  SDL_Event evt;
  while (SDL_PollEvent(&evt) != 0) {
    if (evt.type == SDL_EVENT_QUIT) {
      m_should_quit = true;
    }
    {
      auto p = scoped_prof("sdl-display-manager");
      m_display_manager->process_sdl_event(evt);
    }
    if (!m_should_quit) {
      {
        auto p = scoped_prof("imgui-sdl-process");
        ImGui_ImplSDL3_ProcessEvent(&evt);
      }
    }
    {
      auto p = scoped_prof("sdl-input-monitor-process-event");
      m_input_manager->process_sdl_event(evt);
    }
  }
}

/*!
 * 60 Hz game clock for XR display frames: true when a game vsync is due. Starts over when the
 * display falls more than a few ticks behind.
 */
bool GLDisplay::xr_game_tick() {
  using namespace std::chrono;
  constexpr auto period =
      duration_cast<steady_clock::duration>(duration<double>(1. / Gfx::kXrGameHz));
  const auto now = steady_clock::now();
  if (!m_xr_tick_started || now - m_xr_next_tick > 3 * period) {
    m_xr_tick_started = true;
    m_xr_next_tick = now;
  }
  if (now < m_xr_next_tick) {
    return false;
  }
  m_xr_next_tick += period;
  return true;
}

namespace {
int swap_interval() {
  int i = -9;
  SDL_GL_GetSwapInterval(&i);
  return i;
}

void log_present_interval(const char* path) {
  auto& g = *g_gfx_data;
  const double dt = g.present_timer.getSeconds();
  g.present_timer.start();
  if (!g.present_started) {
    g.present_started = true;  // the first window starts at the first present, not at boot
    return;
  }
  g.present_sum += dt;
  g.present_min = std::min(g.present_min, dt);
  g.present_max = std::max(g.present_max, dt);
  if (++g.present_count >= 30 && g.present_sum >= 5.0) {
    const double avg = g.present_sum / g.present_count;
    lg::info(
        "present: {} frames in {:.1f} s, avg {:.2f} ms ({:.1f} fps), min {:.2f} max {:.2f} ms, "
        "swap interval {}, {}",
        g.present_count, g.present_sum, avg * 1e3, 1.0 / avg, g.present_min * 1e3,
        g.present_max * 1e3, swap_interval(),
        std::strcmp(path, "hdr dxgi") ? std::string(path)
                                      : fmt::format("{} ({})", path, hdr::present_tag()));
    g.present_sum = g.present_max = 0;
    g.present_min = 1e9;
    g.present_count = 0;
  }
}
}  // namespace

/*!
 * Main function called to render graphics frames. This is called in a loop.
 */
void GLDisplay::render() {
  // Before we process the current frames SDL events we for keyboard/mouse button inputs.
  //
  // This technically means that keyboard/mouse button inputs will be a frame behind but the
  // event-based code is limiting (there aren't enough events to achieve a totally stateless
  // approach). Binding handling is still taken care of by the event code though.
  {
    auto p = scoped_prof("sdl-input-monitor-poll-for-kb-mouse");
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureKeyboard) {
      m_input_manager->clear_keyboard_actions();
    } else {
      m_input_manager->poll_keyboard_data();
    }
    if (io.WantCaptureMouse) {
      m_input_manager->clear_mouse_actions();
    } else {
      m_input_manager->poll_mouse_data();
    }
    m_input_manager->finish_polling();
  }
  // Now process SDL Events
  process_sdl_events();
  // Also process any display related events received from the EE (the game)
  // this is done here so they run from the perspective of the graphics thread
  {
    auto p = scoped_prof("display-manager-ee-events");
    m_display_manager->process_ee_events();
  }
  {
    auto p = scoped_prof("input-manager-ee-events");
    m_input_manager->process_ee_events();
  }

  // imgui start of frame
  {
    auto p = scoped_prof("imgui-new-frame");
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
  }

  // framebuffer size
  int fbuf_w, fbuf_h;
  SDL_GetWindowSizeInPixels(m_window, &fbuf_w, &fbuf_h);
  // an XR session: the window is a mirror; headsets take sRGB eyes, so no HDR present (its
  // interop lock stalls the render thread every display frame)
  hdr::update(m_window, g_game_version == GameVersion::Jak1 && !Gfx::g_xr_session_running, fbuf_w,
              fbuf_h);

  // render game!
  bool xr_frame = false;  // xrWaitFrame paced this display frame
  g_gfx_data->debug_gui.master_enable = is_imgui_visible();
  if (g_gfx_data->debug_gui.should_advance_frame()) {
    auto p = scoped_prof("game-render");
    int game_res_w = Gfx::g_global_settings.game_res_w;
    int game_res_h = Gfx::g_global_settings.game_res_h;
    if (game_res_w <= 0 || game_res_h <= 0) {
      // if the window size reports 0, the game will ask for a 0 sized window, and nothing likes
      // that.
      game_res_w = 640;
      game_res_h = 480;
    }
    // set the size of the visible/playable portion of the game in the window
    get_display_manager()->set_game_size(Gfx::g_global_settings.lbox_w,
                                         Gfx::g_global_settings.lbox_h);

    // draw splash screen on startup if requested
    if (SplashScreen && DiskBoot && !m_splash_program) {
      init_splash();
    }
    const bool splash =
        SplashScreen && Gfx::g_splash.ready.load() && SplashTimer.getSeconds() < SPLASH_SCREEN_TIME;
    if (splash) {
      draw_splash(fbuf_w, fbuf_h);
    }
    hdr::allow_direct(splash);

    xr::FrameTargets* xr_targets = nullptr;
    if (g_game_version == GameVersion::Jak1) {
      auto& gs = Gfx::g_global_settings;
      xr::update(gs.xr_enable);
      if (gs.xr_recentre_requests != m_xr_recentre_seen) {
        m_xr_recentre_seen = gs.xr_recentre_requests;
        xr::request_recentre();
      }
      float fake_pose[6];
      std::memcpy(fake_pose, gs.xr_fake_pose, sizeof(fake_pose));
      const float amp = std::abs(fake_pose[0]);
      if (gs.xr_fake_sweep_dps > 0.f && amp > 0.f) {
        // triangle wave over -amp..amp, at the sweep rate
        const double period = 4. * amp / gs.xr_fake_sweep_dps;
        // per-frame sweep (dev): a fixed step per display frame (1/90 s), from -amp when set
        static double sweep_t = 0.;
        static int sweep_serial = -1;
        if (gs.xr_fake_sweep_frames != sweep_serial) {
          sweep_serial = gs.xr_fake_sweep_frames;
          sweep_t = 0.;
        }
        sweep_t = sweep_serial > 0 ? sweep_t + 1. / 90. : steady_s();
        const double ph = std::fmod(sweep_t, period) / period;
        fake_pose[0] = (float)(amp * (ph < 0.5 ? 4. * ph - 1. : 3. - 4. * ph));
      }
      xr::set_fake(gs.xr_fake, fake_pose, gs.xr_fake_sweep_dps > 0.f && amp > 0.f);
      xr_targets = xr::begin_frame(gs.xr_pos_scale);
      xr_frame = xr::frame_in_flight();
      Gfx::g_xr_session_running = xr::state() == xr::State::RUNNING;
      if (xr_targets) {
        Gfx::g_xr_eye_w = xr_targets->render_w;
        Gfx::g_xr_eye_h = xr_targets->render_h;
        publish_xr_cull_view(*xr_targets, gs.stereo_world_scale);
      } else {
        // not drawn (dashboard, not visible): the game goes back to its own camera
        Gfx::set_xr_cull_view({});
      }
      if (!xr_targets && !Gfx::g_xr_session_running) {
        // a later session may get another size: no stale per-eye text
        Gfx::g_xr_eye_w = 0;
        Gfx::g_xr_eye_h = 0;
      }
    }

    render_game_frame(
        game_res_w, game_res_h, fbuf_w, fbuf_h, Gfx::g_global_settings.lbox_w,
        Gfx::g_global_settings.lbox_h, Gfx::g_global_settings.msaa_samples,
        Gfx::g_global_settings.brightness_contrast_color,
        Gfx::g_global_settings.brightness_contrast_alpha,
        m_take_screenshot_next_frame && g_gfx_data->debug_gui.screenshot_hotkey_enabled, xr_frame,
        xr_targets);
    xr::end_frame();
    // If we took a screenshot, stop taking them now!
    if (m_take_screenshot_next_frame) {
      m_take_screenshot_next_frame = false;
    }
  }

  // render debug
  if (is_imgui_visible()) {
    auto p = scoped_prof("debug-gui");
    g_gfx_data->debug_gui.draw(g_gfx_data->dma_copier.get_last_result().stats);
  }
  {
    auto p = scoped_prof("imgui-render");
    perf::Scope ps(perf::Group::IMGUI);
    ImGui::Render();
    if (Gfx::g_hdr_present.active) {
      glBindFramebuffer(GL_FRAMEBUFFER, Gfx::window_fbo());
    }
    ImDrawData* draw_data = ImGui::GetDrawData();
    hdr::note_overlay(draw_data && draw_data->TotalVtxCount > 0);
    ImGui_ImplOpenGL3_RenderDrawData(draw_data);
  }

  // actual vsync
  g_gfx_data->debug_gui.finish_frame();
  // xrWaitFrame paces the loop while a session runs: no limiter, no window vsync
  const bool xr_paced = xr_frame;
  if (!xr_paced) {
    m_xr_tick_started = false;
  }
  if (xr_paced != m_xr_paced) {
    m_xr_paced = xr_paced;
    Gfx::g_xr_game_hz = xr_paced ? Gfx::kXrGameHz : 0;
    SDL_GL_SetSwapInterval(xr_paced ? 0 : Gfx::g_global_settings.vsync);
  }
  if (Gfx::g_global_settings.framelimiter && !xr_paced) {
    auto p = scoped_prof("frame-limiter");
    g_gfx_data->frame_limiter.run(
        Gfx::g_global_settings.target_fps, Gfx::g_global_settings.experimental_accurate_lag,
        Gfx::g_global_settings.sleep_in_frame_limiter, g_gfx_data->last_engine_time);
  }

  bool hdr_presented = false;
  {
    auto p = scoped_prof("swap-buffers");
    perf::Scope ps(perf::Group::PRESENT);
    const bool timing = perf::on();
    const auto t0 = timing ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    hdr_presented = hdr::present(xr_paced ? 0 : Gfx::g_global_settings.vsync);
    if (!hdr_presented) {
      perf::Scope swap(perf::Group::PRESENT_SWAP);
      SDL_GL_SwapWindow(m_window);
    }
    if (timing) {
      perf::cpu_present(
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
  }
  log_present_interval(xr_paced ? "xr" : hdr_presented ? "hdr dxgi" : "gl swap");
  perf::frame_end(xr_paced ? "xr" : hdr_presented ? "hdr-dxgi" : "gl-swap");

  // actually wait for vsync
  if (g_gfx_data->debug_gui.should_gl_finish()) {
    glFinish();
  }

  // switch vsync modes, if requested
  if (Gfx::g_global_settings.vsync != Gfx::g_global_settings.old_vsync) {
    Gfx::g_global_settings.old_vsync = Gfx::g_global_settings.vsync;
    // NOTE - -1 can be used for adaptive vsync, maybe useful for Jak 2+?
    // https://wiki.libsdl.org/SDL3/SDL_GL_SetSwapInterval
    // xrWaitFrame paces an XR session: the window keeps interval 0 until it ends
    if (!xr_paced) {
      SDL_GL_SetSwapInterval(Gfx::g_global_settings.vsync);
    }
  }

  // Start timing for the next frame.
  g_gfx_data->debug_gui.start_frame();
  prof().instant_event("ROOT");

  // toggle even odd and wake up engine waiting on vsync.
  // TODO: we could play with moving this earlier, right after the final bucket renderer.
  //       it breaks the VIF-interrupt profiling though.
  {
    prof().instant_event("engine-notify");
    std::unique_lock<std::mutex> lock(g_gfx_data->sync_mutex);
    // XR: the display runs at the headset's rate, the game keeps its 60 Hz vsync
    if (!xr_paced || xr_game_tick()) {
      g_gfx_data->frame_idx++;
    }
    g_gfx_data->sync_cv.notify_all();
  }

  // reboot whole game, if requested
  if (g_gfx_data->debug_gui.want_reboot_in_debug) {
    g_gfx_data->debug_gui.want_reboot_in_debug = false;
    MasterExit = RuntimeExitStatus::RESTART_IN_DEBUG;
  }
  if (g_gfx_data->debug_gui.want_reboot_in_retail) {
    g_gfx_data->debug_gui.want_reboot_in_retail = false;
    MasterExit = RuntimeExitStatus::RESTART_IN_RETAIL;
  }

  {
    auto p = scoped_prof("check-close-window");
    // exit if display window was closed
    if (m_should_quit) {
      std::unique_lock<std::mutex> lock(g_gfx_data->sync_mutex);
      MasterExit = RuntimeExitStatus::EXIT;
      g_gfx_data->sync_cv.notify_all();
    }
  }
}

/*!
 * Wait for the next vsync. Returns 0 or 1 depending on if frame is even or odd.
 * Called from the game thread, on a GOAL stack.
 */
u32 gl_vsync() {
  if (!g_gfx_data) {
    return 0;
  }
  std::unique_lock<std::mutex> lock(g_gfx_data->sync_mutex);
  auto init_frame = g_gfx_data->frame_idx_of_input_data;
  g_gfx_data->sync_cv.wait(lock, [=] {
    return (MasterExit != RuntimeExitStatus::RUNNING) || g_gfx_data->frame_idx > init_frame;
  });
  if (Gfx::g_global_settings.perf_timing_window != 0) {
    perf::ee_frame_start();
  }
  return g_gfx_data->frame_idx & 1;
}

u32 gl_sync_path() {
  if (!g_gfx_data) {
    return 0;
  }
  if (Gfx::g_global_settings.perf_timing_window != 0) {
    perf::ee_frame_work_done();
  }
  std::unique_lock<std::mutex> lock(g_gfx_data->sync_mutex);
  g_gfx_data->last_engine_time = g_gfx_data->engine_timer.getSeconds();
  if (!g_gfx_data->has_data_to_render) {
    return 0;
  }
  g_gfx_data->sync_cv.wait(lock, [=] { return !g_gfx_data->has_data_to_render; });
  return 0;
}

/*!
 * Send DMA to the renderer.
 * Called from the game thread, on a GOAL stack.
 */
void gl_send_chain(const void* data, u32 offset) {
  if (g_gfx_data) {
    std::unique_lock<std::mutex> lock(g_gfx_data->dma_mutex);
    if (g_gfx_data->has_data_to_render) {
      lg::error(
          "Gfx::send_chain called when the graphics renderer has pending data. Was this called "
          "multiple times per frame?");
      return;
    }

    // we copy the dma data and give a copy of it to the render.
    // the copy has a few advantages:
    // - if the game code has a bug and corrupts the DMA buffer, the renderer won't see it.
    // - the copied DMA is much smaller than the entire game memory, so it can be dumped to a
    // file
    //    separate of the entire RAM.
    // - it verifies the DMA data is valid early on.
    // but it may also be pretty expensive. Both the renderer and the game wait on this to
    // complete.

    // The renderers should just operate on DMA chains, so eliminating this step in the future
    // may be easy.

    g_gfx_data->dma_copier.set_input_data(data, offset, run_dma_copy);
    motion_vectors::latch_cut_for_chain();
    g_gfx_data->xr_view_pending = Gfx::take_xr_cull_handed();
    g_gfx_data->xr_view_pending.sent_s = steady_s();
    motion_blur::latch_for_chain();

    g_gfx_data->has_data_to_render = true;
    g_gfx_data->dma_cv.notify_all();
  }
}

/*!
 * Upload texture outside of main DMA chain.
 * We trust the game to not remove textures that are currently being used, but if the game is messed
 * up, there is a possible race to updating this texture.
 */
void gl_texture_upload_now(const u8* tpage, int mode, u32 s7_ptr) {
  // block
  if (g_gfx_data) {
    // just pass it to the texture pool.
    // the texture pool will take care of locking.
    // we don't want to lock here for the entire duration of the conversion.
    g_gfx_data->texture_pool->handle_upload_now(tpage, mode, g_ee_main_mem, s7_ptr, false);
  }
}

/*!
 * Handle a local->local texture copy. The texture pool can just update texture pointers.
 * This is called from the main thread and the texture pool itself will handle locking.
 */
void gl_texture_relocate(u32 destination, u32 source, u32 format) {
  if (g_gfx_data) {
    g_gfx_data->texture_pool->relocate(destination, source, format);
  }
}

void gl_set_levels(const std::vector<std::string>& levels) {
  const auto& gs = Gfx::g_global_settings;
  static bool neighbours_were_on = false;
  const bool jak1_neighbours =
      g_gfx_data->version == GameVersion::Jak1 && gs.vista_enable && gs.vista_neighbours;
  if (jak1_neighbours && neighbours::is_outdoor(neighbours::viewer())) {
    // neighbour fidelity: the game's levels first, then every adjacent level, loader-only
    std::vector<std::string> game_levels;
    for (const auto& l : levels) {
      if (!l.empty() && l != "none") {
        game_levels.push_back(l);
      }
    }
    auto adj = neighbours::adjacent_to(game_levels);
    if (gs.vista_standin_neighbours) {
      auto seen = neighbours::standin_levels(game_levels, adj);
      adj.insert(adj.end(), seen.begin(), seen.end());
    }
    auto want = levels;
    want.insert(want.end(), adj.begin(), adj.end());
    neighbours::set_resident(game_levels, adj);
    g_gfx_data->loader->set_max_levels(
        std::max(g_gfx_data->loader->default_max_levels(), (int)(game_levels.size() + adj.size()) + 1));
    g_gfx_data->loader->set_want_levels(want);
    neighbours_were_on = true;
    return;
  }
  if (neighbours_were_on) {
    neighbours::set_resident({}, {});
    g_gfx_data->loader->set_max_levels(g_gfx_data->loader->default_max_levels());
    neighbours_were_on = false;
  }
  if (!jak1_neighbours && g_gfx_data->version == GameVersion::Jak1 && gs.vista_enable &&
      gs.vista_level[0]) {
    // keep the vista level resident in the loader's spare slot; GOAL never sees it
    auto with_vista = levels;
    std::string vista = gs.vista_level;
    if (std::find(with_vista.begin(), with_vista.end(), vista) == with_vista.end()) {
      with_vista.push_back(vista);
    }
    g_gfx_data->loader->set_want_levels(with_vista);
    return;
  }
  g_gfx_data->loader->set_want_levels(levels);
}

void gl_set_active_levels(const std::vector<std::string>& levels) {
  g_gfx_data->loader->set_active_levels(levels);
}

void gl_force_reload_all() {
  g_gfx_data->loader->request_reload_all();
}

void gl_force_reload_level(const std::string& name) {
  g_gfx_data->loader->request_reload_level(name);
}

void gl_force_reload_common() {
  g_gfx_data->loader->request_reload_common();
}

void gl_set_pmode_alp(float val) {
  g_gfx_data->pmode_alp = val;
}

const GfxRendererModule gRendererOpenGL = {
    gl_init,                 // init
    gl_make_display,         // make_display
    gl_exit,                 // exit
    gl_vsync,                // vsync
    gl_sync_path,            // sync_path
    gl_send_chain,           // send_chain
    gl_texture_upload_now,   // texture_upload_now
    gl_texture_relocate,     // texture_relocate
    gl_set_levels,           // set_levels
    gl_set_active_levels,    // set_active_levels
    gl_force_reload_all,     // force_reload_all
    gl_force_reload_level,   // force_reload_level
    gl_force_reload_common,  // force_reload_common
    gl_set_pmode_alp,        // set_pmode_alp
    GfxPipeline::OpenGL,     // pipeline
    "OpenGL 4.3"             // name
};
