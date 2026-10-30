#if defined(_WIN32) && defined(SANDOVER_DLSS)
#define SANDOVER_DLSS_IMPL 1
// before the project headers: FileUtil.h undefines FALSE, which d3d11.h uses
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winver.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include "nvsdk_ngx.h"
#include "nvsdk_ngx_helpers.h"
#endif

// after the D3D11 headers: its includes undefine FALSE
#include "DlssUpscaler.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "common/util/Timer.h"
#include "common/util/exr.h"

#include "game/graphics/gfx.h"
#include "game/graphics/opengl_renderer/GlQueries.h"
#include "game/graphics/opengl_renderer/PerfTimers.h"
#include "game/graphics/opengl_renderer/Shader.h"

#include "fmt/format.h"
#include "third-party/imgui/imgui.h"

#ifdef SANDOVER_DLSS_IMPL

#include "third-party/SDL/include/SDL3/SDL.h"

using Microsoft::WRL::ComPtr;

namespace {

// perf-opt bit 25: GL queries off the per-frame path (GlQueries.h), as the other post passes
constexpr int kGlq = PERF_OPT_PRESENT2 + 1;

// fixed GUID-like project id for NGX (NVSDK_NGX_ENGINE_TYPE_CUSTOM)
constexpr const char* kProjectId = "8df2af5e-300f-490d-994e-aaa3dc75e036";
constexpr const char* kEngineVersion = "1.0";
constexpr UINT kNvidia = 0x10DE;

// WGL_NV_DX_interop2, loaded at run time
constexpr GLenum kAccessReadOnly = 0x0000;
constexpr GLenum kAccessWriteDiscard = 0x0002;
using PfnDxOpenDevice = HANDLE(WINAPI*)(void*);
using PfnDxCloseDevice = BOOL(WINAPI*)(HANDLE);
using PfnDxRegisterObject = HANDLE(WINAPI*)(HANDLE, void*, GLuint, GLenum, GLenum);
using PfnDxUnregisterObject = BOOL(WINAPI*)(HANDLE, HANDLE);
using PfnDxLockObjects = BOOL(WINAPI*)(HANDLE, GLint, HANDLE*);

// GL_EXT_memory_object_win32: the adapter LUID of the GL context
constexpr GLenum kGlDeviceLuid = 0x9599;
using PfnGetUnsignedBytev = void(APIENTRY*)(GLenum, GLubyte*);

struct Wgl {
  PfnDxOpenDevice open = nullptr;
  PfnDxCloseDevice close = nullptr;
  PfnDxRegisterObject reg = nullptr;
  PfnDxUnregisterObject unreg = nullptr;
  PfnDxLockObjects lock = nullptr;
  PfnDxLockObjects unlock = nullptr;

  bool load() {
    auto p = [](const char* n) { return (void*)SDL_GL_GetProcAddress(n); };
    open = (PfnDxOpenDevice)p("wglDXOpenDeviceNV");
    close = (PfnDxCloseDevice)p("wglDXCloseDeviceNV");
    reg = (PfnDxRegisterObject)p("wglDXRegisterObjectNV");
    unreg = (PfnDxUnregisterObject)p("wglDXUnregisterObjectNV");
    lock = (PfnDxLockObjects)p("wglDXLockObjectsNV");
    unlock = (PfnDxLockObjects)p("wglDXUnlockObjectsNV");
    return open && close && reg && unreg && lock && unlock;
  }
};

/// The DXGI adapter behind the GL context: by LUID when the driver reports it, else the first
/// NVIDIA adapter. Empty when there is none.
ComPtr<IDXGIAdapter1> gl_adapter(std::string* how) {
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
    *how = "no DXGI factory";
    return {};
  }
  LUID luid{};
  bool have_luid = false;
  if (SDL_GL_ExtensionSupported("GL_EXT_memory_object_win32")) {
    if (auto get = (PfnGetUnsignedBytev)SDL_GL_GetProcAddress("glGetUnsignedBytevEXT")) {
      GLubyte b[8] = {};
      get(kGlDeviceLuid, b);
      std::memcpy(&luid, b, sizeof(luid));
      have_luid = luid.LowPart != 0 || luid.HighPart != 0;
    }
  }
  ComPtr<IDXGIAdapter1> a, first_nv;
  for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
    DXGI_ADAPTER_DESC1 d{};
    a->GetDesc1(&d);
    if (have_luid && d.AdapterLuid.LowPart == luid.LowPart &&
        d.AdapterLuid.HighPart == luid.HighPart) {
      *how = "the GL context's LUID";
      return a;
    }
    if (!first_nv && d.VendorId == kNvidia && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
      first_nv = a;
    }
  }
  *how = have_luid ? "no adapter with the GL context's LUID" : "the first NVIDIA adapter";
  return have_luid ? ComPtr<IDXGIAdapter1>() : first_nv;
}

fs::path exe_dir() {
  return fs::path(file_util::get_current_executable_path()).parent_path();
}

std::wstring ngx_data_path() {
  auto p = file_util::get_user_misc_dir(GameVersion::Jak1) / "ngx";
  std::error_code ec;
  fs::create_directories(p, ec);
  return p.wstring();
}

NVSDK_NGX_FeatureDiscoveryInfo discovery(const std::wstring& data_path) {
  NVSDK_NGX_FeatureDiscoveryInfo info{};
  info.SDKVersion = NVSDK_NGX_Version_API;
  info.FeatureID = NVSDK_NGX_Feature_SuperSampling;
  info.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
  info.Identifier.v.ProjectDesc.ProjectId = kProjectId;
  info.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
  info.Identifier.v.ProjectDesc.EngineVersion = kEngineVersion;
  info.ApplicationDataPath = data_path.c_str();
  info.FeatureInfo = nullptr;
  return info;
}

