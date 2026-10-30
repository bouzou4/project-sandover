#include "Fsr3Upscaler.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "common/log/log.h"
#include "common/util/exr.h"
#include "common/util/FileUtil.h"
#include "common/util/Timer.h"

#include "game/graphics/gfx.h"
#include "game/graphics/opengl_renderer/GlQueries.h"
#include "game/graphics/opengl_renderer/PerfTimers.h"
#include "game/graphics/opengl_renderer/Shader.h"

#include "fmt/format.h"
#include "third-party/imgui/imgui.h"

// the SDK's CPU helpers: SPD dispatch setup and RCAS constants
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wsign-compare"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#define FFX_CPU 1
#include "third-party/fidelityfx/gpu/ffx_core.h"
#include "third-party/fidelityfx/gpu/fsr1/ffx_fsr1.h"
#include "third-party/fidelityfx/gpu/spd/ffx_spd.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif


namespace {

enum Pass : int {
  P_PREPARE_INPUTS,
  P_LUMA_PYRAMID,
  P_SHADING_CHANGE_PYRAMID,
  P_SHADING_CHANGE,
  P_PREPARE_REACTIVITY,
  P_LUMA_INSTABILITY,
  P_ACCUMULATE,
  P_RCAS,
  P_DEBUG_VIEW,
  P_COUNT
};
const char* kPassNames[P_COUNT] = {"prepare_inputs",
                                   "luma_pyramid",
                                   "shading_change_pyramid",
                                   "shading_change",
                                   "prepare_reactivity",
                                   "luma_instability",
                                   "accumulate",
                                   "rcas",
                                   "debug_view"};

// textures
enum Res : int {
  R_EXPOSURE,       // 1x1 RG32F, 0: the shaders read 1
  R_ZERO,           // 1x1 R8 0: no transparency and composition mask, no reactive mask
  R_OPAQUE,         // render size: the scene before the sprite bucket
  R_REACTIVE,       // render size R32F: autogen reactive
  R_DILATED_DEPTH,  // R32F
  R_DILATED_MV,     // RG16F
  R_RECON_DEPTH,    // R32UI
  R_INTERMEDIATE,   // R16F: farthest depth, then luma instability (aliased as in the SDK)
  R_LUMA_1,         // R16F
  R_LUMA_2,
  R_LUMA_HISTORY_1,  // RGBA8
  R_LUMA_HISTORY_2,
  R_ACCUMULATION_1,  // R8
  R_ACCUMULATION_2,
  R_SHADING_CHANGE,    // R8, half size
  R_SPD_MIPS,          // RG16F, half size, mip chain
  R_FARTHEST_MIP1,     // R16F, half size
  R_FRAME_INFO,        // 1x1 RGBA32F
  R_DILATED_REACTIVE,  // RGBA8
  R_NEW_LOCKS,         // R8, display size
  R_UPSCALED_1,        // RGBA16F, display size
  R_UPSCALED_2,
  R_LANCZOS_LUT,  // 128x1 R16_SNORM
  R_COUNT
};
static_assert(R_COUNT <= 24);

// binding names (the pass files' fsr3-bindings lines), resolved per frame for the ping-pong pairs
enum Sym : int {
  S_INPUT_COLOR = 100,
  S_INPUT_DEPTH,
  S_INPUT_MOTION_VECTORS,
  S_INPUT_EXPOSURE,
  S_REACTIVE_MASK,
  S_TRANSPARENCY_AND_COMPOSITION_MASK,
  S_INPUT_OPAQUE_ONLY,
  S_DILATED_REACTIVE_MASKS,
  S_DILATED_MOTION_VECTORS,
  S_DILATED_DEPTH,
  S_INTERNAL_UPSCALED,
  S_LANCZOS_LUT,
  S_FARTHEST_DEPTH_MIP1,
  S_FARTHEST_DEPTH,
  S_CURRENT_LUMA,
  S_PREVIOUS_LUMA,
  S_LUMA_INSTABILITY,
  S_FRAME_INFO,
  S_LUMA_HISTORY,
  S_RECONSTRUCTED_PREV_NEAREST_DEPTH,
  S_ACCUMULATION,
  S_SHADING_CHANGE,
  S_SPD_MIPS,
  S_RCAS_INPUT,
  S_NEW_LOCKS,
  S_UPSCALED_OUTPUT,
  S_AUTOREACTIVE,
  S_AUTOCOMPOSITION,
  S_SPD_MIPS_LEVEL_0,  // .. 5
  S_SPD_GLOBAL_ATOMIC = S_SPD_MIPS_LEVEL_0 + 6,
};

struct Name {
  const char* name;
  int sym;
};
const Name kNames[] = {
    {"INPUT_COLOR", S_INPUT_COLOR},
    {"INPUT_DEPTH", S_INPUT_DEPTH},
    {"INPUT_MOTION_VECTORS", S_INPUT_MOTION_VECTORS},
    {"INPUT_EXPOSURE", S_INPUT_EXPOSURE},
    {"REACTIVE_MASK", S_REACTIVE_MASK},
    {"TRANSPARENCY_AND_COMPOSITION_MASK", S_TRANSPARENCY_AND_COMPOSITION_MASK},
    {"INPUT_OPAQUE_ONLY", S_INPUT_OPAQUE_ONLY},
    {"DILATED_REACTIVE_MASKS", S_DILATED_REACTIVE_MASKS},
    {"DILATED_MOTION_VECTORS", S_DILATED_MOTION_VECTORS},
    {"DILATED_DEPTH", S_DILATED_DEPTH},
    {"INTERNAL_UPSCALED", S_INTERNAL_UPSCALED},
    {"LANCZOS_LUT", S_LANCZOS_LUT},
    {"FARTHEST_DEPTH_MIP1", S_FARTHEST_DEPTH_MIP1},
    {"FARTHEST_DEPTH", S_FARTHEST_DEPTH},
    {"CURRENT_LUMA", S_CURRENT_LUMA},
    {"PREVIOUS_LUMA", S_PREVIOUS_LUMA},
    {"LUMA_INSTABILITY", S_LUMA_INSTABILITY},
    {"FRAME_INFO", S_FRAME_INFO},
    {"LUMA_HISTORY", S_LUMA_HISTORY},
    {"RECONSTRUCTED_PREV_NEAREST_DEPTH", S_RECONSTRUCTED_PREV_NEAREST_DEPTH},
    {"ACCUMULATION", S_ACCUMULATION},
    {"SHADING_CHANGE", S_SHADING_CHANGE},
    {"SPD_MIPS", S_SPD_MIPS},
    {"RCAS_INPUT", S_RCAS_INPUT},
    {"NEW_LOCKS", S_NEW_LOCKS},
    {"UPSCALED_OUTPUT", S_UPSCALED_OUTPUT},
    {"AUTOREACTIVE", S_AUTOREACTIVE},
    {"AUTOCOMPOSITION", S_AUTOCOMPOSITION},
    {"SPD_MIPS_LEVEL_0", S_SPD_MIPS_LEVEL_0},
    {"SPD_MIPS_LEVEL_1", S_SPD_MIPS_LEVEL_0 + 1},
    {"SPD_MIPS_LEVEL_2", S_SPD_MIPS_LEVEL_0 + 2},
    {"SPD_MIPS_LEVEL_3", S_SPD_MIPS_LEVEL_0 + 3},
    {"SPD_MIPS_LEVEL_4", S_SPD_MIPS_LEVEL_0 + 4},
    {"SPD_MIPS_LEVEL_5", S_SPD_MIPS_LEVEL_0 + 5},
    {"SPD_GLOBAL_ATOMIC", S_SPD_GLOBAL_ATOMIC},
};

int lookup(const std::string& n) {
  for (const auto& e : kNames) {
    if (n == e.name) {
      return e.sym;
    }
  }
  return -1;
}

enum Cb : int { CB_FSR3UPSCALER, CB_SPD, CB_RCAS, CB_REACTIVE, CB_COUNT };

// Fsr3UpscalerConstants (ffx_fsr3upscaler_private.h); std140 places every member where C does
struct Constants {
  int32_t render_size[2];
  int32_t prev_render_size[2];
  int32_t upscale_size[2];
  int32_t prev_upscale_size[2];
  int32_t max_render_size[2];
  int32_t max_upscale_size[2];
  float device_to_view_depth[4];
  float jitter[2];
  float prev_jitter[2];
  float mv_scale[2];
  float downscale[2];
  float mv_jitter_cancel[2];
  float tan_half_fov;
  float jitter_phase_count;
  float delta_time;
  float delta_pre_exposure;
  float view_to_meters;
  float frame_index;
  float velocity_factor;
  float reactiveness_scale;
  float shading_change_scale;
  float accumulation_added_per_frame;
  float min_disocclusion_accumulation;
};
struct SpdConstants {
  uint32_t mips;
  uint32_t num_work_groups;
  uint32_t work_group_offset[2];
  uint32_t render_size[2];
};
struct ReactiveConstants {
  float scale;
  float threshold;
  float binary_value;
  uint32_t flags;
};

float lanczos2(float x) {
  constexpr float kPi = 3.14159265358979f;
  return std::abs(x) < 1e-6f
             ? 1.f
             : (std::sin(kPi * x) / (kPi * x)) * (std::sin(0.5f * kPi * x) / (0.5f * kPi * x));
}

fs::path gpu_root() {
  return file_util::get_file_path({"third-party", "fidelityfx", "gpu"});
}

fs::path pass_root() {
  return file_util::get_file_path({Shader::shader_folder, "fsr3"});
}

// textual #include as the C preprocessor does it (no include-once: the SDK re-includes
// ffx_fsr1.h with other defines); #extension lines dropped, the context needs none
bool expand(const fs::path& file, std::string& out, int depth, std::string& err) {
  if (depth > 32) {
    err = "include depth at " + file.string();
    return false;
  }
  if (!fs::exists(file)) {
    err = "missing " + file.string();
    return false;
  }
  const std::string text = file_util::read_text_file(file);
  const fs::path dirs[] = {file.parent_path(), gpu_root(), gpu_root() / "fsr3upscaler",
                           pass_root()};
  size_t at = 0;
  while (at < text.size()) {
    size_t end = text.find('\n', at);
    if (end == std::string::npos) {
      end = text.size();
    }
    const std::string_view line(text.data() + at, end - at);
    size_t p = line.find_first_not_of(" \t");
    bool handled = false;
    if (p != std::string_view::npos && line[p] == '#') {
      size_t q = line.find_first_not_of(" \t", p + 1);
      if (q != std::string_view::npos && line.substr(q, 9) == "extension") {
        handled = true;
      } else if (q != std::string_view::npos && line.substr(q, 7) == "include") {
        const size_t a = line.find('"', q);
        const size_t b = a == std::string_view::npos ? a : line.find('"', a + 1);
        if (b != std::string_view::npos) {
          const std::string name(line.substr(a + 1, b - a - 1));
          bool found = false;
          for (const auto& d : dirs) {
            if (fs::exists(d / name)) {
              if (!expand(d / name, out, depth + 1, err)) {
                return false;
              }
              found = true;
              break;
            }
          }
          if (!found) {
            err = "include " + name + " from " + file.string();
            return false;
          }
          handled = true;
        }
      }
    }
    if (!handled) {
      out.append(line);
    }
    out.push_back('\n');
    at = end + 1;
  }
  return true;
}

GLuint compile(const std::string& name, const std::string& src) {
  GLuint sh = glCreateShader(GL_COMPUTE_SHADER);
  const char* s = src.c_str();
  glShaderSource(sh, 1, &s, nullptr);
  glCompileShader(sh);
  GLint ok = 0;
  glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[4096];
    glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
    lg::error("fsr3: {} failed to compile:\n{}", name, log);
    glDeleteShader(sh);
    return 0;
  }
  GLuint prog = glCreateProgram();
  glAttachShader(prog, sh);
  glLinkProgram(prog);
  glDeleteShader(sh);
  glGetProgramiv(prog, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[4096];
    glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
    lg::error("fsr3: {} failed to link:\n{}", name, log);
    glDeleteProgram(prog);
    return 0;
  }
  return prog;
}

}  // namespace

