#include "Neighbours.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "common/log/log.h"

namespace neighbours {
namespace {

struct MoodRow {
  const char* level;
  int quick_slot;  // -1: update-mood-palette over `hours`
  float hours[24][4];
};
constexpr MoodRow kMoodRows[] = {
#include "NeighboursMood.inc"
};

// kb/facts/vista.md adjacency (level-info continues + load-boundary pairs), title excluded.
constexpr std::array<std::pair<const char*, const char*>, 20> kAdjacency = {{
    {"citadel", "finalboss"},  {"firecanyon", "village1"}, {"jungle", "jungleb"},
    {"jungle", "village1"},    {"lavatube", "citadel"},    {"lavatube", "village3"},
    {"maincave", "darkcave"},  {"maincave", "robocave"},   {"maincave", "village3"},
    {"ogre", "village2"},      {"sunken", "sunkenb"},      {"sunken", "village2"},
    {"swamp", "village2"},     {"training", "village1"},   {"village1", "beach"},
    {"village1", "misty"},     {"village2", "firecanyon"}, {"village2", "rolling"},
    {"village3", "ogre"},      {"village3", "snow"},
}};

// Mirror of sandover/presets/vista.toml [outdoor] (test_presets checks).
constexpr const char* kOutdoor[] = {"training", "village1",   "beach",    "jungle",
                                    "misty",    "firecanyon", "village2", "rolling",
                                    "swamp",    "village3",   "snow"};

// Mirror of sandover/presets/vista.toml [[keep_out]] (test_presets checks): `level` is not drawn
// while the camera is inside `inside`'s geometry box (metres, kb/facts/vista.md detail box).
struct KeepOut {
  const char* level;
  const char* inside;
  float lo[3];
  float hi[3];
};
constexpr KeepOut kKeepOut[] = {
    {"firecanyon", "beach", {-514.73f, -19.71f, -606.67f}, {-12.86f, 152.13f, -86.02f}},
};

// Candidate stand-ins (standins.csv): hidden while all of `depicts` is drawn, if the census median
// distance puts them on real geometry. vil1-medres-grass stays: it is the ground skirt.
struct Standin {
  const char* host;
  const char* name;
  std::vector<const char*> depicts;
  float census_median_m;
};
const std::vector<Standin> kStandins = {
    {"village1", "v1-lores-trees", {"firecanyon"}, 72.2f},
    {"village1", "v1-lores-vil2-canvas", {"village2"}, 7.8f},
    {"village1", "v1-lores-vil2-wall", {"village2"}, 4.3f},
    {"village1", "v1-lores-rolling-precursor", {"rolling"}, 4.1f},  // max over its 3 parts
};

// Census gap: true-position stand-ins 0.4-10.2 m, the next row 18.7 m (a ground skirt).
constexpr float kStandinOnGeometryM = 15.f;

bool depicts_drawn_geometry(const Standin& s) {
  return s.census_median_m < kStandinOnGeometryM;
}

std::mutex g_mutex;
std::vector<std::string> g_game_levels, g_neighbour_levels;
std::string g_viewer;

bool contains(const std::vector<std::string>& v, const std::string& s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

}  // namespace

bool is_outdoor(const std::string& level) {
  return std::any_of(std::begin(kOutdoor), std::end(kOutdoor),
                     [&](const char* l) { return level == l; });
}

bool kept_out(const std::string& level, const math::Vector3f& cam_m) {
  return std::any_of(std::begin(kKeepOut), std::end(kKeepOut), [&](const KeepOut& k) {
    if (level != k.level) {
      return false;
    }
    for (int i = 0; i < 3; i++) {
      if (cam_m[i] < k.lo[i] || cam_m[i] > k.hi[i]) {
        return false;
      }
    }
    return true;
  });
}

std::vector<std::string> adjacent_to(const std::vector<std::string>& loaded) {
  std::vector<std::string> out;
  for (const auto& lev : loaded) {
    for (const auto& [a, b] : kAdjacency) {
      const char* other = lev == a ? b : (lev == b ? a : nullptr);
      if (other && is_outdoor(other) && !contains(loaded, other) && !contains(out, other)) {
        out.push_back(other);
      }
    }
  }
  return out;
}

std::vector<std::string> standin_levels(const std::vector<std::string>& loaded,
                                        const std::vector<std::string>& exclude) {
  std::vector<std::string> out;
  for (const auto& s : kStandins) {
    if (!contains(loaded, s.host) || !depicts_drawn_geometry(s)) {
      continue;
    }
    for (const char* d : s.depicts) {
      if (is_outdoor(d) && !contains(loaded, d) && !contains(exclude, d) && !contains(out, d)) {
        out.push_back(d);
      }
    }
  }
  return out;
}

void set_viewer(const std::string& level) {
  std::lock_guard<std::mutex> lk(g_mutex);
  g_viewer = level;
}

std::string viewer() {
  std::lock_guard<std::mutex> lk(g_mutex);
  return g_viewer;
}

void set_resident(const std::vector<std::string>& game_levels,
                  const std::vector<std::string>& neighbour_levels) {
  std::lock_guard<std::mutex> lk(g_mutex);
  g_game_levels = game_levels;
  g_neighbour_levels = neighbour_levels;
}

void get_resident(std::vector<std::string>* game_levels,
                  std::vector<std::string>* neighbour_levels) {
  std::lock_guard<std::mutex> lk(g_mutex);
  *game_levels = g_game_levels;
  *neighbour_levels = g_neighbour_levels;
}

std::string standin_filter(const std::vector<std::string>& viewer_levels,
                           const std::vector<std::string>& drawn_neighbours) {
  std::string out;
  for (const auto& s : kStandins) {
    if (!contains(viewer_levels, s.host) || !depicts_drawn_geometry(s)) {
      continue;
    }
    bool all = std::all_of(s.depicts.begin(), s.depicts.end(), [&](const char* d) {
      return contains(drawn_neighbours, d) || contains(viewer_levels, d);
    });
    if (all) {
      if (!out.empty()) {
        out += ',';
      }
      out += s.name;
    }
  }
  return out;
}

bool is_medres_of(const char* model_name, const std::vector<std::string>& levels) {
  constexpr const char* kPrefix = "medres-";
  constexpr size_t kLen = 7;
  if (strncmp(model_name, kPrefix, kLen) != 0) {
    return false;
  }
  const char* rest = model_name + kLen;
  for (const auto& lev : levels) {
    if (strncmp(rest, lev.c_str(), lev.size()) == 0) {
      const char c = rest[lev.size()];
      if (c == 0 || c == '-' || (c >= '0' && c <= '9')) {
        return true;
      }
    }
  }
  return false;
}

void pack_itimes(const float w[8], math::Vector<s32, 4> out[4]) {
  for (int q = 0; q < 4; q++) {
    for (int k = 0; k < 2; k++) {
      const u32 v = ((u32)((s32)(w[2 * q + k] * 4096.f) >> 6)) & 0xffff;
      out[q][2 * k] = (s32)(v | (v << 16));
      out[q][2 * k + 1] = (s32)(v | (v << 16));
    }
  }
}

int itimes_diff(const math::Vector<s32, 4> a[4], const math::Vector<s32, 4> b[4]) {
  int worst = 0;
  for (int q = 0; q < 4; q++) {
    for (int k = 0; k < 4; k++) {
      for (int h = 0; h < 2; h++) {
        const int va = ((u32)a[q][k] >> (16 * h)) & 0xff;
        const int vb = ((u32)b[q][k] >> (16 * h)) & 0xff;
        worst = std::max(worst, std::abs(va - vb));
      }
    }
  }
  return worst;
}

const MoodTable::Level& MoodTable::get(const std::string& level) {
  auto it = m_levels.find(level);
  if (it != m_levels.end()) {
    return it->second;
  }
  Level& lev = m_levels[level];
  for (const auto& row : kMoodRows) {
    if (level == row.level) {
      lev.quick_slot = row.quick_slot;
      for (const auto& h : row.hours) {
        lev.hours.push_back({(int)h[0], (int)h[1], h[2], h[3]});
      }
      lev.ok = true;
      return lev;
    }
  }
  lg::warn("neighbours: no mood table for {}", level);
  return lev;
}

bool MoodTable::itimes_at(const std::string& level, float hour, math::Vector<s32, 4> out[4]) {
  const Level& lev = get(level);
  if (!lev.ok) {
    return false;
  }
  float w[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  if (lev.quick_slot >= 0) {
    w[lev.quick_slot & 7] = 1.f;
  } else {
    // update-mood-palette
    const int h = (int)hour;
    const float f = hour - (float)h;
    const Entry& e = lev.hours[((h % 24) + 24) % 24];
    const float morph = (1.f - f) * e.morph_start + f * e.morph_end;
    if (e.s1 == e.s2) {
      w[e.s1 & 7] = 1.f;
    } else {
      const int v = (int)(0.5f + 64.f * morph);
      w[e.s1 & 7] = 0.015625f * (float)(64 - v);
      w[e.s2 & 7] = 0.015625f * (float)v;
    }
  }
  pack_itimes(w, out);
  return true;
}

bool MoodTable::slot_hours(const std::string& level, float out[8]) {
  const Level& lev = get(level);
  if (!lev.ok || lev.quick_slot >= 0 || lev.hours.size() < 24) {
    return false;
  }
  // circular mean of the peak hours, so a plateau across midnight (22-1 h) lands at 23.5, not 11.5
  constexpr float kTau = 6.2831853f;
  float best[8], cs[8], sn[8], n[8];
  for (int s = 0; s < 8; s++) {
    best[s] = cs[s] = sn[s] = n[s] = 0.f;
  }
  constexpr int kSteps = 16;  // samples per hour, unquantized weights
  for (int i = 0; i < 24 * kSteps; i++) {
    const float hour = (float)i / kSteps;
    const Entry& e = lev.hours[i / kSteps];
    const float f = hour - (float)(i / kSteps);
    const float morph = (1.f - f) * e.morph_start + f * e.morph_end;
    float w[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    w[e.s1 & 7] += e.s1 == e.s2 ? 1.f : 1.f - morph;
    if (e.s1 != e.s2) {
      w[e.s2 & 7] += morph;
    }
    const float c = std::cos(hour / 24.f * kTau), sv = std::sin(hour / 24.f * kTau);
    for (int s = 0; s < 8; s++) {
      if (w[s] > best[s] + 1e-4f) {
        best[s] = w[s];
        cs[s] = c;
        sn[s] = sv;
        n[s] = 1.f;
      } else if (w[s] > 0.f && w[s] > best[s] - 1e-4f) {
        cs[s] += c;
        sn[s] += sv;
        n[s] += 1.f;
      }
    }
  }
  for (int s = 0; s < 8; s++) {
    out[s] = n[s] > 0.f ? std::fmod(std::atan2(sn[s], cs[s]) / kTau * 24.f + 24.f, 24.f) : -1.f;
  }
  return true;
}

}  // namespace neighbours
