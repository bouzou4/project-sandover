#include "EmissiveTable.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <unordered_map>
#include <vector>

#include "common/log/log.h"
#include "common/util/FileUtil.h"

namespace emissive_table {
namespace {

bool g_loaded = false;
std::string g_path;
std::unordered_map<u32, Entry> g_rows;

std::vector<std::string> split(const std::string& line) {
  std::vector<std::string> out(1);
  for (char c : line) {
    if (c == ',') {
      out.emplace_back();
    } else if (c != '\r' && c != '"') {
      out.back() += c;
    }
  }
  return out;
}

void load() {
  g_loaded = true;
  g_rows.clear();
  if (const char* p = std::getenv("SANDOVER_EMISSIVE_ACTORS"); p) {
    g_path = p;
  } else {
    const char* d = std::getenv("SANDOVER_DATA");
    const fs::path data =
        d && *d ? fs::path(d) : file_util::get_jak_project_dir() / "sandover" / "data";
    g_path = (data / "materials" / "emissive-actors.csv").string();
  }
  std::ifstream f(g_path);
  std::string line;
  if (!f || !std::getline(f, line)) {
    return;
  }
  const auto head = split(line);
  auto col = [&](const char* n) {
    for (size_t i = 0; i < head.size(); i++) {
      if (head[i] == n) {
        return (int)i;
      }
    }
    return -1;
  };
  const int c_id = col("combo_id"), c_e = col("e"), c_chroma = col("min_chroma"),
            c_lights = col("lights");
  if (c_id < 0 || c_e < 0) {
    lg::error("emissive table: {} lacks combo_id or e", g_path);
    return;
  }
  while (std::getline(f, line)) {
    const auto row = split(line);
    if ((int)row.size() <= std::max(c_id, c_e) || row[c_id].empty()) {
      continue;
    }
    Entry e;
    e.e = std::strtof(row[c_e].c_str(), nullptr);
    if (c_chroma >= 0 && c_chroma < (int)row.size()) {
      e.min_chroma = std::strtof(row[c_chroma].c_str(), nullptr);
    }
    if (c_lights >= 0 && c_lights < (int)row.size() && !row[c_lights].empty()) {
      e.lights = std::clamp(std::strtof(row[c_lights].c_str(), nullptr), 0.f, 1.f);
    }
    if (e.e > 0.f) {
      g_rows[(u32)std::strtoul(row[c_id].c_str(), nullptr, 0)] = e;
    }
  }
  lg::info("emissive table: {} textures from {}", g_rows.size(), g_path);
}
}  // namespace

bool active() {
  if (!g_loaded) {
    load();
  }
  return !g_rows.empty();
}

const Entry* find(u32 combo_id) {
  auto it = g_rows.find(combo_id);
  return it == g_rows.end() ? nullptr : &it->second;
}

void reload() {
  load();
}

int size() {
  return (int)g_rows.size();
}

const std::string& path() {
  return g_path;
}

}  // namespace emissive_table