Fsr3Upscaler::~Fsr3Upscaler() {
  release();
  for (auto& variant : m_prog) {
    for (auto& p : variant) {
      if (p.id) {
        glDeleteProgram(p.id);
      }
    }
  }
  for (auto& p : m_reactive_prog) {
    if (p.id) {
      glDeleteProgram(p.id);
    }
  }
  if (m_samplers[0]) {
    glDeleteSamplers(3, m_samplers);
  }
  if (m_ubo[0]) {
    glDeleteBuffers(CB_COUNT, m_ubo);
  }
  if (m_atomic) {
    glDeleteBuffers(1, &m_atomic);
  }
  if (m_scratch_fbo) {
    glDeleteFramebuffers(1, &m_scratch_fbo);
  }
}

bool Fsr3Upscaler::context_supported() {
  GLint major = 0, minor = 0;
  glGetIntegerv(GL_MAJOR_VERSION, &major);
  glGetIntegerv(GL_MINOR_VERSION, &minor);
  const bool gl43 = (major > 4 || (major == 4 && minor >= 3)) && GLAD_GL_VERSION_4_3 &&
                    glad_glDispatchCompute != nullptr;
  const bool files = fs::exists(gpu_root() / "fsr3upscaler" / "ffx_fsr3upscaler_accumulate.h") &&
                     fs::exists(pass_root() / "fsr3_accumulate.comp");
  if (!gl43 || !files) {
    lg::warn("fsr3: unavailable (GL {}.{}{}), vanilla rendering in its place", major, minor,
             files ? "" : ", headers missing");
  }
  return gl43 && files;
}

