#include "OpenXr.h"

#include "common/log/log.h"

#ifdef SANDOVER_XR

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// openxr_platform.h names IUnknown, which WIN32_LEAN_AND_MEAN leaves out
#include <unknwn.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_OPENGL
#include "fmt/format.h"
#include "third-party/openxr/include/openxr/openxr.h"
#include "third-party/openxr/include/openxr/openxr_platform.h"

namespace xr {
namespace {

using Clock = std::chrono::steady_clock;

constexpr XrViewConfigurationType kViewConfig = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
// seated model: past this offset from the reference the camera stops following the head
constexpr float kMaxHeadOffsetM = 0.5f;

struct Swapchain {
  XrSwapchain handle = XR_NULL_HANDLE;
  int width = 0;
  int height = 0;
  std::vector<XrSwapchainImageOpenGLKHR> images;
  bool acquired = false;
};

struct Timing {
  Clock::time_point window_start;
  Clock::time_point last_wait_return;
  bool have_last = false;
  int frames = 0;
  int submitted = 0;
  double interval_ms = 0;
  double wait_ms = 0;
  double period_ms = 0;
  std::string summary;
};

struct Context {
  State state = State::OFF;
  Clock::time_point next_retry;
  XrResult last_logged_result = XR_SUCCESS;

  XrInstance instance = XR_NULL_HANDLE;
  XrSystemId system = XR_NULL_SYSTEM_ID;
  XrSession session = XR_NULL_HANDLE;
  XrSpace local = XR_NULL_HANDLE;
  XrSpace view_space = XR_NULL_HANDLE;
  XrSpace layer_space = XR_NULL_HANDLE;  // space of this frame's layer poses
  XrSessionState session_state = XR_SESSION_STATE_UNKNOWN;
  XrEnvironmentBlendMode blend_mode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  XrViewConfigurationView view_cfg[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW},
                                         {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
  Swapchain swapchains[2];
  int64_t format = 0;
  std::string runtime_name;

  bool frame_begun = false;
  bool views_valid = false;
  XrFrameState frame_state{XR_TYPE_FRAME_STATE};
  XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
  XrPosef layer_pose[2];  // the pose each eye was drawn from, as submitted
  FrameTargets targets;

  // seated reference in LOCAL: head position and yaw (cos, sin) at the last recentre
  bool have_ref = false;
  bool recentre = false;
  float ref_pos[3] = {0.f, 0.f, 0.f};
  float ref_c = 1.f, ref_s = 0.f;
  bool was_tracked = false;
  bool fov_logged = false;
  Timing timing;
};

Context g;

void quat_to_mat(const XrQuaternionf& q, float m[3][3]) {
  m[0][0] = 1.f - 2.f * (q.y * q.y + q.z * q.z);
  m[0][1] = 2.f * (q.x * q.y - q.w * q.z);
  m[0][2] = 2.f * (q.x * q.z + q.w * q.y);
  m[1][0] = 2.f * (q.x * q.y + q.w * q.z);
  m[1][1] = 1.f - 2.f * (q.x * q.x + q.z * q.z);
  m[1][2] = 2.f * (q.y * q.z - q.w * q.x);
  m[2][0] = 2.f * (q.x * q.z - q.w * q.y);
  m[2][1] = 2.f * (q.y * q.z + q.w * q.x);
  m[2][2] = 1.f - 2.f * (q.x * q.x + q.y * q.y);
}

const char* gl_format_name(int64_t f) {
  switch (f) {
    case 0x8058:
      return "GL_RGBA8";
    case 0x8C43:
      return "GL_SRGB8_ALPHA8";
    case 0x881A:
      return "GL_RGBA16F";
    case 0x881B:
      return "GL_RGB16F";
    case 0x8814:
      return "GL_RGBA32F";
    case 0x8059:
      return "GL_RGB10_A2";
    case 0x8C3A:
      return "GL_R11F_G11F_B10F";
    case 0x805B:
      return "GL_RGBA16";
    case 0x8051:
      return "GL_RGB8";
    case 0x8C41:
      return "GL_SRGB8";
    case 0x81A5:
      return "GL_DEPTH_COMPONENT16";
    case 0x81A6:
      return "GL_DEPTH_COMPONENT24";
    case 0x8CAC:
      return "GL_DEPTH_COMPONENT32F";
    case 0x88F0:
      return "GL_DEPTH24_STENCIL8";
    case 0x8CAD:
      return "GL_DEPTH32F_STENCIL8";
    default:
      return "?";
  }
}

const char* session_state_name(XrSessionState s) {
  switch (s) {
    case XR_SESSION_STATE_IDLE:
      return "IDLE";
    case XR_SESSION_STATE_READY:
      return "READY";
    case XR_SESSION_STATE_SYNCHRONIZED:
      return "SYNCHRONIZED";
    case XR_SESSION_STATE_VISIBLE:
      return "VISIBLE";
    case XR_SESSION_STATE_FOCUSED:
      return "FOCUSED";
    case XR_SESSION_STATE_STOPPING:
      return "STOPPING";
    case XR_SESSION_STATE_LOSS_PENDING:
      return "LOSS_PENDING";
    case XR_SESSION_STATE_EXITING:
      return "EXITING";
    default:
      return "UNKNOWN";
  }
}

std::string result_name(XrResult r) {
  char buf[XR_MAX_RESULT_STRING_SIZE] = {};
  if (g.instance != XR_NULL_HANDLE && XR_SUCCEEDED(xrResultToString(g.instance, r, buf))) {
    return buf;
  }
  switch (r) {
    case XR_ERROR_RUNTIME_FAILURE:
      return "XR_ERROR_RUNTIME_FAILURE";
    case XR_ERROR_RUNTIME_UNAVAILABLE:
      return "XR_ERROR_RUNTIME_UNAVAILABLE";
    case XR_ERROR_INSTANCE_LOST:
      return "XR_ERROR_INSTANCE_LOST";
    default:
      return fmt::format("XrResult({})", (int)r);
  }
}

bool check(XrResult r, const char* what) {
  if (XR_SUCCEEDED(r)) {
    return true;
  }
  lg::error("[xr] {} failed: {}", what, result_name(r));
  return false;
}

void set_state(State s, const std::string& reason) {
  if (s != g.state) {
    lg::info("[xr] state {} -> {} ({})", state_name(g.state), state_name(s), reason);
    g.state = s;
  }
}

void retry_in(double seconds) {
  g.next_retry = Clock::now() + std::chrono::milliseconds((int)(seconds * 1000));
}

std::string read_active_runtime() {
  char buf[1024] = {};
  DWORD size = sizeof(buf);
  if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Khronos\\OpenXR\\1", "ActiveRuntime",
                   RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS) {
    return buf;
  }
  return "(not set)";
}

void destroy_session() {
  for (auto& sc : g.swapchains) {
    if (sc.handle != XR_NULL_HANDLE) {
      xrDestroySwapchain(sc.handle);
    }
    sc = Swapchain{};
  }
  if (g.local != XR_NULL_HANDLE) {
    xrDestroySpace(g.local);
    g.local = XR_NULL_HANDLE;
  }
  if (g.view_space != XR_NULL_HANDLE) {
    xrDestroySpace(g.view_space);
    g.view_space = XR_NULL_HANDLE;
  }
  g.layer_space = XR_NULL_HANDLE;
  g.have_ref = false;
  if (g.session != XR_NULL_HANDLE) {
    xrDestroySession(g.session);
    g.session = XR_NULL_HANDLE;
  }
  g.session_state = XR_SESSION_STATE_UNKNOWN;
  g.frame_begun = false;
}

void destroy_all() {
  destroy_session();
  if (g.instance != XR_NULL_HANDLE) {
    xrDestroyInstance(g.instance);
    g.instance = XR_NULL_HANDLE;
  }
  g.system = XR_NULL_SYSTEM_ID;
}

bool create_instance() {
  const char* env_json = std::getenv("XR_RUNTIME_JSON");
  auto runtime =
      fmt::format("{}{}", read_active_runtime(),
                  env_json ? fmt::format(", XR_RUNTIME_JSON override: {}", env_json) : "");
  static std::string logged_runtime;
  if (runtime != logged_runtime) {
    lg::info("[xr] active runtime (registry): {}", runtime);
    logged_runtime = runtime;
  }

  u32 count = 0;
  XrResult r = xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
  if (!XR_SUCCEEDED(r)) {
    if (r != g.last_logged_result) {
      lg::warn("[xr] no OpenXR runtime available: {}", result_name(r));
      g.last_logged_result = r;
    }
    return false;
  }
  std::vector<XrExtensionProperties> exts(count, {XR_TYPE_EXTENSION_PROPERTIES});
  xrEnumerateInstanceExtensionProperties(nullptr, count, &count, exts.data());
  bool has_gl = false;
  std::string ext_list;
  for (auto& e : exts) {
    has_gl |= std::strcmp(e.extensionName, XR_KHR_OPENGL_ENABLE_EXTENSION_NAME) == 0;
    ext_list += fmt::format("{}{}", ext_list.empty() ? "" : " ", e.extensionName);
  }
  lg::info("[xr] runtime extensions ({}): {}", count, ext_list);
  if (!has_gl) {
    lg::error("[xr] runtime does not offer {}", XR_KHR_OPENGL_ENABLE_EXTENSION_NAME);
    return false;
  }

  const char* enabled[] = {XR_KHR_OPENGL_ENABLE_EXTENSION_NAME};
  XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
  std::strncpy(ci.applicationInfo.applicationName, "Sandover", XR_MAX_APPLICATION_NAME_SIZE - 1);
  ci.applicationInfo.applicationVersion = 1;
  std::strncpy(ci.applicationInfo.engineName, "OpenGOAL", XR_MAX_ENGINE_NAME_SIZE - 1);
  ci.applicationInfo.engineVersion = 1;
  ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  ci.enabledExtensionCount = 1;
  ci.enabledExtensionNames = enabled;
  if (!check(xrCreateInstance(&ci, &g.instance), "xrCreateInstance")) {
    g.instance = XR_NULL_HANDLE;
    return false;
  }

  XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
  if (XR_SUCCEEDED(xrGetInstanceProperties(g.instance, &props))) {
    g.runtime_name =
        fmt::format("{} {}.{}.{}", props.runtimeName, XR_VERSION_MAJOR(props.runtimeVersion),
                    XR_VERSION_MINOR(props.runtimeVersion), XR_VERSION_PATCH(props.runtimeVersion));
    lg::info("[xr] instance created, runtime: {}", g.runtime_name);
  }
  return true;
}

bool get_system() {
  XrSystemGetInfo si{XR_TYPE_SYSTEM_GET_INFO};
  si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
  XrResult r = xrGetSystem(g.instance, &si, &g.system);
  if (!XR_SUCCEEDED(r)) {
    if (r != g.last_logged_result) {
      lg::warn("[xr] xrGetSystem(HMD): {} (no headset yet, retrying)", result_name(r));
      g.last_logged_result = r;
    }
    g.system = XR_NULL_SYSTEM_ID;
    return false;
  }
  g.last_logged_result = XR_SUCCESS;

  XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
  if (XR_SUCCEEDED(xrGetSystemProperties(g.instance, g.system, &sp))) {
    lg::info("[xr] system: '{}' vendor {:#x}, max swapchain {}x{}, max layers {}", sp.systemName,
             sp.vendorId, sp.graphicsProperties.maxSwapchainImageWidth,
             sp.graphicsProperties.maxSwapchainImageHeight, sp.graphicsProperties.maxLayerCount);
  }

  u32 n = 0;
  if (!check(xrEnumerateViewConfigurationViews(g.instance, g.system, kViewConfig, 0, &n, nullptr),
             "xrEnumerateViewConfigurationViews") ||
      n != 2) {
    lg::error("[xr] PRIMARY_STEREO view configuration unavailable (views: {})", n);
    return false;
  }
  xrEnumerateViewConfigurationViews(g.instance, g.system, kViewConfig, 2, &n, g.view_cfg);
  for (int i = 0; i < 2; i++) {
    auto& v = g.view_cfg[i];
    lg::info("[xr] view {}: recommended {}x{} ({} samples), max {}x{} ({} samples)", i,
             v.recommendedImageRectWidth, v.recommendedImageRectHeight,
             v.recommendedSwapchainSampleCount, v.maxImageRectWidth, v.maxImageRectHeight,
             v.maxSwapchainSampleCount);
  }

  u32 nb = 0;
  xrEnumerateEnvironmentBlendModes(g.instance, g.system, kViewConfig, 0, &nb, nullptr);
  std::vector<XrEnvironmentBlendMode> modes(nb);
  xrEnumerateEnvironmentBlendModes(g.instance, g.system, kViewConfig, nb, &nb, modes.data());
  if (!modes.empty()) {
    g.blend_mode = modes[0];
  }
  lg::info("[xr] blend modes: {}, using {}", nb, (int)g.blend_mode);

  PFN_xrGetOpenGLGraphicsRequirementsKHR get_reqs = nullptr;
  xrGetInstanceProcAddr(g.instance, "xrGetOpenGLGraphicsRequirementsKHR",
                        (PFN_xrVoidFunction*)&get_reqs);
  XrGraphicsRequirementsOpenGLKHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR};
  if (!get_reqs ||
      !check(get_reqs(g.instance, g.system, &reqs), "xrGetOpenGLGraphicsRequirementsKHR")) {
    return false;
  }
  lg::info(
      "[xr] GL requirements: min {}.{}, max known {}.{}",
      XR_VERSION_MAJOR(reqs.minApiVersionSupported), XR_VERSION_MINOR(reqs.minApiVersionSupported),
      XR_VERSION_MAJOR(reqs.maxApiVersionSupported), XR_VERSION_MINOR(reqs.maxApiVersionSupported));
  return true;
}

bool create_session() {
  // SDL made the window's context current on this thread; the runtime shares it
  HMODULE gl = GetModuleHandleA("opengl32.dll");
  auto get_dc = gl ? (HDC(WINAPI*)())GetProcAddress(gl, "wglGetCurrentDC") : nullptr;
  auto get_rc = gl ? (HGLRC(WINAPI*)())GetProcAddress(gl, "wglGetCurrentContext") : nullptr;
  XrGraphicsBindingOpenGLWin32KHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR};
  binding.hDC = get_dc ? get_dc() : nullptr;
  binding.hGLRC = get_rc ? get_rc() : nullptr;
  if (!binding.hDC || !binding.hGLRC) {
    lg::error("[xr] no current WGL context");
    return false;
  }

  XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
  sci.next = &binding;
  sci.systemId = g.system;
  if (!check(xrCreateSession(g.instance, &sci, &g.session), "xrCreateSession")) {
    g.session = XR_NULL_HANDLE;
    return false;
  }

  XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
  rs.poseInReferenceSpace.orientation.w = 1.f;
  if (!check(xrCreateReferenceSpace(g.session, &rs, &g.local), "xrCreateReferenceSpace(LOCAL)")) {
    return false;
  }
  rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
  if (!check(xrCreateReferenceSpace(g.session, &rs, &g.view_space),
             "xrCreateReferenceSpace(VIEW)")) {
    return false;
  }

  u32 nf = 0;
  if (!check(xrEnumerateSwapchainFormats(g.session, 0, &nf, nullptr),
             "xrEnumerateSwapchainFormats")) {
    return false;
  }
  std::vector<int64_t> formats(nf);
  xrEnumerateSwapchainFormats(g.session, nf, &nf, formats.data());
  std::string list;
  for (auto f : formats) {
    list += fmt::format("{}{} ({:#x})", list.empty() ? "" : ", ", gl_format_name(f), f);
  }
  lg::info("[xr] swapchain formats, runtime order: {}", list);

  // the engine output is display-encoded: sRGB storage passes it through unchanged
  const int64_t preferred[] = {0x8C43 /* SRGB8_ALPHA8 */, 0x8058 /* RGBA8 */, 0x881A /* RGBA16F */};
  g.format = 0;
  for (auto p : preferred) {
    for (auto f : formats) {
      if (f == p && !g.format) {
        g.format = f;
      }
    }
  }
  if (!g.format) {
    lg::error("[xr] none of SRGB8_ALPHA8, RGBA8, RGBA16F offered");
    return false;
  }
  lg::info("[xr] using {}", gl_format_name(g.format));

