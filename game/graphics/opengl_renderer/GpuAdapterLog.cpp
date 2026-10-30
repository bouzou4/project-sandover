#ifdef _WIN32
// before the project headers: FileUtil.h undefines FALSE, which the D3D headers use
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h>

#include <d3dkmthk.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#endif

#include <cstring>
#include <string>
#include <vector>

#include "GpuAdapterLog.h"

#include "common/log/log.h"

#include "game/graphics/pipelines/opengl.h"

#include "fmt/format.h"
#include "third-party/SDL/include/SDL3/SDL.h"

namespace {

constexpr GLenum kTextureFreeMemoryAti = 0x87FC;   // GL_ATI_meminfo, 4 ints in KiB
constexpr GLenum kNvxDedicatedVidmem = 0x9047;     // GL_NVX_gpu_memory_info, KiB
constexpr GLenum kNvxCurrentAvailVidmem = 0x9049;  // KiB

void log_gl_memory() {
  // no portable GL query reports clocks; free memory is the only driver state exposed
  if (SDL_GL_ExtensionSupported("GL_ATI_meminfo")) {
    GLint v[4] = {};
    glGetIntegerv(kTextureFreeMemoryAti, v);
    lg::info("GPU adapter: GL free texture memory {} MiB, largest block {} MiB (GL_ATI_meminfo)",
             v[0] / 1024, v[1] / 1024);
  } else if (SDL_GL_ExtensionSupported("GL_NVX_gpu_memory_info")) {
    GLint total = 0, avail = 0;
    glGetIntegerv(kNvxDedicatedVidmem, &total);
    glGetIntegerv(kNvxCurrentAvailVidmem, &avail);
    lg::info("GPU adapter: GL dedicated memory {} MiB, available {} MiB (GL_NVX_gpu_memory_info)",
             total / 1024, avail / 1024);
  }
}

}  // namespace

#ifdef _WIN32

using Microsoft::WRL::ComPtr;