bool Fsr3Upscaler::build(int variant) {
  auto make = [&](const char* pass, Program& out) {
    const fs::path file = pass_root() / (std::string("fsr3_") + pass + ".comp");
    std::string body, err;
    if (!expand(file, body, 0, err)) {
      lg::error("fsr3: {}: {}", pass, err);
      return false;
    }
    // the pass's binding names, numbered per kind
    const size_t at = body.find("// fsr3-bindings srv:");
    if (at == std::string::npos) {
      lg::error("fsr3: {}: no fsr3-bindings line", pass);
      return false;
    }
    const std::string spec = body.substr(at, body.find('\n', at) - at);
    std::string head =
        "#version 430 core\n"
        "#define FFX_GPU 1\n"
        "#define FFX_GLSL 1\n"
        "#define FFX_HALF 0\n"
        "#define FFX_SPD_NO_WAVE_OPERATIONS 1\n"
        "#define FFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS 1\n"
        "#define FFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS 0\n"
        "#define FFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH 1\n"
        "#define FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING 1\n";
    head +=
        std::string("#define FFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT ") + (variant ? "1\n" : "0\n");
    out = Program();
    out.srv.fill(-1);
    out.uav.fill(-1);
    out.cb.fill(-1);
    int kind = -1;  // 0 srv, 1 uav, 2 cb
    size_t p = spec.find("srv:");
    std::string word;
    auto flush = [&]() -> bool {
      if (word.empty()) {
        return true;
      }
      const std::string w = word;
      word.clear();
      if (w == "|") {
        return true;
      }
      if (w == "srv:" || w == "uav:" || w == "cb:") {
        kind = w == "srv:" ? 0 : w == "uav:" ? 1 : 2;
        return true;
      }
      if (kind == 2) {
        const int cb = w == "FSR3UPSCALER" ? CB_FSR3UPSCALER
                       : w == "SPD"        ? CB_SPD
                       : w == "RCAS"       ? CB_RCAS
                       : w == "REACTIVE"   ? CB_REACTIVE
                                           : -1;
        if (cb < 0 || out.ncb >= (int)out.cb.size()) {
          lg::error("fsr3: {}: uniform block {}", pass, w);
          return false;
        }
        head += "#define FSR3UPSCALER_BIND_CB_" + w + " " + std::to_string(out.ncb) + "\n";
        out.cb[out.ncb++] = cb;
        return true;
      }
      const int sym = lookup(w);
      if (sym < 0) {
        lg::error("fsr3: {}: binding {}", pass, w);
        return false;
      }
      if (kind == 0) {
        if (out.nsrv >= (int)out.srv.size()) {
          return false;
        }
        head += "#define FSR3UPSCALER_BIND_SRV_" + w + " " + std::to_string(out.nsrv) + "\n";
        out.srv[out.nsrv++] = sym;
      } else if (sym == S_SPD_GLOBAL_ATOMIC) {
        head += "#define FSR3UPSCALER_BIND_UAV_" + w + " 0\n";
        out.atomic = true;
      } else {
        if (out.nuav >= (int)out.uav.size()) {
          lg::error("fsr3: {}: more than {} images", pass, out.uav.size());
          return false;
        }
        head += "#define FSR3UPSCALER_BIND_UAV_" + w + " " + std::to_string(out.nuav) + "\n";
        out.uav[out.nuav++] = sym;
      }
      return true;
    };
    for (; p < spec.size(); p++) {
      if (spec[p] == ' ') {
        if (!flush()) {
          return false;
        }
      } else {
        word.push_back(spec[p]);
      }
    }
    if (!flush()) {
      return false;
    }
    out.id = compile(std::string("fsr3_") + pass, head + "#line 1\n" + body);
    return out.id != 0;
  };

  perf::AddMs hitch_t(perf::Counter::HITCH_UPSCALER_MS);
  Timer t;
  for (int i = 0; i < P_COUNT; i++) {
    if (!make(kPassNames[i], m_prog[variant][i])) {
      return false;
    }
  }
  if (!make("autogen_reactive", m_reactive_prog[variant])) {
    return false;
  }
  lg::info("fsr3: {} programs built in {:.0f} ms ({})", P_COUNT + 1, t.getMs(),
           variant ? "HDR input" : "SDR input");
  return true;
}