  for (int i = 0; i < 2; i++) {
    auto& sc = g.swapchains[i];
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = g.format;
    ci.sampleCount = 1;
    ci.width = g.view_cfg[i].recommendedImageRectWidth;
    ci.height = g.view_cfg[i].recommendedImageRectHeight;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    if (!check(xrCreateSwapchain(g.session, &ci, &sc.handle), "xrCreateSwapchain")) {
      sc.handle = XR_NULL_HANDLE;
      return false;
    }
    sc.width = (int)ci.width;
    sc.height = (int)ci.height;
    u32 ni = 0;
    xrEnumerateSwapchainImages(sc.handle, 0, &ni, nullptr);
    sc.images.assign(ni, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR});
    if (!check(xrEnumerateSwapchainImages(sc.handle, ni, &ni,
                                          (XrSwapchainImageBaseHeader*)sc.images.data()),
               "xrEnumerateSwapchainImages")) {
      return false;
    }
    lg::info("[xr] eye {} swapchain {}x{}, {} images", i, sc.width, sc.height, ni);
  }
  return true;
}

void poll_events() {
  XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
  while (g.instance != XR_NULL_HANDLE && xrPollEvent(g.instance, &ev) == XR_SUCCESS) {
    switch (ev.type) {
      case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
        auto* e = (XrEventDataSessionStateChanged*)&ev;
        lg::info("[xr] session {} -> {}", session_state_name(g.session_state),
                 session_state_name(e->state));
        g.session_state = e->state;
        if (e->state == XR_SESSION_STATE_READY) {
          XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
          bi.primaryViewConfigurationType = kViewConfig;
          if (check(xrBeginSession(g.session, &bi), "xrBeginSession")) {
            g.timing = Timing{};
            g.have_ref = false;
            g.fov_logged = false;
            g.timing.window_start = Clock::now();
            set_state(State::RUNNING, "session begun");
          }
        } else if (e->state == XR_SESSION_STATE_STOPPING) {
          check(xrEndSession(g.session), "xrEndSession");
          set_state(State::IDLE, "session stopping");
        } else if (e->state == XR_SESSION_STATE_EXITING ||
                   e->state == XR_SESSION_STATE_LOSS_PENDING) {
          destroy_session();
          g.system = XR_NULL_SYSTEM_ID;
          retry_in(3.0);
          set_state(State::NO_SYSTEM, "session ended by runtime");
        }
        break;
      }
      case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
        destroy_all();
        retry_in(5.0);
        set_state(State::NO_RUNTIME, "instance loss pending");
        return;
      default:
        lg::debug("[xr] event type {}", (int)ev.type);
        break;
    }
    ev = {XR_TYPE_EVENT_DATA_BUFFER};
  }
}

