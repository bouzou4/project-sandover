#include "Materials.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "common/util/Timer.h"
#include "common/util/json_util.h"

#include "game/graphics/gfx.h"
#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/opengl_renderer/GlQueries.h"
#include "game/graphics/opengl_renderer/WorldShadows.h"
#include "game/graphics/opengl_renderer/PerfTimers.h"
#include "game/graphics/opengl_renderer/SsaoRenderer.h"
#include "game/graphics/opengl_renderer/background/CanopyVol.h"
#include "game/graphics/opengl_renderer/background/Unbake.h"
#include "game/graphics/opengl_renderer/background/background_common.h"

#include "fmt/format.h"
#include "third-party/imgui/imgui.h"
#include "third-party/stb_image/stb_image.h"

PbrClassParams g_pbr_params[PBR_CLASS_COUNT] = {
    // normal, detail, scale_m, uv_rep, spec, gloss, tint, albedo
    {0.f, 0.f, 1.f, 1.f, 0.f, 1.f, {1.f, 1.f, 1.f}, 0.f},              // none
    {0.6f, 0.30f, 2.0f, 4.f, 0.07f, 6.f, {1.f, 0.97f, 0.9f}, 0.3f},    // stone
    {0.6f, 0.25f, 1.2f, 4.f, 0.08f, 8.f, {1.f, 0.93f, 0.8f}, 0.5f},    // wood
    {0.5f, 0.12f, 1.5f, 3.f, 0.35f, 16.f, {1.f, 1.f, 1.f}, 0.85f},     // metal
    {0.5f, 0.20f, 0.6f, 3.f, 0.08f, 8.f, {0.95f, 1.f, 0.85f}, 0.5f},   // foliage
    {0.4f, 0.10f, 0.5f, 6.f, 0.10f, 10.f, {1.f, 0.95f, 0.92f}, 0.3f},  // organic
    {0.f, 0.f, 1.f, 1.f, 0.f, 1.f, {1.f, 1.f, 1.f}, 0.f},              // special
    {0.f, 0.f, 1.f, 1.f, 0.30f, 40.f, {1.f, 1.f, 1.f}, 0.f},           // glass
    {0.5f, 0.20f, 0.35f, 8.f, 0.06f, 5.f, {1.f, 1.f, 1.f}, 0.6f},      // cloth
    {0.5f, 0.25f, 0.6f, 4.f, 0.20f, 30.f, {0.92f, 0.97f, 1.f}, 0.f},   // snow
    {0.f, 0.f, 1.f, 1.f, 0.f, 1.f, {1.f, 1.f, 1.f}, 0.f},              // lava
    {0.6f, 0.10f, 2.0f, 3.f, 0.40f, 14.f, {1.f, 0.72f, 0.42f}, 0.5f},  // precursor (bronze)
    {1.0f, 0.10f, 1.0f, 3.f, 0.30f, 60.f, {0.92f, 0.97f, 1.f}, 0.f, 1.f, -0.4f, 1.5f},  // ice
};

namespace {

const char* kClassNames[PBR_CLASS_COUNT] = {"none",    "stone",   "wood",    "metal",
                                            "foliage", "organic", "special", "glass",
                                            "cloth",   "snow",    "lava",    "precursor",
                                            "ice"};

const float kClassColors[PBR_CLASS_COUNT][3] = {
    {0.f, 0.f, 0.f},    {0.6f, 0.6f, 0.6f}, {0.6f, 0.35f, 0.1f}, {0.3f, 0.6f, 1.f},
    {0.1f, 0.8f, 0.1f}, {1.f, 0.55f, 0.6f}, {1.f, 0.f, 1.f},     {0.5f, 1.f, 1.f},
    {0.9f, 0.9f, 0.2f}, {1.f, 1.f, 1.f},    {1.f, 0.2f, 0.f},    {0.9f, 0.6f, 0.2f},
    {0.2f, 0.5f, 1.f}};

struct Table {
  bool loaded = false;
  std::unordered_map<u32, u8> by_combo;
};

std::mutex g_table_mutex;
Table g_table;

std::vector<std::string> split_csv_line(const std::string& line) {
  std::vector<std::string> out;
  std::string cur;
  bool quoted = false;
  for (size_t i = 0; i < line.size(); i++) {
    char c = line[i];
    if (quoted) {
      if (c == '"' && i + 1 < line.size() && line[i + 1] == '"') {
        cur += '"';
        i++;
      } else if (c == '"') {
        quoted = false;
      } else {
        cur += c;
      }
    } else if (c == '"') {
      quoted = true;
    } else if (c == ',') {
      out.push_back(cur);
      cur.clear();
    } else if (c != '\r') {
      cur += c;
    }
  }
  out.push_back(cur);
  return out;
}

fs::path csv_path() {
  if (const char* p = std::getenv("SANDOVER_MATERIALS")) {
    return fs::path(p);
  }
  if (const char* d = std::getenv("SANDOVER_DATA")) {
    return fs::path(d) / "materials" / "materials.csv";
  }
  return file_util::get_jak_project_dir() / "sandover" / "data" / "materials" / "materials.csv";
}

u8 class_from_name(const std::string& name) {
  for (int i = 1; i < PBR_CLASS_COUNT; i++) {
    if (name == kClassNames[i]) {
      return (u8)i;
    }
  }
  return PBR_SPECIAL;
}

void load_table_locked() {
  if (g_table.loaded) {
    return;
  }
  g_table.loaded = true;
  const auto path = csv_path();
  if (!fs::exists(path)) {
    lg::warn("materials: {} not found, every texture is `special`", path.string());
    return;
  }
  std::string text;
  try {
    text = file_util::read_text_file(path);
  } catch (const std::exception& e) {
    lg::warn("materials: {} unreadable, {}", path.string(), e.what());
    return;
  }
  size_t pos = 0;
  int col_combo = -1, col_class = -1, col_tags = -1;
  int rows = 0, bad = 0;
  int counts[PBR_CLASS_COUNT] = {};
  bool header = true;
  while (pos < text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string::npos) {
      end = text.size();
    }
    auto cols = split_csv_line(text.substr(pos, end - pos));
    pos = end + 1;
    if (header) {
      for (int i = 0; i < (int)cols.size(); i++) {
        if (cols[i] == "combo_id") {
          col_combo = i;
        } else if (cols[i] == "class") {
          col_class = i;
        } else if (cols[i] == "tags") {
          col_tags = i;
        }
      }
      header = false;
      if (col_combo < 0 || col_class < 0) {
        lg::warn("materials: {} has no combo_id/class columns", path.string());
        return;
      }
      continue;
    }
    if ((int)cols.size() <= std::max(col_combo, col_class)) {
      continue;
    }
    char* e = nullptr;
    const u32 combo = (u32)std::strtoul(cols[col_combo].c_str(), &e, 16);
    if (e == cols[col_combo].c_str()) {
      bad++;
      continue;
    }
    u8 cls = class_from_name(cols[col_class]);
    const std::string tags = col_tags >= 0 && col_tags < (int)cols.size() ? cols[col_tags] : "";
    if (cls == PBR_METAL && tags.find("precursor") != std::string::npos) {
      cls = PBR_PRECURSOR;
    }
    counts[cls]++;
    if (tags.find("fur") != std::string::npos) {
      cls |= kPbrFurBit;
    }
    if (tags.find("baked-tint") != std::string::npos) {
      cls |= kPbrBakedTintBit;
    }
    if (tags.find("shaft") != std::string::npos) {
      cls |= kPbrShaftBit;
    }
    g_table.by_combo[combo] = cls;
    rows++;
  }
  std::string summary;
  for (int i = 1; i < PBR_CLASS_COUNT; i++) {
    summary += fmt::format(" {} {}", kClassNames[i], counts[i]);
  }
  lg::info("materials: {} rows ({} bad) from {}:{}", rows, bad, path.string(), summary);
}

// Tileable value noise on a period-p lattice.
float hash2(int x, int y, int seed) {
  u32 h = (u32)x * 374761393u + (u32)y * 668265263u + (u32)seed * 2246822519u;
  h = (h ^ (h >> 13)) * 1274126177u;
  h ^= h >> 16;
  return (h & 0xffffff) / float(0xffffff);
}

float vnoise(float u, float v, int p, int seed) {
  float x = u * p, y = v * p;
  int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
  float fx = x - x0, fy = y - y0;
  fx = fx * fx * (3 - 2 * fx);
  fy = fy * fy * (3 - 2 * fy);
  auto at = [&](int i, int j) { return hash2(((i % p) + p) % p, ((j % p) + p) % p, seed); };
  float a = at(x0, y0), b = at(x0 + 1, y0), c = at(x0, y0 + 1), d = at(x0 + 1, y0 + 1);
  return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
}

float fbm(float u, float v, int p, int octaves, int seed) {
  float sum = 0, amp = 0.5f, norm = 0;
  for (int o = 0; o < octaves; o++) {
    sum += amp * vnoise(u, v, p << o, seed + o);
    norm += amp;
    amp *= 0.5f;
  }
  return sum / norm;
}

constexpr int kDetailSize = 256;
constexpr float kPi = 3.14159265f;

float fractf(float x) {
  return x - std::floor(x);
}

/// Height field for one class, u, v in [0, 1), tileable.
float detail_height(int cls, float u, float v, const std::vector<float>& scratch) {
  switch (cls) {
    case PBR_STONE:
    case PBR_ICE:  // stone's layer: with the rule off, `ice` rows take stone's response
      return fbm(u, v, 6, 4, 11) + 0.35f * fbm(u, v, 24, 2, 17);
    case PBR_WOOD: {
      float warp = 0.08f * fbm(u, v, 4, 2, 23);
      float grain = 0.5f + 0.5f * std::sin(2 * kPi * (v * 28 + warp * 28));
      return 0.6f * grain + 0.4f * vnoise(u, v, 64, 29);
    }
    case PBR_METAL:
    case PBR_PRECURSOR:
      return scratch[(int)(v * kDetailSize) * kDetailSize + (int)(u * kDetailSize)];
    case PBR_FOLIAGE: {
      float d = std::abs(fractf(u * 4 + 0.35f * std::sin(2 * kPi * v)) - 0.5f);
      float vein = 1 - std::min(1.f, d / 0.06f);
      return 0.5f * vein * vein + 0.5f * fbm(u, v, 8, 3, 31);
    }
    case PBR_ORGANIC:
      return fbm(u, v, 32, 2, 37);
    case PBR_CLOTH: {
      constexpr int n = 32;
      int i = (int)std::floor(u * n), j = (int)std::floor(v * n);
      float a = std::sin(kPi * fractf(u * n)), b = std::sin(kPi * fractf(v * n));
      return ((i + j) % 2 == 0) ? 0.3f + 0.7f * b : 0.3f + 0.7f * a;
    }
    case PBR_SNOW: {
      float bumps = fbm(u, v, 32, 2, 41);
      float spark = hash2((int)(u * kDetailSize), (int)(v * kDetailSize), 43);
      return bumps + (spark > 0.985f ? 0.8f : 0.f);
    }
    default:
      return 0.f;
  }
}

