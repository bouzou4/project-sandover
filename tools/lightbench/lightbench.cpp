/*!
 * lightbench: render a level's background (tfrag, tie, shrub) offline from its FR3 file with a
 * synthesized camera and synthesized time-of-day weights. No GOAL runtime is involved.
 *
 * Jak 1 only for now: the camera math mirrors engine/gfx/math-camera.gc and
 * engine/camera/cam-update.gc, the continue points come from engine/level/level-info.gc, and
 * the shaders are compiled with the Jak 1 constants.
 */

#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <numeric>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "common/goal_constants.h"
#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "common/util/Timer.h"

#include "game/graphics/gfx.h"
#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/opengl_renderer/background/Shrub.h"
#include "game/graphics/opengl_renderer/background/TFragment.h"
#include "game/graphics/opengl_renderer/background/Tie3.h"
#include "game/graphics/opengl_renderer/background/background_common.h"
#include "game/graphics/opengl_renderer/loader/Loader.h"
#include "game/graphics/texture/TexturePool.h"

#include "third-party/CLI11.hpp"
#include "third-party/SDL/include/SDL3/SDL.h"
#include "third-party/glad/include/glad/glad.h"
#include "third-party/json.hpp"

using math::Vector4f;

// defined in background_common.cpp, not exported by its header
std::array<math::Vector4f, 4> make_new_cam_mat(const math::Vector4f cam_T_w[4],
                                               const math::Vector4f persp[4],
                                               float fog_constant,
                                               float hvdf_z);

namespace {

constexpr float kMeters = 4096.f;

[[noreturn]] void fail(const std::string& msg) {
  fmt::print(stderr, "lightbench: {}\n", msg);
  std::exit(1);
}

/*!
 * Background shaders swapped in for the texture-id pass. Each keeps the upstream vertex shader
 * verbatim and replaces the fragment shader.
 */
struct IdShader {
  ShaderId id;
  const char* upstream;
  const char* generated;
  bool discard_all;
};
constexpr IdShader kIdShaders[] = {
    {ShaderId::TFRAG3, "tfrag3", "lb_id_tfrag3", false},
    {ShaderId::ETIE_BASE, "etie_base", "lb_id_etie_base", false},
    {ShaderId::TIE_WIND, "tie_wind", "lb_id_tie_wind", false},
    {ShaderId::SHRUB, "shrub", "lb_id_shrub", false},
    {ShaderId::ETIE, "etie", "lb_id_etie", true},
};

const char* kIdFrag = R"(#version 410 core
layout (location = 0) out uvec4 id_out;
in vec4 fragment_color;
in vec3 tex_coord;
uniform sampler2D tex_T0;
uniform float alpha_min;
uniform float alpha_max;
uniform uint lb_source;
void main() {
  vec4 T0 = texture(tex_T0, tex_coord.xy);
  float a = fragment_color.a * T0.a;
  if (a < alpha_min || a > alpha_max) {
    discard;
  }
  uvec3 b = uvec3(round(T0.rgb * 255.0));
  id_out = uvec4(b.r | (b.g << 8) | (b.b << 16) | (lb_source << 24), 0, 0, 0);
}
)";

const char* kDiscardFrag = R"(#version 410 core
layout (location = 0) out uvec4 id_out;
void main() {
  discard;
}
)";

