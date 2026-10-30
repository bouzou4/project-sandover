#ifdef _WIN32
#define SANDOVER_FSR4_IMPL 1
// before the project headers: FileUtil.h undefines FALSE, which the D3D headers use
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d12.h>

#include "api/include/dx12/ffx_api_dx12.h"
#include "api/include/ffx_api.h"
#include "upscalers/include/ffx_upscale.h"
#endif

#include "Fsr4Upscaler.h"

#include <algorithm>
#include <atomic>
#include <thread>
#include <cmath>
#include <cstring>
#include <vector>

#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "common/util/Timer.h"
#include "common/util/exr.h"

#include "game/graphics/gfx.h"
#include "game/graphics/opengl_renderer/D3d12Bridge.h"
#include "game/graphics/opengl_renderer/GlQueries.h"
#include "game/graphics/opengl_renderer/PerfTimers.h"
#include "game/graphics/opengl_renderer/Shader.h"

#include "fmt/format.h"
#include "third-party/imgui/imgui.h"

#ifdef SANDOVER_FSR4_IMPL

namespace {

// perf-opt bit 25: GL queries off the per-frame path (GlQueries.h), as the other post passes
constexpr int kGlq = PERF_OPT_PRESENT2 + 1;
constexpr GLenum kAccessReadOnly = 0x0000;     // WGL_ACCESS_READ_ONLY_NV
constexpr GLenum kAccessWriteDiscard = 0x0002;  // WGL_ACCESS_WRITE_DISCARD_NV
constexpr const wchar_t* kLoader = L"amd_fidelityfx_loader_dx12.dll";

struct Ffx {
  HMODULE dll = nullptr;
  PfnFfxCreateContext create = nullptr;
  PfnFfxDestroyContext destroy = nullptr;
  PfnFfxConfigure configure = nullptr;
  PfnFfxQuery query = nullptr;
  PfnFfxDispatch dispatch = nullptr;
};

fs::path exe_dir() {
  return fs::path(file_util::get_current_executable_path()).parent_path();
}

std::string narrow(const wchar_t* w) {
  std::string s;
  for (; w && *w; w++) {
    s += *w < 128 ? (char)*w : '?';
  }
  return s;
}

std::atomic<int> g_messages{0};

void ffx_message(uint32_t type, const wchar_t* msg) {
  // the runtime can repeat a warning every frame
  if (g_messages.fetch_add(1) < 32) {
    std::string s = narrow(msg);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
      s.pop_back();
    }
    if (type == FFX_API_MESSAGE_TYPE_ERROR) {
      lg::error("fsr4: runtime: {}", s);
    } else {
      lg::warn("fsr4: runtime: {}", s);
    }
  }
}

/// The version number a provider name starts with ("4.1.1" or "FSR 4.1.1"): major * 1e6 +
/// minor * 1e3 + patch; 0 when the name has no digits.
u64 parse_version(const char* name) {
  if (!name) {
    return 0;
  }
  while (*name && (*name < '0' || *name > '9')) {
    name++;
  }
  u64 parts[3] = {0, 0, 0};
  for (int i = 0; i < 3 && *name; i++) {
    while (*name >= '0' && *name <= '9') {
      parts[i] = parts[i] * 10 + (*name++ - '0');
    }
    if (*name != '.') {
      break;
    }
    name++;
  }
  return parts[0] * 1000000 + parts[1] * 1000 + parts[2];
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
    lg::error("fsr4: input shader: {}", log);
  }
  return s;
}

