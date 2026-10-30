#ifdef _WIN32
// before the project headers: FileUtil.h undefines FALSE, which the D3D headers use
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d11_4.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#endif

#include "D3d12Bridge.h"

#include <cstdlib>
#include <cstring>
#include <vector>

#include "common/log/log.h"

#include "game/graphics/pipelines/opengl.h"

#include "fmt/format.h"

#ifdef _WIN32

#include "third-party/SDL/include/SDL3/SDL.h"

using Microsoft::WRL::ComPtr;

namespace {

constexpr int kRing = 3;            // command lists in flight
constexpr int kTimerSlots = 4;      // per timer: spans in flight
constexpr int kMaxTimers = 16;
constexpr GLenum kGlDeviceLuid = 0x9599;  // GL_EXT_memory_object_win32
using PfnGetUnsignedBytev = void(APIENTRY*)(GLenum, GLubyte*);

using PfnDxOpenDevice = HANDLE(WINAPI*)(void*);
using PfnDxCloseDevice = BOOL(WINAPI*)(HANDLE);
using PfnDxRegisterObject = HANDLE(WINAPI*)(HANDLE, void*, GLuint, GLenum, GLenum);
using PfnDxUnregisterObject = BOOL(WINAPI*)(HANDLE, HANDLE);
using PfnDxLockObjects = BOOL(WINAPI*)(HANDLE, GLint, HANDLE*);

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

std::string narrow(const wchar_t* w) {
  std::string s;
  for (; *w; w++) {
    s += *w < 128 ? (char)*w : '?';
  }
  return s;
}

}  // namespace

struct D3d12Bridge::Impl {
  bool ok = false;
  int users = 0;
  std::string status = "not initialised";
  Wgl wgl;
  ComPtr<IDXGIAdapter1> adapter;
  ComPtr<ID3D12Device> dev;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12Fence> fence;
  HANDLE event = nullptr;
  u64 fence_value = 0;
  ComPtr<ID3D12CommandAllocator> alloc[kRing];
  ComPtr<ID3D12GraphicsCommandList> list[kRing];
  u64 list_fence[kRing] = {};
  int ring = 0;
  bool recording = false;
  ComPtr<ID3D11Device> d11;
  ComPtr<ID3D11DeviceContext> d11_ctx;
  ComPtr<ID3D11On12Device> on12;
  HANDLE interop = nullptr;

  // timestamps: kMaxTimers x kTimerSlots x (begin, end)
  ComPtr<ID3D12QueryHeap> ts_heap;
  ComPtr<ID3D12Resource> ts_readback;
  u64 ts_freq = 0;
  int timers = 0;
  struct Slot {
    u64 fence = 0;
    bool pending = false;
  };
  struct TimerState {
    Slot slots[kTimerSlots];
    int next = 0;
    int open = -1;
    float last_ms = -1.f;
  };
  TimerState timer[kMaxTimers];

  bool fail(const std::string& why) {
    status = why;
    lg::warn("d3d12: unavailable ({})", why);
    teardown();
    return false;
  }

  void teardown() {
    if (fence && fence_value) {
      // the queue drains before anything it uses goes away
      if (fence->GetCompletedValue() < fence_value) {
        fence->SetEventOnCompletion(fence_value, event);
        WaitForSingleObject(event, 2000);
      }
    }
    if (interop) {
      wgl.close(interop);
      interop = nullptr;
    }
    on12.Reset();
    if (d11_ctx) {
      d11_ctx->ClearState();
      d11_ctx->Flush();
    }
    d11_ctx.Reset();
    d11.Reset();
    for (int i = 0; i < kRing; i++) {
      list[i].Reset();
      alloc[i].Reset();
      list_fence[i] = 0;
    }
    ts_heap.Reset();
    ts_readback.Reset();
    timers = 0;
    for (auto& t : timer) {
      t = TimerState();
    }
    fence.Reset();
    queue.Reset();
    dev.Reset();
    adapter.Reset();
    if (event) {
      CloseHandle(event);
      event = nullptr;
    }
    fence_value = 0;
    recording = false;
    ok = false;
  }

