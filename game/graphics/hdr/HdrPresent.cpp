#include "HdrPresent.h"

#ifdef _WIN32
// before the project headers: FileUtil.h undefines FALSE, which d3d11.h uses
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#endif

#include "common/log/log.h"

#include "game/graphics/gfx.h"
#include "game/graphics/opengl_renderer/PerfTimers.h"

#include "fmt/format.h"

#ifdef _WIN32

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <vector>

#include "third-party/SDL/include/SDL3/SDL.h"
#include "third-party/glad/include/glad/glad.h"

using Microsoft::WRL::ComPtr;

namespace hdr {
namespace {

// WGL_NV_DX_interop2, loaded at run time
constexpr GLenum kWglAccessWriteDiscard = 0x0002;
using PfnDxOpenDevice = HANDLE(WINAPI*)(void*);
using PfnDxCloseDevice = BOOL(WINAPI*)(HANDLE);
using PfnDxRegisterObject = HANDLE(WINAPI*)(HANDLE, void*, GLuint, GLenum, GLenum);
using PfnDxUnregisterObject = BOOL(WINAPI*)(HANDLE, HANDLE);
using PfnDxLockObjects = BOOL(WINAPI*)(HANDLE, GLint, HANDLE*);

// mscms entry points, loaded at run time (ColorProfileGetDisplayDefault is Windows 11 only)
using PfnGetDisplayDefault = HRESULT(WINAPI*)(int, LUID, UINT32, int, int, LPWSTR*);
using PfnGetColorDirectory = BOOL(WINAPI*)(PCWSTR, PWSTR, PDWORD);
constexpr int kScopeSystemWide = 0;
constexpr int kScopeCurrentUser = 1;
constexpr int kCptIcc = 0;
constexpr int kCpstExtendedDisplayColorMode = 8;

struct DisplayInfo {
  std::string name;  // monitor friendly name
  bool dc_found = false;
  LUID adapter{};
  UINT32 target_id = 0;
  UINT32 source_id = 0;
  bool hdr_supported = false;
  bool hdr_enabled = false;
  float sdr_white_nits = 0.f;
  int bpc = 0;
  int encoding = -1;  // DISPLAYCONFIG_COLOR_ENCODING: 0 RGB, 1 YCbCr444, 2 422, 3 420

  bool dxgi_found = false;
  LUID dxgi_adapter{};
  DXGI_OUTPUT_DESC1 desc{};

  std::string icc_path;
  bool mhc2 = false;
  float mhc2_min = 0.f;
  float mhc2_max = 0.f;
  float mhc2_matrix[12] = {};
};

enum class State { OFF, RUNNING, FAILED };

struct Presenter {
  State state = State::OFF;
  SDL_Window* window = nullptr;
  HWND hwnd = nullptr;   // the SDL window: display queries, OS HDR switch
  HWND child = nullptr;  // covers hwnd's client area and owns our flip chain
  int child_w = 0, child_h = 0;
  int format = 0;
  int w = 0, h = 0;
  DisplayInfo info;
  bool switched_os = false;
  DisplayInfo switched_on;  // the display whose HDR state we changed

  ComPtr<ID3D11Device> dev;
  ComPtr<ID3D11DeviceContext> ctx;
  ComPtr<IDXGISwapChain1> swapchain;
  bool tearing = false;
  int presents = 0;
  int logged_mode = -1;
  float refresh_hz = 0.f;
  int refresh_checks = 0;
  int logged_interval = -1;
  int win_mode = -1;  // WindowMode of the SDL window, -1 before the first frame
  int recheck = 0;    // presents until the state is logged again after a mode change
  DXGI_HDR_METADATA_HDR10 md{};  // last metadata sent (PQ only)
  bool md_sent = false;
  DXGI_COLOR_SPACE_TYPE cs = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;

  PfnDxOpenDevice dx_open = nullptr;
  PfnDxCloseDevice dx_close = nullptr;
  PfnDxRegisterObject dx_register = nullptr;
  PfnDxUnregisterObject dx_unregister = nullptr;
  PfnDxLockObjects dx_lock = nullptr;
  PfnDxLockObjects dx_unlock = nullptr;
  HANDLE interop_dev = nullptr;

  // interop textures in the swapchain format; two so GL never waits on last frame's D3D copy
  struct Slot {
    ComPtr<ID3D11Texture2D> tex;
    HANDLE obj = nullptr;
    GLuint gl_tex = 0, fbo = 0;
  };
  Slot slots[2];
  int cur = 0;          // slot of the frame being built
  int last = -1;        // slot holding the last presented image
  bool locked = false;  // slots[cur] is locked for GL

  // direct: the final pass writes slots[cur] itself (tonemap + encode in one pass)
  bool direct_allowed = false;  // this frame may use it (set before the render)
  bool final_ran = false;       // the final pass ran this frame
  bool overlay_seen = false;    // something drew over the window after it last frame
  bool last_direct = false;     // slots[last] came from the final pass, not the stand-in
  bool check = false;           // SANDOVER_HDR_DIRECT_CHECK frame
  bool logged_direct = false;
  int failed_format = -1;
  int direct_frames = 0;

