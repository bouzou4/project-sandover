#pragma once

#include <memory>
#include <string>

#include "common/common_types.h"

struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;
struct ID3D11Device;
struct ID3D11Texture2D;
struct IDXGIAdapter1;

/*!
 * The process's one D3D12 device, the container for every DirectX-side feature (FSR 4 today; the
 * DLSS port and the DXGI flip presenter next). Windows only; elsewhere every call reports
 * unavailable.
 *
 * - Device: D3D12CreateDevice at feature level 12_0 on the adapter whose LUID the GL context
 *   reports (GL_EXT_memory_object_win32), one direct queue, one fence, a small ring of command
 *   allocators and lists.
 * - GL bridge: WGL_NV_DX_interop2 takes D3D10/11 devices only, so a D3D11On12 device wraps the
 *   D3D12 device on the same queue. A shared texture is a D3D12 committed resource, wrapped for
 *   D3D11On12 and registered with the interop as a GL texture name. There is no second device and
 *   no cross-API fence: to_d3d12() unlocks the GL side, releases the wrapped resources and flushes
 *   D3D11On12 onto the queue, so later D3D12 work on the queue follows GL's commands; to_gl()
 *   acquires and locks them again, so GL's next commands follow the queue's.
 * - Resource state: a shared texture is in D3D12_RESOURCE_STATE_COMMON between hand-overs. D3D12
 *   work on it transitions from and back to COMMON (or passes COMMON as the state to an SDK that
 *   transitions itself).
 * - Rows: D3D's row 0 is the top of the image, GL's the bottom. The bridge does not flip; callers
 *   copy with flipped rows (as the DLSS input pass does).
 *
 * SANDOVER_D3D12_OFF=1 makes init() fail (diagnostic: the path of a machine without D3D12).
 *
 * Lifetime: init() on the render thread with the GL context current, at renderer start; on any
 * failure it logs one line and tears everything down (available() false). A consumer that finds
 * its feature unsupported calls release_if_unused(); shutdown() at renderer exit. All calls on the
 * render thread.
 */
class D3d12Bridge {
 public:
  struct SharedTexture {
    ID3D12Resource* res = nullptr;     ///< the D3D12 resource (state COMMON between hand-overs)
    ID3D11Texture2D* d11 = nullptr;    ///< its D3D11On12 wrapper
    unsigned gl = 0;                   ///< the GL texture name (GL_TEXTURE_2D)
    void* interop = nullptr;           ///< the WGL interop object
    int w = 0, h = 0;
    int dxgi_format = 0;
    bool valid() const { return interop != nullptr; }
  };

  /// The process-wide bridge.
  static D3d12Bridge& get();

  /// Device, queue, fence, D3D11On12 and the interop device. False, with one log line naming the
  /// step, and nothing left behind, when the adapter has no D3D12 or the interop refuses it.
  bool init();
  bool available() const;
  /// The failure reason or a one-line description of the device.
  const std::string& status() const;
  /// Tear down while no feature holds the bridge (consumers call acquire/release).
  void acquire();
  /// A holder lets go; the device is torn down when none is left.
  void release();
  void release_if_unused();
  void shutdown();

  ID3D12Device* device();
  ID3D12CommandQueue* queue();
  IDXGIAdapter1* adapter();
  ID3D11Device* d3d11();

  /// A reset command list from the ring; record, then submit().
  ID3D12GraphicsCommandList* begin_commands();
  /// Close and execute the list from begin_commands(), signal the fence; returns the value.
  u64 submit(ID3D12GraphicsCommandList* cl);
  /// The value of the last submit's signal, and the fence's completed value.
  u64 last_signal() const;
  u64 completed() const;
  /// Blocks until the queue reaches value (render thread; use sparingly).
  void wait_cpu(u64 value);

  /// A w x h texture of the format (DXGI_FORMAT), D3D12 flags as given (D3D12_RESOURCE_FLAGS),
  /// registered for GL with the access (WGL_ACCESS_*_NV); locked for GL on return.
  bool create_shared(SharedTexture& t, int w, int h, int dxgi_format, unsigned d3d12_flags,
                     unsigned gl_access, const char* name);
  void destroy_shared(SharedTexture& t);
  /// GL's commands on these textures happen before any D3D12 work submitted after this call.
  void to_d3d12(SharedTexture* const* list, int n);
  /// D3D12 work submitted before this call happens before GL's next commands on these textures.
  void to_gl(SharedTexture* const* list, int n);

  /// GPU timestamps on the queue: an id, then begin and end around recorded work in one list;
  /// timer_ms is the newest completed span (a few frames late), -1 before the first.
  int make_timer();
  void timer_begin(int timer, ID3D12GraphicsCommandList* cl);
  void timer_end(int timer, ID3D12GraphicsCommandList* cl);
  float timer_ms(int timer);

  ~D3d12Bridge();

 private:
  D3d12Bridge();
  struct Impl;
  std::unique_ptr<Impl> m;
};