  bool find_adapter() {
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
      status = "no DXGI factory";
      return false;
    }
    LUID luid{};
    if (SDL_GL_ExtensionSupported("GL_EXT_memory_object_win32")) {
      if (auto get = (PfnGetUnsignedBytev)SDL_GL_GetProcAddress("glGetUnsignedBytevEXT")) {
        GLubyte b[8] = {};
        get(kGlDeviceLuid, b);
        std::memcpy(&luid, b, sizeof(luid));
      }
    }
    if (luid.LowPart == 0 && luid.HighPart == 0) {
      status = "the GL context reports no adapter LUID (GL_EXT_memory_object_win32)";
      return false;
    }
    if (FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)))) {
      status = "no DXGI adapter with the GL context's LUID";
      return false;
    }
    return true;
  }

  bool init() {
    if (const char* e = std::getenv("SANDOVER_D3D12_OFF"); e && *e == '1') {
      return fail("SANDOVER_D3D12_OFF=1");
    }
    if (!wgl.load()) {
      return fail("no WGL_NV_DX_interop2");
    }
    if (!find_adapter()) {
      return fail(status);
    }
    DXGI_ADAPTER_DESC1 ad{};
    adapter->GetDesc1(&ad);
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev)))) {
      return fail("D3D12CreateDevice at feature level 12_0 failed");
    }
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) ||
        FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
      return fail("queue or fence creation failed");
    }
    queue->SetName(L"sandover direct");
    event = CreateEventW(nullptr, 0, 0, nullptr);
    for (int i = 0; i < kRing; i++) {
      if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             IID_PPV_ARGS(&alloc[i]))) ||
          FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[i].Get(), nullptr,
                                        IID_PPV_ARGS(&list[i])))) {
        return fail("command list creation failed");
      }
      list[i]->Close();
    }
    IUnknown* queues[] = {queue.Get()};
    if (FAILED(D3D11On12CreateDevice(dev.Get(), 0, nullptr, 0, queues, 1, 0, &d11, &d11_ctx,
                                     nullptr)) ||
        FAILED(d11.As(&on12))) {
      return fail("D3D11On12CreateDevice failed");
    }
    interop = wgl.open(d11.Get());
    if (!interop) {
      return fail(fmt::format("wglDXOpenDeviceNV refused the D3D11On12 device ({:#x})",
                              GetLastError()));
    }
    D3D12_QUERY_HEAP_DESC hd{};
    hd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    hd.Count = kMaxTimers * kTimerSlots * 2;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = hd.Count * sizeof(u64);
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(dev->CreateQueryHeap(&hd, IID_PPV_ARGS(&ts_heap))) ||
        FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            IID_PPV_ARGS(&ts_readback))) ||
        FAILED(queue->GetTimestampFrequency(&ts_freq))) {
      // timers are optional
      ts_heap.Reset();
      ts_readback.Reset();
      ts_freq = 0;
    }
    status = fmt::format("D3D12 device on {} (the GL context's adapter), D3D11On12 bridge",
                         narrow(ad.Description));
    lg::info("d3d12: {}", status);
    ok = true;
    return true;
  }

  void wait(u64 v) {
    if (fence && fence->GetCompletedValue() < v) {
      fence->SetEventOnCompletion(v, event);
      WaitForSingleObject(event, INFINITE);
    }
  }
};

D3d12Bridge::D3d12Bridge() : m(std::make_unique<Impl>()) {}

D3d12Bridge::~D3d12Bridge() = default;

D3d12Bridge& D3d12Bridge::get() {
  static D3d12Bridge b;
  return b;
}

bool D3d12Bridge::init() {
  if (m->ok) {
    return true;
  }
  return m->init();
}

bool D3d12Bridge::available() const {
  return m->ok;
}

const std::string& D3d12Bridge::status() const {
  return m->status;
}

void D3d12Bridge::acquire() {
  m->users++;
}

void D3d12Bridge::release() {
  m->users--;
  release_if_unused();
}

void D3d12Bridge::release_if_unused() {
  if (m->ok && m->users <= 0) {
    lg::info("d3d12: no feature uses the device; torn down");
    m->teardown();
    m->status = "torn down (no feature uses it)";
  }
}

void D3d12Bridge::shutdown() {
  m->teardown();
  m->users = 0;
}

ID3D12Device* D3d12Bridge::device() {
  return m->dev.Get();
}

ID3D12CommandQueue* D3d12Bridge::queue() {
  return m->queue.Get();
}

IDXGIAdapter1* D3d12Bridge::adapter() {
  return m->adapter.Get();
}

ID3D11Device* D3d12Bridge::d3d11() {
  return m->d11.Get();
}

ID3D12GraphicsCommandList* D3d12Bridge::begin_commands() {
  auto& d = *m;
  d.ring = (d.ring + 1) % kRing;
  d.wait(d.list_fence[d.ring]);
  d.alloc[d.ring]->Reset();
  d.list[d.ring]->Reset(d.alloc[d.ring].Get(), nullptr);
  d.recording = true;
  return d.list[d.ring].Get();
}

u64 D3d12Bridge::submit(ID3D12GraphicsCommandList* cl) {
  auto& d = *m;
  cl->Close();
  ID3D12CommandList* lists[] = {cl};
  d.queue->ExecuteCommandLists(1, lists);
  d.queue->Signal(d.fence.Get(), ++d.fence_value);
  d.list_fence[d.ring] = d.fence_value;
  d.recording = false;
  for (auto& t : d.timer) {
    for (auto& s : t.slots) {
      if (s.pending && s.fence == 0) {
        s.fence = d.fence_value;
      }
    }
  }
  return d.fence_value;
}