std::string result_str(NVSDK_NGX_Result r) {
  return fmt::format("{:#x}", (u32)r);
}

bool local_runtime() {
  return fs::exists(exe_dir() / "nvngx_dlss.dll");
}

/// What the "unavailable" lines say was tried, in resolution order.
u64 file_version(const fs::path& path) {
  DWORD handle = 0;
  const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
  if (!size) {
    return 0;
  }
  std::vector<char> buf(size);
  VS_FIXEDFILEINFO* fi = nullptr;
  UINT len = 0;
  if (!GetFileVersionInfoW(path.c_str(), 0, size, buf.data()) ||
      !VerQueryValueW(buf.data(), L"\\", (void**)&fi, &len) || !fi) {
    return 0;
  }
  return ((u64)fi->dwFileVersionMS << 32) | fi->dwFileVersionLS;
}

std::string version_str(u64 v) {
  return fmt::format("{}.{}.{}.{}", (v >> 48) & 0xffff, (v >> 32) & 0xffff, (v >> 16) & 0xffff,
                     v & 0xffff);
}

/// The newest nvngx_dlss.dll the driver's NGX store holds as a plain file (the NVIDIA App's
/// override cache, dlss_override/versions/<n>/files/<app>/); empty if none. Diagnostic:
/// SANDOVER_DLSS_NO_STORE=1 skips it.
fs::path store_runtime_dir() {
  if (const char* e = std::getenv("SANDOVER_DLSS_NO_STORE"); e && *e == '1') {
    return {};
  }
  wchar_t pd[MAX_PATH] = {};
  if (!GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH)) {
    return {};
  }
  const fs::path root = fs::path(pd) / "NVIDIA" / "NGX" / "models" / "dlss_override" / "versions";
  std::error_code ec;
  fs::path best;
  u64 best_v = 0;
  for (auto it = fs::recursive_directory_iterator(root, ec);
       !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (it.depth() > 3 || !it->is_regular_file(ec) || it->path().filename() != "nvngx_dlss.dll") {
      continue;
    }
    const u64 v = file_version(it->path());
    if (v > best_v) {
      best_v = v;
      best = it->path().parent_path();
    }
  }
  return best;
}

/// What the "unavailable" lines say was tried, in resolution order.
std::string tried(const fs::path& store) {
  return fmt::format("tried nvngx_dlss.dll beside gk ({}), then the driver's NGX store ({})",
                     local_runtime() ? "present" : "absent",
                     store.empty() ? "no nvngx_dlss.dll there" : "found one");
}

/// NGX's log at init: everything with SANDOVER_DLSS_NGX_LOG=1, else only the lines naming the
/// DLSS snippet it loaded and whether the NVIDIA App's override replaced it.
bool g_ngx_log_all = false;

void NVSDK_CONV ngx_log(const char* msg, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
  std::string s(msg ? msg : "");
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
    s.pop_back();
  }
  if (g_ngx_log_all) {
    lg::info("dlss: ngx: {}", s);
  } else if (s.find("feature dlss snippet:") != std::string::npos ||
             s.find("Feature dlss override enabled") != std::string::npos) {
    const auto at = s.find("] [");
    lg::info("dlss: runtime: {}", at == std::string::npos ? s : s.substr(at + 2));
  }
}

// NGX's over-the-air update of the DLSS snippet (programming guide 2.4): on its own thread, taking
// effect at the next NGX init; never waited on
std::atomic<bool> g_update_started{false};

void start_update() {
  if (g_update_started.exchange(true)) {
    return;
  }
  std::thread([] {
    static const std::wstring data = ngx_data_path();
    const auto info = discovery(data);
    const NVSDK_NGX_Result r = NVSDK_NGX_UpdateFeature(&info.Identifier, info.FeatureID);
    lg::info("dlss: NGX update check {}", result_str(r));
  }).detach();
}

/// The DLSS mode by ratio alone, used only if NGX reports no ranges.
NVSDK_NGX_PerfQuality_Value quality_by_ratio(int rw, int dw) {
  const float r = (float)rw / (float)std::max(1, dw);
  if (r >= 0.99f) {
    return NVSDK_NGX_PerfQuality_Value_DLAA;
  }
  if (r >= 0.62f) {
    return NVSDK_NGX_PerfQuality_Value_MaxQuality;
  }
  if (r >= 0.54f) {
    return NVSDK_NGX_PerfQuality_Value_Balanced;
  }
  if (r >= 0.42f) {
    return NVSDK_NGX_PerfQuality_Value_MaxPerf;
  }
  return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
}

constexpr NVSDK_NGX_PerfQuality_Value kModes[] = {
    NVSDK_NGX_PerfQuality_Value_DLAA, NVSDK_NGX_PerfQuality_Value_MaxQuality,
    NVSDK_NGX_PerfQuality_Value_Balanced, NVSDK_NGX_PerfQuality_Value_MaxPerf,
    NVSDK_NGX_PerfQuality_Value_UltraPerformance};

const char* quality_name(int q) {
  switch (q) {
    case NVSDK_NGX_PerfQuality_Value_DLAA:
      return "DLAA";
    case NVSDK_NGX_PerfQuality_Value_MaxQuality:
      return "quality";
    case NVSDK_NGX_PerfQuality_Value_Balanced:
      return "balanced";
    case NVSDK_NGX_PerfQuality_Value_MaxPerf:
      return "performance";
    case NVSDK_NGX_PerfQuality_Value_UltraPerformance:
      return "ultra performance";
    default:
      return "?";
  }
}