namespace {

constexpr GLenum kGlDeviceLuid = 0x9599;      // GL_EXT_memory_object_win32
constexpr GLenum kGlDeviceNodeMask = 0x959A;  // GL_EXT_memory_object_win32
using PfnGetUnsignedBytev = void(APIENTRY*)(GLenum, GLubyte*);
using PfnOpenFromLuid = NTSTATUS(APIENTRY*)(D3DKMT_OPENADAPTERFROMLUID*);
using PfnQueryInfo = NTSTATUS(APIENTRY*)(const D3DKMT_QUERYADAPTERINFO*);
using PfnClose = NTSTATUS(APIENTRY*)(const D3DKMT_CLOSEADAPTER*);

std::string narrow(const wchar_t* w) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) {
    return {};
  }
  std::string s(n - 1, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

std::string luid_str(const LUID& l) {
  return fmt::format("{:08X}:{:08X}", (u32)l.HighPart, (u32)l.LowPart);
}

bool luid_eq(const LUID& a, const LUID& b) {
  return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

std::string pci_address(const LUID& luid) {
  HMODULE gdi = GetModuleHandleA("gdi32.dll");
  if (!gdi) {
    gdi = LoadLibraryA("gdi32.dll");
  }
  auto open = gdi ? (PfnOpenFromLuid)GetProcAddress(gdi, "D3DKMTOpenAdapterFromLuid") : nullptr;
  auto query = gdi ? (PfnQueryInfo)GetProcAddress(gdi, "D3DKMTQueryAdapterInfo") : nullptr;
  auto close = gdi ? (PfnClose)GetProcAddress(gdi, "D3DKMTCloseAdapter") : nullptr;
  if (!open || !query || !close) {
    return "PCI address unknown";
  }
  D3DKMT_OPENADAPTERFROMLUID o{};
  o.AdapterLuid = luid;
  if (open(&o) != 0) {
    return "PCI address unknown";
  }
  D3DKMT_ADAPTERADDRESS addr{};
  D3DKMT_QUERYADAPTERINFO q{};
  q.hAdapter = o.hAdapter;
  q.Type = KMTQAITYPE_ADAPTERADDRESS;
  q.pPrivateDriverData = &addr;
  q.PrivateDriverDataSize = sizeof(addr);
  NTSTATUS st = query(&q);
  D3DKMT_CLOSEADAPTER c{};
  c.hAdapter = o.hAdapter;
  close(&c);
  if (st != 0) {
    return "PCI address unknown";
  }
  if (addr.BusNumber == 0xFFFFFFFF) {
    return "no PCI address";  // indirect display or other virtual adapter
  }
  return fmt::format("PCI bus {} device {} function {}", addr.BusNumber, addr.DeviceNumber,
                     addr.FunctionNumber);
}

struct Adapter {
  ComPtr<IDXGIAdapter1> a;
  DXGI_ADAPTER_DESC1 d{};
};

std::string describe(const Adapter& ad, int index, int count, int hp_rank) {
  return fmt::format(
      "luid {}, \"{}\", {} MiB dedicated, {}, DXGI adapter {} of {}, "
      "high-performance rank {}",
      luid_str(ad.d.AdapterLuid), narrow(ad.d.Description), ad.d.DedicatedVideoMemory >> 20,
      pci_address(ad.d.AdapterLuid), index, count, hp_rank);
}

}  // namespace

void log_gpu_adapter() {
  LUID gl_luid{};
  GLuint node_mask = 0;
  if (SDL_GL_ExtensionSupported("GL_EXT_memory_object_win32")) {
    if (auto get = (PfnGetUnsignedBytev)SDL_GL_GetProcAddress("glGetUnsignedBytevEXT")) {
      GLubyte b[8] = {};
      get(kGlDeviceLuid, b);
      std::memcpy(&gl_luid, b, sizeof(gl_luid));
      glGetIntegerv(kGlDeviceNodeMask, (GLint*)&node_mask);
    }
  }
  const bool have_luid = gl_luid.LowPart != 0 || gl_luid.HighPart != 0;

  ComPtr<IDXGIFactory6> factory;
  std::vector<Adapter> adapters;
  std::vector<LUID> hp_order;
  if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
    for (UINT i = 0;; ++i) {
      Adapter ad;
      if (FAILED(factory->EnumAdapters1(i, &ad.a))) {
        break;
      }
      ad.a->GetDesc1(&ad.d);
      if (!(ad.d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
        adapters.push_back(ad);
      }
    }
    for (UINT i = 0;; ++i) {
      ComPtr<IDXGIAdapter1> a;
      if (FAILED(factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                     IID_PPV_ARGS(&a)))) {
        break;
      }
      DXGI_ADAPTER_DESC1 d{};
      a->GetDesc1(&d);
      hp_order.push_back(d.AdapterLuid);
    }
  }
  auto hp_rank = [&](const LUID& l) {
    for (size_t i = 0; i < hp_order.size(); ++i) {
      if (luid_eq(hp_order[i], l)) {
        return (int)i;
      }
    }
    return -1;
  };

  const int count = (int)adapters.size();
  int gl_index = -1;
  for (int i = 0; i < count; ++i) {
    if (have_luid && luid_eq(adapters[i].d.AdapterLuid, gl_luid)) {
      gl_index = i;
    }
  }
  if (gl_index >= 0) {
    lg::info("GPU adapter: GL context on {}, node mask {}",
             describe(adapters[gl_index], gl_index, count, hp_rank(gl_luid)), node_mask);
  } else if (have_luid) {
    lg::info("GPU adapter: GL context on luid {} (no DXGI adapter with it)", luid_str(gl_luid));
  } else {
    lg::info("GPU adapter: the GL context reports no LUID (GL_EXT_memory_object_win32)");
  }
  for (int i = 0; i < count; ++i) {
    if (i != gl_index) {
      lg::info("GPU adapter: other: {}",
               describe(adapters[i], i, count, hp_rank(adapters[i].d.AdapterLuid)));
    }
  }

  // the display the window opened on, and the adapter whose output drives it
  SDL_Window* window = SDL_GL_GetCurrentWindow();
  HWND hwnd = window ? (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                                    SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr)
                     : nullptr;
  if (hwnd) {
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    const char* sdl_name = SDL_GetDisplayName(SDL_GetDisplayForWindow(window));
    std::string owner = "no adapter output";
    for (auto& ad : adapters) {
      for (UINT o = 0;; ++o) {
        ComPtr<IDXGIOutput> out;
        if (FAILED(ad.a->EnumOutputs(o, &out))) {
          break;
        }
        DXGI_OUTPUT_DESC od{};
        out->GetDesc(&od);
        if (od.Monitor == mon) {
          owner = fmt::format(
              "output of luid {}{}", luid_str(ad.d.AdapterLuid),
              luid_eq(ad.d.AdapterLuid, gl_luid) ? " (the GL adapter)" : " (not the GL adapter)");
        }
      }
    }
    const RECT& r = mi.rcMonitor;
    lg::info("GPU adapter: window on display \"{}\" ({}, {}x{} at {},{}{}), {}",
             sdl_name ? sdl_name : "?", narrow(mi.szDevice), r.right - r.left, r.bottom - r.top,
             r.left, r.top, (mi.dwFlags & MONITORINFOF_PRIMARY) ? ", primary" : "", owner);
  }
  log_gl_memory();
}

#else

void log_gpu_adapter() {
  log_gl_memory();
}

#endif