u64 D3d12Bridge::last_signal() const {
  return m->fence_value;
}

u64 D3d12Bridge::completed() const {
  return m->fence ? m->fence->GetCompletedValue() : 0;
}

void D3d12Bridge::wait_cpu(u64 value) {
  m->wait(value);
}

bool D3d12Bridge::create_shared(SharedTexture& t,
                                int w,
                                int h,
                                int dxgi_format,
                                unsigned d3d12_flags,
                                unsigned gl_access,
                                const char* name) {
  auto& d = *m;
  destroy_shared(t);
  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC rd{};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  rd.Width = w;
  rd.Height = h;
  rd.DepthOrArraySize = rd.MipLevels = 1;
  rd.Format = (DXGI_FORMAT)dxgi_format;
  rd.SampleDesc.Count = 1;
  rd.Flags = (D3D12_RESOURCE_FLAGS)d3d12_flags | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  ID3D12Resource* res = nullptr;
  if (FAILED(d.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                            D3D12_RESOURCE_STATE_COMMON, nullptr,
                                            IID_PPV_ARGS(&res)))) {
    lg::error("d3d12: {} {}x{} format {} failed", name, w, h, dxgi_format);
    return false;
  }
  std::wstring wn;
  for (const char* c = name; *c; c++) {
    wn += (wchar_t)*c;
  }
  res->SetName(wn.c_str());
  D3D11_RESOURCE_FLAGS fl{};
  fl.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  ID3D11Texture2D* tex = nullptr;
  if (FAILED(d.on12->CreateWrappedResource(res, &fl, D3D12_RESOURCE_STATE_COMMON,
                                           D3D12_RESOURCE_STATE_COMMON, IID_PPV_ARGS(&tex)))) {
    lg::error("d3d12: wrapping {} for D3D11On12 failed", name);
    res->Release();
    return false;
  }
  GLuint gl = 0;
  glGenTextures(1, &gl);
  HANDLE obj = d.wgl.reg(d.interop, tex, gl, GL_TEXTURE_2D, gl_access);
  if (!obj) {
    lg::error("d3d12: wglDXRegisterObjectNV {} failed ({:#x})", name, GetLastError());
    glDeleteTextures(1, &gl);
    tex->Release();
    res->Release();
    return false;
  }
  t.res = res;
  t.d11 = tex;
  t.gl = gl;
  t.interop = obj;
  t.w = w;
  t.h = h;
  t.dxgi_format = dxgi_format;
  // locked for GL between hand-overs
  ID3D11Resource* r11[] = {tex};
  d.on12->AcquireWrappedResources(r11, 1);
  d.wgl.lock(d.interop, 1, &obj);
  return true;
}

void D3d12Bridge::destroy_shared(SharedTexture& t) {
  auto& d = *m;
  if (t.interop && d.interop) {
    HANDLE obj = t.interop;
    d.wgl.unlock(d.interop, 1, &obj);
    ID3D11Resource* r11[] = {t.d11};
    d.on12->ReleaseWrappedResources(r11, 1);
    d.wgl.unreg(d.interop, obj);
  }
  if (t.gl) {
    glDeleteTextures(1, &t.gl);
  }
  // the queue may still read the resource
  if (t.res) {
    d.d11_ctx->Flush();
    d.wait(d.fence_value);
  }
  if (t.d11) {
    t.d11->Release();
  }
  if (t.res) {
    t.res->Release();
  }
  t = SharedTexture();
}

void D3d12Bridge::to_d3d12(SharedTexture* const* list, int n) {
  auto& d = *m;
  HANDLE objs[16];
  ID3D11Resource* r11[16];
  int k = 0;
  for (int i = 0; i < n && k < 16; i++) {
    if (list[i] && list[i]->interop) {
      objs[k] = list[i]->interop;
      r11[k++] = list[i]->d11;
    }
  }
  if (!k) {
    return;
  }
  if (!d.wgl.unlock(d.interop, k, objs)) {
    lg::error("d3d12: wglDXUnlockObjectsNV failed ({:#x})", GetLastError());
  }
  d.on12->ReleaseWrappedResources(r11, k);
  d.d11_ctx->Flush();
}

void D3d12Bridge::to_gl(SharedTexture* const* list, int n) {
  auto& d = *m;
  HANDLE objs[16];
  ID3D11Resource* r11[16];
  int k = 0;
  for (int i = 0; i < n && k < 16; i++) {
    if (list[i] && list[i]->interop) {
      objs[k] = list[i]->interop;
      r11[k++] = list[i]->d11;
    }
  }
  if (!k) {
    return;
  }
  d.on12->AcquireWrappedResources(r11, k);
  if (!d.wgl.lock(d.interop, k, objs)) {
    lg::error("d3d12: wglDXLockObjectsNV failed ({:#x})", GetLastError());
  }
}