struct ContinuePoint {
  std::string name;
  std::string level;
  std::vector<std::string> levels;  // lev0, lev1
  float trans[3] = {0, 0, 0};
  float rot[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
};

float goal_field(const std::string& text, const std::string& key) {
  std::smatch m;
  std::regex re(":" + key + R"(\s+(-?[0-9.]+))");
  if (std::regex_search(text, m, re)) {
    return std::stof(m[1]);
  }
  return 0.f;
}

/*!
 * Find a continue point by name in level-info.gc. camera-rot there is the 3x3 of
 * *math-camera* inv-camera-rot (see trsq->continue-point in game-info.gc).
 */
std::optional<ContinuePoint> find_continue_point(const std::string& name) {
  auto src = file_util::read_text_file(
      file_util::get_file_path({"goal_src", "jak1", "engine", "level", "level-info.gc"}));
  auto pos = src.find(":name \"" + name + "\"");
  if (pos == std::string::npos) {
    return std::nullopt;
  }
  auto end = src.find("'continue-point", pos);
  auto block = src.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
  ContinuePoint cp;
  cp.name = name;
  std::smatch m;
  if (std::regex_search(block, m, std::regex(R"(:level\s+'([\w-]+))"))) {
    cp.level = m[1];
  }
  for (const char* key : {"lev0", "lev1"}) {
    if (std::regex_search(block, m, std::regex(std::string(":") + key + R"(\s+'([\w-]+))"))) {
      cp.levels.push_back(m[1]);
    }
  }
  if (std::regex_search(block, m,
                        std::regex(R"(:camera-trans\s*\(new 'static 'vector([^)]*)\))"))) {
    std::string v = m[1];
    cp.trans[0] = goal_field(v, "x");
    cp.trans[1] = goal_field(v, "y");
    cp.trans[2] = goal_field(v, "z");
  } else {
    return std::nullopt;
  }
  if (std::regex_search(block, m,
                        std::regex(R"(:camera-rot\s*\(new 'static 'array float 9([^)]*)\))"))) {
    std::stringstream ss(m[1].str());
    for (auto& r : cp.rot) {
      ss >> r;
    }
  } else {
    return std::nullopt;
  }
  return cp;
}

struct Fog {
  float color[3] = {0, 0, 0};
  // start, end, max, min: mood-fog fog-dists order
  float dists[4] = {262144.f, 6717440.f, 255.f, 25.5f};
};

struct TimeOfDay {
  float weights[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  float rgb[8][3] = {};
  bool has_rgb = false;
  std::optional<Fog> fog;
};

/*!
 * Mirror of update-mood-palette / update-mood-fog for one hour, from the JSON mirror of
 * mood-tables.gc. Level effects (flames, lightning) are not simulated.
 */
TimeOfDay eval_hour(const nlohmann::json& mood, float hour) {
  TimeOfDay tod;
  int h = (int)hour;
  float f = hour - h;
  auto interp = [&](const nlohmann::json& table) {
    const auto& e = table.at(((h % 24) + 24) % 24);
    float morph = (1.f - f) * e.at("morph_start").get<float>() + f * e.at("morph_end").get<float>();
    return std::tuple<int, int, float>(e.at("snapshot1"), e.at("snapshot2"), morph);
  };
  auto [s1, s2, morph] = interp(mood.at("palette_interp"));
  if (s1 == s2) {
    tod.weights[s1] = 1.f;
  } else {
    int k = (int)(0.5f + 64.f * morph);
    tod.weights[s1] = (64 - k) / 64.f;
    tod.weights[s2] = k / 64.f;
  }
  auto [f1, f2, fmorph] = interp(mood.at("fog_interp"));
  const auto& a = mood.at("fog").at(f1);
  const auto& b = mood.at("fog").at(f2);
  Fog fog;
  for (int i = 0; i < 3; i++) {
    fog.color[i] = (1.f - fmorph) * a.at("fog_color")[i].get<float>() +
                   fmorph * b.at("fog_color")[i].get<float>();
  }
  for (int i = 0; i < 4; i++) {
    fog.dists[i] = (1.f - fmorph) * a.at("fog_dists")[i].get<float>() +
                   fmorph * b.at("fog_dists")[i].get<float>();
  }
  tod.fog = fog;
  return tod;
}

/*!
 * Pack weights the way update-mood-itimes does: vftoi12 then >> 6 (1.0 -> 64), one halfword
 * per channel, two palettes per quadword.
 */
void pack_itimes(const TimeOfDay& tod, math::Vector<s32, 4> itimes[4]) {
  for (int p = 0; p < 8; p++) {
    u16 ch[4];
    for (int c = 0; c < 4; c++) {
      float w = tod.weights[p];
      if (tod.has_rgb && c < 3) {
        w = tod.rgb[p][c];
      }
      int v = (int)(w * 4096.f) >> 6;
      ch[c] = (u16)std::clamp(v, 0, 255);
    }
    int q = p / 2;
    int off = (p % 2) * 2;
    itimes[q][off] = (s32)(ch[0] | ((u32)ch[1] << 16));
    itimes[q][off + 1] = (s32)(ch[2] | ((u32)ch[3] << 16));
  }
}

struct CameraParams {
  float fov = 11650.845f;  // GOAL degrees, 65536 per turn
  float aspect = 16.f / 9.f;
  Fog fog;
};

/*!
 * Build the TfragPcPortData the GOAL side would send (add-pc-tfrag3-data), following
 * update-math-camera for the perspective, and update-camera for camera-rot and camera-temp.
 */
GoalBackgroundCameraData make_camera(const ContinuePoint& cp, const CameraParams& p) {
  GoalBackgroundCameraData cam;
  memset(&cam, 0, sizeof(cam));
  const float d = 1024.f, f = 40960000.f, x_pix = 256.f, y_pix = 112.f;
  const float fov_mult = 0.00008583069f * std::min(11650.845f, p.fov);
  float half = p.fov / 65536.f * 3.14159265f;
  // PC setting with use-vis? off: 4x3 vertical cropping, wider aspects see more on the sides
  float x_ratio = std::tan(half);
  float y_ratio = 0.75f * x_ratio;
  x_ratio *= p.aspect / (4.f / 3.f);

  const float fog_start = p.fog.dists[0] * fov_mult, fog_end = p.fog.dists[1] * fov_mult;
  const float fog_max = p.fog.dists[2], fog_min = p.fog.dists[3];
  const float min_depth = 100.f, max_depth = 16760631.f;
  float fog_slope = d * (fog_min - fog_max) / (fog_end - fog_start);
  float half_range = -0.5f * (max_depth - min_depth);
  float half_slope = half_range / (d * (f - d));
  auto& P = cam.perspective;
  P[0][0] = fov_mult * -(x_pix / (x_ratio * d));
  P[1][1] = fov_mult * -(y_pix / (y_ratio * d));
  P[2][2] = fov_mult * (f + d) * half_slope;
  P[2][3] = fov_mult / d * fog_slope;
  P[3][2] = -2.f * half_slope * f * d * fov_mult;

  cam.hvdf_off = Vector4f(2048.f, 2048.f, 0.5f * (max_depth + min_depth),
                          (fog_end * fog_max - fog_start * fog_min) / (fog_end - fog_start));
  cam.fog = Vector4f(fog_slope, fog_min, fog_max, 0.f);
  cam.trans = Vector4f(cp.trans[0], cp.trans[1], cp.trans[2], 1.f);

  // camera-rot = transpose(inv-camera-rot), then row 3 = -trans * camera-rot
  auto& R = cam.rot;
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) {
      R[i][j] = cp.rot[j * 3 + i];
    }
  }
  for (int j = 0; j < 3; j++) {
    R[3][j] = -(cp.trans[0] * R[0][j] + cp.trans[1] * R[1][j] + cp.trans[2] * R[2][j]);
  }
  R[3][3] = 1.f;