  GLuint scene_tex = 0, scene_fbo = 0;  // window stand-in, linear scRGB
  GLuint program = 0, vao = 0;
};

Presenter g_p;

bool set_os_hdr(const DisplayInfo& i, bool on);

void restore_os_hdr() {
  if (g_p.switched_os) {
    g_p.switched_os = false;
    set_os_hdr(g_p.switched_on, false);
  }
}

LPTOP_LEVEL_EXCEPTION_FILTER g_prev_filter = nullptr;

LONG WINAPI crash_filter(EXCEPTION_POINTERS* e) {
  restore_os_hdr();
  return g_prev_filter ? g_prev_filter(e) : EXCEPTION_CONTINUE_SEARCH;
}

void install_restore_hooks() {
  static bool installed = false;
  if (!installed) {
    installed = true;
    std::atexit(restore_os_hdr);
    g_prev_filter = SetUnhandledExceptionFilter(crash_filter);
  }
}

std::string narrow(const wchar_t* w) {
  if (!w || !*w) {
    return {};
  }
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  std::string s(n > 0 ? n - 1 : 0, '\0');
  if (n > 1) {
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  }
  return s;
}

bool luid_eq(const LUID& a, const LUID& b) {
  return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

u32 be32(const u8* p) {
  return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | u32(p[3]);
}

float s15f16(const u8* p) {
  return (s32)be32(p) / 65536.f;
}

/// Windows display config for the monitor: HDR state, SDR white, ids for the colour APIs.
void query_display_config(HMONITOR mon, DisplayInfo& info) {
  MONITORINFOEXW mi{};
  mi.cbSize = sizeof(mi);
  if (!GetMonitorInfoW(mon, &mi)) {
    return;
  }
  UINT32 npaths = 0, nmodes = 0;
  if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &npaths, &nmodes) != ERROR_SUCCESS) {
    return;
  }
  std::vector<DISPLAYCONFIG_PATH_INFO> paths(npaths);
  std::vector<DISPLAYCONFIG_MODE_INFO> modes(nmodes);
  if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &npaths, paths.data(), &nmodes, modes.data(),
                         nullptr) != ERROR_SUCCESS) {
    return;
  }
  for (UINT32 i = 0; i < npaths; i++) {
    const auto& path = paths[i];
    DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
    src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    src.header.size = sizeof(src);
    src.header.adapterId = path.sourceInfo.adapterId;
    src.header.id = path.sourceInfo.id;
    if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS ||
        wcscmp(src.viewGdiDeviceName, mi.szDevice) != 0) {
      continue;
    }
    info.dc_found = true;
    info.adapter = path.targetInfo.adapterId;
    info.target_id = path.targetInfo.id;
    info.source_id = path.sourceInfo.id;

    DISPLAYCONFIG_TARGET_DEVICE_NAME tgt{};
    tgt.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    tgt.header.size = sizeof(tgt);
    tgt.header.adapterId = info.adapter;
    tgt.header.id = info.target_id;
    if (DisplayConfigGetDeviceInfo(&tgt.header) == ERROR_SUCCESS) {
      info.name = narrow(tgt.monitorFriendlyDeviceName);
    }

    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO aci{};
    aci.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
    aci.header.size = sizeof(aci);
    aci.header.adapterId = info.adapter;
    aci.header.id = info.target_id;
    if (DisplayConfigGetDeviceInfo(&aci.header) == ERROR_SUCCESS) {
      info.hdr_supported = aci.advancedColorSupported;
      info.hdr_enabled = aci.advancedColorEnabled;
      info.bpc = (int)aci.bitsPerColorChannel;
      info.encoding = (int)aci.colorEncoding;
    }

    DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
    white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
    white.header.size = sizeof(white);
    white.header.adapterId = info.adapter;
    white.header.id = info.target_id;
    if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS) {
      info.sdr_white_nits = white.SDRWhiteLevel / 1000.f * 80.f;
    }
    return;
  }
}

void query_dxgi(HMONITOR mon, DisplayInfo& info) {
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
    return;
  }
  ComPtr<IDXGIAdapter1> adapter;
  for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; a++) {
    ComPtr<IDXGIOutput> out;
    for (UINT o = 0; adapter->EnumOutputs(o, &out) != DXGI_ERROR_NOT_FOUND; o++) {
      ComPtr<IDXGIOutput6> out6;
      DXGI_OUTPUT_DESC1 d{};
      if (SUCCEEDED(out.As(&out6)) && SUCCEEDED(out6->GetDesc1(&d)) && d.Monitor == mon) {
        DXGI_ADAPTER_DESC1 ad{};
        adapter->GetDesc1(&ad);
        info.dxgi_found = true;
        info.dxgi_adapter = ad.AdapterLuid;
        info.desc = d;
        return;
      }
    }
  }
}

std::wstring color_directory() {
  wchar_t buf[MAX_PATH] = {};
  DWORD size = sizeof(buf);
  if (HMODULE m = LoadLibraryW(L"mscms.dll")) {
    auto fn = (PfnGetColorDirectory)(void*)GetProcAddress(m, "GetColorDirectoryW");
    if (fn && fn(nullptr, buf, &size)) {
      return buf;
    }
  }
  GetWindowsDirectoryW(buf, MAX_PATH);
  return std::wstring(buf) + L"\\System32\\spool\\drivers\\color";
}

/// The display's default profile: the HDR (advanced colour) one where Windows 11 has it, which
/// is where the Windows HDR Calibration app stores its result, else the SDR one from GDI.
std::wstring display_profile(HMONITOR mon, const DisplayInfo& info) {
  if (info.dc_found) {
    if (HMODULE m = LoadLibraryW(L"mscms.dll")) {
      auto fn = (PfnGetDisplayDefault)(void*)GetProcAddress(m, "ColorProfileGetDisplayDefault");
      for (int scope : {kScopeCurrentUser, kScopeSystemWide}) {
        LPWSTR name = nullptr;
        if (fn && SUCCEEDED(fn(scope, info.adapter, info.source_id, kCptIcc,
                               kCpstExtendedDisplayColorMode, &name)) &&
            name) {
          std::wstring s = name;
          LocalFree(name);
          if (!s.empty()) {
            return s.find(L'\\') == std::wstring::npos ? color_directory() + L"\\" + s : s;
          }
        }
      }
    }
  }
  MONITORINFOEXW mi{};
  mi.cbSize = sizeof(mi);
  if (GetMonitorInfoW(mon, &mi)) {
    if (HDC dc = CreateDCW(L"DISPLAY", mi.szDevice, nullptr, nullptr)) {
      wchar_t buf[MAX_PATH] = {};
      DWORD size = MAX_PATH;
      std::wstring s;
      if (GetICMProfileW(dc, &size, buf)) {
        s = buf;
      }
      DeleteDC(dc);
      return s;
    }
  }
  return {};
}

/// MHC2 tag (Microsoft's ICC extension written by the Windows HDR Calibration app):
/// min/max luminance in nits and a 3x4 colour matrix, all s15Fixed16, offsets from the tag.
void parse_mhc2(const std::wstring& path, DisplayInfo& info) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    return;
  }
  std::vector<u8> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (d.size() < 132) {
    return;
  }
  const u32 count = be32(&d[128]);
  for (u32 i = 0; i < count && 132 + 12 * (size_t)i + 12 <= d.size(); i++) {
    const u8* e = &d[132 + 12 * i];
    if (be32(e) != 0x4D484332) {  // 'MHC2'
      continue;
    }
    const size_t off = be32(e + 4), size = be32(e + 8);
    if (off + 36 > d.size() || size < 36) {
      return;
    }
    const u8* t = &d[off];
    info.mhc2 = true;
    info.mhc2_min = s15f16(t + 12);
    info.mhc2_max = s15f16(t + 16);
    size_t m = off + be32(t + 20);
    if (m + 8 <= d.size() && be32(&d[m]) == 0x73663332) {  // 'sf32' type header
      m += 8;
    }
    if (m + 48 <= d.size()) {
      for (int k = 0; k < 12; k++) {
        info.mhc2_matrix[k] = s15f16(&d[m + 4 * k]);
      }
    }
    return;
  }
}