bool Fsr3Upscaler::ready(bool hdr) {
  if (m_programs) {
    return m_programs->ready(hdr);
  }
  const int v = hdr ? 1 : 0;
  if (m_state[v] == 0) {
    m_state[v] = build(v) ? 1 : -1;
    if (m_state[v] < 0) {
      lg::warn("fsr3: programs failed to build, vanilla rendering in its place");
    }
  }
  return m_state[v] == 1;
}

void Fsr3Upscaler::clear_jitter() {
  scene_jitter::set(0.f, 0.f);
}

void Fsr3Upscaler::begin_frame(int rw, int rh, int dw, s64 index) {
  // ffxFsr3UpscalerGetJitterPhaseCount, ffxFsr3UpscalerGetJitterOffset
  const int phases = scene_jitter::phase_count(rw, dw);
  const int i = index >= 0 ? (int)((u64)index % (u64)phases)
                           : (int)(m_jitter_index++ % (unsigned)phases);
  float jx, jy;
  scene_jitter::halton_offset(i, &jx, &jy);
  const int mode = Gfx::g_global_settings.fsr3_jitter;
  if (mode == 0) {
    jx = jy = 0.f;
  }
  m_jitter[0] = jx;
  m_jitter[1] = jy;
  // the jitter is the content's offset in texel-grid pixels; GL rows grow with NDC y.
  // Modes 2 and 3 negate one axis of the projection only (a sign check)
  scene_jitter::set((mode == 3 ? -2.f : 2.f) * jx / (float)rw,
                    (mode == 2 ? -2.f : 2.f) * jy / (float)rh);
  m_reactive_built = false;
  m_opaque_captured = false;
}

void Fsr3Upscaler::free_targets() {
  for (auto& t : m_tex) {
    if (t.id) {
      glDeleteTextures(1, &t.id);
    }
    t = Tex();
  }
  m_display.clear();
  m_rw = m_rh = m_dw = m_dh = 0;
}

void Fsr3Upscaler::release() {
  free_targets();
  m_first = true;
}

void Fsr3Upscaler::clear(const Tex& t, float v0, float v1, float v2, float v3) {
  // glClearBuffer honours the scissor and colour mask the last bucket left
  const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
  GLboolean mask[4];
  glGetBooleani_v(GL_COLOR_WRITEMASK, 0, mask);
  glDisable(GL_SCISSOR_TEST);
  glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_scratch_fbo);
  for (int level = 0; level < t.levels; level++) {
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.id, level);
    const GLenum draw = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &draw);
    if (t.fmt == GL_R32UI) {
      const GLuint u[4] = {(GLuint)v0, (GLuint)v1, (GLuint)v2, (GLuint)v3};
      glClearBufferuiv(GL_COLOR, 0, u);
    } else {
      const float f[4] = {v0, v1, v2, v3};
      glClearBufferfv(GL_COLOR, 0, f);
    }
  }
  glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
  glColorMaski(0, mask[0], mask[1], mask[2], mask[3]);
  if (scissor) {
    glEnable(GL_SCISSOR_TEST);
  }
}