void update_timing(double wait_ms) {
  auto& t = g.timing;
  auto now = Clock::now();
  if (t.have_last) {
    t.interval_ms += std::chrono::duration<double, std::milli>(now - t.last_wait_return).count();
  }
  t.last_wait_return = now;
  t.have_last = true;
  t.frames++;
  t.wait_ms += wait_ms;
  t.period_ms += g.frame_state.predictedDisplayPeriod / 1e6;
  double window = std::chrono::duration<double>(now - t.window_start).count();
  if (window >= 5.0 && t.frames > 1) {
    t.summary = fmt::format(
        "{:.1f} fps, interval {:.2f} ms, xrWaitFrame {:.2f} ms, predicted period {:.2f} ms, "
        "submitted {}/{}",
        t.frames / window, t.interval_ms / (t.frames - 1), t.wait_ms / t.frames,
        t.period_ms / t.frames, t.submitted, t.frames);
    lg::info("[xr] timing: {}", t.summary);
    auto summary = t.summary;
    t = Timing{};
    t.summary = summary;
    t.window_start = now;
    t.last_wait_return = now;
    t.have_last = true;
  }
}

}  // namespace

void update(bool want) {
  if (!want) {
    if (g.state != State::OFF) {
      destroy_all();
      set_state(State::OFF, "toggle off");
    }
    return;
  }

  const bool retry_due = Clock::now() >= g.next_retry;
  switch (g.state) {
    case State::OFF:
    case State::NO_RUNTIME:
    case State::FAILED:
      if (!retry_due) {
        break;
      }
      destroy_all();
      if (!create_instance()) {
        retry_in(5.0);
        set_state(State::NO_RUNTIME, "no runtime");
        break;
      }
      set_state(State::NO_SYSTEM, "instance created");
      [[fallthrough]];
    case State::NO_SYSTEM:
      if (!retry_due) {
        break;
      }
      if (g.system == XR_NULL_SYSTEM_ID && !get_system()) {
        retry_in(2.0);
        break;
      }
      if (!create_session()) {
        destroy_session();
        g.system = XR_NULL_SYSTEM_ID;
        retry_in(10.0);
        set_state(State::FAILED, "session setup failed");
        break;
      }
      set_state(State::IDLE, "session created");
      break;
    default:
      break;
  }
  poll_events();
}