DisplayInfo query_display(HWND hwnd) {
  DisplayInfo info;
  HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
  query_display_config(mon, info);
  query_dxgi(mon, info);
  const std::wstring icc = display_profile(mon, info);
  info.icc_path = narrow(icc.c_str());
  if (!icc.empty()) {
    parse_mhc2(icc, info);
  }
  return info;
}

void log_display(const DisplayInfo& i) {
  lg::info("hdr: display '{}' hdr supported {} enabled {} sdr white {:.0f} nits", i.name,
           i.hdr_supported, i.hdr_enabled, i.sdr_white_nits);
  if (i.dxgi_found) {
    const auto& d = i.desc;
    lg::info(
        "hdr: dxgi min {:.4f} max {:.0f} max full frame {:.0f} nits, {} bpc, colour space {}, "
        "primaries r({:.3f},{:.3f}) g({:.3f},{:.3f}) b({:.3f},{:.3f}) w({:.4f},{:.4f})",
        d.MinLuminance, d.MaxLuminance, d.MaxFullFrameLuminance, d.BitsPerColor, (int)d.ColorSpace,
        d.RedPrimary[0], d.RedPrimary[1], d.GreenPrimary[0], d.GreenPrimary[1], d.BluePrimary[0],
        d.BluePrimary[1], d.WhitePoint[0], d.WhitePoint[1]);
  } else {
    lg::warn("hdr: no DXGI output for the window's monitor");
  }
  lg::info("hdr: display profile '{}'", i.icc_path.empty() ? "(none)" : i.icc_path);
  if (i.mhc2) {
    const float* m = i.mhc2_matrix;
    lg::info(
        "hdr: MHC2 min {:.4f} max {:.0f} nits, matrix [{:.4f} {:.4f} {:.4f} | {:.4f} {:.4f} "
        "{:.4f} | {:.4f} {:.4f} {:.4f}]",
        i.mhc2_min, i.mhc2_max, m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]);
  } else {
    lg::info("hdr: no MHC2 tag in the profile (run the Windows HDR Calibration app to add one)");
  }
}

bool set_os_hdr(const DisplayInfo& i, bool on) {
  DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE s{};
  s.header.type = DISPLAYCONFIG_DEVICE_INFO_SET_ADVANCED_COLOR_STATE;
  s.header.size = sizeof(s);
  s.header.adapterId = i.adapter;
  s.header.id = i.target_id;
  s.enableAdvancedColor = on ? 1 : 0;
  const LONG r = DisplayConfigSetDeviceInfo(&s.header);
  if (r != ERROR_SUCCESS) {
    lg::error("hdr: could not turn Windows HDR {} (error {})", on ? "on" : "off", r);
    return false;
  }
  lg::info("hdr: turned Windows HDR {} on '{}'", on ? "on" : "off", i.name);
  return true;
}

GLuint compile(GLenum type, const char* src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, nullptr);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char err[1024];
    glGetShaderInfoLog(s, sizeof(err), nullptr, err);
    lg::error("hdr: encode shader: {}", err);
  }
  return s;
}

constexpr const char* kVert = R"(#version 410 core
out vec2 uv;
void main() {
  vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
  uv = vec2(p.x, 1.0 - p.y);  // GL writes the D3D image top row first
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

constexpr const char* kFrag = R"(#version 410 core
in vec2 uv;
out vec4 color;
uniform sampler2D scene;
uniform int pq;
const mat3 k709_to_2020 = mat3(0.6274, 0.0691, 0.0164, 0.3293, 0.9195, 0.0880,
                               0.0433, 0.0114, 0.8956);
void main() {
  vec3 c = texture(scene, uv).rgb;  // linear scRGB, 1.0 = 80 nits
  if (pq == 1) {
    vec3 y = pow(clamp(k709_to_2020 * c * (80.0 / 10000.0), 0.0, 1.0), vec3(0.1593017578125));
    c = pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), vec3(78.84375));
  }
  color = vec4(c, 1.0);
}
)";

void release_targets() {
  auto& p = g_p;
  if (p.locked) {
    p.dx_unlock(p.interop_dev, 1, &p.slots[p.cur].obj);
    p.locked = false;
  }
  for (auto& sl : p.slots) {
    if (sl.obj) {
      p.dx_unregister(p.interop_dev, sl.obj);
    }
    sl.tex.Reset();
    glDeleteFramebuffers(1, &sl.fbo);
    glDeleteTextures(1, &sl.gl_tex);
    sl = Presenter::Slot();
  }
  p.cur = 0;
  p.last = -1;
  p.last_direct = false;
  glDeleteFramebuffers(1, &p.scene_fbo);
  glDeleteTextures(1, &p.scene_tex);
  p.scene_fbo = p.scene_tex = 0;
}

// p.format: HdrOutputFormat (0 scRGB, 1 PQ) or kSdr
constexpr int kSdr = 2;

DXGI_FORMAT dxgi_format(int format) {
  return format == kSdr                       ? DXGI_FORMAT_R8G8B8A8_UNORM
         : format == (int)HdrOutputFormat::PQ ? DXGI_FORMAT_R10G10B10A2_UNORM
                                              : DXGI_FORMAT_R16G16B16A16_FLOAT;
}

bool make_targets(int w, int h) {
  auto& p = g_p;
  D3D11_TEXTURE2D_DESC td{};
  td.Width = w;
  td.Height = h;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = dxgi_format(p.format);
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  for (auto& sl : p.slots) {
    if (FAILED(p.dev->CreateTexture2D(&td, nullptr, &sl.tex))) {
      lg::error("hdr: CreateTexture2D {}x{} failed", w, h);
      return false;
    }
    glGenTextures(1, &sl.gl_tex);
    sl.obj = p.dx_register(p.interop_dev, sl.tex.Get(), sl.gl_tex, GL_TEXTURE_2D,
                           kWglAccessWriteDiscard);
    if (!sl.obj) {
      lg::error("hdr: wglDXRegisterObjectNV failed ({:#x})", GetLastError());
      return false;
    }
    glGenFramebuffers(1, &sl.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, sl.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sl.gl_tex, 0);
  }

  glGenTextures(1, &p.scene_tex);
  glBindTexture(GL_TEXTURE_2D, p.scene_tex);
  // SDR: 8 bits like the GL window it stands in for
  if (p.format == kSdr) {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  } else {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
  }
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glBindTexture(GL_TEXTURE_2D, 0);
  glGenFramebuffers(1, &p.scene_fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, p.scene_fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, p.scene_tex, 0);
  const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  glClearColor(0.f, 0.f, 0.f, 1.f);
  glClear(GL_COLOR_BUFFER_BIT);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  if (status != GL_FRAMEBUFFER_COMPLETE) {
    lg::error("hdr: window stand-in framebuffer incomplete ({:#x})", status);
    return false;
  }
  p.w = w;
  p.h = h;
  return true;
}