  for (int i = 0; i < 4; i++) {
    for (int j = 0; j < 4; j++) {
      float acc = 0;
      for (int k = 0; k < 4; k++) {
        acc += R[i][k] * P[k][j];
      }
      cam.camera[i][j] = acc;
    }
  }

  // planes that accept every sphere: culling is left to the depth test
  for (int i = 0; i < 4; i++) {
    cam.planes[i] = Vector4f(0, 0, 0, 0);
  }
  cam.planes[3] = Vector4f(-1e30f, -1e30f, -1e30f, -1e30f);
  return cam;
}

/*!
 * Minimal DMA chain in the layout Tie3/Shrub::render expect: CNT transfers, then an END tag
 * whose offset is next_bucket.
 */
class FakeDma {
 public:
  void add(const void* data, u32 bytes, u32 vif1 = 0) {
    ASSERT(bytes % 16 == 0);
    u64 tag = (u64)(bytes / 16) | ((u64)DmaTag::Kind::CNT << 28);
    push(&tag, 8);
    u32 vif0 = 0;
    push(&vif0, 4);
    push(&vif1, 4);
    if (bytes) {
      push(data, bytes);
    }
  }
  void add_empty() { add(nullptr, 0); }
  u32 finish() {
    u32 end = m_buf.size();
    u64 tag = ((u64)DmaTag::Kind::END << 28);
    push(&tag, 8);
    u64 zero = 0;
    push(&zero, 8);
    return end;
  }
  const u8* data() const { return m_buf.data(); }

 private:
  void push(const void* p, size_t n) {
    auto* b = (const u8*)p;
    m_buf.insert(m_buf.end(), b, b + n);
  }
  std::vector<u8> m_buf;
};

struct Target {
  GLuint fbo = 0, color = 0, depth = 0;
  int samples = 1;
};

Target make_target(int w, int h, int samples, GLenum color_fmt) {
  Target t;
  t.samples = samples;
  glGenFramebuffers(1, &t.fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
  glGenRenderbuffers(1, &t.color);
  glBindRenderbuffer(GL_RENDERBUFFER, t.color);
  if (samples > 1) {
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, color_fmt, w, h);
  } else {
    glRenderbufferStorage(GL_RENDERBUFFER, color_fmt, w, h);
  }
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, t.color);
  glGenRenderbuffers(1, &t.depth);
  glBindRenderbuffer(GL_RENDERBUFFER, t.depth);
  if (samples > 1) {
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH24_STENCIL8, w, h);
  } else {
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
  }
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, t.depth);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    fail("framebuffer incomplete");
  }
  return t;
}

void write_npy(const fs::path& path,
               const char* descr,
               int h,
               int w,
               const void* data,
               size_t bytes) {
  std::string header =
      fmt::format("{{'descr': '{}', 'fortran_order': False, 'shape': ({}, {}), }}", descr, h, w);
  size_t total = 10 + header.size() + 1;
  header.append((64 - total % 64) % 64, ' ');
  header.push_back('\n');
  std::ofstream f(path, std::ios::binary);
  const char magic[] = "\x93NUMPY\x01\x00";
  f.write(magic, 8);
  u16 len = (u16)header.size();
  f.write((const char*)&len, 2);
  f.write(header.data(), header.size());
  f.write((const char*)data, bytes);
}

template <typename T>
void flip_rows(std::vector<T>& img, int w, int h, int channels) {
  for (int y = 0; y < h / 2; y++) {
    std::swap_ranges(img.begin() + (size_t)y * w * channels,
                     img.begin() + (size_t)(y + 1) * w * channels,
                     img.begin() + (size_t)(h - 1 - y) * w * channels);
  }
}