FrameTargets* begin_frame(float pos_scale) {
  if (g.state != State::RUNNING || g.frame_begun) {
    return nullptr;
  }
  XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
  g.frame_state = {XR_TYPE_FRAME_STATE};
  auto t0 = Clock::now();
  if (!check(xrWaitFrame(g.session, &wi, &g.frame_state), "xrWaitFrame")) {
    return nullptr;
  }
  update_timing(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
  XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
  if (!check(xrBeginFrame(g.session, &bi), "xrBeginFrame")) {
    return nullptr;
  }
  g.frame_begun = true;
  g.views_valid = false;
  g.targets = FrameTargets{};
  if (!g.frame_state.shouldRender) {
    return nullptr;
  }

  auto locate = [&](XrSpace space) {
    XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
    li.viewConfigurationType = kViewConfig;
    li.displayTime = g.frame_state.predictedDisplayTime;
    li.space = space;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    u32 nv = 0;
    g.views[0] = {XR_TYPE_VIEW};
    g.views[1] = {XR_TYPE_VIEW};
    if (!XR_SUCCEEDED(xrLocateViews(g.session, &li, &vs, 2, &nv, g.views)) || nv != 2) {
      return (XrViewStateFlags)0;
    }
    return vs.viewStateFlags;
  };
  XrViewStateFlags flags = locate(g.local);
  const bool tracked = (flags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
  const bool pos_valid = tracked && (flags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;
  if (!tracked) {
    // no tracking: head-locked eyes in VIEW space, drawn by the fixed stereo path
    flags = locate(g.view_space);
    if (!(flags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
      return nullptr;
    }
  }
  if (tracked != g.was_tracked) {
    lg::info("[xr] head tracking {}", tracked ? "on" : "lost: head-locked stereo fallback");
    g.was_tracked = tracked;
  }
  g.layer_space = tracked ? g.local : g.view_space;
  g.views_valid = true;
  g.targets.tracked = tracked;

  const auto& p0 = g.views[0].pose.position;
  const auto& p1 = g.views[1].pose.position;
  const float head[3] = {0.5f * (p0.x + p1.x), 0.5f * (p0.y + p1.y), 0.5f * (p0.z + p1.z)};
  float head_rot[3][3];
  quat_to_mat(g.views[0].pose.orientation, head_rot);

  // seated reference: yaw only, so the horizon stays level after a recentre
  if (tracked && (g.recentre || !g.have_ref)) {
    // forward is -z; yaw turns (0, 0, -1) to (-sin, 0, -cos)
    const float fx = -head_rot[0][2], fz = -head_rot[2][2];
    const float yaw = std::atan2(-fx, -fz);
    g.ref_c = std::cos(yaw);
    g.ref_s = std::sin(yaw);
    for (int k = 0; k < 3; k++) {
      g.ref_pos[k] = pos_valid ? head[k] : 0.f;
    }
    g.have_ref = true;
    g.recentre = false;
    lg::info("[xr] recentred: yaw {:.1f} deg, head at ({:.2f}, {:.2f}, {:.2f}) m",
             yaw * 57.29578f, g.ref_pos[0], g.ref_pos[1], g.ref_pos[2]);
  }
  // LOCAL <-> reference frame: rotation about +y by the reference yaw
  const float rc = g.ref_c, rs = g.ref_s;
  auto to_ref = [&](const float v[3], float out[3]) {
    out[0] = rc * v[0] - rs * v[2];
    out[1] = v[1];
    out[2] = rs * v[0] + rc * v[2];
  };
  auto from_ref = [&](const float v[3], float out[3]) {
    out[0] = rc * v[0] + rs * v[2];
    out[1] = v[1];
    out[2] = -rs * v[0] + rc * v[2];
  };
  float d[3] = {0.f, 0.f, 0.f};
  if (pos_valid) {
    const float rel[3] = {head[0] - g.ref_pos[0], head[1] - g.ref_pos[1],
                          head[2] - g.ref_pos[2]};
    to_ref(rel, d);
    const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len > kMaxHeadOffsetM) {
      for (auto& x : d) {
        x *= kMaxHeadOffsetM / len;
      }
    }
  }

  // eye positions along the head's x axis (view 0's orientation; both share it on a HMD)
  const float ax = head_rot[0][0], ay = head_rot[1][0], az = head_rot[2][0];
  // OpenXR (+x right, +y up, +z back) to the game camera (+x left, +y up, +z forward)
  const float sgn[3] = {-1.f, 1.f, -1.f};

  for (int i = 0; i < 2; i++) {
    auto& e = g.targets.eye[i];
    const auto& v = g.views[i];
    e.head_x_m = (v.pose.position.x - head[0]) * ax + (v.pose.position.y - head[1]) * ay +
                 (v.pose.position.z - head[2]) * az;
    e.tan_left = std::tan(v.fov.angleLeft);
    e.tan_right = std::tan(v.fov.angleRight);
    e.tan_up = std::tan(v.fov.angleUp);
    e.tan_down = std::tan(v.fov.angleDown);
    g.layer_pose[i] = v.pose;
    if (!tracked) {
      e.pos_m[0] = -e.head_x_m;  // game camera +x is left
      continue;
    }
    // eye orientation relative to the reference: yaw^T * eye
    float eye_rot[3][3], rel[3][3];
    quat_to_mat(v.pose.orientation, eye_rot);
    for (int col = 0; col < 3; col++) {
      const float cv[3] = {eye_rot[0][col], eye_rot[1][col], eye_rot[2][col]};
      float r[3];
      to_ref(cv, r);
      for (int row = 0; row < 3; row++) {
        rel[row][col] = r[row];
      }
    }
    // game camera -> eye camera: S * rel^T * S
    for (int r = 0; r < 3; r++) {
      for (int cc = 0; cc < 3; cc++) {
        e.rot[r][cc] = sgn[r] * sgn[cc] * rel[cc][r];
      }
    }
    const float off[3] = {v.pose.position.x - head[0], v.pose.position.y - head[1],
                          v.pose.position.z - head[2]};
    float o[3];
    to_ref(off, o);
    for (int k = 0; k < 3; k++) {
      e.pos_m[k] = sgn[k] * (d[k] * pos_scale + o[k]);
    }
    if (pos_valid) {
      // submit the pose the eye was drawn from (the clamped head offset)
      const float drawn_ref[3] = {d[0] + o[0], d[1] + o[1], d[2] + o[2]};
      float drawn[3];
      from_ref(drawn_ref, drawn);
      g.layer_pose[i].position = {g.ref_pos[0] + drawn[0], g.ref_pos[1] + drawn[1],
                                  g.ref_pos[2] + drawn[2]};
    }
  }

  // one symmetric frustum covers both eyes; each eye is cropped from it at its own density
  auto& t = g.targets;
  t.render_tan_x = std::max({-t.eye[0].tan_left, t.eye[0].tan_right, -t.eye[1].tan_left,
                             t.eye[1].tan_right});
  t.render_tan_y =
      std::max({t.eye[0].tan_up, -t.eye[0].tan_down, t.eye[1].tan_up, -t.eye[1].tan_down});
  const int eye_w = g.swapchains[0].width, eye_h = g.swapchains[0].height;
  const float span_x = t.eye[0].tan_right - t.eye[0].tan_left;
  const float span_y = t.eye[0].tan_up - t.eye[0].tan_down;
  t.render_w = eye_w;
  t.render_h = eye_h;
  if (span_x > 0.f && span_y > 0.f) {
    t.render_w = std::min((int)std::ceil(eye_w * 2.f * t.render_tan_x / span_x), 2 * eye_w);
    t.render_h = std::min((int)std::ceil(eye_h * 2.f * t.render_tan_y / span_y), 2 * eye_h);
  }
  if (!g.fov_logged) {
    g.fov_logged = true;
    for (int i = 0; i < 2; i++) {
      const auto& f = g.views[i].fov;
      lg::info("[xr] eye {} fov deg: left {:.2f} right {:.2f} up {:.2f} down {:.2f}, image {}x{}",
               i, f.angleLeft * 57.29578f, f.angleRight * 57.29578f, f.angleUp * 57.29578f,
               f.angleDown * 57.29578f, g.swapchains[i].width, g.swapchains[i].height);
    }
    lg::info("[xr] drawn frustum half-tangents x {:.3f} y {:.3f}, frame {}x{}", t.render_tan_x,
             t.render_tan_y, t.render_w, t.render_h);
  }

  for (int i = 0; i < 2; i++) {
    auto& sc = g.swapchains[i];
    u32 index = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (!check(xrAcquireSwapchainImage(sc.handle, &ai, &index), "xrAcquireSwapchainImage")) {
      return nullptr;
    }
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = XR_INFINITE_DURATION;
    if (!check(xrWaitSwapchainImage(sc.handle, &wait), "xrWaitSwapchainImage")) {
      return nullptr;
    }
    sc.acquired = true;
    auto& e = g.targets.eye[i];
    e.gl_texture = sc.images[index].image;
    e.width = sc.width;
    e.height = sc.height;
  }
  return &g.targets;
}

void end_frame() {
  if (!g.frame_begun) {
    return;
  }
  g.frame_begun = false;
  for (auto& sc : g.swapchains) {
    if (sc.acquired) {
      XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
      check(xrReleaseSwapchainImage(sc.handle, &ri), "xrReleaseSwapchainImage");
      sc.acquired = false;
    }
  }

  XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                            {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
  XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  const XrCompositionLayerBaseHeader* layers[] = {(XrCompositionLayerBaseHeader*)&layer};
  XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
  ei.displayTime = g.frame_state.predictedDisplayTime;
  ei.environmentBlendMode = g.blend_mode;
  if (g.targets.rendered && g.views_valid) {
    for (int i = 0; i < 2; i++) {
      pv[i].pose = g.layer_pose[i];
      pv[i].fov = g.views[i].fov;
      pv[i].subImage.swapchain = g.swapchains[i].handle;
      pv[i].subImage.imageRect.offset = {0, 0};
      pv[i].subImage.imageRect.extent = {g.swapchains[i].width, g.swapchains[i].height};
      pv[i].subImage.imageArrayIndex = 0;
    }
    layer.space = g.layer_space;
    layer.viewCount = 2;
    layer.views = pv;
    ei.layerCount = 1;
    ei.layers = layers;
    g.timing.submitted++;
  }
  check(xrEndFrame(g.session, &ei), "xrEndFrame");
}

State state() {
  return g.state;
}

bool frame_in_flight() {
  return g.frame_begun;
}

void request_recentre() {
  g.recentre = true;
}

std::string status_line() {
  std::string s = state_name(g.state);
  if (!g.runtime_name.empty()) {
    s += ", " + g.runtime_name;
  }
  if (g.swapchains[0].handle != XR_NULL_HANDLE) {
    s += fmt::format(", {}x{} {}", g.swapchains[0].width, g.swapchains[0].height,
                     gl_format_name(g.format));
  }
  if (g.session != XR_NULL_HANDLE) {
    s += fmt::format(", session {}", session_state_name(g.session_state));
  }
  if (!g.timing.summary.empty()) {
    s += "\n" + g.timing.summary;
  }
  return s;
}

void shutdown() {
  if (g.state != State::OFF) {
    destroy_all();
    set_state(State::OFF, "shutdown");
  }
}

}  // namespace xr

#else  // no OpenXR on this platform yet

namespace xr {
void update(bool want) {
  static bool warned = false;
  if (want && !warned) {
    lg::warn("[xr] OpenXR is not built on this platform");
    warned = true;
  }
}
FrameTargets* begin_frame(float) {
  return nullptr;
}
void end_frame() {}
State state() {
  return State::OFF;
}
bool frame_in_flight() {
  return false;
}
void request_recentre() {}
std::string status_line() {
  return "not built on this platform";
}
void shutdown() {}
}  // namespace xr

#endif

namespace xr {
const char* state_name(State s) {
  switch (s) {
    case State::OFF:
      return "OFF";
    case State::NO_RUNTIME:
      return "NO_RUNTIME";
    case State::NO_SYSTEM:
      return "NO_SYSTEM";
    case State::IDLE:
      return "IDLE";
    case State::RUNNING:
      return "RUNNING";
    case State::FAILED:
      return "FAILED";
  }
  return "?";
}
}  // namespace xr