int D3d12Bridge::make_timer() {
  if (!m->ts_heap || m->timers >= kMaxTimers) {
    return -1;
  }
  return m->timers++;
}

void D3d12Bridge::timer_begin(int id, ID3D12GraphicsCommandList* cl) {
  auto& d = *m;
  if (id < 0 || id >= d.timers) {
    return;
  }
  auto& t = d.timer[id];
  auto& s = t.slots[t.next];
  if (s.pending) {
    t.open = -1;  // all slots in flight: skip this span
    return;
  }
  t.open = t.next;
  cl->EndQuery(d.ts_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, (id * kTimerSlots + t.open) * 2);
}

void D3d12Bridge::timer_end(int id, ID3D12GraphicsCommandList* cl) {
  auto& d = *m;
  if (id < 0 || id >= d.timers || d.timer[id].open < 0) {
    return;
  }
  auto& t = d.timer[id];
  const UINT q = (id * kTimerSlots + t.open) * 2;
  cl->EndQuery(d.ts_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, q + 1);
  cl->ResolveQueryData(d.ts_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, q, 2, d.ts_readback.Get(),
                       q * sizeof(u64));
  t.slots[t.open].pending = true;
  t.slots[t.open].fence = 0;  // set by submit()
  t.next = (t.open + 1) % kTimerSlots;
  t.open = -1;
}

float D3d12Bridge::timer_ms(int id) {
  auto& d = *m;
  if (id < 0 || id >= d.timers || !d.ts_freq) {
    return -1.f;
  }
  auto& t = d.timer[id];
  const u64 done = completed();
  u64 newest = 0;
  for (int i = 0; i < kTimerSlots; i++) {
    auto& s = t.slots[i];
    if (!s.pending || !s.fence || s.fence > done) {
      continue;
    }
    const UINT q = (id * kTimerSlots + i) * 2;
    D3D12_RANGE range{q * sizeof(u64), (q + 2) * sizeof(u64)};
    u64* p = nullptr;
    if (SUCCEEDED(d.ts_readback->Map(0, &range, (void**)&p))) {
      if (s.fence >= newest && p[q + 1] >= p[q]) {
        t.last_ms = (float)((double)(p[q + 1] - p[q]) * 1000.0 / (double)d.ts_freq);
        newest = s.fence;
      }
      D3D12_RANGE none{0, 0};
      d.ts_readback->Unmap(0, &none);
    }
    s.pending = false;
  }
  return t.last_ms;
}

#else  // not Windows

struct D3d12Bridge::Impl {
  std::string status = "D3D12 is Windows only";
};

D3d12Bridge::D3d12Bridge() : m(std::make_unique<Impl>()) {}
D3d12Bridge::~D3d12Bridge() = default;
D3d12Bridge& D3d12Bridge::get() {
  static D3d12Bridge b;
  return b;
}
bool D3d12Bridge::init() {
  return false;
}
bool D3d12Bridge::available() const {
  return false;
}
const std::string& D3d12Bridge::status() const {
  return m->status;
}
void D3d12Bridge::acquire() {}
void D3d12Bridge::release() {}
void D3d12Bridge::release_if_unused() {}
void D3d12Bridge::shutdown() {}
ID3D12Device* D3d12Bridge::device() {
  return nullptr;
}
ID3D12CommandQueue* D3d12Bridge::queue() {
  return nullptr;
}
IDXGIAdapter1* D3d12Bridge::adapter() {
  return nullptr;
}
ID3D11Device* D3d12Bridge::d3d11() {
  return nullptr;
}
ID3D12GraphicsCommandList* D3d12Bridge::begin_commands() {
  return nullptr;
}
u64 D3d12Bridge::submit(ID3D12GraphicsCommandList*) {
  return 0;
}
u64 D3d12Bridge::last_signal() const {
  return 0;
}
u64 D3d12Bridge::completed() const {
  return 0;
}
void D3d12Bridge::wait_cpu(u64) {}
bool D3d12Bridge::create_shared(SharedTexture&, int, int, int, unsigned, unsigned, const char*) {
  return false;
}
void D3d12Bridge::destroy_shared(SharedTexture&) {}
void D3d12Bridge::to_d3d12(SharedTexture* const*, int) {}
void D3d12Bridge::to_gl(SharedTexture* const*, int) {}
int D3d12Bridge::make_timer() {
  return -1;
}
void D3d12Bridge::timer_begin(int, ID3D12GraphicsCommandList*) {}
void D3d12Bridge::timer_end(int, ID3D12GraphicsCommandList*) {}
float D3d12Bridge::timer_ms(int) {
  return -1.f;
}

#endif