constexpr const char* kVert = R"(#version 410 core
void main() {
  vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// rows flipped: D3D's row 0 is the top of the frame
constexpr const char* kFrag = R"(#version 410 core
uniform sampler2D u_colour;
uniform sampler2D u_depth;
uniform sampler2D u_velocity;
uniform int u_has_velocity;
layout(location = 0) out vec4 o_colour;
layout(location = 1) out float o_depth;
layout(location = 2) out vec2 o_velocity;
void main() {
  int h = textureSize(u_colour, 0).y;
  ivec2 p = ivec2(int(gl_FragCoord.x), h - 1 - int(gl_FragCoord.y));
  o_colour = texelFetch(u_colour, p, 0);
  o_depth = texelFetch(u_depth, p, 0).r;
  o_velocity = u_has_velocity != 0 ? texelFetch(u_velocity, p, 0).xy : vec2(0.0);
}
)";

GLuint compile(GLenum type, const char* src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, nullptr);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[1024] = {};
    glGetShaderInfoLog(s, sizeof(log), nullptr, log);
    lg::error("dlss: input shader: {}", log);
  }
  return s;
}

float half_to_float(u16 h) {
  const u32 s = (u32)(h >> 15) << 31, e = (h >> 10) & 0x1f, m = h & 0x3ff;
  u32 f = e == 0 ? s : e == 31 ? (s | 0x7f800000 | (m << 13)) : (s | ((e + 112) << 23) | (m << 13));
  float r;
  memcpy(&r, &f, 4);
  return r;
}

/// Diagnostic (SANDOVER_DLSS_PROBE=1): what D3D sees in a texture: the mean of the first channel
/// over a sparse grid and the centre texel.
std::string probe_texture(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex) {
  D3D11_TEXTURE2D_DESC td{};
  tex->GetDesc(&td);
  td.Usage = D3D11_USAGE_STAGING;
  td.BindFlags = 0;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  td.MiscFlags = 0;
  ComPtr<ID3D11Texture2D> st;
  if (FAILED(dev->CreateTexture2D(&td, nullptr, &st))) {
    return "staging failed";
  }
  ctx->CopyResource(st.Get(), tex);
  D3D11_MAPPED_SUBRESOURCE ms{};
  if (FAILED(ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &ms))) {
    return "map failed";
  }
  auto first = [&](unsigned x, unsigned y) -> float {
    const u8* row = (const u8*)ms.pData + (size_t)y * ms.RowPitch;
    switch (td.Format) {
      case DXGI_FORMAT_R16G16B16A16_FLOAT:
      case DXGI_FORMAT_R16G16_FLOAT:
        return half_to_float(((const u16*)row)[x * (td.Format == DXGI_FORMAT_R16G16_FLOAT ? 2 : 4)]);
      case DXGI_FORMAT_R32_FLOAT:
        return ((const float*)row)[x];
      case DXGI_FORMAT_R8G8B8A8_UNORM:
        return row[x * 4] / 255.f;
      default:
        return -1.f;
    }
  };
  double sum = 0;
  int n = 0, nonzero = 0;
  for (unsigned y = 0; y < td.Height; y += 37) {
    for (unsigned x = 0; x < td.Width; x += 37) {
      const float v = first(x, y);
      sum += v;
      nonzero += v != 0.f;
      n++;
    }
  }
  const float c = first(td.Width / 2, td.Height / 2);
  ctx->Unmap(st.Get(), 0);
  return fmt::format("{}x{} fmt {} mean {:.4f} nonzero {}/{} centre {:.4f}", td.Width, td.Height,
                     (int)td.Format, sum / std::max(1, n), nonzero, n, c);
}

}  // namespace

struct DlssUpscaler::Impl {
  struct Shared {
    ComPtr<ID3D11Texture2D> tex;
    GLuint gl = 0;
    HANDLE obj = nullptr;
  };
  enum { S_COLOUR, S_DEPTH, S_VELOCITY, S_OUTPUT, S_COUNT };

  Wgl wgl;
  int state = 0;  // 0 not tried, 1 ready, -1 failed
  ComPtr<ID3D11Device> dev;
  ComPtr<ID3D11DeviceContext> ctx;
  HANDLE interop = nullptr;
  bool ngx = false;
  NVSDK_NGX_Parameter* params = nullptr;
  NVSDK_NGX_Handle* feature = nullptr;
  int feature_q = -1;
  int feature_rw = 0, feature_dw = 0;
  int logged_modes_w = 0;
  int probe_left = std::getenv("SANDOVER_DLSS_PROBE") ? 3 : 0;
  bool feature_hdr = false;
  bool failed = false;
  fs::path store;  // the NGX store folder passed to NGX, empty when none

  Shared shared[S_COUNT];
  ComPtr<ID3D11Texture2D> out_uav;  // DLSS's output when a UAV texture cannot be shared
  ComPtr<ID3D11Texture2D> exposure;
  bool locked = false;
  GLuint in_fbo = 0, out_fbo = 0, prog = 0, vao = 0, sampler = 0;
  GLint u_colour = -1, u_depth = -1, u_velocity = -1, u_has_velocity = -1;
  Fbo display;
  int rw = 0, rh = 0, dw = 0, dh = 0;
  bool hdr = false;

  unsigned jitter_index = 0;
  float jitter[2] = {0.f, 0.f};
  int phases = 0;
  int resets = 0;
  u64 evals = 0;
  std::string status = "not run";

  // D3D timestamps of the evaluate call, read a few frames late
  struct Ts {
    ComPtr<ID3D11Query> disjoint, begin, end;
    bool pending = false;
  };
  Ts ts[4];
  int ts_next = 0;
  float d3d_ms = 0.f, sync_cpu_ms = 0.f;