void Fsr3Upscaler::alloc(int rw, int rh, int dw, int dh, bool hdr) {
  if (rw == m_rw && rh == m_rh && dw == m_dw && dh == m_dh && hdr == m_hdr && m_tex[0].id) {
    return;
  }
  free_targets();
  if (!m_scratch_fbo) {
    glGenFramebuffers(1, &m_scratch_fbo);
    glGenSamplers(3, m_samplers);
    const GLenum min[3] = {GL_LINEAR, GL_LINEAR_MIPMAP_NEAREST, GL_NEAREST};
    const GLenum mag[3] = {GL_LINEAR, GL_LINEAR, GL_NEAREST};
    for (int i = 0; i < 3; i++) {
      glSamplerParameteri(m_samplers[i], GL_TEXTURE_MIN_FILTER, min[i]);
      glSamplerParameteri(m_samplers[i], GL_TEXTURE_MAG_FILTER, mag[i]);
      glSamplerParameteri(m_samplers[i], GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glSamplerParameteri(m_samplers[i], GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glGenBuffers(CB_COUNT, m_ubo);
    const size_t sizes[CB_COUNT] = {sizeof(Constants), sizeof(SpdConstants), 16,
                                    sizeof(ReactiveConstants)};
    for (int i = 0; i < CB_COUNT; i++) {
      glBindBuffer(GL_UNIFORM_BUFFER, m_ubo[i]);
      glBufferData(GL_UNIFORM_BUFFER, (GLsizeiptr)std::max<size_t>(sizes[i], 16), nullptr,
                   GL_DYNAMIC_DRAW);
    }
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
    glGenBuffers(1, &m_atomic);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_atomic);
    const GLuint zero = 0;
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(zero), &zero, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
  }

  const int hw = std::max(1, rw / 2), hh = std::max(1, rh / 2);
  auto make = [&](int r, GLenum fmt, int w, int h, int levels = 1) {
    Tex& t = m_tex[r];
    glGenTextures(1, &t.id);
    glBindTexture(GL_TEXTURE_2D, t.id);
    glTexStorage2D(GL_TEXTURE_2D, levels, fmt, w, h);
    t.fmt = fmt;
    t.w = w;
    t.h = h;
    t.levels = levels;
  };
  make(R_EXPOSURE, GL_RG32F, 1, 1);
  make(R_ZERO, GL_R8, 1, 1);
  make(R_OPAQUE, hdr ? GL_RGBA16F : GL_RGBA8, rw, rh);
  make(R_REACTIVE, GL_R32F, rw, rh);
  make(R_DILATED_DEPTH, GL_R32F, rw, rh);
  make(R_DILATED_MV, GL_RG16F, rw, rh);
  make(R_RECON_DEPTH, GL_R32UI, rw, rh);
  make(R_INTERMEDIATE, GL_R16F, rw, rh);
  make(R_LUMA_1, GL_R16F, rw, rh);
  make(R_LUMA_2, GL_R16F, rw, rh);
  make(R_LUMA_HISTORY_1, GL_RGBA8, rw, rh);
  make(R_LUMA_HISTORY_2, GL_RGBA8, rw, rh);
  make(R_ACCUMULATION_1, GL_R8, rw, rh);
  make(R_ACCUMULATION_2, GL_R8, rw, rh);
  make(R_SHADING_CHANGE, GL_R8, hw, hh);
  const int spd_levels = 1 + (int)std::floor(std::log2((float)std::max(hw, hh)));
  make(R_SPD_MIPS, GL_RG16F, hw, hh, std::max(6, spd_levels));
  make(R_FARTHEST_MIP1, GL_R16F, hw, hh);
  make(R_FRAME_INFO, GL_RGBA32F, 1, 1);
  make(R_DILATED_REACTIVE, GL_RGBA8, rw, rh);
  make(R_NEW_LOCKS, GL_R8, dw, dh);
  make(R_UPSCALED_1, GL_RGBA16F, dw, dh);
  make(R_UPSCALED_2, GL_RGBA16F, dw, dh);
  make(R_LANCZOS_LUT, GL_R16_SNORM, 128, 1);
  {
    int16_t lut[128];
    for (int i = 0; i < 128; i++) {
      lut[i] = (int16_t)std::lround(lanczos2(2.f * (float)i / 127.f) * 32767.f);
    }
    glBindTexture(GL_TEXTURE_2D, m_tex[R_LANCZOS_LUT].id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 128, 1, GL_RED, GL_SHORT, lut);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  }
  glBindTexture(GL_TEXTURE_2D, 0);
  GLint prev_draw = 0;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw);
  for (int r = 0; r < R_COUNT; r++) {
    if (r != R_LANCZOS_LUT) {
      clear(m_tex[r], 0.f);
    }
  }

  // display target: the output image, with depth and stencil for the text buckets
  m_display = Fbo();
  glGenFramebuffers(1, &m_display.fbo_id);
  glBindFramebuffer(GL_FRAMEBUFFER, m_display.fbo_id);
  GLuint tex, z;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexStorage2D(GL_TEXTURE_2D, 1, hdr ? GL_RGBA16F : GL_RGBA8, dw, dh);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
  glGenTextures(1, &z);
  glBindTexture(GL_TEXTURE_2D, z);
  glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH24_STENCIL8, dw, dh);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, z, 0);
  const GLenum draw = GL_COLOR_ATTACHMENT0;
  glDrawBuffers(1, &draw);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    lg::error("fsr3: incomplete display framebuffer ({}x{})", dw, dh);
  }
  glBindTexture(GL_TEXTURE_2D, 0);
  m_display.tex_id = tex;
  m_display.zbuf_stencil_id = z;
  m_display.zbuf_is_texture = true;
  m_display.valid = true;
  m_display.multisample_count = 1;
  m_display.width = dw;
  m_display.height = dh;
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev_draw);

  m_rw = rw;
  m_rh = rh;
  m_dw = dw;
  m_dh = dh;
  m_hdr = hdr;
  m_first = true;
  lg::info("fsr3: targets {}x{} -> {}x{} {}", rw, rh, dw, dh, hdr ? "RGBA16F" : "RGBA8");
}

unsigned Fsr3Upscaler::srv_texture(int sym) const {
  const bool odd = m_frame & 1;
  switch (sym) {
    case S_INPUT_COLOR:
      return m_in_colour;
    case S_INPUT_DEPTH:
      return m_in_depth;
    case S_INPUT_MOTION_VECTORS:
      return m_in_velocity ? m_in_velocity : m_tex[R_ZERO].id;
    case S_INPUT_EXPOSURE:
      return m_tex[R_EXPOSURE].id;
    case S_REACTIVE_MASK:
      return m_reactive_built ? m_tex[R_REACTIVE].id : m_tex[R_ZERO].id;
    case S_TRANSPARENCY_AND_COMPOSITION_MASK:
      return m_tex[R_ZERO].id;
    case S_INPUT_OPAQUE_ONLY:
      return m_tex[R_OPAQUE].id;
    case S_DILATED_REACTIVE_MASKS:
      return m_tex[R_DILATED_REACTIVE].id;
    case S_DILATED_MOTION_VECTORS:
      return m_tex[R_DILATED_MV].id;
    case S_DILATED_DEPTH:
      return m_tex[R_DILATED_DEPTH].id;
    case S_INTERNAL_UPSCALED:
      return m_tex[odd ? R_UPSCALED_2 : R_UPSCALED_1].id;
    case S_RCAS_INPUT:
      return m_tex[odd ? R_UPSCALED_1 : R_UPSCALED_2].id;
    case S_LANCZOS_LUT:
      return m_tex[R_LANCZOS_LUT].id;
    case S_FARTHEST_DEPTH_MIP1:
      return m_tex[R_FARTHEST_MIP1].id;
    case S_FARTHEST_DEPTH:
    case S_LUMA_INSTABILITY:
      return m_tex[R_INTERMEDIATE].id;
    case S_CURRENT_LUMA:
      return m_tex[odd ? R_LUMA_2 : R_LUMA_1].id;
    case S_PREVIOUS_LUMA:
      return m_tex[odd ? R_LUMA_1 : R_LUMA_2].id;
    case S_FRAME_INFO:
      return m_tex[R_FRAME_INFO].id;
    case S_LUMA_HISTORY:
      return m_tex[odd ? R_LUMA_HISTORY_2 : R_LUMA_HISTORY_1].id;
    case S_RECONSTRUCTED_PREV_NEAREST_DEPTH:
      return m_tex[R_RECON_DEPTH].id;
    case S_ACCUMULATION:
      return m_tex[odd ? R_ACCUMULATION_2 : R_ACCUMULATION_1].id;
    case S_SHADING_CHANGE:
      return m_tex[R_SHADING_CHANGE].id;
    case S_SPD_MIPS:
      return m_tex[R_SPD_MIPS].id;
    case S_NEW_LOCKS:
      return m_tex[R_NEW_LOCKS].id;
    default:
      return 0;
  }
}