std::vector<float> parse_floats(const std::string& s) {
  std::vector<float> out;
  std::stringstream ss(s);
  std::string tok;
  while (std::getline(ss, tok, ',')) {
    if (!tok.empty()) {
      out.push_back(std::stof(tok));
    }
  }
  return out;
}

}  // namespace

namespace {
struct LevelSet {
  std::string name;
  int index = 0;
  LevelData* data = nullptr;
  std::unique_ptr<TFragment> tfrag;
  std::unique_ptr<Tie3WithEnvmapJak1> tie;
  std::unique_ptr<Shrub> shrub;
  TimeOfDay tod;
  std::string tod_desc;
  TfragPcPortData port;
  TfragRenderSettings settings;
};
}  // namespace

int main(int argc, char** argv) {
  std::string levels_arg, cp_name = "village1-hut", out_dir, prefix, itimes_arg, cam_trans_arg,
                          cam_rot_arg, fog_color_arg, fog_dists_arg;
  std::vector<std::string> slot_weights;
  int width = 1920, height = 1080, msaa = 1, solo = -1, repeat = 3;
  float hour = -1.f, fov_deg = -1.f;
  bool no_fog = false, no_tfrag = false, no_tie = false, no_shrub = false, no_ids = false;

  CLI::App app{"lightbench: offline background renderer (Jak 1)"};
  app.add_option("--continue", cp_name, "Continue point name from level-info.gc");
  app.add_option("--levels", levels_arg,
                 "Comma-separated FR3 levels (default: the continue point's lev0,lev1)");
  app.add_option("--cam-trans", cam_trans_arg, "Override camera position x,y,z (GOAL units)");
  app.add_option("--cam-rot", cam_rot_arg, "Override inv-camera-rot, 9 floats row-major");
  app.add_option("--width", width);
  app.add_option("--height", height);
  app.add_option("--msaa", msaa, "Samples for the color pass");
  app.add_option("--fov", fov_deg, "Horizontal FOV at 4:3 in degrees (default 64)");
  app.add_option("--hour", hour, "Evaluate each level's palette-interp table at this hour");
  app.add_option("--solo", solo, "Weight 1.0 on palette slot N, 0 elsewhere");
  app.add_option("--itimes", itimes_arg,
                 "8 comma-separated weights, or 24 (r,g,b per slot); 1.0 = full");
  app.add_option("--slot-weight", slot_weights, "N=W, set slot N after --hour/--solo/--itimes");
  app.add_option("--fog-color", fog_color_arg, "r,g,b in 0-255");
  app.add_option("--fog-dists", fog_dists_arg, "start,end,max,min (mood-fog fog-dists)");
  app.add_flag("--no-fog", no_fog);
  app.add_flag("--no-tfrag", no_tfrag);
  app.add_flag("--no-tie", no_tie);
  app.add_flag("--no-shrub", no_shrub);
  app.add_flag("--no-ids", no_ids, "Skip the texture-id pass");
  app.add_option("--repeat", repeat, "Timed render repetitions");
  app.add_option("--out", out_dir, "Output directory");
  app.add_option("--prefix", prefix, "Output file prefix");
  CLI11_PARSE(app, argc, argv);

  if (!file_util::setup_project_path(std::nullopt, true)) {
    fail("could not find the project path (set OPENGOAL_PROJ_PATH)");
  }
  const auto proj = file_util::get_jak_project_dir();

  // ---- camera
  auto cp_opt = find_continue_point(cp_name);
  if (!cp_opt) {
    fail(fmt::format("continue point {} not found in level-info.gc", cp_name));
  }
  auto cp = *cp_opt;
  if (!cam_trans_arg.empty()) {
    auto v = parse_floats(cam_trans_arg);
    if (v.size() != 3) {
      fail("--cam-trans needs 3 values");
    }
    std::copy(v.begin(), v.end(), cp.trans);
  }
  if (!cam_rot_arg.empty()) {
    auto v = parse_floats(cam_rot_arg);
    if (v.size() != 9) {
      fail("--cam-rot needs 9 values");
    }
    std::copy(v.begin(), v.end(), cp.rot);
  }
  std::vector<std::string> level_names;
  if (!levels_arg.empty()) {
    std::stringstream ss(levels_arg);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
      level_names.push_back(tok);
    }
  } else {
    level_names = cp.levels.empty() ? std::vector<std::string>{cp.level} : cp.levels;
  }
  if (level_names.empty() || level_names.size() > 2) {
    fail("need one or two levels");
  }

  // ---- time of day, per level so each uses its own mood tables with --hour
  std::vector<LevelSet> sets(level_names.size());
  for (size_t i = 0; i < sets.size(); i++) {
    auto& L = sets[i];
    L.name = level_names[i];
    L.index = (int)i;
    if (hour >= 0.f) {
      auto path = proj / "sandover" / "tools" / "data" / "mood" / (L.name + ".json");
      if (!fs::exists(path)) {
        path = proj / "sandover" / "tools" / "data" / "mood" / (level_names[0] + ".json");
      }
      if (!fs::exists(path)) {
        fail(fmt::format("no mood mirror for {}; run sandover.mood_tables {}", L.name, L.name));
      }
      L.tod = eval_hour(nlohmann::json::parse(file_util::read_text_file(path)), hour);
      L.tod_desc = fmt::format("hour {} ({})", hour, path.stem().string());
    } else if (solo >= 0) {
      if (solo > 7) {
        fail("--solo takes 0-7");
      }
      L.tod.weights[solo] = 1.f;
      L.tod_desc = fmt::format("solo {}", solo);
    } else if (!itimes_arg.empty()) {
      auto v = parse_floats(itimes_arg);
      if (v.size() == 8) {
        std::copy(v.begin(), v.end(), L.tod.weights);
      } else if (v.size() == 24) {
        L.tod.has_rgb = true;
        for (int p = 0; p < 8; p++) {
          for (int c = 0; c < 3; c++) {
            L.tod.rgb[p][c] = v[p * 3 + c];
          }
          L.tod.weights[p] = std::max({L.tod.rgb[p][0], L.tod.rgb[p][1], L.tod.rgb[p][2]});
        }
      } else {
        fail("--itimes needs 8 or 24 values");
      }
      L.tod_desc = "explicit itimes";
    } else {
      fail("one of --hour, --solo, --itimes is required");
    }
    for (auto& sw : slot_weights) {
      auto eq = sw.find('=');
      if (eq == std::string::npos) {
        fail("--slot-weight takes N=W");
      }
      int slot = std::stoi(sw.substr(0, eq));
      if (slot < 0 || slot > 7) {
        fail("--slot-weight slot must be 0-7");
      }
      L.tod.weights[slot] = std::stof(sw.substr(eq + 1));
      L.tod.rgb[slot][0] = L.tod.rgb[slot][1] = L.tod.rgb[slot][2] = L.tod.weights[slot];
    }
  }

  // fog and projection come from the first level, as with level0 fully blended in
  CameraParams cam_params;
  cam_params.aspect = (float)width / (float)height;
  if (fov_deg > 0) {
    cam_params.fov = fov_deg / 360.f * 65536.f;
  }
  if (sets[0].tod.fog) {
    cam_params.fog = *sets[0].tod.fog;
  }
  if (!fog_color_arg.empty()) {
    auto v = parse_floats(fog_color_arg);
    if (v.size() != 3) {
      fail("--fog-color needs 3 values");
    }
    std::copy(v.begin(), v.end(), cam_params.fog.color);
  }
  if (!fog_dists_arg.empty()) {
    auto v = parse_floats(fog_dists_arg);
    if (v.size() != 4) {
      fail("--fog-dists needs 4 values");
    }
    std::copy(v.begin(), v.end(), cam_params.fog.dists);
  }
  bool fog_on = !no_fog && (sets[0].tod.fog.has_value() || !fog_color_arg.empty());
  auto camera = make_camera(cp, cam_params);
  for (auto& L : sets) {
    memset(&L.port, 0, sizeof(L.port));
    L.port.camera = camera;
    pack_itimes(L.tod, L.port.camera.itimes);
    strncpy(L.port.level_name, L.name.c_str(), sizeof(L.port.level_name) - 1);
    L.settings.camera = L.port.camera;
    L.settings.tree_idx = 0;
    L.settings.occlusion_culling = nullptr;
  }

  // ---- GL context, hidden window as in gfx_test.cpp
  Timer total_timer;
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    fail(fmt::format("SDL_Init failed: {}", SDL_GetError()));
  }
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
  SDL_Window* window =
      SDL_CreateWindow("lightbench", 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
  if (!window) {
    fail(fmt::format("SDL_CreateWindow failed: {}", SDL_GetError()));
  }
  SDL_GLContext ctx = SDL_GL_CreateContext(window);
  if (!ctx || !gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)) {
    fail(fmt::format("GL 4.3 context failed: {}", SDL_GetError()));
  }
  fmt::print("[lightbench] GL {} (SDL video driver {})\n", (const char*)glGetString(GL_VERSION),
             SDL_GetCurrentVideoDriver());
  double t_context = total_timer.getMs();

  // ---- load
  Timer load_timer;
  auto tex_pool = std::make_shared<TexturePool>(GameVersion::Jak1);
  auto loader = std::make_shared<Loader>(proj / "out" / "jak1" / "fr3", jak1::LEVEL_TOTAL);
  SharedRenderState rs(tex_pool, loader, GameVersion::Jak1);
  double t_shaders = load_timer.getMs();
  loader->load_common(*tex_pool, "GAME");
  double t_common = load_timer.getMs();
  loader->set_want_levels(level_names);
  loader->update_blocking(*tex_pool);
  for (auto& L : sets) {
    L.data = const_cast<LevelData*>(loader->get_tfrag3_level(L.name));
    if (!L.data) {
      fail(fmt::format("level {} did not load from out/jak1/fr3", L.name));
    }
  }
  double t_load = load_timer.getMs();

  // ---- renderers, constructed as OpenGLRenderer::init_bucket_renderers_jak1 does
  using K = tfrag3::TFragmentTreeKind;
  const std::vector<K> all_kinds = {K::NORMAL, K::LOWRES, K::TRANS, K::LOWRES_TRANS,
                                    K::DIRT,   K::ICE,    K::WATER};
  for (auto& L : sets) {
    L.tfrag = std::make_unique<TFragment>("lb-tfrag", 0, all_kinds, false, L.index, nullptr);
    L.tie = std::make_unique<Tie3WithEnvmapJak1>("lb-tie", 1, L.index);
    L.shrub = std::make_unique<Shrub>("lb-shrub", 2);
  }
  auto init_all_shaders = [&]() {
    for (auto& L : sets) {
      L.tfrag->init_shaders(rs.shaders);
      L.tie->init_shaders(rs.shaders);
      L.shrub->init_shaders(rs.shaders);
    }
  };
  init_all_shaders();
  rs.bucket_for_vis_copy = -1;
  rs.use_occlusion_culling = false;
  if (fog_on) {
    for (int i = 0; i < 3; i++) {
      rs.fog_color[i] = (u8)std::clamp((int)cam_params.fog.color[i], 0, 255);
    }
    rs.fog_intensity = 1.f;
  } else {
    rs.fog_intensity = 0.f;
  }

  Tie3::WindWork wind;
  memset(&wind, 0, sizeof(wind));
  u8 zeros[160] = {};
  u8 envmap[16] = {128, 128, 128, 128};  // 2.0 after Jak 1 scaling, the renderer default

  Profiler profiler;
  const int tfrag_lod = sets[0].tfrag->lod();
  bool id_pass = false;
  auto set_source = [&](int renderer, const LevelSet& L) {
    if (!id_pass) {
      return;
    }
    for (auto& s : kIdShaders) {
      auto id = rs.shaders[s.id].id();
      glUseProgram(id);
      glUniform1ui(glGetUniformLocation(id, "lb_source"), (u32)(renderer | (L.index << 4)));
    }
  };
  auto draw_scene = [&]() {
    auto node = profiler.root()->make_scoped_child("frame");
    rs.frame_idx++;
    rs.has_pc_data = false;
    for (auto& L : sets) {
      if (!no_tfrag) {
        set_source(1, L);
        L.tfrag->setup_for_level(all_kinds, L.name, &rs);
        L.tfrag->render_matching_trees(tfrag_lod, {K::NORMAL, K::LOWRES}, L.settings, &rs, node);
      }
      if (!no_tie) {
        set_source(2, L);
        FakeDma dma;
        dma.add_empty();
        dma.add(zeros, 32);      // gs test
        dma.add(zeros, 9 * 16);  // tie consts
        dma.add_empty();         // mscal
        dma.add(zeros, 32);      // row
        dma.add_empty();
        dma.add(&L.port, sizeof(L.port));
        dma.add(&wind, sizeof(wind));
        dma.add(envmap, 16);
        rs.next_bucket = dma.finish();
        DmaFollower follow(dma.data(), 0);
        L.tie->render(follow, &rs, node);
      }
    }
    if (!no_shrub) {
      for (auto& L : sets) {
        set_source(3, L);
        FakeDma dma;
        dma.add_empty();
        dma.add(&L.port, sizeof(L.port));
        rs.next_bucket = dma.finish();
        DmaFollower follow(dma.data(), 0);
        L.shrub->render(follow, &rs, node);
      }
    }
    if (!no_tfrag) {
      for (auto& L : sets) {
        set_source(1, L);
        for (auto kinds : {std::vector<K>{K::TRANS, K::LOWRES_TRANS}, std::vector<K>{K::DIRT},
                           std::vector<K>{K::ICE}, std::vector<K>{K::WATER}}) {
          L.tfrag->render_matching_trees(tfrag_lod, kinds, L.settings, &rs, node);
        }
      }
    }
  };

  auto begin_pass = [&](const Target& t) {
    glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
    glViewport(0, 0, width, height);
    glDepthMask(GL_TRUE);
    glClearDepth(0.0);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GEQUAL);
    glDisable(GL_BLEND);
    rs.render_fb = t.fbo;
    rs.render_fb_w = rs.draw_region_w = width;
    rs.render_fb_h = rs.draw_region_h = height;
  };

  // ---- color pass
  Target color_ms = make_target(width, height, msaa, GL_RGBA8);
  Target color_ss = msaa > 1 ? make_target(width, height, 1, GL_RGBA8) : color_ms;
  std::vector<double> frame_ms;
  for (int i = 0; i < std::max(1, repeat); i++) {
    Timer ft;
    begin_pass(color_ms);
    draw_scene();
    glFinish();
    frame_ms.push_back(ft.getMs());
  }
  if (msaa > 1) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, color_ms.fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, color_ss.fbo);
    glBlitFramebuffer(0, 0, width, height, 0, 0, width, height,
                      GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, GL_NEAREST);
  }

  Timer readback_timer;
  glBindFramebuffer(GL_READ_FRAMEBUFFER, color_ss.fbo);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  std::vector<u8> rgba((size_t)width * height * 4);
  glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
  std::vector<float> depth((size_t)width * height);
  glReadPixels(0, 0, width, height, GL_DEPTH_COMPONENT, GL_FLOAT, depth.data());
  flip_rows(rgba, width, height, 4);
  flip_rows(depth, width, height, 1);
  for (size_t i = 0; i < rgba.size(); i += 4) {
    rgba[i + 3] = 255;
  }

  // window depth -> view-axis depth, inverting the pc_camera built by make_new_cam_mat
  auto pc = make_new_cam_mat(camera.rot, camera.perspective, camera.fog.x(), camera.hvdf_off.z());
  const float pzw = camera.perspective[2][3];
  const float sz = camera.fog.x() * camera.perspective[2][2] / 8388608.f +
                   (camera.hvdf_off.z() / 8388608.f - 1.f) * pzw;
  const float r32 = pc[3][2];
  std::vector<float> linear(depth.size());
  float dmin = 1e30f, dmax = 0.f;
  for (size_t i = 0; i < depth.size(); i++) {
    if (depth[i] <= 0.f) {
      linear[i] = INFINITY;
      continue;
    }
    float zndc = 2.f * depth[i] - 1.f;
    linear[i] = r32 / (pzw * zndc - sz) / kMeters;
    dmin = std::min(dmin, linear[i]);
    dmax = std::max(dmax, linear[i]);
  }
  double t_readback = readback_timer.getMs();

  // ---- texture-id pass
  std::vector<u32> ids;
  double t_ids = 0;
  if (!no_ids) {
    Timer id_timer;
    id_pass = true;
    auto gen_dir = proj / "out" / "lightbench" / "shaders";
    fs::create_directories(gen_dir);
    std::vector<std::pair<ShaderId, Shader>> saved;
    for (auto& s : kIdShaders) {
      auto vert = file_util::read_text_file(
          file_util::get_file_path({Shader::shader_folder, std::string(s.upstream) + ".vert"}));
      file_util::write_text_file(gen_dir / (std::string(s.generated) + ".vert"), vert);
      file_util::write_text_file(gen_dir / (std::string(s.generated) + ".frag"),
                                 s.discard_all ? kDiscardFrag : kIdFrag);
      // Shader only loads from the shader folder, so reach the generated copy relative to it
      Shader sh("../../../../out/lightbench/shaders/" + std::string(s.generated),
                GameVersion::Jak1);
      if (!sh.okay()) {
        fail(fmt::format("id shader {} failed", s.generated));
      }
      saved.emplace_back(s.id, rs.shaders[s.id]);
      rs.shaders[s.id] = sh;
    }
    init_all_shaders();

    // id textures: rgb = texture index + 1, alpha kept for the alpha test
    std::vector<std::vector<GLuint>> id_tex(sets.size());
    for (auto& L : sets) {
      auto& tex = id_tex[L.index];
      tex.resize(L.data->textures.size());
      glGenTextures(tex.size(), tex.data());
      for (size_t i = 0; i < tex.size(); i++) {
        const auto& src = L.data->level->textures.at(i);
        u32 code = (u32)(i + 1) & 0xffffff;
        std::vector<u32> px(src.data.size());
        for (size_t j = 0; j < px.size(); j++) {
          px[j] = code | (src.data[j] & 0xff000000);
        }
        glBindTexture(GL_TEXTURE_2D, tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, src.w, src.h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     px.data());
        glGenerateMipmap(GL_TEXTURE_2D);
      }
      std::swap(L.data->textures, tex);
    }

    Target id_target = make_target(width, height, 1, GL_R32UI);
    begin_pass(id_target);
    GLuint zero[4] = {0, 0, 0, 0};
    glClearBufferuiv(GL_COLOR, 0, zero);
    draw_scene();
    glFinish();
    ids.resize((size_t)width * height);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, id_target.fbo);
    glReadPixels(0, 0, width, height, GL_RED_INTEGER, GL_UNSIGNED_INT, ids.data());
    flip_rows(ids, width, height, 1);

    for (auto& L : sets) {
      std::swap(L.data->textures, id_tex[L.index]);
      glDeleteTextures(id_tex[L.index].size(), id_tex[L.index].data());
    }
    for (auto& [id, sh] : saved) {
      rs.shaders[id] = sh;
    }
    init_all_shaders();
    id_pass = false;
    t_ids = id_timer.getMs();
  }

  // ---- write
  fs::path out = out_dir.empty() ? proj / "sandover" / "data" / "lightbench" : fs::path(out_dir);
  fs::create_directories(out);
  std::string stem = prefix.empty() ? cp.name : prefix;
  file_util::write_rgba_png(out / (stem + "-color.png"), rgba.data(), width, height);
  write_npy(out / (stem + "-depth.npy"), "<f4", height, width, linear.data(),
            linear.size() * sizeof(float));
  {
    std::vector<u8> preview((size_t)width * height * 4, 0);
    float lmin = std::log(std::max(dmin, 0.01f)), lmax = std::log(std::max(dmax, 0.02f));
    for (size_t i = 0; i < linear.size(); i++) {
      u8 v = 0;
      if (std::isfinite(linear[i])) {
        float t = (std::log(std::max(linear[i], 0.01f)) - lmin) / std::max(lmax - lmin, 1e-6f);
        v = (u8)std::clamp(255.f - 235.f * t, 0.f, 255.f);
      }
      preview[i * 4] = preview[i * 4 + 1] = preview[i * 4 + 2] = v;
      preview[i * 4 + 3] = 255;
    }
    file_util::write_rgba_png(out / (stem + "-depth.png"), preview.data(), width, height);
  }
  nlohmann::json meta;
  if (!ids.empty()) {
    write_npy(out / (stem + "-texid.npy"), "<u4", height, width, ids.data(),
              ids.size() * sizeof(u32));
    std::vector<u8> preview((size_t)width * height * 4, 0);
    for (size_t i = 0; i < ids.size(); i++) {
      u32 h = (ids[i] ^ (ids[i] >> 7)) * 2654435761u;
      if (ids[i]) {
        preview[i * 4] = h >> 24;
        preview[i * 4 + 1] = h >> 16;
        preview[i * 4 + 2] = h >> 8;
      }
      preview[i * 4 + 3] = 255;
    }
    file_util::write_rgba_png(out / (stem + "-texid.png"), preview.data(), width, height);
  }
  for (auto& L : sets) {
    nlohmann::json lj;
    lj["name"] = L.name;
    lj["index"] = L.index;
    lj["time_of_day"] = L.tod_desc;
    lj["weights"] = std::vector<float>(L.tod.weights, L.tod.weights + 8);
    auto& texs = lj["textures"];
    for (size_t i = 0; i < L.data->level->textures.size(); i++) {
      const auto& t = L.data->level->textures[i];
      texs.push_back({{"id", i + 1},
                      {"name", t.debug_name},
                      {"tpage", t.debug_tpage_name},
                      {"combo_id", t.combo_id},
                      {"w", t.w},
                      {"h", t.h}});
    }
    meta["levels"].push_back(lj);
  }

  double steady = frame_ms.size() > 1 ? std::accumulate(frame_ms.begin() + 1, frame_ms.end(), 0.0) /
                                            (frame_ms.size() - 1)
                                      : frame_ms[0];
  meta["continue_point"] = cp.name;
  meta["camera_trans"] = {cp.trans[0], cp.trans[1], cp.trans[2]};
  meta["camera_rot"] = std::vector<float>(cp.rot, cp.rot + 9);
  meta["fog"] =
      fog_on ? nlohmann::json{{"color",
                               std::vector<float>(cam_params.fog.color, cam_params.fog.color + 3)},
                              {"dists",
                               std::vector<float>(cam_params.fog.dists, cam_params.fog.dists + 4)}}
             : nlohmann::json(nullptr);
  meta["size"] = {width, height};
  meta["msaa"] = msaa;
  meta["fov_goal_units"] = cam_params.fov;
  meta["lod_tfrag"] = tfrag_lod;
  meta["depth"] = "float32 meters along the view axis; inf where nothing was drawn";
  meta["texid"] =
      "u32: bits 0-23 texture id (levels[L].textures[id-1]), bits 24-27 renderer "
      "(1 tfrag, 2 tie, 3 shrub), bits 28-31 level index L; 0 = nothing drawn; last draw wins";
  meta["timings_ms"] = {{"context", t_context},
                        {"shaders", t_shaders},
                        {"common_fr3", t_common - t_shaders},
                        {"level_fr3", t_load - t_common},
                        {"first_frame", frame_ms[0]},
                        {"steady_frame", steady},
                        {"readback", t_readback},
                        {"texid_pass", t_ids},
                        {"total", total_timer.getMs()}};
  file_util::write_text_file(out / (stem + ".json"), meta.dump(1));

  std::string lev_list;
  for (auto& n : level_names) {
    lev_list += (lev_list.empty() ? "" : ",") + n;
  }
  fmt::print("[lightbench] {} at {} ({}), {}x{} msaa {}\n", lev_list, cp.name, sets[0].tod_desc,
             width, height, msaa);
  fmt::print(
      "[lightbench] context {:.0f} ms, shaders {:.0f} ms, common fr3 {:.0f} ms, level fr3 {:.0f} "
      "ms\n",
      t_context, t_shaders, t_common - t_shaders, t_load - t_common);
  fmt::print(
      "[lightbench] frame first {:.1f} ms, steady {:.1f} ms, readback {:.1f} ms, id pass {:.1f} "
      "ms, total {:.0f} ms\n",
      frame_ms[0], steady, t_readback, t_ids, total_timer.getMs());
  fmt::print("[lightbench] depth {:.2f}-{:.2f} m, wrote {}-*\n", dmin, dmax, (out / stem).string());

  sets.clear();
  SDL_GL_DestroyContext(ctx);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