bool client_size(HWND hwnd, int* w, int* h) {
  RECT r{};
  if (!GetClientRect(hwnd, &r)) {
    return false;
  }
  *w = r.right - r.left;
  *h = r.bottom - r.top;
  return *w > 0 && *h > 0;
}

/// A disabled STATIC child covering the SDL window's client area. The GL driver may already own
/// a flip chain on the SDL window (NVIDIA "layered on DXGI", AMD with Windows HDR on) and an
/// HWND takes only one; the child has none. STATIC answers WM_NCHITTEST with HTTRANSPARENT and a
/// disabled window takes no focus, so mouse and keyboard stay with the SDL window.
bool create_child() {
  auto& p = g_p;
  if (GetWindowThreadProcessId(p.hwnd, nullptr) != GetCurrentThreadId()) {
    lg::warn("hdr: present window created off the SDL window's thread; input queues attach");
  }
  int w = 0, h = 0;
  client_size(p.hwnd, &w, &h);
  p.child =
      CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, L"STATIC", L"",
                      WS_CHILD | WS_VISIBLE | WS_DISABLED | WS_CLIPSIBLINGS, 0, 0, std::max(w, 1),
                      std::max(h, 1), p.hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (!p.child) {
    lg::error("hdr: CreateWindowEx for the present window failed ({:#x})", GetLastError());
    return false;
  }
  SetWindowPos(p.child, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  p.child_w = w;
  p.child_h = h;
  return true;
}

void fit_child() {
  auto& p = g_p;
  int w = 0, h = 0;
  if (p.child && client_size(p.hwnd, &w, &h) && (w != p.child_w || h != p.child_h)) {
    SetWindowPos(p.child, nullptr, 0, 0, w, h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
    p.child_w = w;
    p.child_h = h;
  }
}

enum WindowMode { WINDOWED, EXCLUSIVE, BORDERLESS };

int window_mode(SDL_Window* w) {
  if (!(SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN)) {
    return WINDOWED;
  }
  return SDL_GetWindowFullscreenMode(w) ? EXCLUSIVE : BORDERLESS;
}

const char* window_mode_name(int m) {
  return m == WINDOWED     ? "windowed"
         : m == EXCLUSIVE  ? "exclusive fullscreen"
         : m == BORDERLESS ? "borderless fullscreen"
                           : "?";
}

const char* composition_name(int m) {
  static const char* names[] = {"composed", "hardware overlay (MPO)", "none",
                                "composition failure"};
  return m >= 0 && m < 4 ? names[m] : "not reported yet";
}

const char* colour_space_name(int cs) {
  switch (cs) {
    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
      return "G2084 P2020 (HDR10)";
    case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
      return "G10 P709 (scRGB)";
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:
      return "G22 P709 (sRGB)";
    default:
      return cs < 0 ? "?" : "other";
  }
}

const char* format_name(int f) {
  return f == 1 ? "HDR10 PQ R10G10B10A2" : f == 0 ? "scRGB R16G16B16A16F" : "SDR R8G8B8A8";
}

/// Logs the composition mode DXGI reports (checked every 120 presents) when it changes.
void log_present_mode() {
  auto& p = g_p;
  if (++p.presents % 120 != 0) {
    return;
  }
  ComPtr<IDXGISwapChainMedia> media;
  DXGI_FRAME_STATISTICS_MEDIA st{};
  if (FAILED(p.swapchain.As(&media)) || FAILED(media->GetFrameStatisticsMedia(&st))) {
    return;
  }
  const int mode = (int)st.CompositionMode;
  if (mode == p.logged_mode) {
    return;
  }
  p.logged_mode = mode;
  lg::info("hdr: DXGI present mode {} ({})", composition_name(mode), window_mode_name(p.win_mode));
}

/// Everything that decides how the display reads the swapchain, re-queried now: window mode,
/// swapchain format and colour space, the output's colour space, Windows HDR and the link.
void log_state(const char* why) {
  auto& p = g_p;
  DXGI_SWAP_CHAIN_DESC1 sd{};
  p.swapchain->GetDesc1(&sd);
  ComPtr<IDXGISwapChain3> sc3;
  UINT support = 0;
  if (SUCCEEDED(p.swapchain.As(&sc3))) {
    sc3->CheckColorSpaceSupport(p.cs, &support);
  }
  int out_cs = -1;
  ComPtr<IDXGIOutput> out;
  ComPtr<IDXGIOutput6> out6;
  DXGI_OUTPUT_DESC1 od{};
  if (SUCCEEDED(p.swapchain->GetContainingOutput(&out)) && SUCCEEDED(out.As(&out6)) &&
      SUCCEEDED(out6->GetDesc1(&od))) {
    out_cs = (int)od.ColorSpace;
  }
  DisplayInfo now;
  query_display_config(MonitorFromWindow(p.hwnd, MONITOR_DEFAULTTONEAREST), now);
  const SDL_DisplayMode* dm = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(p.window));
  RECT cr{};
  GetClientRect(p.child, &cr);
  static const char* kEnc[] = {"RGB", "YCbCr444", "YCbCr422", "YCbCr420"};
  lg::info(
      "hdr: {}: {} on {}x{}@{:.2f}; swapchain {} {}x{} colour space {} (present {}, overlay {}); "
      "output colour space {}; Windows HDR {}; link {} {} bpc; child {}x{}; metadata {}",
      why, window_mode_name(p.win_mode), dm ? dm->w : 0, dm ? dm->h : 0,
      dm ? dm->refresh_rate : 0.f, format_name(p.format), sd.Width, sd.Height, colour_space_name(p.cs),
      (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) != 0,
      (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_OVERLAY_PRESENT) != 0,
      colour_space_name(out_cs), now.hdr_enabled ? "on" : "off",
      now.encoding >= 0 && now.encoding < 4 ? kEnc[now.encoding] : "?", now.bpc,
      cr.right - cr.left, cr.bottom - cr.top,
      p.md_sent ? fmt::format("HDR10 mastering {}-{:.4f} nits, MaxCLL {}, MaxFALL {}",
                              p.md.MaxMasteringLuminance, p.md.MinMasteringLuminance / 10000.0,
                              p.md.MaxContentLightLevel, p.md.MaxFrameAverageLightLevel)
                : std::string("none"));
}

/// Vsync on: sync interval refresh / target when that is a whole number (1-4), else 0 without
/// tearing so the limiter paces and the newest frame flips at the next vblank.
UINT sync_interval() {
  auto& p = g_p;
  const auto& gs = Gfx::g_global_settings;
  int n = 1;
  if (gs.framelimiter && gs.target_fps > 0.f && p.refresh_hz > 0.f) {
    const float k = p.refresh_hz / gs.target_fps;
    const int r = (int)std::lround(k);
    n = (r >= 1 && r <= 4 && std::abs(k - (float)r) < 0.01f * (float)r) ? r : 0;
  }
  if (n != p.logged_interval && p.refresh_hz > 0.f) {
    p.logged_interval = n;
    lg::info("hdr: vsync at {:.2f} Hz, target {:.0f} fps: sync interval {}{}", p.refresh_hz,
             gs.target_fps, n, n ? "" : " (not a multiple, the frame limiter paces)");
  }
  return (UINT)n;
}

void stop() {
  auto& p = g_p;
  Gfx::g_hdr_present.active = false;
  if (p.interop_dev) {
    release_targets();
    p.dx_close(p.interop_dev);
    p.interop_dev = nullptr;
  }
  if (p.program) {
    glDeleteProgram(p.program);
    glDeleteVertexArrays(1, &p.vao);
    p.program = p.vao = 0;
  }
  p.swapchain.Reset();
  if (p.ctx) {
    p.ctx->ClearState();
    p.ctx->Flush();
  }
  p.ctx.Reset();
  p.dev.Reset();
  if (p.child) {
    DestroyWindow(p.child);
    p.child = nullptr;
    p.child_w = p.child_h = 0;
  }
  p.presents = 0;
  p.logged_mode = -1;
  p.refresh_hz = 0.f;
  p.refresh_checks = 0;
  p.logged_interval = -1;
  p.win_mode = -1;
  p.recheck = 0;
  if (p.switched_os) {
    set_os_hdr(p.switched_on, false);
    p.switched_os = false;
  }
}

void draw_encode(GLuint fbo);
void publish_display_values(const DisplayInfo& i);

void send_hdr_metadata();

bool apply_colour_space() {
  auto& p = g_p;
  ComPtr<IDXGISwapChain3> sc3;
  UINT support = 0;
  return SUCCEEDED(p.swapchain.As(&sc3)) && SUCCEEDED(sc3->CheckColorSpaceSupport(p.cs, &support)) &&
         (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) &&
         SUCCEEDED(sc3->SetColorSpace1(p.cs));
}

bool start(SDL_Window* window, int w, int h, int format) {
  auto& p = g_p;
  const auto& gs = Gfx::g_global_settings;
  const bool sdr = format == kSdr;
  p.hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                        SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
  if (!p.hwnd) {
    lg::error("hdr: no HWND for the window");
    return false;
  }
  p.format = format;
  p.window = window;
  p.info = query_display(p.hwnd);
  if (!sdr && !p.info.hdr_enabled && gs.hdr_os_switch && p.info.hdr_supported && p.info.dc_found) {
    if (set_os_hdr(p.info, true)) {
      install_restore_hooks();
      p.switched_os = true;
      p.switched_on = p.info;
      for (int i = 0; i < 30 && !p.info.hdr_enabled; i++) {
        Sleep(100);
        p.info = query_display(p.hwnd);
      }
    }
  }
  log_display(p.info);
  publish_display_values(p.info);
  if (!sdr && !p.info.hdr_enabled) {
    lg::warn(
        "hdr: Windows HDR is off for '{}'; staying SDR (turn HDR on in Settings > Display or "
        "Win+Alt+B, then toggle HDR output; option 7 lets the game switch it)",
        p.info.name);
    return false;
  }

  auto gl_proc = [](const char* n) { return (void*)SDL_GL_GetProcAddress(n); };
  p.dx_open = (PfnDxOpenDevice)gl_proc("wglDXOpenDeviceNV");
  p.dx_close = (PfnDxCloseDevice)gl_proc("wglDXCloseDeviceNV");
  p.dx_register = (PfnDxRegisterObject)gl_proc("wglDXRegisterObjectNV");
  p.dx_unregister = (PfnDxUnregisterObject)gl_proc("wglDXUnregisterObjectNV");
  p.dx_lock = (PfnDxLockObjects)gl_proc("wglDXLockObjectsNV");
  p.dx_unlock = (PfnDxLockObjects)gl_proc("wglDXUnlockObjectsNV");
  if (!p.dx_open || !p.dx_close || !p.dx_register || !p.dx_unregister || !p.dx_lock ||
      !p.dx_unlock) {
    lg::error("hdr: the GL driver has no WGL_NV_DX_interop2");
    return false;
  }

  ComPtr<IDXGIFactory2> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
    lg::error("hdr: CreateDXGIFactory1 failed");
    return false;
  }
  ComPtr<IDXGIAdapter1> adapter;
  if (p.info.dxgi_found) {
    ComPtr<IDXGIAdapter1> a;
    for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
      DXGI_ADAPTER_DESC1 ad{};
      a->GetDesc1(&ad);
      if (luid_eq(ad.AdapterLuid, p.info.dxgi_adapter)) {
        adapter = a;
        break;
      }
    }
  }
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  if (FAILED(D3D11CreateDevice(adapter.Get(),
                               adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                               nullptr, 0, levels, 2, D3D11_SDK_VERSION, &p.dev, nullptr,
                               &p.ctx))) {
    lg::error("hdr: D3D11CreateDevice failed");
    return false;
  }
  ComPtr<IDXGIDevice1> dxgi_dev;
  if (SUCCEEDED(p.dev.As(&dxgi_dev))) {
    dxgi_dev->SetMaximumFrameLatency(1);
  }

  ComPtr<IDXGIFactory5> f5;
  BOOL tearing = 0;
  if (SUCCEEDED(factory.As(&f5))) {
    f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing));
  }
  p.tearing = tearing;

  DXGI_SWAP_CHAIN_DESC1 sd{};
  sd.Width = w;
  sd.Height = h;
  sd.Format = dxgi_format(p.format);
  sd.SampleDesc.Count = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.BufferCount = 2;
  sd.Scaling = DXGI_SCALING_NONE;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
  sd.Flags = p.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
  if (!create_child()) {
    return false;
  }
  HRESULT hr =
      factory->CreateSwapChainForHwnd(p.dev.Get(), p.child, &sd, nullptr, nullptr, &p.swapchain);
  if (FAILED(hr)) {
    lg::error("hdr: CreateSwapChainForHwnd failed ({:#x})", (u32)hr);
    if (hr == E_ACCESSDENIED) {
      lg::error("hdr: the present window already has a flip chain (a hooking overlay?)");
    }
    return false;
  }
  factory->MakeWindowAssociation(p.hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);

  p.cs = p.format == 1   ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
         : p.format == 0 ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
                         : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
  if (!apply_colour_space()) {
    lg::error("hdr: colour space {} not supported for present on this display", (int)p.cs);
    return false;
  }

  p.interop_dev = p.dx_open(p.dev.Get());
  if (!p.interop_dev) {
    lg::error("hdr: wglDXOpenDeviceNV failed ({:#x})", GetLastError());
    return false;
  }
  if (!make_targets(w, h)) {
    return false;
  }

  GLuint vs = compile(GL_VERTEX_SHADER, kVert);
  GLuint fs = compile(GL_FRAGMENT_SHADER, kFrag);
  p.program = glCreateProgram();
  glAttachShader(p.program, vs);
  glAttachShader(p.program, fs);
  glLinkProgram(p.program);
  glDeleteShader(vs);
  glDeleteShader(fs);
  GLint linked = 0;
  glGetProgramiv(p.program, GL_LINK_STATUS, &linked);
  if (!linked) {
    lg::error("hdr: encode program did not link");
    return false;
  }
  glGenVertexArrays(1, &p.vao);
  // run the separate encode once per slot now, so toggling the direct path later never pays
  // for the driver's first-draw work on this program and target format
  for (auto& sl : p.slots) {
    if (p.dx_lock(p.interop_dev, 1, &sl.obj)) {
      draw_encode(sl.fbo);
      p.dx_unlock(p.interop_dev, 1, &sl.obj);
    }
  }

  lg::info("hdr: presenting {} {}x{} through DXGI on a child window (tearing {})",
           p.format == 1   ? "HDR10 PQ"
           : p.format == 0 ? "scRGB"
                           : "SDR",
           w, h, p.tearing);
  return true;
}