void Fsr3Upscaler::uav_image(int sym, unsigned* tex, int* level, unsigned* fmt) const {
  const bool odd = m_frame & 1;
  int r = -1;
  *level = 0;
  switch (sym) {
    case S_UPSCALED_OUTPUT:
      *tex = *m_display.tex_id;
      *fmt = m_hdr ? GL_RGBA16F : GL_RGBA8;
      return;
    case S_INTERNAL_UPSCALED:
      r = odd ? R_UPSCALED_1 : R_UPSCALED_2;
      break;
    case S_ACCUMULATION:
      r = odd ? R_ACCUMULATION_1 : R_ACCUMULATION_2;
      break;
    case S_LUMA_HISTORY:
      r = odd ? R_LUMA_HISTORY_1 : R_LUMA_HISTORY_2;
      break;
    case S_CURRENT_LUMA:
      r = odd ? R_LUMA_2 : R_LUMA_1;
      break;
    case S_DILATED_MOTION_VECTORS:
      r = R_DILATED_MV;
      break;
    case S_DILATED_DEPTH:
      r = R_DILATED_DEPTH;
      break;
    case S_RECONSTRUCTED_PREV_NEAREST_DEPTH:
      r = R_RECON_DEPTH;
      break;
    case S_FARTHEST_DEPTH:
    case S_LUMA_INSTABILITY:
      r = R_INTERMEDIATE;
      break;
    case S_FARTHEST_DEPTH_MIP1:
      r = R_FARTHEST_MIP1;
      break;
    case S_FRAME_INFO:
      r = R_FRAME_INFO;
      break;
    case S_DILATED_REACTIVE_MASKS:
      r = R_DILATED_REACTIVE;
      break;
    case S_NEW_LOCKS:
      r = R_NEW_LOCKS;
      break;
    case S_SHADING_CHANGE:
      r = R_SHADING_CHANGE;
      break;
    case S_AUTOREACTIVE:
      r = R_REACTIVE;
      break;
    default:
      if (sym >= S_SPD_MIPS_LEVEL_0 && sym < S_SPD_MIPS_LEVEL_0 + 6) {
        r = R_SPD_MIPS;
        *level = sym - S_SPD_MIPS_LEVEL_0;
      }
      break;
  }
  if (r < 0) {
    *tex = 0;
    *fmt = GL_R32F;
    return;
  }
  *tex = m_tex[r].id;
  *fmt = m_tex[r].fmt;
}

void Fsr3Upscaler::dispatch(const Program& p, int gx, int gy) {
  glUseProgram(p.id);
  for (int u = 0; u < p.nsrv; u++) {
    glActiveTexture(GL_TEXTURE0 + u);
    glBindTexture(GL_TEXTURE_2D, srv_texture(p.srv[u]));
    const int s = p.srv[u] == S_SPD_MIPS                           ? 1
                  : p.srv[u] == S_RECONSTRUCTED_PREV_NEAREST_DEPTH ? 2
                                                                   : 0;
    glBindSampler(u, m_samplers[s]);
  }
  for (int u = 0; u < p.nuav; u++) {
    unsigned tex = 0, fmt = 0;
    int level = 0;
    uav_image(p.uav[u], &tex, &level, &fmt);
    if (tex) {
      glBindImageTexture(u, tex, level, GL_FALSE, 0, GL_READ_WRITE, fmt);
    }
  }
  for (int b = 0; b < p.ncb; b++) {
    glBindBufferBase(GL_UNIFORM_BUFFER, b, m_ubo[p.cb[b]]);
  }
  if (p.atomic) {
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_atomic);
  }
  glDispatchCompute((GLuint)std::max(1, gx), (GLuint)std::max(1, gy), 1);
  glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT |
                  GL_SHADER_STORAGE_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT);
}

void Fsr3Upscaler::capture_opaque(unsigned src_fbo, int rw, int rh, int dw, int dh, bool hdr) {
  if (!ready(hdr)) {
    return;
  }
  alloc(rw, rh, dw, dh, hdr);
  perf::Scope ps(perf::Group::FSR3_REACTIVE);
  GLint prev_draw = 0, prev_read = 0;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw);
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
  const bool scissor = glIsEnabled(GL_SCISSOR_TEST);
  glDisable(GL_SCISSOR_TEST);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, src_fbo);
  glReadBuffer(GL_COLOR_ATTACHMENT0);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_scratch_fbo);
  glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         m_tex[R_OPAQUE].id, 0);
  const GLenum draw = GL_COLOR_ATTACHMENT0;
  glDrawBuffers(1, &draw);
  glBlitFramebuffer(0, 0, rw, rh, 0, 0, rw, rh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev_draw);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_read);
  if (scissor) {
    glEnable(GL_SCISSOR_TEST);
  }
  m_opaque_captured = true;
}