/// Scratches (metal) or engraved grid lines (precursor), as a height field.
std::vector<float> line_field(int cls) {
  std::vector<float> h(kDetailSize * kDetailSize, 0.f);
  auto dig = [&](float x0, float y0, float dx, float dy, int len, float depth) {
    for (int s = 0; s < len; s++) {
      int x = (((int)std::lround(x0 + dx * s)) % kDetailSize + kDetailSize) % kDetailSize;
      int y = (((int)std::lround(y0 + dy * s)) % kDetailSize + kDetailSize) % kDetailSize;
      h[y * kDetailSize + x] = std::min(h[y * kDetailSize + x], -depth);
    }
  };
  if (cls == PBR_METAL) {
    for (int k = 0; k < 70; k++) {
      float a = (hash2(k, 1, 5) < 0.7f ? 0.35f : 1.9f) + 0.25f * (hash2(k, 2, 5) - 0.5f);
      dig(hash2(k, 3, 5) * kDetailSize, hash2(k, 4, 5) * kDetailSize, std::cos(a), std::sin(a),
          20 + (int)(hash2(k, 5, 5) * 70), 0.3f + 0.4f * hash2(k, 6, 5));
    }
  } else {
    for (int i = 0; i < kDetailSize; i += kDetailSize / 4) {
      dig((float)i, 0, 0, 1, kDetailSize, 1.f);
      dig(0, (float)i + kDetailSize / 8, 1, 0, kDetailSize, 1.f);
    }
    for (int k = 0; k < 6; k++) {
      dig(hash2(k, 7, 9) * kDetailSize, hash2(k, 8, 9) * kDetailSize, 1, 1, 40, 0.7f);
    }
  }
  return h;
}

// lights of the most recent background setup, world space, for renderers without world positions
struct PbrFrameLights {
  int sun_ok = 0, key_ok = 0;
  math::Vector3f sun_dir, sun_col, key_dir, key_col;
  math::Vector3f env_sky{1, 1, 1}, env_hor{1, 1, 1}, env_gnd{1, 1, 1}, env_sun{0, 0, 0};
  float env_bake = 0;
  float day = 0;  // share of the palette weight on day slots
  math::Vector4f rot[3];
} g_frame_lights;

bool g_pbr_used = false;  // set once shading has run, so the off path can reset pbr_on
GLuint g_detail_array = 0;

GLuint detail_array() {
  if (g_detail_array) {
    return g_detail_array;
  }
  const int n = kDetailSize;
  std::vector<u8> pixels((size_t)n * n * 4 * PBR_CLASS_COUNT);
  std::vector<float> h((size_t)n * n);
  for (int cls = 0; cls < PBR_CLASS_COUNT; cls++) {
    std::vector<float> scratch;
    if (cls == PBR_METAL || cls == PBR_PRECURSOR) {
      scratch = line_field(cls);
    }
    for (int y = 0; y < n; y++) {
      for (int x = 0; x < n; x++) {
        h[y * n + x] = detail_height(cls, (x + 0.5f) / n, (y + 0.5f) / n, scratch);
      }
    }
    std::vector<float> gx((size_t)n * n), gy((size_t)n * n), mag;
    for (int y = 0; y < n; y++) {
      for (int x = 0; x < n; x++) {
        gx[y * n + x] = h[y * n + (x + 1) % n] - h[y * n + (x + n - 1) % n];
        gy[y * n + x] = h[((y + 1) % n) * n + x] - h[((y + n - 1) % n) * n + x];
        mag.push_back(std::sqrt(gx[y * n + x] * gx[y * n + x] + gy[y * n + x] * gy[y * n + x]));
      }
    }
    // normalize so the 95th-percentile slope is 45 degrees; detail strength scales from there
    const float peak = *std::max_element(mag.begin(), mag.end());
    std::nth_element(mag.begin(), mag.begin() + mag.size() * 95 / 100, mag.end());
    const float p95 = std::max(mag[mag.size() * 95 / 100], 0.25f * peak);
    const float k = p95 > 1e-6f ? 1.f / p95 : 0.f;
    for (int y = 0; y < n; y++) {
      for (int x = 0; x < n; x++) {
        float nx = -gx[y * n + x] * k, ny = -gy[y * n + x] * k, nz = 1.f;
        float l = std::sqrt(nx * nx + ny * ny + nz * nz);
        u8* p = &pixels[(((size_t)cls * n + y) * n + x) * 4];
        p[0] = (u8)std::clamp((int)std::lround((nx / l * 0.5f + 0.5f) * 255), 0, 255);
        p[1] = (u8)std::clamp((int)std::lround((ny / l * 0.5f + 0.5f) * 255), 0, 255);
        p[2] = (u8)std::clamp((int)std::lround((nz / l * 0.5f + 0.5f) * 255), 0, 255);
        p[3] = 255;
      }
    }
  }
  glGenTextures(1, &g_detail_array);
  glActiveTexture(GL_TEXTURE16);
  glBindTexture(GL_TEXTURE_2D_ARRAY, g_detail_array);
  glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, n, n, PBR_CLASS_COUNT, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, pixels.data());
  glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
  glActiveTexture(GL_TEXTURE0);
  return g_detail_array;
}


// ---- per-texture maps (sandover-materials derive-maps) ----

struct MapIndexEntry {
  float metallic = 0.f;
  bool flat = true;
  bool parallax = false;  // has <name>.parallax.png
};

struct MapIndex {
  bool loaded = false;
  fs::path root;
  std::unordered_map<std::string, MapIndexEntry> by_key;  // "<tpage>/<name>"
};

std::mutex g_map_index_mutex;
MapIndex g_map_index;

fs::path maps_root() {
  if (const char* p = std::getenv("SANDOVER_PBR_MAPS")) {
    return fs::path(p);
  }
  if (const char* d = std::getenv("SANDOVER_DATA")) {
    return fs::path(d) / "materials" / "pbr-maps";
  }
  return file_util::get_jak_project_dir() / "sandover" / "data" / "materials" / "pbr-maps";
}

const MapIndex& map_index() {
  std::lock_guard<std::mutex> lk(g_map_index_mutex);
  if (g_map_index.loaded) {
    return g_map_index;
  }
  g_map_index.loaded = true;
  g_map_index.root = maps_root();
  const auto path = g_map_index.root / "index.json";
  if (!fs::exists(path)) {
    lg::warn("materials: {} not found, every texture gets flat maps", path.string());
    return g_map_index;
  }
  try {
    auto j = nlohmann::json::parse(file_util::read_text_file(path));
    int flat = 0;
    for (auto& [key, e] : j.at("textures").items()) {
      MapIndexEntry m;
      m.metallic = e.value("metallic", 0.f);
      m.flat = e.value("flat", false);
      m.parallax = e.value("parallax", false);
      flat += m.flat;
      g_map_index.by_key[key] = m;
    }
    lg::info("materials: map index {} textures ({} flat) from {}", g_map_index.by_key.size(), flat,
             path.string());
  } catch (const std::exception& e) {
    lg::warn("materials: {} unreadable, {}", path.string(), e.what());
    g_map_index.by_key.clear();
  }
  return g_map_index;
}

struct MapImage {
  int w = 0, h = 0;
  bool packed = false;
  std::vector<u8> rgba;
};

/*!
 * Normal (rgb) and roughness (a) of one texture; empty if absent or unreadable. With `parallax`
 * and a matching `<name>.parallax.png`, packed instead: normal xy (rg), roughness (b), height (a).
 */
MapImage decode_map(const fs::path& root, const std::string& key, int max_size, bool parallax) {
  MapImage out;
  const auto base = (root / key).string();
  int w, h, n;
  u8* nrm = stbi_load((base + ".normal.png").c_str(), &w, &h, &n, 4);
  if (!nrm) {
    return out;
  }
  int rw, rh, rn;
  u8* rough = stbi_load((base + ".rough.png").c_str(), &rw, &rh, &rn, 1);
  out.w = w;
  out.h = h;
  out.rgba.assign(nrm, nrm + (size_t)w * h * 4);
  for (size_t i = 0; i < (size_t)w * h; i++) {
    out.rgba[i * 4 + 3] = (rough && rw == w && rh == h) ? rough[i] : 255;
  }
  stbi_image_free(nrm);
  if (rough) {
    stbi_image_free(rough);
  }
  if (parallax) {
    int hw, hh, hn;
    u8* height = stbi_load((base + ".parallax.png").c_str(), &hw, &hh, &hn, 1);
    if (height && hw == w && hh == h) {
      out.packed = true;
      for (size_t i = 0; i < (size_t)w * h; i++) {
        out.rgba[i * 4 + 2] = out.rgba[i * 4 + 3];
        out.rgba[i * 4 + 3] = height[i];
      }
    }
    if (height) {
      stbi_image_free(height);
    }
  }
  // optional cap (preset [maps] max_size): 2x2 box halving
  while (max_size > 0 && (out.w > max_size || out.h > max_size) && out.w > 1 && out.h > 1) {
    const int nw = out.w / 2, nh = out.h / 2;
    std::vector<u8> half((size_t)nw * nh * 4);
    for (int y = 0; y < nh; y++) {
      for (int x = 0; x < nw; x++) {
        for (int c = 0; c < 4; c++) {
          int sum = 0;
          for (int k = 0; k < 4; k++) {
            sum += out.rgba[((size_t)(2 * y + k / 2) * out.w + 2 * x + k % 2) * 4 + c];
          }
          half[((size_t)y * nw + x) * 4 + c] = (u8)((sum + 2) / 4);
        }
      }
    }
    out.w = nw;
    out.h = nh;
    out.rgba = std::move(half);
  }
  return out;
}

int g_maps_max_size = 0;  // preset [maps] max_size, 0 = as derived

struct MapStats {
  size_t bytes = 0;  // estimated VRAM of live map textures, with mips
  int textures = 0;
  int levels = 0;
  int packed = 0;  // of `textures`, packed with a parallax height
} g_map_stats;

GLuint g_flat_map = 0;

GLuint flat_map() {
  if (!g_flat_map) {
    const u8 px[4] = {128, 128, 255, 255};
    glGenTextures(1, &g_flat_map);
    glActiveTexture(GL_TEXTURE17);
    glBindTexture(GL_TEXTURE_2D, g_flat_map);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glActiveTexture(GL_TEXTURE0);
  }
  return g_flat_map;
}

// maps by combo_id for generic draws, filled as levels upload
std::unordered_map<u32, std::pair<GLuint, float>> g_combo_maps;
// map textures packed with a parallax height (normal xy, roughness b, height a)
std::unordered_set<GLuint> g_packed_maps;

float class_metallic(u8 cls) {
  cls &= kPbrClassMask;
  return cls == PBR_METAL || cls == PBR_PRECURSOR ? 1.f : 0.f;
}

float lum(const math::Vector3f& c) {
  return 0.299f * c.x() + 0.587f * c.y() + 0.114f * c.z();
}

math::Vector3f norm3(const math::Vector3f& v) {
  float l = v.length();
  return l > 0 ? v / l : v;
}