  void lock_all(bool gl) {
    HANDLE objs[S_COUNT];
    int n = 0;
    for (auto& s : shared) {
      if (s.obj) {
        objs[n++] = s.obj;
      }
    }
    if (!n || locked == gl) {
      return;
    }
    if (!(gl ? wgl.lock : wgl.unlock)(interop, n, objs)) {
      lg::error("dlss: wglDX{}ObjectsNV failed ({:#x})", gl ? "Lock" : "Unlock", GetLastError());
    }
    locked = gl;
  }

  void free_targets() {
    if (feature) {
      NVSDK_NGX_D3D11_ReleaseFeature(feature);
      feature = nullptr;
    }
    feature_q = -1;
    lock_all(false);
    for (auto& s : shared) {
      if (s.obj) {
        wgl.unreg(interop, s.obj);
      }
      if (s.gl) {
        glDeleteTextures(1, &s.gl);
      }
      s = Shared();
    }
    locked = false;
    out_uav.Reset();
    if (in_fbo) {
      glDeleteFramebuffers(1, &in_fbo);
      in_fbo = 0;
    }
    if (out_fbo) {
      glDeleteFramebuffers(1, &out_fbo);
      out_fbo = 0;
    }
    display.clear();
    rw = rh = dw = dh = 0;
  }