void Fsr3Upscaler::build_reactive(unsigned colour_tex, int rw, int rh) {
  if (!m_opaque_captured || m_rw != rw || m_rh != rh || !ready(m_hdr)) {
    return;
  }
  perf::Scope ps(perf::Group::FSR3_REACTIVE);
  GLint prog = 0;
  glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
  GLint unit = 0;
  glGetIntegerv(GL_ACTIVE_TEXTURE, &unit);
  // the SDK sample's autogen settings: tonemapped, per-channel maximum, thresholded
  ReactiveConstants rc{1.f, 0.2f, 0.9f, 1u | 4u | 8u};
  glBindBuffer(GL_UNIFORM_BUFFER, m_ubo[CB_REACTIVE]);
  glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(rc), &rc);
  glBindBuffer(GL_UNIFORM_BUFFER, 0);
  m_in_colour = colour_tex;
  dispatch(programs().m_reactive_prog[m_hdr ? 1 : 0], (rw + 7) / 8, (rh + 7) / 8);
  for (int u = 0; u < 2; u++) {
    glBindSampler(u, 0);
    glActiveTexture(GL_TEXTURE0 + u);
    glBindTexture(GL_TEXTURE_2D, 0);
  }
  glActiveTexture(unit);
  glUseProgram(prog);
  m_reactive_built = true;
}

void Fsr3Upscaler::run(SharedRenderState* rs,
                       ScopedProfilerNode& prof,
                       const Inputs& in,
                       int dw,
                       int dh) {
  const int rw = rs->render_fb_w, rh = rs->render_fb_h;
  // the program, VAO, active unit and textures here change every frame: never cached
  static glq::SavedState saved("fsr3", glq::kLive, 0xffff, 0);
  saved.capture();
  alloc(rw, rh, dw, dh, in.hdr);
  const auto& gs = Gfx::g_global_settings;
  const bool odd = m_frame & 1;
  const bool reset = in.reset || m_first || !in.velocity;

  {
    perf::Scope ps(perf::Group::FSR3_PREPARE);
    if (m_first) {
      for (int r : {R_ACCUMULATION_1, R_ACCUMULATION_2, R_LUMA_1, R_LUMA_2}) {
        clear(m_tex[r], 0.f);
      }
    }
    if (reset) {
      clear(m_tex[odd ? R_ACCUMULATION_2 : R_ACCUMULATION_1], 0.f);
      clear(m_tex[R_FRAME_INFO], -1.f, 1.f, 0.f, 0.f);
      m_resets++;
    }
    clear(m_tex[R_SPD_MIPS], 0.f);
    clear(m_tex[R_RECON_DEPTH], 0.f);  // inverted depth: the max store starts at 0
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, rs->render_fb);
    const GLuint zero = 0;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_atomic);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(zero), &zero);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
  }
  m_first = false;

  // constants (fsr3upscalerDispatch)
  Constants c{};
  c.render_size[0] = rw;
  c.render_size[1] = rh;
  c.prev_render_size[0] = m_prev_render[0] ? m_prev_render[0] : rw;
  c.prev_render_size[1] = m_prev_render[1] ? m_prev_render[1] : rh;
  c.upscale_size[0] = dw;
  c.upscale_size[1] = dh;
  c.prev_upscale_size[0] = m_prev_upscale[0] ? m_prev_upscale[0] : dw;
  c.prev_upscale_size[1] = m_prev_upscale[1] ? m_prev_upscale[1] : dh;
  c.max_render_size[0] = rw;
  c.max_render_size[1] = rh;
  c.max_upscale_size[0] = dw;
  c.max_upscale_size[1] = dh;
  // view depth = [1] / (device depth - [0]); clip w = k0 / (2 d - 1 - k1), in game units
  c.device_to_view_depth[0] = 0.5f * (1.f + in.w_k1);
  c.device_to_view_depth[1] = 0.5f * in.w_k0;
  c.device_to_view_depth[2] = in.tan_half_x;
  c.device_to_view_depth[3] = in.tan_half_y;
  c.prev_jitter[0] = m_prev_jitter[0];
  c.prev_jitter[1] = m_prev_jitter[1];
  c.jitter[0] = m_jitter[0];
  c.jitter[1] = m_jitter[1];
  c.mv_scale[0] = 1.f / (float)rw;
  c.mv_scale[1] = 1.f / (float)rh;
  c.downscale[0] = (float)rw / (float)dw;
  c.downscale[1] = (float)rh / (float)dh;
  c.tan_half_fov = in.tan_half_x;
  const int phases = scene_jitter::phase_count(rw, dw);
  if (reset || m_phase_count == 0.f) {
    m_phase_count = (float)phases;
  } else if ((float)phases > m_phase_count) {
    m_phase_count += 1.f;
  } else if ((float)phases < m_phase_count) {
    m_phase_count -= 1.f;
  }
  c.jitter_phase_count = m_phase_count;
  c.delta_time = std::clamp(in.frame_ms / 1000.f, 0.f, 1.f);
  c.delta_pre_exposure = 1.f;
  c.view_to_meters = 1.f / 4096.f;
  m_frame_index = reset ? 0.f : m_frame_index + 1.f;
  c.frame_index = m_frame_index;
  c.velocity_factor = 1.f;
  c.reactiveness_scale = 1.f;
  c.shading_change_scale = 1.f;
  c.accumulation_added_per_frame = 1.f / 3.f;
  c.min_disocclusion_accumulation = -1.f / 3.f;
  m_prev_jitter[0] = m_jitter[0];
  m_prev_jitter[1] = m_jitter[1];
  m_prev_render[0] = rw;
  m_prev_render[1] = rh;
  m_prev_upscale[0] = dw;
  m_prev_upscale[1] = dh;

  FfxUInt32 groups[2], offset[2], wg_mips[2];
  FfxUInt32 rect[4] = {0, 0, (FfxUInt32)rw, (FfxUInt32)rh};
  ffxSpdSetup(groups, offset, wg_mips, rect);
  SpdConstants spd{wg_mips[1], wg_mips[0], {offset[0], offset[1]}, {(uint32_t)rw, (uint32_t)rh}};
  FfxUInt32 rcas[4];
  FsrRcasCon(rcas, -2.f * gs.fsr3_sharpness + 2.f);

  glBindBuffer(GL_UNIFORM_BUFFER, m_ubo[CB_FSR3UPSCALER]);
  glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(c), &c);
  glBindBuffer(GL_UNIFORM_BUFFER, m_ubo[CB_SPD]);
  glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(spd), &spd);
  glBindBuffer(GL_UNIFORM_BUFFER, m_ubo[CB_RCAS]);
  glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(rcas), rcas);
  glBindBuffer(GL_UNIFORM_BUFFER, 0);

  m_in_colour = in.colour;
  m_in_depth = in.depth;
  m_in_velocity = in.velocity;
  const auto& P = programs().m_prog[in.hdr ? 1 : 0];
  const int sx = (rw + 7) / 8, sy = (rh + 7) / 8;
  const int dx = (dw + 7) / 8, dy = (dh + 7) / 8;
  {
    perf::Scope ps(perf::Group::FSR3_PREPARE);
    dispatch(P[P_PREPARE_INPUTS], sx, sy);
  }
  {
    perf::Scope ps(perf::Group::FSR3_LUMA);
    dispatch(P[P_LUMA_PYRAMID], (int)groups[0], (int)groups[1]);
    dispatch(P[P_SHADING_CHANGE_PYRAMID], (int)groups[0], (int)groups[1]);
    dispatch(P[P_SHADING_CHANGE], ((int)(rw * 0.5f) + 7) / 8, ((int)(rh * 0.5f) + 7) / 8);
  }
  {
    perf::Scope ps(perf::Group::FSR3_REACTIVITY);
    dispatch(P[P_PREPARE_REACTIVITY], sx, sy);
    dispatch(P[P_LUMA_INSTABILITY], sx, sy);
  }
  {
    perf::Scope ps(perf::Group::FSR3_ACCUMULATE);
    dispatch(P[P_ACCUMULATE], dx, dy);
  }
  {
    perf::Scope ps(perf::Group::FSR3_RCAS);
    dispatch(P[P_RCAS], (dw + 15) / 16, (dh + 15) / 16);
    if (gs.fsr3_debug) {
      dispatch(P[P_DEBUG_VIEW], dx, dy);
    }
  }
  prof.add_draw_call(P_COUNT);
  m_frame = (m_frame + 1) % 16;
  m_reactive_built = false;
  m_opaque_captured = false;
  m_status = fmt::format("{}x{} -> {}x{}, jitter ({:.3f}, {:.3f}) of {} phases, {} resets", rw, rh,
                         dw, dh, m_jitter[0], m_jitter[1], (int)m_phase_count, m_resets);

  for (int u = 0; u < 16; u++) {
    glBindSampler(u, 0);
  }
  for (int u = 0; u < 8; u++) {
    glBindImageTexture(u, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
  }
  // the text buckets draw at display size: an empty depth and stencil, as at the frame's start
  glBindFramebuffer(GL_FRAMEBUFFER, m_display.fbo_id);
  glDisable(GL_SCISSOR_TEST);
  glDepthMask(GL_TRUE);
  glStencilMask(0xFF);
  glClearBufferfi(GL_DEPTH_STENCIL, 0, 0.f, 0);
  saved.restore();
  glBindFramebuffer(GL_FRAMEBUFFER, m_display.fbo_id);
  glViewport(0, 0, dw, dh);
  rs->render_fb = m_display.fbo_id;
  rs->render_fb_w = dw;
  rs->render_fb_h = dh;
  rs->render_fb_depth_tex = *m_display.zbuf_stencil_id;
  clear_jitter();
}