void transition(ID3D12GraphicsCommandList* cl,
                ID3D12Resource* const* res,
                int n,
                D3D12_RESOURCE_STATES from,
                D3D12_RESOURCE_STATES to) {
  D3D12_RESOURCE_BARRIER b[8] = {};
  for (int i = 0; i < n; i++) {
    b[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[i].Transition.pResource = res[i];
    b[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b[i].Transition.StateBefore = from;
    b[i].Transition.StateAfter = to;
  }
  cl->ResourceBarrier(n, b);
}

}  // namespace

struct Fsr4Upscaler::Impl {
  enum { S_COLOUR, S_DEPTH, S_VELOCITY, S_OUTPUT, S_COUNT };

  Ffx ffx;
  bool supported = false;
  bool failed = false;
  Impl* owner = nullptr;  // a second eye: the runtime is the owner's
  int eye = 0;
  u64 version_id = 0;
  std::string version_name;
  ffxContext ctx = nullptr;
  int ctx_rw = 0, ctx_rh = 0, ctx_dw = 0, ctx_dh = 0;
  bool ctx_hdr = false;
  bool fresh = false;  // a new context: its first dispatch resets
  std::thread warm;    // the first context, off the render thread (probe)
  std::atomic<int> warm_state{0};  // 0 running, 1 FSR 4, -1 not
  std::string warm_msg;

  D3d12Bridge::SharedTexture shared[S_COUNT];
  ID3D12Resource* exposure = nullptr;
  GLuint in_fbo = 0, out_fbo = 0, prog = 0, vao = 0, sampler = 0;
  GLint u_colour = -1, u_depth = -1, u_velocity = -1, u_has_velocity = -1;
  Fbo display;
  int rw = 0, rh = 0, dw = 0, dh = 0;
  bool hdr = false;

  unsigned jitter_index = 0;
  float jitter[2] = {0.f, 0.f};
  int phases = 0;
  int resets = 0;
  u64 dispatches = 0;
  // deferred run: the targets are still the queue's (pending) and the output is not blitted
  bool pending = false, unblitted = false;
  int timer = -1;
  float d3d_ms = -1.f, sync_cpu_ms = 0.f;
  float near_m = 0.f, far_m = 0.f;
  std::string status = "not run";

  bool load() {
    const fs::path path = exe_dir() / kLoader;
    ffx.dll = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!ffx.dll) {
      status = fmt::format("no {} beside gk", narrow(kLoader));
      return false;
    }
    ffx.create = (PfnFfxCreateContext)GetProcAddress(ffx.dll, "ffxCreateContext");
    ffx.destroy = (PfnFfxDestroyContext)GetProcAddress(ffx.dll, "ffxDestroyContext");
    ffx.configure = (PfnFfxConfigure)GetProcAddress(ffx.dll, "ffxConfigure");
    ffx.query = (PfnFfxQuery)GetProcAddress(ffx.dll, "ffxQuery");
    ffx.dispatch = (PfnFfxDispatch)GetProcAddress(ffx.dll, "ffxDispatch");
    if (!ffx.create || !ffx.destroy || !ffx.configure || !ffx.query || !ffx.dispatch) {
      status = "the loader lacks the ffx-api entry points";
      unload();
      return false;
    }
    return true;
  }

  void unload() {
    if (ffx.dll && !owner) {
      FreeLibrary(ffx.dll);
    }
    ffx = Ffx();
  }

  /// A second eye on first use: the owner's runtime and provider, its own GL and D3D12 parts.
  bool adopt() {
    if (supported) {
      return true;
    }
    if (!owner || !owner->supported || owner->warm_state <= 0) {
      return false;
    }
    ffx = owner->ffx;
    version_id = owner->version_id;
    version_name = owner->version_name;
    auto& b = D3d12Bridge::get();
    if (!init_gl(b)) {
      ffx = Ffx();
      return false;
    }
    timer = b.make_timer();
    warm_state = 1;
    supported = true;
    return true;
  }

  /// The newest provider of version 4 or later the runtime offers on this device.
  bool pick_version(ID3D12Device* dev) {
    ffxCreateBackendDX12Desc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = dev;
    ffxQueryDescGetVersions q{};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.header.pNext = &backend.header;
    q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    q.device = dev;
    uint64_t count = 0;
    q.outputCount = &count;
    ffxReturnCode_t r = ffx.query(nullptr, &q.header);
    if (r != FFX_API_RETURN_OK || count == 0) {
      status = fmt::format("no upscaler providers (query {})", r);
      return false;
    }
    std::vector<uint64_t> ids(count);
    std::vector<const char*> names(count);
    q.versionIds = ids.data();
    q.versionNames = names.data();
    r = ffx.query(nullptr, &q.header);
    if (r != FFX_API_RETURN_OK) {
      status = fmt::format("provider list query failed ({})", r);
      return false;
    }
    std::string listed;
    u64 best = 0;
    for (uint64_t i = 0; i < count; i++) {
      // the names may be overwritten by a later call: copy them now
      const std::string name = names[i] ? names[i] : "?";
      listed += fmt::format("{}{}", listed.empty() ? "" : ", ", name);
      const u64 v = parse_version(names[i]);
      if (v >= 4000000 && v > best) {
        best = v;
        version_id = ids[i];
        version_name = name;
      }
    }
    lg::info("fsr4: upscaler providers on this device: {}", listed);
    if (!best) {
      status = fmt::format("no FSR 4 provider on this device (offered: {})", listed);
      return false;
    }
    return true;
  }

  ffxReturnCode_t create_context(ffxContext* out,
                                 int rw_,
                                 int rh_,
                                 int dw_,
                                 int dh_,
                                 bool hdr_,
                                 ID3D12Device* dev) {
    ffxCreateContextDescUpscale desc{};
    desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    // k1 is -1 to 1e-4 (clip w = k0 / (ndc z - k1)): an infinite far plane. The scene colour is
    // sRGB-encoded (headroom above 1 with FP16): told so, not passed as linear HDR
    (void)hdr_;
    desc.flags = FFX_UPSCALE_ENABLE_DEPTH_INVERTED | FFX_UPSCALE_ENABLE_DEPTH_INFINITE |
                 FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
    desc.maxRenderSize = {(uint32_t)rw_, (uint32_t)rh_};
    desc.maxUpscaleSize = {(uint32_t)dw_, (uint32_t)dh_};
    desc.fpMessage = ffx_message;
    ffxCreateContextDescUpscaleVersion ver{};
    ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    ver.version = FFX_UPSCALER_VERSION;
    ffxCreateBackendDX12Desc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = dev;
    ffxOverrideVersion override_v{};
    override_v.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    override_v.versionId = version_id;
    desc.header.pNext = &ver.header;
    ver.header.pNext = &backend.header;
    backend.header.pNext = &override_v.header;
    return ffx.create(out, &desc.header, nullptr);
  }

  std::string provider_of(ffxContext* c) {
    ffxQueryGetProviderVersion pv{};
    pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (ffx.query(c, &pv.header) != FFX_API_RETURN_OK || !pv.versionName) {
      return "";
    }
    return pv.versionName;
  }

  bool probe() {
    auto& b = D3d12Bridge::get();
    if (!b.available()) {
      status = fmt::format("no D3D12 device ({})", b.status());
      return false;
    }
    Timer t;
    if (!load()) {
      return false;
    }
    const double load_ms = t.getMs();
    if (!pick_version(b.device())) {
      unload();
      return false;
    }
    if (!init_gl(b)) {
      unload();
      return false;
    }
    timer = b.make_timer();
    status = fmt::format("{} offered on the D3D12 device", version_name);
    lg::info("fsr4: available ({}); startup check {:.0f} ms (runtime load {:.0f})", status,
             t.getMs(), load_ms);
    // the first context in a process takes seconds: made, checked and dropped off the render
    // thread; run() waits for it, poll() withdraws the mode if it was not FSR 4
    warm = std::thread([this, dev = b.device()] {
      Timer w;
      ffxContext test = nullptr;
      const ffxReturnCode_t r = create_context(&test, 64, 64, 128, 128, false, dev);
      if (r != FFX_API_RETURN_OK || !test) {
        warm_msg = fmt::format("{} context creation failed ({})", version_name, r);
        warm_state = -1;
        return;
      }
      const std::string live = provider_of(&test);
      ffx.destroy(&test, nullptr);
      if (parse_version(live.c_str()) < 4000000) {
        warm_msg = fmt::format("the runtime runs {} instead of FSR 4", live.empty() ? "?" : live);
        warm_state = -1;
        return;
      }
      warm_msg = fmt::format("{} on the D3D12 device; warm-up context {:.0f} ms", live, w.getMs());
      warm_state = 1;
    });
    return true;
  }

  /// Waits for the warm-up thread; false (logged once) when it found no FSR 4.
  bool join_warm() {
    if (warm.joinable()) {
      warm.join();
      if (warm_state < 0) {
        lg::warn("fsr4: unavailable ({})", warm_msg);
      } else {
        lg::info("fsr4: {}", warm_msg);
      }
      status = warm_msg;
    }
    return warm_state > 0;
  }

  void release_exposure() {
    if (exposure) {
      exposure->Release();
      exposure = nullptr;
    }
  }

  bool init_gl(D3d12Bridge& b) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = rd.Height = 1;
    rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R32_FLOAT;
    rd.SampleDesc.Count = 1;
    D3D12_HEAP_PROPERTIES up{};
    up.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 256;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* upload = nullptr;
    if (FAILED(b.device()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   IID_PPV_ARGS(&exposure))) ||
        FAILED(b.device()->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                   IID_PPV_ARGS(&upload)))) {
      if (upload) {
        upload->Release();
      }
      release_exposure();
      status = "exposure texture failed";
      return false;
    }
    float* p = nullptr;
    upload->Map(0, nullptr, (void**)&p);
    *p = 1.f;
    upload->Unmap(0, nullptr);
    auto* cl = b.begin_commands();
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = exposure;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = upload;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint = {DXGI_FORMAT_R32_FLOAT, 1, 1, 1, 256};
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    transition(cl, &exposure, 1, D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    b.wait_cpu(b.submit(cl));
    upload->Release();

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
      glDeleteProgram(prog);
      prog = 0;
      release_exposure();
      status = "input program did not link";
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
    return true;
  }

  void destroy_context() {
    if (ctx) {
      auto& b = D3d12Bridge::get();
      b.wait_cpu(b.last_signal());
      ffx.destroy(&ctx, nullptr);
      ctx = nullptr;
    }
    ctx_rw = ctx_rh = ctx_dw = ctx_dh = 0;
  }

  void free_targets() {
    destroy_context();
    auto& b = D3d12Bridge::get();
    for (auto& s : shared) {
      b.destroy_shared(s);
    }
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

  bool alloc(int rw_, int rh_, int dw_, int dh_, bool hdr_) {
    if (rw == rw_ && rh == rh_ && dw == dw_ && dh == dh_ && hdr == hdr_ && display.valid) {
      return true;
    }
    free_targets();
    auto& b = D3d12Bridge::get();
    const int colour = hdr_ ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
    bool ok = b.create_shared(shared[S_COLOUR], rw_, rh_, colour, 0, kAccessWriteDiscard,
                              "fsr4 colour") &&
              b.create_shared(shared[S_DEPTH], rw_, rh_, DXGI_FORMAT_R32_FLOAT, 0,
                              kAccessWriteDiscard, "fsr4 depth") &&
              b.create_shared(shared[S_VELOCITY], rw_, rh_, DXGI_FORMAT_R16G16_FLOAT, 0,
                              kAccessWriteDiscard, "fsr4 velocity") &&
              b.create_shared(shared[S_OUTPUT], dw_, dh_, colour,
                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, kAccessReadOnly,
                              "fsr4 output");
    if (!ok) {
      free_targets();
      return false;
    }
    glGenFramebuffers(1, &in_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, in_fbo);
    for (int i = 0; i < 3; i++) {
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D,
                             shared[S_COLOUR + i].gl, 0);
    }
    const GLenum draws[3] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2};
    glDrawBuffers(3, draws);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      lg::error("fsr4: incomplete input framebuffer");
      ok = false;
    }
    glGenFramebuffers(1, &out_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, out_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           shared[S_OUTPUT].gl, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      lg::error("fsr4: incomplete output framebuffer");
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
      lg::error("fsr4: incomplete display framebuffer ({}x{})", dw_, dh_);
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
    lg::info("fsr4: targets {}x{} -> {}x{} {}", rw, rh, dw, dh, hdr ? "RGBA16F" : "RGBA8");
    return true;
  }

  bool ensure_context() {
    if (ctx && ctx_rw == rw && ctx_rh == rh && ctx_dw == dw && ctx_dh == dh && ctx_hdr == hdr) {
      return true;
    }
    perf::AddMs hitch_t(perf::Counter::HITCH_UPSCALER_MS);
    destroy_context();
    const ffxReturnCode_t r =
        create_context(&ctx, rw, rh, dw, dh, hdr, D3d12Bridge::get().device());
    if (r != FFX_API_RETURN_OK || !ctx) {
      lg::warn("fsr4: unavailable (context {}x{} -> {}x{} failed: {})", rw, rh, dw, dh, r);
      ctx = nullptr;
      return false;
    }
    ffxQueryDescUpscaleGetResourceRequirements req{};
    req.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GET_RESOURCE_REQUIREMENTS;
    const bool have_req = ffx.query(&ctx, &req.header) == FFX_API_RETURN_OK;
    lg::info("fsr4: context {}x{} -> {}x{} {} sRGB-encoded, provider {}, resources required {:#x} "
             "optional {:#x}",
             rw, rh, dw, dh, hdr ? "RGBA16F" : "RGBA8", provider_of(&ctx),
             have_req ? req.required_resources : 0, have_req ? req.optional_resources : 0);
    ctx_rw = rw;
    ctx_rh = rh;
    ctx_dw = dw;
    ctx_dh = dh;
    ctx_hdr = hdr;
    fresh = true;
    return true;
  }

  void shutdown() {
    if (warm.joinable()) {
      warm.join();
    }
    free_targets();
    if (exposure) {
      D3d12Bridge::get().wait_cpu(D3d12Bridge::get().last_signal());
      exposure->Release();
      exposure = nullptr;
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
    unload();
  }
};