/// HDR10 static metadata from what the panel reports (MHC2, else DXGI), capped at the peak the
/// game maps to, so composed and overlay or independent-flip presents carry the same values.
void send_hdr_metadata() {
  auto& p = g_p;
  ComPtr<IDXGISwapChain4> sc4;
  p.md_sent = false;
  if (p.format != 1 || FAILED(p.swapchain.As(&sc4))) {
    return;
  }
  const auto& v = Gfx::g_hdr_present;
  const auto& i = p.info;
  const float panel = v.display_peak_nits > 0.f ? v.display_peak_nits : v.peak_nits;
  const float max_nits = std::max(1.f, std::min(panel, v.peak_nits));
  const float min_nits = i.mhc2 ? i.mhc2_min : i.dxgi_found ? i.desc.MinLuminance : 0.f;
  const float full = i.dxgi_found && i.desc.MaxFullFrameLuminance > 0.f
                         ? i.desc.MaxFullFrameLuminance
                         : max_nits;
  // BT.2020 primaries and D65 in units of 0.00002
  DXGI_HDR_METADATA_HDR10 md{};
  md.RedPrimary[0] = 35400;
  md.RedPrimary[1] = 14600;
  md.GreenPrimary[0] = 8500;
  md.GreenPrimary[1] = 39850;
  md.BluePrimary[0] = 6550;
  md.BluePrimary[1] = 2300;
  md.WhitePoint[0] = 15635;
  md.WhitePoint[1] = 16450;
  md.MaxMasteringLuminance = (UINT)std::lround(max_nits);
  md.MinMasteringLuminance = (UINT)std::lround(std::max(0.f, min_nits) * 10000.f);
  md.MaxContentLightLevel = (UINT16)std::lround(max_nits);
  md.MaxFrameAverageLightLevel = (UINT16)std::lround(std::min(full, max_nits));
  p.md = md;
  p.md_sent = SUCCEEDED(sc4->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10, sizeof(md), &md));
}