void Fsr3Upscaler::dump_output(const std::string& stem, u64 frame) {
  if (!m_display.valid) {
    return;
  }
  std::vector<float> px((size_t)m_dw * m_dh * 4);
  GLint prev = 0;
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, m_display.fbo_id);
  glReadBuffer(GL_COLOR_ATTACHMENT0);
  glPixelStorei(GL_PACK_ALIGNMENT, 4);
  glReadPixels(0, 0, m_dw, m_dh, GL_RGBA, GL_FLOAT, px.data());
  glBindFramebuffer(GL_READ_FRAMEBUFFER, prev);
  exr::StringAttrs attrs;
  attrs["frame"] = std::to_string(frame);
  attrs["content"] = "FSR 3 output, display-encoded, before the text buckets";
  attrs["render"] = fmt::format("{}x{}", m_rw, m_rh);
  exr::write_rgb_half(stem + "_fsr3.exr", px.data(), m_dw, m_dh, 4, true, attrs);
}

void Fsr3Upscaler::draw_debug_window() {
  auto& gs = Gfx::g_global_settings;
  ImGui::TextUnformatted(Gfx::g_fsr3_supported ? "context: supported" : "context: unsupported");
  ImGui::TextUnformatted(m_status.c_str());
  ImGui::Checkbox("Debug view", &gs.fsr3_debug);
  ImGui::SliderInt("Jitter (1 on, 0 off, 2/3 y/x negated)", &gs.fsr3_jitter, 0, 3);
  ImGui::SliderFloat("Sharpness", &gs.fsr3_sharpness, 0.f, 1.f);
}