  bool init() {
    if (!wgl.load()) {
      lg::warn("dlss: unavailable (no WGL_NV_DX_interop2)");
      return false;
    }
    std::string how;
    auto adapter = gl_adapter(&how);
    if (!adapter) {
      lg::warn("dlss: unavailable ({})", how);
      return false;
    }
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 2,
                                 D3D11_SDK_VERSION, &dev, nullptr, &ctx))) {
      lg::error("dlss: D3D11CreateDevice failed");
      return false;
    }
    interop = wgl.open(dev.Get());
    if (!interop) {
      lg::error("dlss: wglDXOpenDeviceNV failed ({:#x})", GetLastError());
      return false;
    }
    const std::wstring data = ngx_data_path();
    NVSDK_NGX_FeatureCommonInfo common{};
    if (const char* e = std::getenv("SANDOVER_DLSS_NGX_LOG"); e && *e == '1') {
      g_ngx_log_all = true;
    }
    common.LoggingInfo.LoggingCallback = ngx_log;
    common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_VERBOSE;
    common.LoggingInfo.DisableOtherLoggingSinks = true;
    // the game folder first (NGX's default); without a DLL there, the store's newest signed copy
    store = local_runtime() ? fs::path() : store_runtime_dir();
    const std::wstring store_w = store.wstring();
    const wchar_t* paths[1] = {store_w.c_str()};
    if (!store.empty()) {
      common.PathListInfo.Path = paths;
      common.PathListInfo.Length = 1;
      lg::info("dlss: no nvngx_dlss.dll beside gk; NGX path {}", store.string());
    }
    NVSDK_NGX_Result r = NVSDK_NGX_D3D11_Init_with_ProjectID(
        kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kEngineVersion, data.c_str(), dev.Get(), &common);
    if (NVSDK_NGX_FAILED(r)) {
      lg::error("dlss: NGX init failed ({})", result_str(r));
      return false;
    }
    ngx = true;
    r = NVSDK_NGX_D3D11_GetCapabilityParameters(&params);
    if (NVSDK_NGX_FAILED(r) || !params) {
      lg::error("dlss: NGX capability parameters failed ({})", result_str(r));
      return false;
    }
    int available = 0, needs_driver = 0;
    NVSDK_NGX_Parameter_GetI(params, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    NVSDK_NGX_Parameter_GetI(params, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver,
                             &needs_driver);
    if (!available) {
      int init_result = 0;
      NVSDK_NGX_Parameter_GetI(params, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult,
                               &init_result);
      lg::warn("dlss: unavailable ({}: NGX reports no super sampling{}, init result {:#x})", tried(store),
               needs_driver ? ", the driver needs an update" : "", (u32)init_result);
      return false;
    }
    NVSDK_NGX_Parameter_SetI(params, NVSDK_NGX_Parameter_FreeMemOnReleaseFeature, 1);

    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = 1;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const float one = 1.f;
    D3D11_SUBRESOURCE_DATA sd{&one, sizeof(one), 0};
    if (FAILED(dev->CreateTexture2D(&td, &sd, &exposure))) {
      lg::error("dlss: exposure texture failed");
      return false;
    }
    for (auto& t : ts) {
      D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
      dev->CreateQuery(&qd, &t.disjoint);
      qd.Query = D3D11_QUERY_TIMESTAMP;
      dev->CreateQuery(&qd, &t.begin);
      dev->CreateQuery(&qd, &t.end);
    }

    GLuint vs = compile(GL_VERTEX_SHADER, kVert);
    GLuint fs = compile(GL_FRAGMENT_SHADER, kFrag);
    prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
      lg::error("dlss: input program did not link");
      return false;
    }
    u_colour = glGetUniformLocation(prog, "u_colour");
    u_depth = glGetUniformLocation(prog, "u_depth");
    u_velocity = glGetUniformLocation(prog, "u_velocity");
    u_has_velocity = glGetUniformLocation(prog, "u_has_velocity");
    glGenVertexArrays(1, &vao);
    // texelFetch still needs complete textures: the scene's have mip filters and no mips
    glGenSamplers(1, &sampler);
    glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    lg::info("dlss: D3D11 device on {}, NGX ready", how);
    return true;
  }

  bool share(Shared& s, int w, int h, DXGI_FORMAT fmt, UINT bind, GLenum access) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = bind;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &s.tex))) {
      lg::error("dlss: CreateTexture2D {}x{} format {} failed", w, h, (int)fmt);
      return false;
    }
    glGenTextures(1, &s.gl);
    s.obj = wgl.reg(interop, s.tex.Get(), s.gl, GL_TEXTURE_2D, access);
    if (!s.obj) {
      glDeleteTextures(1, &s.gl);
      s = Shared();
      return false;
    }
    return true;
  }

  bool alloc(int rw_, int rh_, int dw_, int dh_, bool hdr_) {
    if (rw == rw_ && rh == rh_ && dw == dw_ && dh == dh_ && hdr == hdr_ && display.valid) {
      return true;
    }
    free_targets();
    const DXGI_FORMAT colour =
        hdr_ ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
    const UINT in_bind = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    bool ok = share(shared[S_COLOUR], rw_, rh_, colour, in_bind, kAccessWriteDiscard) &&
              share(shared[S_DEPTH], rw_, rh_, DXGI_FORMAT_R32_FLOAT, in_bind,
                    kAccessWriteDiscard) &&
              share(shared[S_VELOCITY], rw_, rh_, DXGI_FORMAT_R16G16_FLOAT, in_bind,
                    kAccessWriteDiscard);
    if (ok && !share(shared[S_OUTPUT], dw_, dh_, colour,
                     D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, kAccessReadOnly)) {
      // DLSS writes a D3D-only UAV texture, copied into a shareable one after evaluate
      lg::warn("dlss: a UAV output cannot be shared; output through a copy");
      D3D11_TEXTURE2D_DESC td{};
      td.Width = dw_;
      td.Height = dh_;
      td.MipLevels = td.ArraySize = 1;
      td.Format = colour;
      td.SampleDesc.Count = 1;
      td.Usage = D3D11_USAGE_DEFAULT;
      td.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
      ok = SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &out_uav)) &&
           share(shared[S_OUTPUT], dw_, dh_, colour, in_bind, kAccessReadOnly);
    }
    if (!ok) {
      lg::error("dlss: shared targets failed ({:#x})", GetLastError());
      free_targets();
      return false;
    }
    lock_all(true);

    glGenFramebuffers(1, &in_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, in_fbo);
    for (int i = 0; i < 3; i++) {
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D,
                             shared[S_COLOUR + i].gl, 0);
    }
    const GLenum draws[3] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2};
    glDrawBuffers(3, draws);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      lg::error("dlss: incomplete input framebuffer");
      ok = false;
    }
    glGenFramebuffers(1, &out_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, out_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, shared[S_OUTPUT].gl,
                           0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      lg::error("dlss: incomplete output framebuffer");
      ok = false;
    }

    // display target: the output, with depth and stencil for the text buckets
    display = Fbo();
    glGenFramebuffers(1, &display.fbo_id);
    glBindFramebuffer(GL_FRAMEBUFFER, display.fbo_id);
    GLuint tex, z;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, hdr_ ? GL_RGBA16F : GL_RGBA8, dw_, dh_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glGenTextures(1, &z);
    glBindTexture(GL_TEXTURE_2D, z);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH24_STENCIL8, dw_, dh_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, z, 0);
    const GLenum draw = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &draw);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      lg::error("dlss: incomplete display framebuffer ({}x{})", dw_, dh_);
      ok = false;
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    display.tex_id = tex;
    display.zbuf_stencil_id = z;
    display.zbuf_is_texture = true;
    display.valid = true;
    display.multisample_count = 1;
    display.width = dw_;
    display.height = dh_;
    if (!ok) {
      free_targets();
      return false;
    }
    rw = rw_;
    rh = rh_;
    dw = dw_;
    dh = dh_;
    hdr = hdr_;
    lg::info("dlss: targets {}x{} -> {}x{} {}{}", rw, rh, dw, dh, hdr ? "RGBA16F" : "RGBA8",
             out_uav ? ", output copied" : "");
    return true;
  }

  /// The DLSS mode for the render width: of the modes whose render width range at this display
  /// size (NGX optimal settings) contains it, the one whose optimal width is nearest. The render
  /// size is passed as is: NGX accepts any size inside the mode's range.
  int quality_for() {
    int best = -1;
    unsigned best_d = ~0u;
    std::string table;
    for (const auto q : kModes) {
      unsigned ow = 0, oh = 0, xw = 0, xh = 0, nw = 0, nh = 0;
      float sharp = 0.f;
      if (NVSDK_NGX_FAILED(NGX_DLSS_GET_OPTIMAL_SETTINGS(params, dw, dh, q, &ow, &oh, &xw, &xh,
                                                         &nw, &nh, &sharp)) ||
          !ow) {
        continue;
      }
      xw = xw ? xw : ow;
      nw = nw ? nw : ow;
      table += fmt::format("{}{} {} ({}..{})", table.empty() ? "" : ", ", quality_name(q), ow, nw,
                           xw);
      const unsigned d = (unsigned)std::abs((int)ow - rw);
      if ((unsigned)rw >= nw && (unsigned)rw <= xw && d < best_d) {
        best = q;
        best_d = d;
      }
    }
    if (dw != logged_modes_w) {
      lg::info("dlss: modes at {}x{}: {}", dw, dh, table.empty() ? "none reported" : table);
      logged_modes_w = dw;
    }
    if (best < 0) {
      best = quality_by_ratio(rw, dw);
      lg::warn("dlss: render width {} in no mode's range; {} by ratio", rw, quality_name(best));
    }
    return best;
  }

  bool ensure_feature() {
    const int q = feature && rw == feature_rw && dw == feature_dw ? feature_q : quality_for();
    if (feature && q == feature_q && hdr == feature_hdr) {
      return true;
    }
    if (feature) {
      NVSDK_NGX_D3D11_ReleaseFeature(feature);
      feature = nullptr;
    }
    // NVIDIA's own per-mode defaults in this SDK, pinned so a driver-side default change does
    // not move the look: transformer K for DLAA and quality, M for performance, L below.
    NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
                              NVSDK_NGX_DLSS_Hint_Render_Preset_K);
    NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                              NVSDK_NGX_DLSS_Hint_Render_Preset_K);
    NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
                              NVSDK_NGX_DLSS_Hint_Render_Preset_K);
    NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                              NVSDK_NGX_DLSS_Hint_Render_Preset_M);
    NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
                              NVSDK_NGX_DLSS_Hint_Render_Preset_L);
    NVSDK_NGX_DLSS_Create_Params cp{};
    cp.Feature.InWidth = rw;
    cp.Feature.InHeight = rh;
    cp.Feature.InTargetWidth = dw;
    cp.Feature.InTargetHeight = dh;
    cp.Feature.InPerfQualityValue = (NVSDK_NGX_PerfQuality_Value)q;
    cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                              NVSDK_NGX_DLSS_Feature_Flags_DepthInverted |
                              (hdr ? NVSDK_NGX_DLSS_Feature_Flags_IsHDR : 0);
    cp.InEnableOutputSubrects = false;
    const NVSDK_NGX_Result r = NGX_D3D11_CREATE_DLSS_EXT(ctx.Get(), &feature, params, &cp);
    lg::info("dlss: feature {}x{} -> {}x{} {} ({}), {}, result {}", rw, rh, dw, dh,
             quality_name(q), q == NVSDK_NGX_PerfQuality_Value_MaxPerf                ? "preset M"
             : q == NVSDK_NGX_PerfQuality_Value_UltraPerformance ? "preset L"
                                                                 : "preset K",
             hdr ? "HDR" : "LDR", result_str(r));
    if (NVSDK_NGX_FAILED(r) || !feature) {
      lg::warn("dlss: unavailable ({}: feature creation {})", tried(store), result_str(r));
      feature = nullptr;
      return false;
    }
    feature_q = q;
    feature_hdr = hdr;
    feature_rw = rw;
    feature_dw = dw;
    return true;
  }

  void read_timestamps() {
    for (auto& t : ts) {
      if (!t.pending) {
        continue;
      }
      D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
      UINT64 a = 0, b = 0;
      if (ctx->GetData(t.disjoint.Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
          ctx->GetData(t.begin.Get(), &a, sizeof(a), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
          ctx->GetData(t.end.Get(), &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
        continue;
      }
      t.pending = false;
      if (!dj.Disjoint && dj.Frequency) {
        d3d_ms = (float)((double)(b - a) * 1000.0 / (double)dj.Frequency);
        perf::count(perf::Counter::DLSS_D3D_MS, d3d_ms);
      }
    }
  }

  void shutdown() {
    free_targets();
    if (params) {
      NVSDK_NGX_D3D11_DestroyParameters(params);
      params = nullptr;
    }
    if (ngx) {
      NVSDK_NGX_D3D11_Shutdown1(dev.Get());
      ngx = false;
    }
    if (interop) {
      wgl.close(interop);
      interop = nullptr;
    }
    if (prog) {
      glDeleteProgram(prog);
      prog = 0;
    }
    if (vao) {
      glDeleteVertexArrays(1, &vao);
      vao = 0;
    }
    if (sampler) {
      glDeleteSamplers(1, &sampler);
      sampler = 0;
    }
    exposure.Reset();
    for (auto& t : ts) {
      t = Ts();
    }
    ctx.Reset();
    dev.Reset();
  }
};

DlssUpscaler::DlssUpscaler() : m(std::make_unique<Impl>()) {}

DlssUpscaler::~DlssUpscaler() {
  m->shutdown();
}

bool DlssUpscaler::context_supported() {
  Wgl wgl;
  if (!wgl.load()) {
    lg::info("dlss: unavailable (no WGL_NV_DX_interop2)");
    return false;
  }
  std::string how;
  auto adapter = gl_adapter(&how);
  DXGI_ADAPTER_DESC1 d{};
  if (!adapter || FAILED(adapter->GetDesc1(&d)) || d.VendorId != kNvidia) {
    lg::info("dlss: unavailable (not an NVIDIA adapter: {})", how);
    return false;
  }
  const std::wstring data = ngx_data_path();
  const auto info = discovery(data);
  NVSDK_NGX_FeatureRequirement req{};
  const NVSDK_NGX_Result r = NVSDK_NGX_D3D11_GetFeatureRequirements(adapter.Get(), &info, &req);
  // the D3D11 query is not implemented by every driver (0xbad00012): then NGX init decides
  if (r == NVSDK_NGX_Result_FAIL_NotImplemented) {
    lg::info("dlss: feature requirements query not implemented by the driver; NGX init decides");
  } else if (NVSDK_NGX_FAILED(r) ||
             req.FeatureSupported != NVSDK_NGX_FeatureSupportResult_Supported) {
    lg::info("dlss: unavailable (feature requirements: result {}, unsupported bits {:#x})",
             result_str(r), (u32)req.FeatureSupported);
    return false;
  }
  lg::info("dlss: requirements met ({}); nvngx_dlss.dll beside gk: {}", how,
           local_runtime() ? "present" : "absent, the driver's NGX store is next");
  start_update();
  return true;
}

bool DlssUpscaler::probe() {
  // NGX init and its capability parameters: the feature resolves (beside gk, else the store)
  return ready();
}

bool DlssUpscaler::ready() {
  if (m->state == 0) {
    m->state = m->init() ? 1 : -1;
    if (m->state < 0) {
      m->shutdown();
      m->status = "unavailable (see the log)";
    }
  }
  return m->state == 1 && !m->failed;
}

void DlssUpscaler::begin_frame(int rw, int rh, int dw) {
  const int phases = scene_jitter::phase_count(rw, dw);
  if (phases != m->phases) {
    m->phases = phases;
    m->jitter_index = 0;
  }
  float jx, jy;
  scene_jitter::halton_offset((int)(m->jitter_index++ % (unsigned)phases), &jx, &jy);
  m->jitter[0] = jx;
  m->jitter[1] = jy;
  // the content's offset in GL texel-grid pixels (+y up)
  scene_jitter::set(2.f * jx / (float)rw, 2.f * jy / (float)rh);
}

bool DlssUpscaler::run(SharedRenderState* rs,
                       ScopedProfilerNode& /*prof*/,
                       const Inputs& in,
                       int dw,
                       int dh) {
  auto& d = *m;
  const int rw = rs->render_fb_w, rh = rs->render_fb_h;
  static glq::SavedState saved("dlss", kGlq, 0x7, 0);
  saved.capture();
  const bool reset = in.reset || !in.velocity || d.feature == nullptr;
  if (!d.alloc(rw, rh, dw, dh, in.hdr)) {
    d.failed = true;
    d.status = "shared targets failed (see the log)";
    saved.restore();
    scene_jitter::set(0.f, 0.f);
    return false;
  }
  if (!d.ensure_feature()) {
    // this frame unfiltered; FSR 3.1 from the next
    d.failed = true;
    d.status = "feature creation failed (see the log)";
    glBindFramebuffer(GL_READ_FRAMEBUFFER, rs->render_fb);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, d.display.fbo_id);
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0, 0, rw, rh, 0, 0, dw, dh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    finish(rs, saved, dw, dh);
    return false;
  }

  const bool probing = d.probe_left > 0 && d.evals >= 300 && d.evals % 100 == 0;
  if (probing) {
    float px[4] = {};
    glBindFramebuffer(GL_READ_FRAMEBUFFER, rs->render_fb);
    glReadPixels(rw / 2, rh / 2, 1, 1, GL_RGBA, GL_FLOAT, px);
    lg::info("dlss: probe GL scene centre {:.4f} {:.4f} {:.4f}, colour tex {}", px[0], px[1], px[2],
             in.colour);
  }
  {
    perf::Scope ps(perf::Group::DLSS_INPUTS);
    glBindFramebuffer(GL_FRAMEBUFFER, d.in_fbo);
    glViewport(0, 0, rw, rh);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(d.prog);
    const unsigned tex[3] = {in.colour, in.depth, in.velocity};
    for (int i = 0; i < 3; i++) {
      glActiveTexture(GL_TEXTURE0 + i);
      glBindTexture(GL_TEXTURE_2D, tex[i]);
      glBindSampler(i, d.sampler);
    }
    glUniform1i(d.u_colour, 0);
    glUniform1i(d.u_depth, 1);
    glUniform1i(d.u_velocity, 2);
    glUniform1i(d.u_has_velocity, in.velocity ? 1 : 0);
    glBindVertexArray(d.vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    for (int i = 0; i < 3; i++) {
      glBindSampler(i, 0);
    }
    if (probing) {
      float px[4] = {};
      glReadBuffer(GL_COLOR_ATTACHMENT0);
      glReadPixels(rw / 2, rh / 2, 1, 1, GL_RGBA, GL_FLOAT, px);
      lg::info("dlss: probe GL shared colour centre {:.4f} {:.4f} {:.4f}, error {:#x}, fbo {}",
               px[0], px[1], px[2], glGetError(),
               glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    }
  }

  NVSDK_NGX_Result r;
  {
    perf::Scope ps(perf::Group::DLSS_EVAL);
    Timer cpu;
    d.read_timestamps();
    // GL's commands on the shared textures complete before D3D reads them, and D3D's before GL
    d.lock_all(false);
    NVSDK_NGX_D3D11_DLSS_Eval_Params e{};
    e.Feature.pInColor = d.shared[Impl::S_COLOUR].tex.Get();
    e.Feature.pInOutput = d.out_uav ? d.out_uav.Get() : d.shared[Impl::S_OUTPUT].tex.Get();
    e.pInDepth = d.shared[Impl::S_DEPTH].tex.Get();
    e.pInMotionVectors = d.shared[Impl::S_VELOCITY].tex.Get();
    // the content offset in DLSS's top-down pixel space; bits of dlss_jitter_sign negate an axis
    const int sign = Gfx::g_global_settings.dlss_jitter_sign;
    e.InJitterOffsetX = (sign & 1) ? -d.jitter[0] : d.jitter[0];
    e.InJitterOffsetY = (sign & 2) ? d.jitter[1] : -d.jitter[1];
    e.InRenderSubrectDimensions = {(unsigned)rw, (unsigned)rh};
    e.InReset = reset ? 1 : 0;
    e.InMVScaleX = 1.f;
    e.InMVScaleY = -1.f;
    e.pInExposureTexture = d.hdr ? d.exposure.Get() : nullptr;
    e.InPreExposure = 1.f;
    e.InExposureScale = 1.f;
    e.InFrameTimeDeltaInMsec = in.frame_ms;
    auto& t = d.ts[d.ts_next];
    const bool timed = !t.pending;
    if (timed) {
      d.ctx->Begin(t.disjoint.Get());
      d.ctx->End(t.begin.Get());
    }
    r = NGX_D3D11_EVALUATE_DLSS_EXT(d.ctx.Get(), d.feature, d.params, &e);
    if (timed) {
      d.ctx->End(t.end.Get());
      d.ctx->End(t.disjoint.Get());
      t.pending = true;
      d.ts_next = (d.ts_next + 1) % 4;
    }
    if (d.out_uav) {
      d.ctx->CopyResource(d.shared[Impl::S_OUTPUT].tex.Get(), d.out_uav.Get());
    }
    if (probing) {
      d.probe_left--;
      lg::info("dlss: probe colour {}", probe_texture(d.dev.Get(), d.ctx.Get(), d.shared[Impl::S_COLOUR].tex.Get()));
      lg::info("dlss: probe depth {}", probe_texture(d.dev.Get(), d.ctx.Get(), d.shared[Impl::S_DEPTH].tex.Get()));
      lg::info("dlss: probe velocity {}", probe_texture(d.dev.Get(), d.ctx.Get(), d.shared[Impl::S_VELOCITY].tex.Get()));
      lg::info("dlss: probe output {}", probe_texture(d.dev.Get(), d.ctx.Get(), d.shared[Impl::S_OUTPUT].tex.Get()));
    }
    d.ctx->Flush();
    d.lock_all(true);
    d.sync_cpu_ms = (float)cpu.getMs();
    perf::count(perf::Counter::DLSS_SYNC_CPU_MS, d.sync_cpu_ms);
  }
  if (NVSDK_NGX_FAILED(r)) {
    lg::error("dlss: evaluate failed ({}); FSR 3.1 in its place", result_str(r));
    d.failed = true;
    d.status = fmt::format("evaluate failed ({})", result_str(r));
  }
  d.evals++;
  if (reset) {
    d.resets++;
  }

  {
    perf::Scope ps(perf::Group::DLSS_OUTPUT);
    // flipped back: D3D's top row is GL's last
    glBindFramebuffer(GL_READ_FRAMEBUFFER, d.out_fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, d.display.fbo_id);
    glBlitFramebuffer(0, 0, dw, dh, 0, dh, dw, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  }
  if (!d.failed) {
    d.status = fmt::format("{}x{} -> {}x{} {}, jitter ({:.3f}, {:.3f}) of {} phases, {} resets",
                           rw, rh, dw, dh, quality_name(d.feature_q), d.jitter[0], d.jitter[1],
                           d.phases, d.resets);
  }
  finish(rs, saved, dw, dh);
  return !d.failed;
}

void DlssUpscaler::finish(SharedRenderState* rs, const glq::SavedState& saved, int dw, int dh) {
  auto& d = *m;
  glBindFramebuffer(GL_FRAMEBUFFER, d.display.fbo_id);
  glDepthMask(GL_TRUE);
  glStencilMask(0xFF);
  glClearBufferfi(GL_DEPTH_STENCIL, 0, 0.f, 0);
  saved.restore();
  glBindFramebuffer(GL_FRAMEBUFFER, d.display.fbo_id);
  glViewport(0, 0, dw, dh);
  rs->render_fb = d.display.fbo_id;
  rs->render_fb_w = dw;
  rs->render_fb_h = dh;
  rs->render_fb_depth_tex = *d.display.zbuf_stencil_id;
  scene_jitter::set(0.f, 0.f);
}

void DlssUpscaler::dump_output(const std::string& stem, u64 frame) {
  const auto& d = *m;
  if (!d.display.valid) {
    return;
  }
  std::vector<float> px((size_t)d.dw * d.dh * 4);
  GLint prev = 0;
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, d.display.fbo_id);
  glReadBuffer(GL_COLOR_ATTACHMENT0);
  glPixelStorei(GL_PACK_ALIGNMENT, 4);
  glReadPixels(0, 0, d.dw, d.dh, GL_RGBA, GL_FLOAT, px.data());
  glBindFramebuffer(GL_READ_FRAMEBUFFER, prev);
  exr::StringAttrs attrs;
  attrs["frame"] = std::to_string(frame);
  attrs["content"] = "DLSS output, display-encoded, before the text buckets";
  attrs["render"] = fmt::format("{}x{}", d.rw, d.rh);
  attrs["mode"] = quality_name(d.feature_q);
  exr::write_rgb_half(stem + "_dlss.exr", px.data(), d.dw, d.dh, 4, true, attrs);
}

Fbo* DlssUpscaler::display() {
  return &m->display;
}

void DlssUpscaler::release() {
  if (m->rw || m->feature) {
    m->free_targets();
  }
  m->failed = false;
}

void DlssUpscaler::draw_debug_window() {
  auto& gs = Gfx::g_global_settings;
  ImGui::TextUnformatted(Gfx::g_dlss_supported ? "available" : "unavailable");
  ImGui::TextUnformatted(m->status.c_str());
  ImGui::Text("evaluate %.3f ms (D3D), sync + submit %.3f ms (CPU)", m->d3d_ms, m->sync_cpu_ms);
  bool nx = gs.dlss_jitter_sign & 1, ny = gs.dlss_jitter_sign & 2;
  ImGui::Checkbox("Negate jitter x", &nx);
  ImGui::Checkbox("Negate jitter y", &ny);
  gs.dlss_jitter_sign = (nx ? 1 : 0) | (ny ? 2 : 0);
}

#else  // no DLSS in this build

struct DlssUpscaler::Impl {
  Fbo display;
};

DlssUpscaler::DlssUpscaler() : m(std::make_unique<Impl>()) {}
DlssUpscaler::~DlssUpscaler() = default;

bool DlssUpscaler::context_supported() {
  lg::info("dlss: not in this build");
  return false;
}

bool DlssUpscaler::ready() {
  return false;
}

bool DlssUpscaler::probe() {
  return false;
}

void DlssUpscaler::begin_frame(int, int, int) {}

bool DlssUpscaler::run(SharedRenderState*, ScopedProfilerNode&, const Inputs&, int, int) {
  return false;
}

void DlssUpscaler::dump_output(const std::string&, u64) {}

Fbo* DlssUpscaler::display() {
  return &m->display;
}

void DlssUpscaler::release() {}

void DlssUpscaler::draw_debug_window() {
  ImGui::TextUnformatted("not in this build");
}

#endif