/// The display's own peak and black (what the calibration page calls "detected").
void publish_display_values(const DisplayInfo& i) {
  auto& out = Gfx::g_hdr_present;
  out.display_peak_nits = 800.f;
  if (i.mhc2 && i.mhc2_max > 0.f) {
    out.display_peak_nits = i.mhc2_max;
  } else if (i.dxgi_found && i.desc.MaxLuminance > 0.f) {
    out.display_peak_nits = i.desc.MaxLuminance;
  }
  // DXGI MinLuminance is EDID, too coarse to lift shadows by
  out.display_black_nits = i.mhc2 ? i.mhc2_min : 0.f;
  out.desktop_white_nits = i.sdr_white_nits > 0.f ? i.sdr_white_nits : -1.f;
}

void publish_values() {
  const auto& gs = Gfx::g_global_settings;
  auto& out = Gfx::g_hdr_present;
  // the calibration page opening re-reads Windows' SDR white, so a slider change there shows
  static int last_cal = 0;
  if (gs.hdr_cal_pattern != 0 && last_cal == 0) {
    DisplayInfo now;
    query_display_config(MonitorFromWindow(g_p.hwnd, MONITOR_DEFAULTTONEAREST), now);
    if (now.sdr_white_nits > 0.f) {
      g_p.info.sdr_white_nits = now.sdr_white_nits;
    }
  }
  last_cal = gs.hdr_cal_pattern;
  publish_display_values(g_p.info);
  const float peak = gs.hdr_peak_nits > 0.f ? gs.hdr_peak_nits : out.display_peak_nits;
  const float black = gs.hdr_black_nits >= 0.f ? gs.hdr_black_nits : out.display_black_nits;
  const bool changed = out.peak_nits != peak || out.black_nits != black ||
                       out.paper_white_nits != gs.paper_white_nits;
  out.peak_nits = peak;
  out.black_nits = black;
  out.paper_white_nits = gs.paper_white_nits;
  out.saturation = gs.hdr_saturation;
  out.knee = gs.hdr_knee;
  if (changed) {
    lg::info("hdr: paper white {:.0f}, peak {:.0f}, black {:.4f} nits", out.paper_white_nits,
             out.peak_nits, out.black_nits);
    send_hdr_metadata();
  }
}