Fsr4Upscaler::Fsr4Upscaler() : m(std::make_unique<Impl>()) {}

Fsr4Upscaler::~Fsr4Upscaler() {
  if (m->supported && D3d12Bridge::get().available()) {
    m->shutdown();
  }
}

bool Fsr4Upscaler::probe() {
  m->supported = m->probe();
  if (m->supported) {
    D3d12Bridge::get().acquire();
  } else {
    lg::info("fsr4: unavailable ({})", m->status);
  }
  return m->supported;
}

void Fsr4Upscaler::share_runtime(Fsr4Upscaler* owner, int eye) {
  m->owner = owner->m.get();
  m->eye = eye;
}

void Fsr4Upscaler::begin_frame(int rw, int rh, int dw, s64 index) {
  const int phases = scene_jitter::phase_count(rw, dw);
  if (phases != m->phases) {
    m->phases = phases;
    m->jitter_index = 0;
  }
  float jx, jy;
  const int i = index >= 0 ? (int)((u64)index % (u64)phases)
                           : (int)(m->jitter_index++ % (unsigned)phases);
  scene_jitter::halton_offset(i, &jx, &jy);
  m->jitter[0] = jx;
  m->jitter[1] = jy;
  // the content's offset in GL texel-grid pixels (+y up)
  scene_jitter::set(2.f * jx / (float)rw, 2.f * jy / (float)rh);
}