/// Class tables and the detail source, shared by background, merc and generic programs.
void set_class_uniforms(GLuint program, bool merc) {
  const auto& gs = Gfx::g_global_settings;
  const bool maps = gs.pbr_source == (int)PbrSource::MAPS;
  float detail[PBR_CLASS_COUNT], scale[PBR_CLASS_COUNT], spec[PBR_CLASS_COUNT],
      gloss[PBR_CLASS_COUNT], tint[PBR_CLASS_COUNT][4], colors[PBR_CLASS_COUNT][3];
  // with the ice rule off, `ice` rows take stone's class response (not each row's old class)
  auto cp = [&](int i) -> const PbrClassParams& {
    return i == PBR_ICE && gs.pbr_ice <= 0.f ? g_pbr_params[PBR_STONE] : g_pbr_params[i];
  };
  for (int i = 0; i < PBR_CLASS_COUNT; i++) {
    const auto& p = cp(i);
    detail[i] = (maps ? p.normal : p.detail) * gs.pbr_strength * gs.pbr_detail;
    scale[i] = merc ? p.uv_rep : 1.f / std::max(p.scale_m, 0.01f);
    spec[i] = p.spec * gs.pbr_strength * gs.pbr_spec;
    gloss[i] = p.gloss;
    for (int c = 0; c < 3; c++) {
      tint[i][c] = p.tint[c];
      colors[i][c] = kClassColors[i][c];
    }
    tint[i][3] = p.albedo;
  }
  glUniform1fv(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_detail"), PBR_CLASS_COUNT, detail);
  glUniform1fv(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_scale"), PBR_CLASS_COUNT, scale);
  glUniform1fv(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_spec"), PBR_CLASS_COUNT, spec);
  glUniform1fv(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_gloss"), PBR_CLASS_COUNT, gloss);
  glUniform4fv(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_tint"), PBR_CLASS_COUNT, &tint[0][0]);
  glUniform3fv(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_ccol"), PBR_CLASS_COUNT, &colors[0][0]);
  float cls3[PBR_CLASS_COUNT][3];
  for (int i = 0; i < PBR_CLASS_COUNT; i++) {
    cls3[i][0] = cp(i).env;
    cls3[i][1] = cp(i).rough_bias;
    cls3[i][2] = cp(i).lobe;
  }
  glUniform3fv(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_cls3"), PBR_CLASS_COUNT, &cls3[0][0]);
  float sheen[PBR_CLASS_COUNT];
  for (int i = 0; i < PBR_CLASS_COUNT; i++) {
    sheen[i] = cp(i).sheen;
  }
  glUniform1fv(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_sheen"), PBR_CLASS_COUNT, sheen);
  glUniform4f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_ice"), std::max(gs.pbr_ice, 0.f),
              std::clamp(gs.pbr_ice_depth_m, 0.f, 1.f), std::max(gs.pbr_ice_absorb_m, 0.01f),
              std::clamp(gs.pbr_ice_rough, 0.02f, 1.f));
  glUniform1f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_ice_tr"),
              std::max(gs.pbr_ice_transmit, 0.f));
  float glint[PBR_CLASS_COUNT];
  for (int i = 0; i < PBR_CLASS_COUNT; i++) {
    glint[i] = cp(i).glint * gs.pbr_glint;
  }
  glUniform1fv(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_glint"), PBR_CLASS_COUNT, glint);
  glUniform4f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_glint_shape"),
              std::max(gs.pbr_glint_cell, 0.002f), std::clamp(gs.pbr_glint_density, 0.f, 1.f),
              std::max(gs.pbr_glint_spread, 0.01f), std::max(gs.pbr_glint_width, 0.005f));
  static_assert(PBR_CLASS_COUNT <= 32, "pbr_scatter[32] in pbr_common_frag.glsl");
  float scatter[PBR_CLASS_COUNT][4];
  for (int i = 0; i < PBR_CLASS_COUNT; i++) {
    scatter[i][0] = cp(i).transmit * gs.pbr_transmit;
    scatter[i][1] = std::max(cp(i).transmit_power, 0.5f);
    scatter[i][2] = cp(i).wrap * gs.pbr_wrap;
    scatter[i][3] = cp(i).bake_wrap * gs.pbr_bake_wrap;
  }
  glUniform4fv(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_scatter"), PBR_CLASS_COUNT,
               &scatter[0][0]);
  glUniform4f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_look"), gs.pbr_metallic, gs.pbr_env,
              gs.pbr_fresnel, gs.pbr_ambient);
  glUniform4f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_look2"), gs.pbr_rough_contrast,
              gs.pbr_band, gs.pbr_lobe * gs.pbr_strength * gs.pbr_spec, gs.pbr_metal_darken);
  glUniform1f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_f0"), 0.04f * gs.pbr_specular);
  glUniform1f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_metal_hue"),
              std::clamp(gs.pbr_metal_hue, 0.f, 1.f));
  glUniform1i(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_view"), gs.pbr_view);
  glUniform1i(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_skip"),
              Gfx::perf_opt(PERF_OPT_GEOMETRY + 1) ? 1 : 0);
  glUniform1i(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_probe"), gs.pbr_probe);
  glUniform1i(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_opt"),
              Gfx::perf_opt(PERF_OPT_PBR_EARLY) ? 1 : 0);
  glUniform1i(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_src"), maps ? 1 : 0);
  glUniform2f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_e_ramp"), gs.ssao_emissive_lo,
              gs.ssao_emissive_hi);
  glUniform4f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_px"),
              gs.pbr_parallax ? std::clamp(gs.pbr_px_depth, 0.f, kPbrParallaxMaxDepth) : 0.f,
              (float)std::clamp(gs.pbr_px_taps, 1, 3), gs.pbr_px_grazing[0],
              std::max(gs.pbr_px_grazing[1], gs.pbr_px_grazing[0] + 1e-3f));
  glUniform3f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_px_fade"), gs.pbr_px_fade[0],
              std::max(gs.pbr_px_fade[1], gs.pbr_px_fade[0] + 1e-3f),
              1.f / std::max(gs.pbr_px_fade_ref, 0.01f));
  {
    // parallax self-shadow (background only): amount, height scale, softness, taps
    const int lv = merc ? 0 : std::clamp(gs.pbr_px_shadow, 0, 2);
    const float pxs[3][4] = {{0.f, 1.f, 1.f, 0.f}, {0.7f, 1.f, 0.3f, 8.f}, {1.f, 1.6f, 0.15f, 8.f}};
    glUniform4fv(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_pxs"), 1, pxs[lv]);
  }
  if (maps) {
    glUniform2f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_draw"), 0.f, 0.f);
    glActiveTexture(GL_TEXTURE17);
    glBindTexture(GL_TEXTURE_2D, flat_map());
  } else {
    glActiveTexture(GL_TEXTURE16);
    glBindTexture(GL_TEXTURE_2D_ARRAY, detail_array());
  }
  glActiveTexture(GL_TEXTURE0);
}

/// Mood environment of the last background setup; `up` is world up in the program's frame.
void set_env_uniforms(GLuint program, const math::Vector3f& up) {
  const auto& fl = g_frame_lights;
  auto u3 = [&](GLint loc, const math::Vector3f& v) { glUniform3f(loc, v.x(), v.y(), v.z()); };
  u3(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_env_sky"), fl.env_sky);
  u3(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_env_hor"), fl.env_hor);
  u3(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_env_gnd"), fl.env_gnd);
  u3(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_env_sun"), fl.env_sun);
  glUniform1f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_env_bake"), fl.env_bake);
  u3(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_up"), up);
}

}  // namespace

struct PbrLevelMaps {
  std::string name;
  std::vector<std::string> keys;  // "<tpage>/<name>" per level texture
  std::vector<u32> combo;
  std::vector<u8> cls;
  std::vector<MapImage> images;  // decoded, dropped after upload
  std::vector<GLuint> tex;       // 0: flat default
  std::vector<float> metallic;
  bool decoded = false;
  bool uploaded = false;
  size_t bytes = 0;
  int packed = 0;
};

namespace {

void decode_level_maps(PbrLevelMaps& m) {
  if (m.decoded) {
    return;
  }
  m.decoded = true;
  Timer timer;
  const auto& index = map_index();
  const size_t n = m.keys.size();
  m.images.assign(n, MapImage{});
  m.metallic.assign(n, 0.f);
  std::vector<size_t> todo;
  std::vector<bool> parallax(n, false);
  for (size_t i = 0; i < n; i++) {
    auto it = index.by_key.find(m.keys[i]);
    if (it == index.by_key.end()) {
      m.metallic[i] = class_metallic(m.cls[i]);
      continue;
    }
    m.metallic[i] = it->second.metallic;
    parallax[i] = it->second.parallax;
    if (!it->second.flat) {
      todo.push_back(i);
    }
  }
  const int hw = (int)std::thread::hardware_concurrency();
  const int workers = std::max(1, std::min<int>(hw - 2, (int)todo.size()));
  std::atomic<size_t> next = 0;
  std::vector<std::thread> threads;
  for (int t = 0; t < workers; t++) {
    threads.emplace_back([&]() {
      for (size_t k = next++; k < todo.size(); k = next++) {
        const size_t i = todo[k];
        m.images[i] = decode_map(index.root, m.keys[i], g_maps_max_size, parallax[i]);
      }
    });
  }
  for (auto& t : threads) {
    t.join();
  }
  int found = 0, packed = 0;
  size_t bytes = 0;
  for (auto& im : m.images) {
    if (im.w) {
      found++;
      packed += im.packed;
      bytes += (size_t)im.w * im.h * 4 * 4 / 3;
    }
  }
  lg::info(
      "materials: {} maps for {} of {} textures ({} non-flat in index, {} with parallax height), "
      "~{:.1f} MB, {:.0f} ms",
      m.name, found, n, todo.size(), packed, bytes / 1048576.0, timer.getMs());
}

void upload_level_maps(PbrLevelMaps& m) {
  if (m.uploaded) {
    return;
  }
  perf::AddMs hitch_t(perf::Counter::HITCH_PBR_MS);
  {
    perf::AddMs hitch_d(perf::Counter::HITCH_PBR_DECODE_MS);
    decode_level_maps(m);
  }
  m.uploaded = true;
  Timer timer;
  float aniso = 0.0f;
  glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &aniso);
  m.tex.assign(m.keys.size(), 0);
  glActiveTexture(GL_TEXTURE17);
  for (size_t i = 0; i < m.images.size(); i++) {
    auto& im = m.images[i];
    if (!im.w) {
      continue;
    }
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, im.w, im.h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 im.rgba.data());
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, aniso);
    m.tex[i] = t;
    if (im.packed) {
      g_packed_maps.insert(t);
      m.packed++;
    }
    m.bytes += (size_t)im.w * im.h * 4 * 4 / 3;
    g_combo_maps[m.combo[i]] = {t, m.metallic[i]};
    g_map_stats.textures++;
    im = MapImage{};
  }
  m.images.clear();
  m.images.shrink_to_fit();
  glActiveTexture(GL_TEXTURE0);
  g_map_stats.bytes += m.bytes;
  g_map_stats.levels++;
  g_map_stats.packed += m.packed;
  lg::info("materials: {} maps uploaded, {:.1f} MB VRAM (all levels {:.1f} MB), {:.0f} ms", m.name,
           m.bytes / 1048576.0, g_map_stats.bytes / 1048576.0, timer.getMs());
}

bool maps_mode() {
  return Gfx::g_global_settings.pbr_source == (int)PbrSource::MAPS;
}

}  // namespace

std::shared_ptr<PbrLevelMaps> pbr_maps_create(const tfrag3::Level& level,
                                              const std::string& level_name) {
  auto m = std::make_shared<PbrLevelMaps>();
  m->name = level_name;
  for (auto& t : level.textures) {
    m->keys.push_back(t.debug_tpage_name + "/" + t.debug_name);
    m->combo.push_back(t.combo_id);
    m->cls.push_back(pbr_class_for_combo(t.combo_id));
  }
  if (Gfx::g_global_settings.pbr_enable && maps_mode()) {
    decode_level_maps(*m);
  }
  return m;
}

void pbr_maps_release(PbrLevelMaps* m, std::vector<GLuint>& garbage) {
  if (!m) {
    return;
  }
  for (size_t i = 0; i < m->tex.size(); i++) {
    if (!m->tex[i]) {
      continue;
    }
    auto it = g_combo_maps.find(m->combo[i]);
    if (it != g_combo_maps.end() && it->second.first == m->tex[i]) {
      g_combo_maps.erase(it);
    }
    g_packed_maps.erase(m->tex[i]);
    garbage.push_back(m->tex[i]);
    g_map_stats.textures--;
  }
  if (m->uploaded) {
    g_map_stats.bytes -= m->bytes;
    g_map_stats.levels--;
    g_map_stats.packed -= m->packed;
  }
  m->packed = 0;
  m->tex.clear();
  m->images.clear();
  m->bytes = 0;
  m->uploaded = false;
  m->decoded = false;
}

void PbrDrawState::begin(GLuint program, const std::vector<u8>* classes, PbrLevelMaps* maps) {
  m_mat_loc = GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_mat");
  m_draw_loc = GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_draw");
  m_classes = classes;
  m_maps = maps_mode() ? maps : nullptr;
  m_last_mat = -1;
  m_last_tex = INT32_MIN;
  m_last_map = UINT32_MAX;
  if (m_maps) {
    upload_level_maps(*m_maps);
  }
}

void PbrDrawState::set(s32 tex) {
  const int cls = pbr_texture_class(m_classes, tex);
  const int mat = pbr_texture_mat(m_classes, tex);
  if (!maps_mode()) {
    if (mat != m_last_mat) {
      glUniform1i(m_mat_loc, mat);
      m_last_mat = mat;
    }
    return;
  }
  if (tex == m_last_tex) {
    return;
  }
  m_last_tex = tex;
  GLuint map = 0;
  float metallic = class_metallic((u8)cls);
  if (m_maps && tex >= 0 && tex < (s32)m_maps->tex.size()) {
    map = m_maps->tex[tex];
    metallic = m_maps->metallic[tex];
  }
  set_direct(mat, map, metallic);
}

void PbrDrawState::set_direct(int mat, GLuint map, float metallic) {
  if (mat != m_last_mat) {
    glUniform1i(m_mat_loc, mat);
    m_last_mat = mat;
  }
  if (!maps_mode()) {
    return;
  }
  const GLuint t = map ? map : flat_map();
  if (t != m_last_map) {
    glActiveTexture(GL_TEXTURE17);
    glBindTexture(GL_TEXTURE_2D, t);
    glActiveTexture(GL_TEXTURE0);
    m_last_map = t;
  }
  // 2: packed normal xy, roughness, parallax height
  glUniform2f(m_draw_loc, metallic, map ? (g_packed_maps.count(map) ? 2.f : 1.f) : 0.f);
}

void pbr_maps_for_combo(u32 combo_id, GLuint* map, float* metallic) {
  auto it = g_combo_maps.find(combo_id);
  if (it == g_combo_maps.end()) {
    *map = 0;
    *metallic = class_metallic(pbr_class_for_combo(combo_id));
  } else {
    *map = it->second.first;
    *metallic = it->second.second;
  }
}

const char* pbr_class_name(int cls) {
  return cls >= 0 && cls < PBR_CLASS_COUNT ? kClassNames[cls] : "?";
}

static const std::unordered_set<std::string> kSandTextures = {
#include "SandTextures.inc"
};

std::vector<u8> pbr_classify_level(const tfrag3::Level& level, const std::string& level_name) {
  std::lock_guard<std::mutex> lk(g_table_mutex);
  load_table_locked();
  std::vector<u8> out(level.textures.size(), PBR_SPECIAL);
  int unknown = 0;
  int counts[PBR_CLASS_COUNT] = {};
  for (size_t i = 0; i < level.textures.size(); i++) {
    auto it = g_table.by_combo.find(level.textures[i].combo_id);
    if (it == g_table.by_combo.end()) {
      unknown++;
    } else {
      out[i] = it->second;
    }
    if (level_name == "swamp" && level.textures[i].debug_name == "swp-water") {
      out[i] |= kPbrSwampWaterBit;
    } else if (level_name != "swamp" && kSandTextures.count(level.textures[i].debug_name)) {
      out[i] |= kPbrSandBit;
    }
    counts[out[i] & kPbrClassMask]++;
  }
  std::string summary;
  for (int i = 1; i < PBR_CLASS_COUNT; i++) {
    if (counts[i]) {
      summary += fmt::format(" {} {}", kClassNames[i], counts[i]);
    }
  }
  lg::info("materials: {} {} textures, {} unknown:{}", level_name, level.textures.size(), unknown,
           summary);
  return out;
}

struct FrameLightsCalc {
  int sun_ok = 0, key_ok = 0;
  math::Vector3f dir{0, 0, 0}, sun_col{0, 0, 0}, kdir{0, 0, 0}, key_col{0, 0, 0};
  math::Vector3f env_sky{1, 1, 1}, env_hor{1, 1, 1}, env_gnd{1, 1, 1}, env_sun{0, 0, 0};
  float env_bake = 0;  // 1: no fitted slot is live, metals reflect with the bake's colour
  float sun_l = 0, amb_l = 0, day = 0, amb_all = 0;
  float w_vis[8] = {};
};

/// Blended sun (the moon at night) and mood key light of a level at the camera's time of day (world
/// space).
FrameLightsCalc compute_frame_lights(const UnbakeSidecar* sc,
                                     const GoalBackgroundCameraData& camera,
                                     const math::Vector<u8, 4>& fog) {
  FrameLightsCalc f;
  // blended day sun: fitted direction where it agrees with the table (validated levels)
  int sun_ok = 0;
  float w_vis[8] = {};
  float day = 0;
  math::Vector3f dir(0, 0, 0), col(0, 0, 0), hue(0, 0, 0);
  float sun_l = 0, amb_l = 0;
  float fitted = 1;  // share of the live palette weight on slots the sidecar has a fit for
  if (sc) {
    float w[8][3];
    unbake_slot_weights(camera.itimes, w);
    float total = 0, wfit = 0;
    for (int s = 0; s < 8; s++) {
      total += std::max(w[s][1], 0.f);
      if (sc->slot_mask & (1u << s)) {
        wfit += std::max(w[s][1], 0.f);
      }
      // highlight hue: every live slot's light by its palette weight, so the dusk slot's orange
      // fades into the night blue instead of lingering at full saturation
      const auto& lgt = sc->slots[s].lgt;
      const float m = std::max({lgt.x(), lgt.y(), lgt.z()});
      if ((sc->slot_mask & (1u << s)) && w[s][1] > 0 && m > 1e-3f) {
        hue += lgt * (w[s][1] / m);
      }
    }
    float wd = 0;
    for (int s = 0; s < 8; s++) {
      // the sun by day, the moon (night slots, their fitted light as the shadows cast it) at night
      const bool is_day = (sc->slot_mask & sc->day_mask & (1u << s)) != 0;
      const bool is_night = (sc->slot_mask & sc->night_mask & (1u << s)) != 0;
      if ((!is_day && !is_night) || w[s][1] <= 0) {
        continue;
      }
      const auto& slot = sc->slots[s];
      if (is_night && norm3(slot.dir_fit).y() < 0.05f) {
        continue;  // a night fit below the horizon (swamp, rolling) is no moon
      }
      const float agree = norm3(slot.dir_fit).dot(norm3(slot.dir_table));
      const auto d =
          is_night || agree > std::cos(20.f * kPi / 180.f) ? slot.dir_fit : slot.dir_table;
      dir += norm3(d) * w[s][1];
      col += slot.lgt * w[s][1];
      sun_l += lum(slot.lgt) * w[s][1];
      amb_l += lum(slot.amb) * w[s][1];
      w_vis[s] = w[s][1];
      wd += w[s][1];
    }
    if (wd > 0 && dir.length() > 0) {
      sun_ok = 1;
      for (auto& x : w_vis) {
        x /= wd;
      }
      dir = norm3(dir);
      col /= wd;
      sun_l /= wd;
      amb_l /= wd;
      day = total > 0 ? std::min(1.f, wd / total) : 0.f;
    }
    fitted = total > 0 ? wfit / total : 1.f;
  }
  const float cmax = std::max({col.x(), col.y(), col.z(), 1e-3f});
  const float intensity = std::min(1.f, cmax / 128.f);
  f.sun_ok = sun_ok;
  f.dir = dir;
  const float hmax = std::max({hue.x(), hue.y(), hue.z()});
  f.sun_col = (hmax > 1e-3f ? hue / hmax : col / cmax) * intensity;
  f.sun_l = sun_l;
  f.amb_l = amb_l;
  f.day = day;
  for (int s = 0; s < 8; s++) {
    f.w_vis[s] = w_vis[s];
  }

  // key light where the sun is absent: the mood-table direction per slot (what
  // update-mood-palette hands characters as light-group 0), blended over every live slot
  int key_ok = 0;
  math::Vector3f kdir(0, 0, 0), kcol(0, 0, 0), acol(0, 0, 0);
  float amb_all = 0;
  if (sc) {
    float w[8][3];
    unbake_slot_weights(camera.itimes, w);
    float wk = 0;
    for (int s = 0; s < 8; s++) {
      const auto& slot = sc->slots[s];
      if (w[s][1] <= 0 || slot.dir_table.length() < 1e-3f) {
        continue;
      }
      kdir += norm3(slot.dir_table) * w[s][1];
      kcol += slot.lgt * w[s][1];
      acol += slot.amb * w[s][1];
      amb_all += lum(slot.amb) * w[s][1];
      wk += w[s][1];
    }
    if (wk > 0 && kdir.length() > 0) {
      key_ok = 1;
      kdir = norm3(kdir);
      kcol /= wk;
      amb_all /= wk;
    }
  }
  const float kmax = std::max({kcol.x(), kcol.y(), kcol.z(), 1e-3f});
  f.key_ok = key_ok;
  f.kdir = kdir;
  f.key_col = kcol / kmax;
  f.amb_all = amb_all;

  // mood environment: horizon from the live fog colour, zenith a deeper version of it, ground
  // from the fitted ambient; scaled so the mean over the sphere has luminance 1
  auto unit_lum = [](math::Vector3f c, const math::Vector3f& fallback) {
    const float l = lum(c);
    return l > 0.02f ? c / l : fallback;
  };
  const math::Vector3f grey(1, 1, 1);
  const math::Vector3f amb_n = unit_lum(acol, grey);
  math::Vector3f hor(fog[0] / 255.f, fog[1] / 255.f, fog[2] / 255.f);
  // unfitted light (finalboss's bespoke dawn slot) gives no anchor for the fog hue: go neutral
  const float t = std::clamp(fitted / 0.15f, 0.f, 1.f);
  const float trust = t * t * (3.f - 2.f * t);
  f.env_bake = 1.f - trust;
  hor = unit_lum(hor, amb_n);
  if (trust < 1.f) {
    hor = grey + (hor - grey) * trust;
  }
  hor = hor * 0.75f + amb_n * 0.25f;
  math::Vector3f sky;
  for (int i = 0; i < 3; i++) {
    sky[i] = std::max(0.f, 1.f + (hor[i] - 1.f) * 1.4f);
  }
  sky = unit_lum(sky, hor) * 1.15f;
  const math::Vector3f warm = unit_lum(math::Vector3f(0.55f, 0.47f, 0.38f), grey);
  const math::Vector3f gnd = (amb_n * 0.5f + warm * 0.5f) * 0.45f;
  const float mean = lum((sky + hor * 2.f + gnd) * 0.25f);
  f.env_sky = sky / mean;
  f.env_hor = hor / mean;
  f.env_gnd = gnd / mean;
  f.env_sun = sun_ok ? f.sun_col * 0.6f : (key_ok ? f.key_col * 0.25f : math::Vector3f(0, 0, 0));
  return f;
}

void store_frame_lights(const FrameLightsCalc& f, const math::Vector4f* camera_rot) {
  g_frame_lights.sun_ok = f.sun_ok;
  g_frame_lights.sun_dir = f.dir;
  g_frame_lights.sun_col = f.sun_col;
  g_frame_lights.day = f.day;
  g_frame_lights.key_ok = f.key_ok;
  g_frame_lights.key_dir = f.kdir;
  g_frame_lights.key_col = f.key_col;
  g_frame_lights.env_sky = f.env_sky;
  g_frame_lights.env_hor = f.env_hor;
  g_frame_lights.env_gnd = f.env_gnd;
  g_frame_lights.env_sun = f.env_sun;
  g_frame_lights.env_bake = f.env_bake;
  for (int i = 0; i < 3; i++) {
    g_frame_lights.rot[i] = camera_rot[i];
  }
}

bool pbr_frame_sun(math::Vector3f* dir, math::Vector3f* col) {
  *dir = g_frame_lights.sun_dir;
  *col = g_frame_lights.sun_col;
  return g_frame_lights.sun_ok != 0;
}

float water_sun_glint_strength(float day) {
  return Gfx::g_global_settings.water_sun_glint_strength * std::clamp(day, 0.f, 1.f);
}

namespace {
// day sun of the last few levels whose background ran, by sidecar
struct LevelSun {
  const UnbakeSidecar* sc = nullptr;
  math::Vector3f dir{0, 0, 0}, col{0, 0, 0};
  float day = 0;
  bool ok = false;
  u64 frame = 0;  // frame of the last setup: an unloaded level's address may come back
};
std::array<LevelSun, 4> g_level_suns;
size_t g_level_sun_next = 0;

void note_level_sun(const UnbakeSidecar* sc, const FrameLightsCalc& f, u64 frame) {
  if (!sc) {
    return;
  }
  LevelSun* e = nullptr;
  for (auto& s : g_level_suns) {
    e = s.sc == sc ? &s : e;
  }
  if (!e) {
    e = &g_level_suns[g_level_sun_next];
    g_level_sun_next = (g_level_sun_next + 1) % g_level_suns.size();
  }
  *e = {sc, f.dir, f.sun_col, f.day, f.sun_ok != 0, frame};
}
}  // namespace

bool water_level_sun(const UnbakeSidecar* sc, u64 frame, math::Vector3f* dir, math::Vector3f* col,
                     float* day) {
  for (const auto& s : g_level_suns) {
    // this frame's or the last one's: the far ocean draws before the backgrounds
    if (sc && s.sc == sc && s.frame + 1 >= frame) {
      *dir = s.dir;
      *col = s.col;
      *day = s.day;
      return s.ok;
    }
  }
  *day = g_frame_lights.day;
  return pbr_frame_sun(dir, col);
}

// wave set (0 sea, 1 lake) of each level with an ocean map (level-info.gc :ocean); mirrors
// presets/water.toml [bodies]
constexpr struct {
  const char* level;
  int body;
} kWaterBodies[] = {{"training", 0}, {"village1", 0}, {"beach", 0},  {"jungle", 0},
                    {"misty", 0},    {"village2", 0}, {"swamp", 0},  {"sunken", 1},
                    {"sunkenb", 1}};

struct {
  u64 frame = UINT64_MAX;
  int body = -1;
  const UnbakeSidecar* sc = nullptr;
} g_water_body;

void note_background_lights(SharedRenderState* render_state,
                            const UnbakeSidecar* sidecar,
                            const GoalBackgroundCameraData& camera) {
  if (!((pbr_active(render_state) && Gfx::g_global_settings.pbr_background) ||
        water_active(render_state))) {
    return;
  }
  const FrameLightsCalc f = compute_frame_lights(sidecar, camera, render_state->fog_color);
  note_level_sun(sidecar, f, render_state->frame_idx);
  store_frame_lights(f, render_state->eye.active ? render_state->game_camera.rot : camera.rot);
}

void water_note_background(SharedRenderState* render_state,
                           const UnbakeSidecar* sidecar,
                           const GoalBackgroundCameraData& camera,
                           const std::string& level_name) {
  if (!water_active(render_state)) {
    return;
  }
  // merc and generic light in the game camera's space, also while an XR eye is drawn
  const FrameLightsCalc f = compute_frame_lights(sidecar, camera, render_state->fog_color);
  note_level_sun(sidecar, f, render_state->frame_idx);
  store_frame_lights(f, render_state->eye.active ? render_state->game_camera.rot : camera.rot);
  if (g_water_body.frame != render_state->frame_idx) {
    g_water_body.frame = render_state->frame_idx;
    g_water_body.body = -1;
    g_water_body.sc = nullptr;
  }
  for (const auto& b : kWaterBodies) {
    if (level_name == b.level) {
      g_water_body.body = b.body;
      g_water_body.sc = sidecar;
    }
  }
}

const UnbakeSidecar* water_ocean_level() {
  return g_water_body.sc;
}

bool water_active(const SharedRenderState* render_state) {
  return render_state->version == GameVersion::Jak1 && Gfx::g_global_settings.water_enable;
}

bool water_ray_depths(const SharedRenderState* render_state, double* z1, double* z2) {
  if (!render_state->has_pc_data) {
    return false;
  }
  const auto fc = frame_camera(*render_state);
  const double fog = fc.fog->x();
  // column 2 of the rotation is the view axis in world space; its sign is checked below
  const double f[3] = {fc.rot[0][2], fc.rot[1][2], fc.rot[2][2]};
  auto gs_depth = [&](double sign, double dist, double* z) {
    double q[4] = {(*fc.pos)[0] + sign * f[0] * dist, (*fc.pos)[1] + sign * f[1] * dist,
                   (*fc.pos)[2] + sign * f[2] * dist, 1.};
    double c[4] = {0, 0, 0, 0};
    for (int i = 0; i < 4; i++) {
      for (int k = 0; k < 4; k++) {
        c[k] -= q[i] * fc.matrix[i][k];
      }
    }
    if (c[3] <= 0.) {
      return false;
    }
    *z = c[2] * (fog / c[3]) + fc.hvdf_off->z();
    return true;
  };
  constexpr double kUnit = 64. * 4096.;
  for (double sign : {1., -1.}) {
    if (gs_depth(sign, kUnit, z1) && gs_depth(sign, 2. * kUnit, z2) && *z1 != *z2) {
      return true;
    }
  }
  return false;
}

float water_ocean_height(const SharedRenderState* render_state) {
  // *swamp-low-ocean-marker*, and 200 m around it
  const auto& c = render_state->camera_pos;
  const double dx = (c.x() - 2621440.) / 4096.;
  const double dz = (c.z() + 8151040.) / 4096.;
  if (dx * dx + dz * dz < 40000.) {
    return -4096.f;
  }
  return c.y() > -409600.f ? 0.f : -98304.f;
}

bool water_level_has_sea(const std::string& level_name) {
  for (const auto& b : kWaterBodies) {
    if (level_name == b.level) {
      return b.body == 0;
    }
  }
  return false;
}

bool water_gs_inverse(const SharedRenderState* render_state, double inv[4][4]) {
  const auto fc = frame_camera(*render_state);
  if (!render_state->has_pc_data || fc.fog->x() == 0.f) {
    return false;
  }
  // a[r][c]: gs-space homogeneous = a * (world, 1), columns are camera_matrix[c]
  double a[4][8];
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      a[r][c] = fc.matrix[c][r];
      a[r][c + 4] = r == c ? 1. : 0.;
    }
  }
  for (int c = 0; c < 4; c++) {
    int piv = c;
    for (int r = c + 1; r < 4; r++) {
      if (std::abs(a[r][c]) > std::abs(a[piv][c])) {
        piv = r;
      }
    }
    if (std::abs(a[piv][c]) < 1e-30) {
      return false;
    }
    if (piv != c) {
      for (int k = 0; k < 8; k++) {
        std::swap(a[c][k], a[piv][k]);
      }
    }
    const double d = a[c][c];
    for (int k = 0; k < 8; k++) {
      a[c][k] /= d;
    }
    for (int r = 0; r < 4; r++) {
      if (r != c) {
        const double f = a[r][c];
        for (int k = 0; k < 8; k++) {
          a[r][k] -= f * a[c][k];
        }
      }
    }
  }
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      inv[r][c] = a[r][c + 4];
    }
  }
  return true;
}

namespace {
struct WaterCamStamp {
  u64 frame;
  math::Vector3f pos;
};
std::unordered_map<GLuint, WaterCamStamp> g_water_cam_stamp;
}  // namespace

bool water_cam_stamp(GLuint program, u64* frame, math::Vector3f* pos) {
  auto it = g_water_cam_stamp.find(program);
  if (it == g_water_cam_stamp.end()) {
    return false;
  }
  *frame = it->second.frame;
  *pos = it->second.pos;
  return true;
}

void water_setup_ocean(SharedRenderState* render_state,
                       GLuint program,
                       float plane_y,
                       bool plane_ok) {
  if (!water_active(render_state)) {
    glUniform1i(glGetUniformLocation(program, "water_on"), 0);
    return;
  }
  const auto& gs = Gfx::g_global_settings;
  const auto& fl = g_frame_lights;
  glUniform1i(glGetUniformLocation(program, "water_on"), 1);
  perf::set_opt_uniform(program);
  // the ocean's GS vertices are the game camera's: shade in its frame (XR eyes reproject after)
  const auto fc = frame_camera(*render_state);
  // camera space to world: w[i] = sum_j rot[i][j] * c[j]; GLSL mat3 is column-major
  float m[9];
  for (int j = 0; j < 3; j++) {
    for (int i = 0; i < 3; i++) {
      m[j * 3 + i] = fc.rot[i][j];
    }
  }
  glUniformMatrix3fv(glGetUniformLocation(program, "water_rot"), 1, GL_FALSE, m);
  const auto* p = fc.persp;
  const float fog = fc.fog->x();
  const float kx = std::abs(fog * p[0][0]) > 1e-9f ? p[2][3] / (fog * p[0][0]) : 0.f;
  const float ky = std::abs(fog * p[1][1]) > 1e-9f ? p[2][3] / (fog * p[1][1]) : 0.f;
  glUniform4f(glGetUniformLocation(program, "water_proj"), fc.hvdf_off->x(), fc.hvdf_off->y(),
              kx, ky);
  glUniform1i(glGetUniformLocation(program, "water_sun_ok"), fl.sun_ok);
  glUniform3f(glGetUniformLocation(program, "water_sun_dir"), fl.sun_dir.x(), fl.sun_dir.y(),
              fl.sun_dir.z());
  glUniform3f(glGetUniformLocation(program, "water_sun_col"), fl.sun_col.x(), fl.sun_col.y(),
              fl.sun_col.z());
  glUniform4f(glGetUniformLocation(program, "water_params"), gs.water_wave, gs.water_glint,
              gs.water_glint_exp, gs.water_fresnel);
  glUniform1f(glGetUniformLocation(program, "water_sparkle"), gs.water_sparkle);
  {
    // the glint takes the sun of the ocean's own level, not of an enclosed one drawn after it
    math::Vector3f gd, gc;
    float day = 0;
    const bool ok =
        water_level_sun(water_ocean_level(), render_state->frame_idx, &gd, &gc, &day);
    glUniform4f(glGetUniformLocation(program, "sun_glint"), ok ? water_sun_glint_strength(day) : 0.f,
                gs.water_sun_glint_rough, gs.water_sun_glint_peak, gs.water_sun_glint ? 1.f : 0.f);
    glUniform3f(glGetUniformLocation(program, "sun_glint_dir"), gd.x(), gd.y(), gd.z());
    glUniform3f(glGetUniformLocation(program, "sun_glint_col"), gc.x(), gc.y(), gc.z());
  }
  world_shadows_set_uniforms(render_state, program, false);  // the glint's sun shadow
  glUniform1i(glGetUniformLocation(program, "water_view"), gs.water_view);

  // Gerstner displacement, foam, shallow colour, subsurface tint
  double inv[4][4];
  const bool disp_ok = water_gs_inverse(render_state, inv);
  glUniform1i(glGetUniformLocation(program, "water_disp_on"), disp_ok ? 1 : 0);
  if (disp_ok) {
    float mf[16], mi[16];
    for (int c = 0; c < 4; c++) {
      for (int r = 0; r < 4; r++) {
        mf[c * 4 + r] = fc.matrix[c][r];
        mi[c * 4 + r] = (float)inv[r][c];
      }
    }
    glUniformMatrix4fv(glGetUniformLocation(program, "water_cam"), 1, GL_FALSE, mf);
    glUniformMatrix4fv(glGetUniformLocation(program, "water_cam_inv"), 1, GL_FALSE, mi);
    const auto& cpos = *fc.pos;
    g_water_cam_stamp[program] = {render_state->frame_idx,
                                  math::Vector3f(cpos.x(), cpos.y(), cpos.z())};
  }
  const auto& hv = *fc.hvdf_off;
  glUniform4f(glGetUniformLocation(program, "water_hvdf"), hv.x(), hv.y(), hv.z(), fog);
  const auto& cp = *fc.pos;
  glUniform4f(glGetUniformLocation(program, "water_eye"), cp.x(), cp.y(), cp.z(), plane_y);
  double rz1 = 0., rz2 = 0.;
  const bool ray_ok = water_ray_depths(render_state, &rz1, &rz2);
  glUniform1i(glGetUniformLocation(program, "water_plane_ok"), plane_ok && ray_ok ? 1 : 0);
  glUniform2f(glGetUniformLocation(program, "water_ray_z"), (float)rz1, (float)rz2);
  glUniform1f(glGetUniformLocation(program, "water_time"),
              gs.water_clock_s >= 0.f ? gs.water_clock_s
                                      : (float)(render_state->frame_idx % 216000) / 60.f);

  // update-ocean also switches to the sunken map with the camera below -100 m
  const bool lake = (g_water_body.frame == render_state->frame_idx && g_water_body.body == 1) ||
                    fc.pos->y() < -409600.f;
  const WaterWave* waves = lake ? gs.water_lake_waves : gs.water_waves;
  float w0[4 * kWaterWaves] = {}, w1[4 * kWaterWaves] = {};
  int n = 0;
  float lmax = 0.f, steep_sum = 0.f;
  // steepest slope (1/m) the phase and amplitude noise add to each wave's wavenumber
  const float noise_dk = (6.2831853f * gs.water_phase_noise + 2.f * gs.water_amp_noise) *
                         2.13f / gs.water_noise_m;
  // the crest warp's steepest shift gradient: across a wave it turns the crests, along it
  // (a third as fast) it shortens them; both octaves (water_gerstner)
  const float warp_g = 3.f * 1.866f * gs.water_tile_warp_m / gs.water_tile_warp_cell_m;
  const float warp_k = std::hypot(1.f + warp_g / 3.f, warp_g);
  for (int i = 0; i < kWaterWaves; i++) {
    const auto& w = waves[i];
    if (w.amp_m > 0.f) {
      n++;
      steep_sum += std::min(w.steep * gs.water_displace, 1.f) *
                   (1.f + noise_dk * w.length_m / 6.2831853f) * warp_k;
      // the swell (last) keeps full range; the chop ranges scale to the longest chop wave
      if (i < kWaterWaves - 1) {
        lmax = std::max(lmax, w.length_m);
      }
    }
  }
  lmax = std::max(lmax, 1e-3f);
  // the steepness sum stays <= 1 so the crests never fold; four waves or fewer divide by their
  // count as before, so adding the swell leaves the chop as it was
  const float steep_div = std::max(steep_sum, (float)std::min(n, kWaterWaves - 1));
  float amp_sum = 0.f;
  float dmax_h = 0.f, dmax_v = 0.f;  // bounds of water_gerstner's D, for the patch cull
  for (int i = 0; i < kWaterWaves; i++) {
    const auto& w = waves[i];
    if (w.amp_m <= 0.f || n == 0) {
      continue;
    }
    const float k = 6.2831853f / w.length_m;
    const float rad = w.dir_deg * 0.017453293f;
    const float amp = w.amp_m * gs.water_displace;
    const float qa = std::min(w.steep * gs.water_displace, 1.f) / (k * steep_div);
    w0[i * 4 + 0] = std::cos(rad);
    w0[i * 4 + 1] = std::sin(rad);
    w0[i * 4 + 2] = k;
    w0[i * 4 + 3] = amp;
    w1[i * 4 + 0] = w.speed * k;
    w1[i * 4 + 1] = qa;
    w1[i * 4 + 2] = std::clamp(std::sqrt(w.length_m / lmax), 0.3f, 1.f);
    dmax_h += std::abs(qa);
    dmax_v += std::abs(amp) * (1.f + std::abs(gs.water_amp_noise));
    // wave height (foam, crest light) is relative to the chop, the swell lifts it
    if (i < kWaterWaves - 1) {
      amp_sum += amp;
    }
  }
  glUniform4fv(glGetUniformLocation(program, "water_w0"), kWaterWaves, w0);
  glUniform2f(glGetUniformLocation(program, "water_dmax"), dmax_h * 1.05f + 0.01f,
              dmax_v * 1.05f + 0.01f);
  glUniform4fv(glGetUniformLocation(program, "water_w1"), kWaterWaves, w1);
  glUniform4f(glGetUniformLocation(program, "water_wnoise"), gs.water_amp_noise,
              gs.water_phase_noise, 1.f / gs.water_noise_m, 0.01f);
  glUniform4f(glGetUniformLocation(program, "water_warp"), gs.water_tile_warp_m,
              1.f / gs.water_tile_warp_cell_m, 0.f, 0.f);
  // the game's ocean texture repeats every 96 m (render-ocean-far, ocean-near st)
  glUniform4f(glGetUniformLocation(program, "water_tiling"), gs.water_tile_hex,
              1.f / gs.water_tile_hex_m, gs.water_tile_hex_rot_deg * 0.017453293f, 1.f / 96.f);
  // x, y: fade start and end (m); z: 1 with perf-opt bit 27
  glUniform4f(glGetUniformLocation(program, "water_lod"), gs.water_lod_start_m,
              std::max(gs.water_lod_end_m, gs.water_lod_start_m + 1.f),
              Gfx::perf_opt(PERF_OPT_WATER_LOD) ? 1.f : 0.f, 0.f);
  glUniform1i(glGetUniformLocation(program, "water_lod_probe"), gs.water_lod_probe);
  glUniform4f(glGetUniformLocation(program, "water_aa"), gs.water_aa_lod_start,
              std::max(gs.water_aa_lod_end, gs.water_aa_lod_start + 0.01f),
              gs.water_aa_foam_edge_px, 0.f);
  glUniform2f(glGetUniformLocation(program, "water_wave_aa"),
              std::max(gs.water_aa_wave_px_full, gs.water_aa_wave_px_zero + 0.01f),
              gs.water_aa_wave_px_zero);

  // mood environment for the fresnel reflection (the material response's, compute_frame_lights)
  auto u3 = [&](const char* name, const math::Vector3f& v) {
    glUniform3f(glGetUniformLocation(program, name), v.x(), v.y(), v.z());
  };
  u3("water_env_sky", fl.env_sky);
  u3("water_env_hor", fl.env_hor);
  u3("water_env_sun", fl.env_sun);
  math::Vector3f glow_dir = fl.sun_ok ? fl.sun_dir : fl.key_dir;
  if (glow_dir.length() < 1e-3f) {
    glow_dir = math::Vector3f(0, 1, 0);  // no light: env_sun is 0, keep normalize() finite
  }
  u3("water_env_sun_dir", glow_dir);
  glUniform2f(glGetUniformLocation(program, "water_sky"), gs.water_sky_refl,
              gs.water_sky_haze_m);
  const float r_zero = std::max(gs.water_range_zero_m, gs.water_range_full_m + 1.f);
  glUniform4f(glGetUniformLocation(program, "water_range"), gs.water_range_full_m, r_zero, 0.f,
              0.f);
  // tessellation: 0.75 m segments within 250 m, 3 GS pixels (about 15 on a 2560 wide target)
  // beyond
  glUniform4f(glGetUniformLocation(program, "water_tess"), 3.f, gs.water_tess_seg_m,
              (float)gs.water_subdiv, r_zero);
  glUniform4f(glGetUniformLocation(program, "water_disp"), amp_sum, r_zero, gs.water_sss,
              gs.water_crest_light);
  glUniform4f(glGetUniformLocation(program, "water_foam"), gs.water_crest_foam, gs.water_crest_j,
              gs.water_shore_foam, gs.water_shore_m);
  glUniform4f(glGetUniformLocation(program, "water_tint"), gs.water_shallow, gs.water_shallow_m,
              gs.water_absorb, gs.water_shallow_depth_m);
  glUniform1f(glGetUniformLocation(program, "water_opaque_m"), gs.water_opaque_m);
  glUniform3f(glGetUniformLocation(program, "water_absorb_rgb"), gs.water_absorb_rgb[0],
              gs.water_absorb_rgb[1], gs.water_absorb_rgb[2]);
  glUniform1i(glGetUniformLocation(program, "water_pass"), 0);
  glUniform2f(glGetUniformLocation(program, "water_shore_fade"), gs.water_shore_fade_near_m,
              std::max(gs.water_shore_fade_far_m, gs.water_shore_fade_near_m + 1.f));
  glUniform1f(glGetUniformLocation(program, "water_shoal"), gs.water_shoal_m);
  glUniform1f(glGetUniformLocation(program, "water_shoal_occ"), gs.water_shoal_occ);
  glUniform1f(glGetUniformLocation(program, "water_depth_blur"), gs.water_depth_blur_m);

  // scene depth under the water, linearized as in make_new_cam_mat
  const GLuint dt = render_state->water_depth_tex;
  glUniform1i(glGetUniformLocation(program, "water_depth_ok"), dt ? 1 : 0);
  if (dt) {
    const float pzw = p[2][3];
    glUniform3f(glGetUniformLocation(program, "water_lin"), fog * p[3][2] / 8388608.f, pzw,
                fog * p[2][2] / 8388608.f + (hv.z() / 8388608.f - 1.f) * pzw);
    glActiveTexture(GL_TEXTURE7);
    glBindTexture(GL_TEXTURE_2D, dt);
    glUniform1i(glGetUniformLocation(program, "water_depth"), 7);
    // the shoaling reads the seabed only: the near ocean's re-copy also holds the mid ocean
    glActiveTexture(GL_TEXTURE12);
    glBindTexture(GL_TEXTURE_2D, render_state->water_shoal_tex ? render_state->water_shoal_tex : dt);
    glUniform1i(glGetUniformLocation(program, "water_shoal_depth"), 12);
    glActiveTexture(GL_TEXTURE0);
  }
}

bool pbr_was_used() {
  return g_pbr_used;
}

bool pbr_active(const SharedRenderState* render_state) {
  return render_state->version == GameVersion::Jak1 && Gfx::g_global_settings.pbr_enable;
}

void pbr_setup_background(SharedRenderState* render_state,
                          GLuint program,
                          const UnbakeSidecar* sc,
                          const UnbakeTree* tree,
                          const GoalBackgroundCameraData& camera) {
  const auto& gs = Gfx::g_global_settings;
  if (!pbr_active(render_state) || !gs.pbr_background) {
    if (g_pbr_used) {
      glUniform1i(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_on"), 0);
    }
    return;
  }
  g_pbr_used = true;
  glUniform1i(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_on"), 1);
  set_class_uniforms(program, false);

  const FrameLightsCalc f = compute_frame_lights(sc, camera, render_state->fog_color);
  note_level_sun(sc, f, render_state->frame_idx);
  glUniform1i(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_sun_ok"), f.sun_ok);
  glUniform3f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_sun_dir"), f.dir.x(), f.dir.y(),
              f.dir.z());
  glUniform3f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_sun_col"), f.sun_col.x(), f.sun_col.y(),
              f.sun_col.z());
  glUniform1f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_sun_l"), f.sun_l);
  glUniform1f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_amb_l"), f.amb_l);
  glUniform1f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_day"), f.day);
  glUniform1fv(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_w"), 8, f.w_vis);
  glUniform1i(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_key_ok"), f.key_ok);
  glUniform3f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_key_dir"), f.kdir.x(), f.kdir.y(),
              f.kdir.z());
  glUniform3f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_key_col"), f.key_col.x(), f.key_col.y(),
              f.key_col.z());
  glUniform1f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_amb_all"), f.amb_all);
  glUniform1f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_key_scale"), gs.pbr_key);
  // enclosed levels (one fixed palette slot in the sidecar: maincave, darkcave, robocave, jungleb,
  // lavatube, citadel) read as damp
  const bool enclosed = sc && sc->slot_mask != 0 && (sc->slot_mask & (sc->slot_mask - 1)) == 0;
  glUniform1i(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_hl_on"),
              render_state->light_material ? 1 : 0);
  glUniform1f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_damp"),
              render_state->light_material && enclosed ? gs.light_highlights_damp : 0.f);
  store_frame_lights(f, render_state->eye.active ? render_state->game_camera.rot : camera.rot);
  set_env_uniforms(program, math::Vector3f(0, 1, 0));
  SsaoRenderer::set_detail_uniforms(render_state, program);
  canopy_set_backlight_uniforms(program);

  const bool has_rec = tree && tree->valid && sc;
  glUniform1i(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_rec"), has_rec ? 1 : 0);
  const GLuint px_tex =
      has_rec && gs.pbr_parallax && gs.pbr_px_basis ? unbake_px_texture(*tree) : 0;
  glUniform1i(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_pxb"), px_tex ? 1 : 0);
  if (px_tex) {
    glActiveTexture(GL_TEXTURE18);
    glBindTexture(GL_TEXTURE_BUFFER, px_tex);
    glActiveTexture(GL_TEXTURE0);
  }
  if (has_rec) {
    float ew[kUnbakeEmissiveSources] = {};
    if (sc->has_emissive) {
      unbake_emissive_weights(*sc, camera, ew);
    }
    glUniform1fv(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_emw"), kUnbakeEmissiveSources, ew);
    glActiveTexture(GL_TEXTURE11);
    glBindTexture(GL_TEXTURE_BUFFER, unbake_texture(*tree));
    glActiveTexture(GL_TEXTURE0);
  }
}

void pbr_setup_merc(SharedRenderState* render_state, GLuint program) {
  const auto& gs = Gfx::g_global_settings;
  if (!pbr_active(render_state) || !gs.pbr_merc) {
    if (g_pbr_used) {
      glUniform1i(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_on"), 0);
    }
    return;
  }
  g_pbr_used = true;
  glUniform1i(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_on"), 1);
  glUniform1f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_vsign"), gs.pbr_merc_vsign);
  set_class_uniforms(program, true);
  // merc camera frame: world up is row 1 of the world-to-camera rotation (as fur and haze use)
  math::Vector3f up(0.f, 1.f, 0.f);
  if (render_state->has_pc_data) {
    const auto& r = frame_camera(*render_state).rot[1];
    up = math::Vector3f(r.x(), r.y(), r.z());
  }
  set_env_uniforms(program, up * gs.pbr_merc_up);
  glUniform2f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "llh_merc"),
              render_state->light_material ? gs.light_highlights_strength : 0.f,
              std::max(gs.light_highlights_max, 0.01f));
}

u8 pbr_class_for_combo(u32 combo_id) {
  std::lock_guard<std::mutex> lk(g_table_mutex);
  load_table_locked();
  auto it = g_table.by_combo.find(combo_id);
  return it == g_table.by_combo.end() ? (u8)PBR_SPECIAL : (u8)(it->second & kPbrClassMask);
}

int pbr_mat_for_combo(u32 combo_id) {
  std::lock_guard<std::mutex> lk(g_table_mutex);
  load_table_locked();
  auto it = g_table.by_combo.find(combo_id);
  return it == g_table.by_combo.end() ? PBR_SPECIAL
                                      : it->second & (kPbrClassMask | kPbrBakedTintBit);
}

void pbr_setup_generic(SharedRenderState* render_state, GLuint program, bool on) {
  const auto& gs = Gfx::g_global_settings;
  if (!on || !pbr_active(render_state) || !gs.pbr_background) {
    if (g_pbr_used) {
      glUniform1i(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_on"), 0);
    }
    return;
  }
  g_pbr_used = true;
  glUniform1i(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_on"), 1);
  set_class_uniforms(program, true);
  // background camera space: c[j] = sum_i rot[i][j] * w[i]
  const auto& fl = g_frame_lights;
  auto to_cam = [&](const math::Vector3f& w) {
    math::Vector3f c;
    for (int j = 0; j < 3; j++) {
      c[j] = fl.rot[0][j] * w[0] + fl.rot[1][j] * w[1] + fl.rot[2][j] * w[2];
    }
    return c;
  };
  const auto sd = to_cam(fl.sun_dir), kd = to_cam(fl.key_dir);
  glUniform1i(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_sun_ok"), fl.sun_ok);
  glUniform3f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_sun_dir"), sd.x(), sd.y(), sd.z());
  glUniform3f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_sun_col"), fl.sun_col.x(),
              fl.sun_col.y(), fl.sun_col.z());
  glUniform1i(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_key_ok"), fl.key_ok);
  glUniform3f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_key_dir"), kd.x(), kd.y(), kd.z());
  glUniform3f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_key_col"), fl.key_col.x(),
              fl.key_col.y(), fl.key_col.z());
  glUniform1f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, program, "pbr_key_scale"), gs.pbr_key);
  set_env_uniforms(program, to_cam(math::Vector3f(0, 1, 0)));
}

namespace {

fs::path preset_path() {
  if (const char* p = std::getenv("SANDOVER_PBR_PRESET")) {
    return fs::path(p);
  }
  return file_util::get_jak_project_dir() / "sandover" / "presets" / "pbr.toml";
}

std::string trim(const std::string& s) {
  const auto a = s.find_first_not_of(" \t\r");
  const auto b = s.find_last_not_of(" \t\r");
  return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

/// Numbers of a TOML value: `1.5` or `[1, 0.9, 0.8]`.
std::vector<float> toml_numbers(const std::string& v) {
  std::vector<float> out;
  std::string cur;
  for (char c : v + ",") {
    if (c == '[' || c == ']' || c == ' ') {
      continue;
    }
    if (c == ',') {
      if (!cur.empty()) {
        out.push_back(std::strtof(cur.c_str(), nullptr));
      }
      cur.clear();
    } else {
      cur += c;
    }
  }
  return out;
}

}  // namespace

namespace {

const char* kLookNames[] = {"base", "faithful", "bold", "cinematic"};
constexpr int kLookCount = 4;

struct PresetEntry {
  std::string section, key, val;
  int line;
};

/// Applies one preset value; false if the key is not understood.
bool apply_preset_value(const std::string& section,
                        const std::string& key,
                        const std::string& val) {
  auto& gs = Gfx::g_global_settings;
  const auto nums = toml_numbers(val);
  const float f = nums.empty() ? 0.f : nums[0];
  if (section == "global") {
    const std::pair<const char*, float*> globals[] = {
        {"strength", &gs.pbr_strength},
        {"detail", &gs.pbr_detail},
        {"highlight", &gs.pbr_spec},
        {"key", &gs.pbr_key},
        {"metallic", &gs.pbr_metallic},
        {"env", &gs.pbr_env},
        {"fresnel", &gs.pbr_fresnel},
        {"ambient", &gs.pbr_ambient},
        {"roughness_contrast", &gs.pbr_rough_contrast},
        {"band", &gs.pbr_band},
        {"lobe", &gs.pbr_lobe},
        {"metal_darken", &gs.pbr_metal_darken},
        {"specular", &gs.pbr_specular},
        {"metal_hue", &gs.pbr_metal_hue},
        {"transmit", &gs.pbr_transmit},
        {"wrap", &gs.pbr_wrap},
        {"bake_wrap", &gs.pbr_bake_wrap},
        {"ice", &gs.pbr_ice},
        {"glint", &gs.pbr_glint},
        {"glint_cell_m", &gs.pbr_glint_cell},
        {"glint_density", &gs.pbr_glint_density},
        {"glint_spread", &gs.pbr_glint_spread},
        {"glint_width", &gs.pbr_glint_width}};
    for (auto& [name, ptr] : globals) {
      if (key == name) {
        *ptr = f;
        return true;
      }
    }
    if (key == "wind_tie") {
      gs.pbr_wind_tie = f != 0.f;
      return true;
    }
    if (key == "sun_occlusion") {
      gs.pbr_transmit_occl = f != 0.f;
      return true;
    }
    if (key == "leaf_density") {
      gs.pbr_transmit_density = std::clamp(f, 0.f, 4.f);
      return true;
    }
    if (key == "source") {
      gs.pbr_source = val.find("procedural") != std::string::npos ? 0 : 1;
      return true;
    }
    return key == "look";  // read by pbr_load_preset
  }
  if (section == "ice") {
    const std::pair<const char*, float*> ice[] = {{"depth_m", &gs.pbr_ice_depth_m},
                                                  {"absorb_m", &gs.pbr_ice_absorb_m},
                                                  {"roughness", &gs.pbr_ice_rough},
                                                  {"transmit", &gs.pbr_ice_transmit}};
    for (auto& [name, ptr] : ice) {
      if (key == name) {
        *ptr = f;
        return true;
      }
    }
    return false;
  }
  if (section == "maps") {
    if (key == "max_size") {
      g_maps_max_size = (int)f;
      return true;
    }
    return false;
  }
  if (section == "parallax") {
    if (key == "enable") {
      gs.pbr_parallax = f != 0.f;
    } else if (key == "depth_m") {
      gs.pbr_px_depth = f;
    } else if (key == "taps") {
      gs.pbr_px_taps = (int)f;
    } else if (key == "grazing" && nums.size() == 2) {
      std::copy(nums.begin(), nums.end(), gs.pbr_px_grazing);
    } else if (key == "fade_m" && nums.size() == 2) {
      std::copy(nums.begin(), nums.end(), gs.pbr_px_fade);
    } else if (key == "fade_ref_m") {
      gs.pbr_px_fade_ref = f;
    } else if (key == "basis") {
      gs.pbr_px_basis = f != 0.f;
    } else {
      return false;
    }
    return true;
  }
  if (section.rfind("class.", 0) != 0) {
    return false;
  }
  const u8 cls = class_from_name(section.substr(6));
  if (section.substr(6) != kClassNames[cls]) {
    return false;
  }
  auto& p = g_pbr_params[cls];
  static const std::pair<const char*, float PbrClassParams::*> kClass[] = {
      {"normal", &PbrClassParams::normal},
      {"detail", &PbrClassParams::detail},
      {"tile_m", &PbrClassParams::scale_m},
      {"uv_repeats", &PbrClassParams::uv_rep},
      {"highlight", &PbrClassParams::spec},
      {"gloss", &PbrClassParams::gloss},
      {"albedo_tint", &PbrClassParams::albedo},
      {"env", &PbrClassParams::env},
      {"rough_bias", &PbrClassParams::rough_bias},
      {"lobe", &PbrClassParams::lobe},
      {"sheen", &PbrClassParams::sheen},
      {"transmit", &PbrClassParams::transmit},
      {"transmit_power", &PbrClassParams::transmit_power},
      {"wrap", &PbrClassParams::wrap},
      {"bake_wrap", &PbrClassParams::bake_wrap},
      {"glint", &PbrClassParams::glint}};
  for (auto& [name, member] : kClass) {
    if (key == name) {
      p.*member = f;
      return true;
    }
  }
  if (key == "tint" && nums.size() == 3) {
    std::copy(nums.begin(), nums.end(), p.tint);
    return true;
  }
  return false;
}

}  // namespace

void pbr_load_preset() {
  // built-in values, restored before every load so a look's overrides never outlive it
  static const auto kBuiltinClasses = [] {
    std::array<PbrClassParams, PBR_CLASS_COUNT> a;
    std::copy(std::begin(g_pbr_params), std::end(g_pbr_params), a.begin());
    return a;
  }();
  static const GfxGlobalSettings kBuiltinSettings = Gfx::g_global_settings;
  static bool first = true;
  auto& gs = Gfx::g_global_settings;

  const auto path = preset_path();
  if (!fs::exists(path)) {
    lg::warn("materials: preset {} not found, built-in defaults", path.string());
    return;
  }
  std::string text;
  try {
    text = file_util::read_text_file(path);
  } catch (const std::exception& e) {
    lg::warn("materials: preset {} unreadable, {}", path.string(), e.what());
    return;
  }
  std::vector<PresetEntry> entries;
  std::string section;
  int line_no = 0;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t end = text.find('\n', pos);
    end = end == std::string::npos ? text.size() : end;
    std::string line = text.substr(pos, end - pos);
    pos = end + 1;
    line_no++;
    const auto hash = line.find('#');
    line = trim(hash == std::string::npos ? line : line.substr(0, hash));
    if (line.empty()) {
      continue;
    }
    if (line.front() == '[') {
      section = trim(line.substr(1, line.find(']') - 1));
      continue;
    }
    const auto eq = line.find('=');
    if (eq == std::string::npos) {
      lg::warn("materials: preset line {} ignored", line_no);
      continue;
    }
    entries.push_back({section, trim(line.substr(0, eq)), trim(line.substr(eq + 1)), line_no});
    if (first && section == "global" && entries.back().key == "look") {
      for (int i = 0; i < kLookCount; i++) {
        if (entries.back().val.find(kLookNames[i]) != std::string::npos) {
          gs.pbr_look = i;
        }
      }
    }
  }
  first = false;

  std::copy(kBuiltinClasses.begin(), kBuiltinClasses.end(), g_pbr_params);
  gs.pbr_strength = kBuiltinSettings.pbr_strength;
  gs.pbr_detail = kBuiltinSettings.pbr_detail;
  gs.pbr_spec = kBuiltinSettings.pbr_spec;
  gs.pbr_key = kBuiltinSettings.pbr_key;
  gs.pbr_metallic = kBuiltinSettings.pbr_metallic;
  gs.pbr_env = kBuiltinSettings.pbr_env;
  gs.pbr_fresnel = kBuiltinSettings.pbr_fresnel;
  gs.pbr_ambient = kBuiltinSettings.pbr_ambient;
  gs.pbr_rough_contrast = kBuiltinSettings.pbr_rough_contrast;
  gs.pbr_band = kBuiltinSettings.pbr_band;
  gs.pbr_lobe = kBuiltinSettings.pbr_lobe;
  gs.pbr_metal_darken = kBuiltinSettings.pbr_metal_darken;
  gs.pbr_specular = kBuiltinSettings.pbr_specular;
  gs.pbr_metal_hue = kBuiltinSettings.pbr_metal_hue;
  gs.pbr_transmit = kBuiltinSettings.pbr_transmit;
  gs.pbr_wrap = kBuiltinSettings.pbr_wrap;
  gs.pbr_bake_wrap = kBuiltinSettings.pbr_bake_wrap;
  gs.pbr_wind_tie = kBuiltinSettings.pbr_wind_tie;
  gs.pbr_transmit_occl = kBuiltinSettings.pbr_transmit_occl;
  gs.pbr_transmit_density = kBuiltinSettings.pbr_transmit_density;
  gs.pbr_ice = kBuiltinSettings.pbr_ice;
  gs.pbr_ice_depth_m = kBuiltinSettings.pbr_ice_depth_m;
  gs.pbr_ice_absorb_m = kBuiltinSettings.pbr_ice_absorb_m;
  gs.pbr_ice_rough = kBuiltinSettings.pbr_ice_rough;
  gs.pbr_ice_transmit = kBuiltinSettings.pbr_ice_transmit;
  gs.pbr_glint = kBuiltinSettings.pbr_glint;
  gs.pbr_glint_cell = kBuiltinSettings.pbr_glint_cell;
  gs.pbr_glint_density = kBuiltinSettings.pbr_glint_density;
  gs.pbr_glint_spread = kBuiltinSettings.pbr_glint_spread;
  gs.pbr_glint_width = kBuiltinSettings.pbr_glint_width;
  gs.pbr_parallax = kBuiltinSettings.pbr_parallax;
  gs.pbr_px_depth = kBuiltinSettings.pbr_px_depth;
  gs.pbr_px_taps = kBuiltinSettings.pbr_px_taps;
  std::copy_n(kBuiltinSettings.pbr_px_grazing, 2, gs.pbr_px_grazing);
  std::copy_n(kBuiltinSettings.pbr_px_fade, 2, gs.pbr_px_fade);
  gs.pbr_px_fade_ref = kBuiltinSettings.pbr_px_fade_ref;
  gs.pbr_px_basis = kBuiltinSettings.pbr_px_basis;

  const int look = std::clamp(gs.pbr_look, 0, kLookCount - 1);
  const std::string prefix = fmt::format("look.{}", kLookNames[look]);
  int applied = 0;
  // base sections first, then the active look's, whatever their order in the file
  for (int pass = 0; pass < 2; pass++) {
    for (auto& e : entries) {
      std::string sec = e.section;
      if (sec.rfind("look.", 0) == 0) {
        if (pass == 0 || !(sec == prefix || sec.rfind(prefix + ".", 0) == 0)) {
          continue;
        }
        sec = sec == prefix ? "global" : sec.substr(prefix.size() + 1);
      } else if (pass == 1) {
        continue;
      }
      if (apply_preset_value(sec, e.key, e.val)) {
        applied++;
      } else {
        lg::warn("materials: preset line {} ([{}] {}) not understood", e.line, e.section, e.key);
      }
    }
  }
  lg::info("materials: preset {} look {} applied {} values", path.string(), kLookNames[look],
           applied);
}

void pbr_draw_debug_window() {
  auto& gs = Gfx::g_global_settings;
  ImGui::Checkbox("Enable (Jak 1, needs materials.csv)", &gs.pbr_enable);
  const char* sources[] = {"procedural (old)", "per-texture maps"};
  ImGui::Combo("Detail source", &gs.pbr_source, sources, 2);
  ImGui::SliderFloat("Strength", &gs.pbr_strength, 0.f, 3.f);
  ImGui::SliderFloat("Detail scale", &gs.pbr_detail, 0.f, 3.f);
  ImGui::SliderFloat("Highlight scale", &gs.pbr_spec, 0.f, 3.f);
  ImGui::SliderFloat("Key light (no sun)", &gs.pbr_key, 0.f, 2.f);
  const char* looks[] = {"base", "faithful", "bold", "cinematic"};
  if (ImGui::Combo("Look", &gs.pbr_look, looks, 4)) {
    pbr_load_preset();
  }
  ImGui::SliderFloat("Metallic workflow", &gs.pbr_metallic, 0.f, 1.f);
  ImGui::SliderFloat("Environment", &gs.pbr_env, 0.f, 2.f);
  ImGui::SliderFloat("Fresnel", &gs.pbr_fresnel, 0.f, 3.f);
  ImGui::SliderFloat("Ambient response", &gs.pbr_ambient, 0.f, 2.f);
  ImGui::SliderFloat("Roughness contrast", &gs.pbr_rough_contrast, 0.f, 3.f);
  ImGui::SliderFloat("Band (0 GGX, 1 stylized)", &gs.pbr_band, 0.f, 1.f);
  ImGui::SliderFloat("Lobe", &gs.pbr_lobe, 0.f, 3.f);
  ImGui::SliderFloat("Metal darkening", &gs.pbr_metal_darken, 0.f, 1.f);
  ImGui::SliderFloat("Dielectric specular", &gs.pbr_specular, 0.f, 4.f);
  ImGui::SliderFloat("Metal bake hue (0: off)", &gs.pbr_metal_hue, 0.f, 1.f);
  ImGui::SliderFloat("Leaf transmission scale", &gs.pbr_transmit, 0.f, 3.f);
  ImGui::SliderFloat("Wrapped N.L scale", &gs.pbr_wrap, 0.f, 2.f);
  ImGui::SliderFloat("Bake sun wrap scale", &gs.pbr_bake_wrap, 0.f, 2.f);
  ImGui::Checkbox("Wind TIE class shading", &gs.pbr_wind_tie);
  ImGui::Checkbox("Backlight sun occlusion", &gs.pbr_transmit_occl);
  ImGui::SliderFloat("Leaf density", &gs.pbr_transmit_density, 0.f, 4.f);
  ImGui::SliderFloat("Ice (gloss, depth, transmission)", &gs.pbr_ice, 0.f, 3.f);
  ImGui::SliderFloat("Glint scale", &gs.pbr_glint, 0.f, 3.f);
  ImGui::SliderFloat("Glint cell (m)", &gs.pbr_glint_cell, 0.005f, 0.1f, "%.3f");
  ImGui::SliderFloat("Glint density", &gs.pbr_glint_density, 0.f, 1.f);
  ImGui::SliderFloat("Glint facet spread (rad)", &gs.pbr_glint_spread, 0.05f, 0.8f);
  ImGui::SliderFloat("Glint lobe width (rad)", &gs.pbr_glint_width, 0.01f, 0.3f);
  ImGui::Checkbox("Parallax (masonry)", &gs.pbr_parallax);
  ImGui::SliderFloat("Parallax depth (m)", &gs.pbr_px_depth, 0.f, kPbrParallaxMaxDepth, "%.3f");
  ImGui::SliderInt("Parallax taps", &gs.pbr_px_taps, 1, 3);
  ImGui::SliderFloat2("Parallax grazing fade (n.v)", gs.pbr_px_grazing, 0.f, 1.f);
  ImGui::SliderFloat2("Parallax distance fade (m)", gs.pbr_px_fade, 0.f, 100.f);
  ImGui::SliderFloat("Parallax fade reference (m per repeat)", &gs.pbr_px_fade_ref, 0.25f, 8.f);
  ImGui::Checkbox("Parallax per-vertex basis", &gs.pbr_px_basis);
  ImGui::Checkbox("Background", &gs.pbr_background);
  ImGui::SameLine();
  ImGui::Checkbox("Characters", &gs.pbr_merc);
  const char* views[] = {"shaded",           "class colours",  "detail normal",
                         "reflection+lobes", "sun visibility", "roughness/metallic",
                         "mood hemisphere",  "parallax"};
  ImGui::Combo("View", &gs.pbr_view, views, 8);
  ImGui::Text("maps: %d textures (%d with parallax), %d levels, %.1f MB", g_map_stats.textures,
              g_map_stats.packed, g_map_stats.levels, g_map_stats.bytes / 1048576.0);
  if (ImGui::Button("Reload preset")) {
    pbr_load_preset();
  }
  for (int i = 1; i < PBR_CLASS_COUNT; i++) {
    auto& p = g_pbr_params[i];
    if (ImGui::TreeNode(kClassNames[i])) {
      ImGui::SliderFloat("normal map", &p.normal, 0.f, 2.f);
      ImGui::SliderFloat("procedural detail", &p.detail, 0.f, 2.f);
      ImGui::SliderFloat("tile (m)", &p.scale_m, 0.1f, 8.f);
      ImGui::SliderFloat("uv repeats", &p.uv_rep, 0.5f, 16.f);
      ImGui::SliderFloat("highlight", &p.spec, 0.f, 1.5f);
      ImGui::SliderFloat("gloss (no map)", &p.gloss, 1.f, 128.f);
      ImGui::ColorEdit3("tint", p.tint);
      ImGui::SliderFloat("albedo tint", &p.albedo, 0.f, 1.f);
      ImGui::SliderFloat("environment", &p.env, 0.f, 2.f);
      ImGui::SliderFloat("roughness bias", &p.rough_bias, -0.5f, 0.5f);
      ImGui::SliderFloat("lobe", &p.lobe, 0.f, 3.f);
      ImGui::SliderFloat("sheen", &p.sheen, 0.f, 2.f);
      ImGui::SliderFloat("transmission", &p.transmit, 0.f, 3.f);
      ImGui::SliderFloat("transmission lobe power", &p.transmit_power, 0.5f, 16.f);
      ImGui::SliderFloat("wrap", &p.wrap, 0.f, 1.f);
      ImGui::SliderFloat("bake wrap", &p.bake_wrap, 0.f, 1.f);
      ImGui::SliderFloat("glint", &p.glint, 0.f, 20.f);
      ImGui::TreePop();
    }
  }
}