/// Picks this frame's slot and locks it for GL. With the double buffer on, the slot the last
/// frame presented from stays with D3D.
bool lock_slot() {
  auto& p = g_p;
  p.cur = (Gfx::perf_opt(PERF_OPT_POST + 1) && p.last >= 0) ? 1 - p.last : 0;
  if (!p.dx_lock(p.interop_dev, 1, &p.slots[p.cur].obj)) {
    lg::error("hdr: wglDXLockObjectsNV failed; back to the SDR window");
    stop();
    p.state = State::FAILED;
    p.failed_format = p.format;
    return false;
  }
  p.locked = true;
  return true;
}

/// The separate encode: the stand-in (linear scRGB) into `fbo`, in the swapchain format.
void draw_encode(GLuint fbo) {
  auto& p = g_p;
  GLint prev_fbo = 0, prev_prog = 0, prev_vao = 0, prev_tex = 0, prev_active = 0;
  GLint vp[4];
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_fbo);
  glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
  glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
  glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
  glActiveTexture(GL_TEXTURE0);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
  glGetIntegerv(GL_VIEWPORT, vp);
  const GLboolean blend = glIsEnabled(GL_BLEND), depth = glIsEnabled(GL_DEPTH_TEST),
                  scissor = glIsEnabled(GL_SCISSOR_TEST), cull = glIsEnabled(GL_CULL_FACE);

  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glViewport(0, 0, p.w, p.h);
  glDisable(GL_BLEND);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_CULL_FACE);
  glUseProgram(p.program);
  glBindTexture(GL_TEXTURE_2D, p.scene_tex);
  glUniform1i(glGetUniformLocation(p.program, "scene"), 0);
  glUniform1i(glGetUniformLocation(p.program, "pq"), p.format);
  glBindVertexArray(p.vao);
  glDrawArrays(GL_TRIANGLES, 0, 3);

  glBindVertexArray(prev_vao);
  glUseProgram(prev_prog);
  glBindTexture(GL_TEXTURE_2D, prev_tex);
  glActiveTexture(prev_active);
  glBindFramebuffer(GL_FRAMEBUFFER, prev_fbo);
  glViewport(vp[0], vp[1], vp[2], vp[3]);
  blend ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
  depth ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST);
  scissor ? glEnable(GL_SCISSOR_TEST) : glDisable(GL_SCISSOR_TEST);
  cull ? glEnable(GL_CULL_FACE) : glDisable(GL_CULL_FACE);
}

bool encode_scene() {
  auto& p = g_p;
  if (!lock_slot()) {
    return false;
  }
  draw_encode(p.slots[p.cur].fbo);
  p.dx_unlock(p.interop_dev, 1, &p.slots[p.cur].obj);
  p.locked = false;
  return true;
}

std::vector<float> read_rgba(GLuint fbo, int w, int h) {
  std::vector<float> px((size_t)w * h * 4);
  GLint prev = 0;
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
  glReadBuffer(fbo ? GL_COLOR_ATTACHMENT0 : GL_BACK);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, w, h, GL_RGBA, GL_FLOAT, px.data());
  glBindFramebuffer(GL_READ_FRAMEBUFFER, prev);
  return px;
}

/// SANDOVER_HDR_DIRECT_CHECK: compares the direct image (locked slot) with the separate encode
/// of the stand-in drawn the same frame, as-is and flipped; logs the largest difference.
void check_direct() {
  auto& p = g_p;
  const bool pq = p.format == 1;
  GLuint tex = 0, fbo = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexImage2D(GL_TEXTURE_2D, 0, pq ? GL_RGB10_A2 : GL_RGBA16F, p.w, p.h, 0, GL_RGBA,
               pq ? GL_UNSIGNED_INT_2_10_10_10_REV : GL_HALF_FLOAT, nullptr);
  glBindTexture(GL_TEXTURE_2D, 0);
  glGenFramebuffers(1, &fbo);
  GLint prev = 0;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, prev);
  draw_encode(fbo);
  const auto a = read_rgba(p.slots[p.cur].fbo, p.w, p.h);
  // SDR: the reference is the GL window itself (rows bottom first), as the GL swap showed it
  const bool sdr = p.format == kSdr;
  const auto b = sdr ? read_rgba(0, p.w, p.h) : read_rgba(fbo, p.w, p.h);
  glDeleteFramebuffers(1, &fbo);
  glDeleteTextures(1, &tex);
  // PQ / SDR: difference in 10- / 8-bit codes; scRGB: relative to max(|ref|, 1/80 nit)
  auto diff = [&](float x, float y) {
    return pq    ? std::abs(x - y) * 1023.f
           : sdr ? std::abs(x - y) * 255.f
                 : std::abs(x - y) / std::max(std::abs(y), 1.f / 80.f);
  };
  float max_same = 0.f, max_flip = 0.f;
  size_t over = 0;
  for (int y = 0; y < p.h; y++) {
    for (int x = 0; x < p.w; x++) {
      for (int c = 0; c < 3; c++) {
        // the slot is top row first; the window reference bottom row first
        const int ry = sdr ? p.h - 1 - y : y;
        const float v = a[((size_t)y * p.w + x) * 4 + c];
        const float d = diff(v, b[((size_t)ry * p.w + x) * 4 + c]);
        max_same = std::max(max_same, d);
        over += d > (pq || sdr ? 1.01f : 0.01f);
        max_flip = std::max(max_flip, diff(v, b[((size_t)(p.h - 1 - ry) * p.w + x) * 4 + c]));
      }
    }
  }
  lg::info("hdr: direct check {}: max diff {:.3f} {} ({} channels over), flipped {:.3f}",
           pq    ? "PQ"
           : sdr ? "SDR vs GL window"
                 : "scRGB",
           max_same, pq || sdr ? "codes" : "rel", over, max_flip);
}

}  // namespace