bool Fsr4Upscaler::run(SharedRenderState* rs,
                       ScopedProfilerNode& prof,
                       const Inputs& in,
                       int dw,
                       int dh) {
  return run(rs, prof, in, dw, dh, false, nullptr);
}

bool Fsr4Upscaler::deferred() const {
  return m->unblitted;
}

bool Fsr4Upscaler::run(SharedRenderState* rs,
                       ScopedProfilerNode& /*prof*/,
                       const Inputs& in,
                       int dw,
                       int dh,
                       bool defer,
                       Fsr4Upscaler* pending) {
  auto& d = *m;
  auto& b = D3d12Bridge::get();
  const int rw = rs->render_fb_w, rh = rs->render_fb_h;
  static glq::SavedState saved("fsr4", kGlq, 0x7, 0);
  saved.capture();
  d.adopt();
  if (d.supported && !d.owner && !d.join_warm()) {
    // as poll(): drop everything so the bridge can go
    d.supported = false;
    d.shutdown();
    b.release();
  }
  if (!d.supported || !d.alloc(rw, rh, dw, dh, in.hdr)) {
    d.failed = true;
    d.status = "shared targets failed (see the log)";
    saved.restore();
    scene_jitter::set(0.f, 0.f);
    return false;
  }
  if (!d.ensure_context()) {
    // this frame unfiltered; FSR 3.1 from the next
    d.failed = true;
    d.status = "context creation failed (see the log)";
    glBindFramebuffer(GL_READ_FRAMEBUFFER, rs->render_fb);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, d.display.fbo_id);
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0, 0, rw, rh, 0, 0, dw, dh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    finish(rs, saved, dw, dh);
    return false;
  }
  const bool reset = in.reset || !in.velocity || d.fresh;
  d.fresh = false;

  {
    perf::Scope ps(perf::Group::FSR4_INPUTS);
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
  }

  ffxReturnCode_t r;
  {
    perf::Scope ps(perf::Group::FSR4_DISPATCH);
    Timer cpu;
    const float t = b.timer_ms(d.timer);
    if (t >= 0.f) {
      d.d3d_ms = t;
      perf::count(perf::Counter::FSR4_D3D_MS, t);
    }
    D3d12Bridge::SharedTexture* all[Impl::S_COUNT] = {
        &d.shared[Impl::S_COLOUR], &d.shared[Impl::S_DEPTH], &d.shared[Impl::S_VELOCITY],
        &d.shared[Impl::S_OUTPUT]};
    // GL's input pass before the queue's dispatch, the dispatch before GL's output blit
    b.to_d3d12(all, Impl::S_COUNT);
    const double t_handover = cpu.getMs();
    auto* cl = b.begin_commands();
    ID3D12Resource* inputs[3] = {d.shared[Impl::S_COLOUR].res, d.shared[Impl::S_DEPTH].res,
                                 d.shared[Impl::S_VELOCITY].res};
    ID3D12Resource* output = d.shared[Impl::S_OUTPUT].res;
    transition(cl, inputs, 3, D3D12_RESOURCE_STATE_COMMON,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(cl, &output, 1, D3D12_RESOURCE_STATE_COMMON,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    b.timer_begin(d.timer, cl);

    ffxDispatchDescUpscale e{};
    e.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    e.commandList = cl;
    e.color = ffxApiGetResourceDX12(inputs[0], FFX_API_RESOURCE_STATE_COMPUTE_READ);
    e.depth = ffxApiGetResourceDX12(inputs[1], FFX_API_RESOURCE_STATE_COMPUTE_READ);
    e.motionVectors = ffxApiGetResourceDX12(inputs[2], FFX_API_RESOURCE_STATE_COMPUTE_READ);
    e.exposure = ffxApiGetResourceDX12(d.exposure, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    e.output = ffxApiGetResourceDX12(output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    // the content offset in D3D's top-down pixel space; bits of fsr4_jitter_sign negate an axis
    const int sign = Gfx::g_global_settings.fsr4_jitter_sign;
    e.jitterOffset.x = (sign & 1) ? -d.jitter[0] : d.jitter[0];
    e.jitterOffset.y = (sign & 2) ? d.jitter[1] : -d.jitter[1];
    e.motionVectorScale.x = 1.f;
    e.motionVectorScale.y = -1.f;
    e.renderSize = {(uint32_t)rw, (uint32_t)rh};
    e.upscaleSize = {(uint32_t)dw, (uint32_t)dh};
    e.enableSharpening = in.sharpness > 0.f;
    e.sharpness = in.sharpness;
    e.frameTimeDelta = std::clamp(in.frame_ms, 0.f, 1000.f);
    e.preExposure = 1.f;
    e.reset = reset;
    // view depth = (k0 / 2) / (d - (1 + k1) / 2), inverted (near at d = 1); with k1 near -1 that
    // is near / d, so the far plane is infinite (DEPTH_INFINITE) and only near matters
    const float k0 = in.w_k0, k1 = in.w_k1;
    float near_v = std::abs(1.f - k1) > 1e-6f ? k0 / (1.f - k1) : 0.f;
    if (!(near_v > 0.f) || !std::isfinite(near_v)) {
      near_v = 1.f;
    }
    const float far_v = near_v * 1e7f;
    d.near_m = near_v / 4096.f;
    d.far_m = far_v / 4096.f;
    e.cameraNear = near_v;
    e.cameraFar = far_v;
    e.cameraFovAngleVertical = 2.f * std::atan(in.tan_half_y);
    e.viewSpaceToMetersFactor = 1.f / 4096.f;
    e.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
    r = d.ffx.dispatch(&d.ctx, &e.header);

    b.timer_end(d.timer, cl);
    transition(cl, inputs, 3, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_COMMON);
    transition(cl, &output, 1, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COMMON);
    b.submit(cl);
    const double t_submit = cpu.getMs();
    if (defer) {
      d.pending = true;
    } else {
      D3d12Bridge::SharedTexture* back[2 * Impl::S_COUNT];
      int n = 0;
      for (auto* t : all) {
        back[n++] = t;
      }
      auto* other = pending && pending != this && pending->m->pending ? pending->m.get() : nullptr;
      if (other) {
        for (auto& t : other->shared) {
          back[n++] = &t;
        }
        other->pending = false;
      }
      b.to_gl(back, n);
    }
    d.sync_cpu_ms = (float)cpu.getMs();
    if (d.dispatches % 600 == 1) {
      lg::info("fsr4: cpu ms: hand-over {:.2f}, record + submit {:.2f}, hand-back {:.2f}; k0 {} k1 {}; "
               "eye {} dispatch {:.3f} ms",
               t_handover, t_submit - t_handover, d.sync_cpu_ms - t_submit, in.w_k0, in.w_k1,
               rs->stereo_active ? rs->eye_slot : -1, d.d3d_ms);
    }
    perf::count(perf::Counter::FSR4_SYNC_CPU_MS, d.sync_cpu_ms);
  }
  if (r != FFX_API_RETURN_OK) {
    lg::error("fsr4: dispatch failed ({}); FSR 3.1 in its place", r);
    d.failed = true;
    d.status = fmt::format("dispatch failed ({})", r);
  }
  if (d.dispatches == 0) {
    lg::info("fsr4: first dispatch {}x{} -> {}x{}, near {} (k0 {}, k1 {}), fov {:.1f} deg", rw, rh,
             dw, dh, d.near_m * 4096.f, in.w_k0, in.w_k1, 2.f * std::atan(in.tan_half_y) * 57.2958f);
  }
  d.dispatches++;
  if (reset) {
    d.resets++;
  }

  d.unblitted = defer;
  if (!defer) {
    output_blit();
  }
  if (!d.failed) {
    d.status = fmt::format("{} {}x{} -> {}x{}, jitter ({:.3f}, {:.3f}) of {} phases, {} resets",
                           d.version_name, rw, rh, dw, dh, d.jitter[0], d.jitter[1], d.phases,
                           d.resets);
  }
  finish(rs, saved, dw, dh);
  return !d.failed;
}

void Fsr4Upscaler::output_blit() {
  auto& d = *m;
  perf::Scope ps(perf::Group::FSR4_OUTPUT);
  // flipped back: D3D's top row is GL's last
  glBindFramebuffer(GL_READ_FRAMEBUFFER, d.out_fbo);
  glReadBuffer(GL_COLOR_ATTACHMENT0);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, d.display.fbo_id);
  glBlitFramebuffer(0, 0, d.dw, d.dh, 0, d.dh, d.dw, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
}

void Fsr4Upscaler::finish_deferred() {
  auto& d = *m;
  if (d.pending) {
    D3d12Bridge::SharedTexture* all[Impl::S_COUNT];
    for (int i = 0; i < Impl::S_COUNT; i++) {
      all[i] = &d.shared[i];
    }
    D3d12Bridge::get().to_gl(all, Impl::S_COUNT);
    d.pending = false;
  }
  if (d.unblitted) {
    d.unblitted = false;
    glDisable(GL_SCISSOR_TEST);
    output_blit();
  }
}

void Fsr4Upscaler::finish(SharedRenderState* rs, const glq::SavedState& saved, int dw, int dh) {
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

void Fsr4Upscaler::dump_output(const std::string& stem, u64 frame) {
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
  attrs["content"] = "FSR 4 output, display-encoded, before the text buckets";
  attrs["render"] = fmt::format("{}x{}", d.rw, d.rh);
  attrs["provider"] = d.version_name;
  exr::write_rgb_half(stem + "_fsr4.exr", px.data(), d.dw, d.dh, 4, true, attrs);
}

bool Fsr4Upscaler::poll() {
  auto& d = *m;
  if (d.warm.joinable() && d.warm_state != 0 && !d.join_warm()) {
    d.supported = false;
    d.shutdown();
    D3d12Bridge::get().release();
  }
  return d.supported;
}

Fbo* Fsr4Upscaler::display() {
  return &m->display;
}

void Fsr4Upscaler::release() {
  m->unblitted = false;
  finish_deferred();  // targets the queue still holds come back before they are freed
  if (m->rw || m->ctx) {
    m->free_targets();
  }
  m->failed = false;
}

void Fsr4Upscaler::draw_debug_window() {
  auto& gs = Gfx::g_global_settings;
  ImGui::TextUnformatted(Gfx::g_fsr4_supported ? "available" : "unavailable");
  ImGui::TextUnformatted(m->status.c_str());
  ImGui::Text("dispatch %.3f ms (D3D12), hand-over + submit %.3f ms (CPU)", m->d3d_ms,
              m->sync_cpu_ms);
  bool nx = gs.fsr4_jitter_sign & 1, ny = gs.fsr4_jitter_sign & 2;
  ImGui::Checkbox("Negate jitter x##fsr4", &nx);
  ImGui::Checkbox("Negate jitter y##fsr4", &ny);
  gs.fsr4_jitter_sign = (nx ? 1 : 0) | (ny ? 2 : 0);
}

#else  // not Windows

struct Fsr4Upscaler::Impl {
  Fbo display;
};

Fsr4Upscaler::Fsr4Upscaler() : m(std::make_unique<Impl>()) {}
Fsr4Upscaler::~Fsr4Upscaler() = default;
bool Fsr4Upscaler::probe() {
  lg::info("fsr4: unavailable (D3D12 is Windows only)");
  return false;
}
void Fsr4Upscaler::share_runtime(Fsr4Upscaler*, int) {}
void Fsr4Upscaler::begin_frame(int, int, int, s64) {}
bool Fsr4Upscaler::run(SharedRenderState*, ScopedProfilerNode&, const Inputs&, int, int) {
  return false;
}
bool Fsr4Upscaler::run(SharedRenderState*,
                       ScopedProfilerNode&,
                       const Inputs&,
                       int,
                       int,
                       bool,
                       Fsr4Upscaler*) {
  return false;
}
void Fsr4Upscaler::finish_deferred() {}
bool Fsr4Upscaler::deferred() const {
  return false;
}
void Fsr4Upscaler::output_blit() {}
void Fsr4Upscaler::dump_output(const std::string&, u64) {}
Fbo* Fsr4Upscaler::display() {
  return &m->display;
}
void Fsr4Upscaler::release() {}
bool Fsr4Upscaler::poll() {
  return false;
}
void Fsr4Upscaler::draw_debug_window() {
  ImGui::TextUnformatted("D3D12 is Windows only");
}

#endif