void update(SDL_Window* window, bool allowed, int fb_w, int fb_h) {
  auto& p = g_p;
  const auto& gs = Gfx::g_global_settings;
  // HDR output off: SDR through the same flip-model swapchain (perf-opt bit 24)
  const bool sdr = !gs.hdr_output && Gfx::perf_opt(PERF_OPT_PRESENT2 + 0);
  const bool want = allowed && (gs.hdr_output || sdr);
  const int format = sdr ? kSdr : gs.hdr_format == (int)HdrOutputFormat::PQ ? 1 : 0;
  if (p.state == State::RUNNING && (!want || format != p.format)) {
    stop();
    p.state = State::OFF;
  }
  if (!want) {
    p.state = State::OFF;
    return;
  }
  if (p.state == State::FAILED) {
    if (format == p.failed_format) {
      return;
    }
    p.state = State::OFF;  // the other mode may still work
  }
  if (p.state == State::OFF) {
    if (fb_w <= 0 || fb_h <= 0) {
      return;
    }
    Gfx::g_hdr_present.peak_nits = -1.f;  // forces the first values log
    if (!start(window, fb_w, fb_h, format)) {
      lg::error("hdr: DXGI {} output unavailable; staying on the GL window (toggle to retry)",
                format == kSdr ? "SDR" : "HDR");
      stop();
      p.state = State::FAILED;
      p.failed_format = format;
      return;
    }
    p.state = State::RUNNING;
  }
  if (fb_w > 0 && fb_h > 0 && (fb_w != p.w || fb_h != p.h)) {
    release_targets();
    const HRESULT hr = p.swapchain->ResizeBuffers(
        0, fb_w, fb_h, DXGI_FORMAT_UNKNOWN, p.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    if (FAILED(hr) || !make_targets(fb_w, fb_h)) {
      lg::error("hdr: resize to {}x{} failed ({:#x})", fb_w, fb_h, (u32)hr);
      stop();
      p.state = State::FAILED;
      p.failed_format = p.format;
      return;
    }
  }
  fit_child();
  if (p.refresh_checks++ % 120 == 0) {
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
    p.refresh_hz = mode ? mode->refresh_rate : 0.f;
  }
  publish_values();
  const int mode = window_mode(window);
  if (mode != p.win_mode) {
    const bool first = p.win_mode < 0;
    p.win_mode = mode;
    // a mode change can re-modeset the display: state it again, and log it once settled
    apply_colour_space();
    send_hdr_metadata();
    log_state(first ? "started" : "window mode changed");
    p.logged_mode = -1;
    p.presents = 0;
    p.recheck = 240;
  }
  p.final_ran = false;
  p.direct_allowed = false;
  Gfx::g_hdr_present.fbo = p.scene_fbo;
  Gfx::g_hdr_present.hdr = p.format != kSdr;
  Gfx::g_hdr_present.active = true;
}

bool present(int vsync) {
  auto& p = g_p;
  if (p.state != State::RUNNING || !Gfx::g_hdr_present.active) {
    return false;
  }
  std::optional<perf::Scope> ps(std::in_place, perf::Group::PRESENT_ENCODE);
  const bool direct = p.locked;
  if (direct) {
    if (p.check) {
      p.check = false;
      check_direct();
    }
    p.dx_unlock(p.interop_dev, 1, &p.slots[p.cur].obj);
    p.locked = false;
  } else if (!p.final_ran && p.last_direct && p.last >= 0) {
    p.cur = p.last;  // nothing rendered and the stand-in is stale: show the last image again
  } else if (!encode_scene()) {
    return false;
  }
  p.last_direct = direct || (p.last_direct && !p.final_ran);
  if (p.final_ran && direct != p.logged_direct) {
    p.logged_direct = direct;
    lg::info("hdr: final pass {} the swapchain image{}", direct ? "writes" : "no longer writes",
             direct           ? ""
             : p.overlay_seen ? " (an overlay drew)"
                              : "");
  }

  ps.emplace(perf::Group::PRESENT_FLIP);
  ComPtr<ID3D11Texture2D> back;
  if (FAILED(p.swapchain->GetBuffer(0, IID_PPV_ARGS(&back)))) {
    lg::error("hdr: GetBuffer failed; back to the SDR window");
    stop();
    p.state = State::FAILED;
    p.failed_format = p.format;
    return false;
  }
  p.ctx->CopyResource(back.Get(), p.slots[p.cur].tex.Get());
  back.Reset();
  p.last = p.cur;
  const UINT interval = vsync ? sync_interval() : 0;
  const UINT flags = (!vsync && p.tearing) ? DXGI_PRESENT_ALLOW_TEARING : 0;
  const HRESULT hr = p.swapchain->Present(interval, flags);
  if (FAILED(hr)) {
    lg::error("hdr: Present failed ({:#x}); back to the SDR window", (u32)hr);
    stop();
    p.state = State::FAILED;
    p.failed_format = p.format;
    return true;
  }
  log_present_mode();
  if (p.recheck > 0 && --p.recheck == 0) {
    log_state("settled");
  }
  return true;
}

void allow_direct(bool splash) {
  auto& p = g_p;
  p.direct_allowed =
      p.state == State::RUNNING && Gfx::perf_opt(PERF_OPT_POST + 0) && !p.overlay_seen && !splash;
}

DirectTarget begin_direct(int fb_w, int fb_h, bool eligible) {
  auto& p = g_p;
  if (p.state != State::RUNNING || !Gfx::g_hdr_present.active) {
    return {};
  }
  p.final_ran = true;
  if (!eligible || !p.direct_allowed || p.locked || fb_w != p.w || fb_h != p.h || !lock_slot()) {
    return {};
  }
  static const bool check_env = std::getenv("SANDOVER_HDR_DIRECT_CHECK") != nullptr;
  p.check = check_env && ++p.direct_frames % 600 == 300;
  return {p.slots[p.cur].fbo, p.h, p.format == 1, p.check};
}

void note_overlay(bool drew) {
  g_p.overlay_seen = drew;
}

std::string present_tag() {
  const auto& p = g_p;
  if (p.state != State::RUNNING) {
    return "off";
  }
  return fmt::format("{}, {} {}, {}", window_mode_name(p.win_mode), format_name(p.format),
                     colour_space_name(p.cs), composition_name(p.logged_mode));
}

std::string status_line() {
  const auto& p = g_p;
  if (p.state != State::RUNNING) {
    return p.state == State::FAILED ? "HDR output failed (see log)" : "HDR output off";
  }
  const auto& v = Gfx::g_hdr_present;
  return fmt::format("{} on '{}' (Windows HDR {}), paper {:.0f} peak {:.0f} black {:.3f} nits",
                     p.format == 1 ? "PQ" : "scRGB", p.info.name, p.info.hdr_enabled ? "on" : "off",
                     v.paper_white_nits, v.peak_nits, v.black_nits);
}

void shutdown() {
  if (g_p.state == State::RUNNING) {
    stop();
  } else if (g_p.switched_os) {
    set_os_hdr(g_p.switched_on, false);
    g_p.switched_os = false;
  }
  g_p.state = State::OFF;
}

}  // namespace hdr

#else

namespace hdr {
void update(SDL_Window*, bool, int, int) {}
bool present(int) {
  return false;
}
void allow_direct(bool) {}
DirectTarget begin_direct(int, int, bool) {
  return {};
}
void note_overlay(bool) {}
std::string present_tag() {
  return "off";
}
std::string status_line() {
  return "HDR output is Windows only";
}
void shutdown() {}
}  // namespace hdr

#endif
