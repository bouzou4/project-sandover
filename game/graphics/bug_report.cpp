#include "game/graphics/bug_report.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <mutex>
#include <optional>
#include <regex>
#include <thread>
#include <utility>

#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "common/versions/versions.h"

#include "game/graphics/display.h"
#include "game/graphics/gfx.h"
#include "game/runtime.h"

#include "fmt/format.h"
#include "third-party/json.hpp"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#pragma comment(lib, "advapi32.lib")
#else
#include <sys/utsname.h>
#endif

namespace bug_report {
namespace {

using json = nlohmann::json;
constexpr int kSchema = 1;
constexpr int kLogLines = 200;
constexpr int kPreviewWidth = 1920;
constexpr auto kShotTimeout = std::chrono::seconds(10);

struct Job {
  std::string id;
  fs::path dir;
  fs::path png;
  Source source = KEY;
  json game;
  std::string profile;
  std::chrono::steady_clock::time_point started;
  bool delivered = false;
};

std::mutex g_mutex;
std::optional<Job> g_job;
std::string g_pending;
std::atomic<int> g_request{-1};
std::atomic<int> g_result{0};

std::string env(const char* name) {
  const char* v = std::getenv(name);
  return v ? v : "";
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}

void replace_ci(std::string& text, const std::string& from, const std::string& to, bool word) {
  if (from.empty()) {
    return;
  }
  const std::string needle = lower(from);
  std::string hay = lower(text);
  size_t pos = 0;
  while ((pos = hay.find(needle, pos)) != std::string::npos) {
    const size_t end = pos + needle.size();
    auto alnum = [&](size_t i) { return std::isalnum((unsigned char)hay[i]) != 0; };
    if (word && ((pos > 0 && alnum(pos - 1)) || (end < hay.size() && alnum(end)))) {
      pos = end;
      continue;
    }
    text.replace(pos, needle.size(), to);
    hay.replace(pos, needle.size(), to);
    pos += to.size();
  }
}

struct Rule {
  std::string from, to;
  bool word;
};

/// What sanitize() replaces: known folders, the home folder's own components, user and host.
std::vector<Rule> build_rules() {
  std::vector<Rule> paths, names;
  auto add = [&](std::string p, const char* label) {
    while (p.size() > 1 && (p.back() == '/' || p.back() == '\\')) {
      p.pop_back();
    }
    if (p.size() < 3) {
      return;
    }
    std::string fwd = p, back = p;
    std::replace(fwd.begin(), fwd.end(), '\\', '/');
    std::replace(back.begin(), back.end(), '/', '\\');
    paths.push_back({fwd, label, false});
    paths.push_back({back, label, false});
  };
  add(env("SANDOVER_RUN_DIR"), "<run>");
  add(env("SANDOVER_DATA"), "<data>");
  add(file_util::get_user_config_dir_override().string(), "<config>");
  add(fs::path(file_util::get_current_executable_path()).parent_path().string(), "<install>");
  add(file_util::get_jak_project_dir().string(), "<project>");
  add(env("APPDATA"), "<appdata>");
  add(env("LOCALAPPDATA"), "<localappdata>");
  add(env("TEMP"), "<temp>");
  add(env("TMP"), "<temp>");
  add(env("OneDriveCommercial"), "<onedrive>");
  add(env("OneDriveConsumer"), "<onedrive>");
  add(env("OneDrive"), "<onedrive>");
  add(env("USERPROFILE"), "<home>");
  add(env("HOME"), "<home>");
  std::stable_sort(paths.begin(), paths.end(),
                   [](const Rule& a, const Rule& b) { return a.from.size() > b.from.size(); });
  // the home folder's own components (Profiles, Org, jdoe.CORP) wherever they stand alone
  for (const char* var : {"USERPROFILE", "HOME"}) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : env(var) + "/") {
      if (c == '/' || c == '\\') {
        if (!cur.empty() && cur.find(':') == std::string::npos) {
          parts.push_back(cur);
        }
        cur.clear();
      } else {
        cur.push_back(c);
      }
    }
    for (size_t k = 0; k < parts.size(); k++) {
      const std::string l = lower(parts[k]);
      if (parts[k].size() >= 3 && l != "users" && l != "home" && l != "documents and settings") {
        names.push_back({parts[k], k + 1 == parts.size() ? "<user>" : "<profile>", true});
      }
    }
  }
  for (const char* var : {"USERNAME", "USER", "LOGNAME"}) {
    if (const std::string v = env(var); v.size() >= 2) {
      names.push_back({v, "<user>", true});
    }
  }
  for (const char* var : {"COMPUTERNAME", "HOSTNAME", "USERDOMAIN"}) {
    if (const std::string v = env(var); v.size() >= 2) {
      names.push_back({v, var[0] == 'U' ? "<domain>" : "<host>", true});
    }
  }
  std::stable_sort(names.begin(), names.end(),
                   [](const Rule& a, const Rule& b) { return a.from.size() > b.from.size(); });
  paths.insert(paths.end(), names.begin(), names.end());
  return paths;
}

const std::vector<Rule>& rules() {
  static const std::vector<Rule> r = build_rules();
  return r;
}

std::string sanitize_with(const std::string& text, const std::vector<Rule>& rs) {
  // tests/test_bug_report.py runs its sample set through these patterns: keep each a raw string.
  // A drive or UNC path: to the closing quote when quoted, else to a character no path has.
  static const std::regex kAbsPath(
      R"re("((?:[A-Za-z]:|\\\\[^\\/\s"']+)[\\/][^"\x01-\x1f]*)|'((?:[A-Za-z]:|\\\\[^\\/\s"']+)[\\/][^'\x01-\x1f]*)|(?:^|[^A-Za-z0-9_])((?:[A-Za-z]:|\\\\[^\\/\s"']+)[\\/][^"<>|*?\x01-\x1f]*))re");
  static const std::regex kUnixHome(R"re((^|[^A-Za-z0-9_.~-])/(home|Users)/[^/\s"'<>|*?]+)re");
  static const std::regex kOrgFolder(R"re((OneDrive|SharePoint) - [^\\/"<>|*?\x01-\x1f]+)re");
  std::string out = text;
  for (const auto& r : rs) {
    replace_ci(out, r.from, r.to, r.word);
  }
  std::string result;
  size_t last = 0;
  for (auto it = std::sregex_iterator(out.begin(), out.end(), kAbsPath);
       it != std::sregex_iterator(); ++it) {
    const auto& m = *it;
    const int g = m[1].matched ? 1 : m[2].matched ? 2 : 3;
    std::string p = m.str(g);
    while (!p.empty() && (p.back() == '/' || p.back() == '\\')) {
      p.pop_back();
    }
    const size_t cut = p.find_last_of("/\\");
    result += out.substr(last, m.position(g) - last);
    result += "<path>/" + (cut == std::string::npos ? std::string() : p.substr(cut + 1));
    last = m.position(g) + m.length(g);
  }
  result += out.substr(last);
  result = std::regex_replace(result, kUnixHome, "$1/$2/<user>");
  return std::regex_replace(result, kOrgFolder, "$1 - <org>");
}

json sanitize_json(const json& j, const std::vector<Rule>& rs) {
  if (j.is_string()) {
    return sanitize_with(j.get<std::string>(), rs);
  }
  if (j.is_array() || j.is_object()) {
    json out = j;
    for (auto& v : out) {
      v = sanitize_json(v, rs);
    }
    return out;
  }
  return j;
}

// ---- settings files (s-expressions) ----

struct Sexp {
  bool list = false;
  bool quoted = false;
  std::string atom;
  std::vector<Sexp> items;
};

void skip_space(const std::string& s, size_t& i) {
  while (i < s.size()) {
    if (std::isspace((unsigned char)s[i])) {
      i++;
    } else if (s[i] == ';') {
      while (i < s.size() && s[i] != '\n') {
        i++;
      }
    } else {
      break;
    }
  }
}

std::optional<Sexp> parse_sexp(const std::string& s, size_t& i, int depth = 0) {
  skip_space(s, i);
  if (i >= s.size() || s[i] == ')' || depth > 32) {
    return std::nullopt;
  }
  Sexp e;
  if (s[i] == '(') {
    e.list = true;
    i++;
    while (true) {
      skip_space(s, i);
      if (i >= s.size()) {
        return std::nullopt;
      }
      if (s[i] == ')') {
        i++;
        return e;
      }
      auto child = parse_sexp(s, i, depth + 1);
      if (!child) {
        return std::nullopt;
      }
      e.items.push_back(std::move(*child));
    }
  }
  if (s[i] == '"') {
    e.quoted = true;
    for (i++; i < s.size() && s[i] != '"'; i++) {
      if (s[i] == '\\' && i + 1 < s.size()) {
        i++;
      }
      e.atom.push_back(s[i]);
    }
    i++;
    return e;
  }
  while (i < s.size() && !std::isspace((unsigned char)s[i]) && s[i] != '(' && s[i] != ')') {
    e.atom.push_back(s[i++]);
  }
  return e;
}

std::string sexp_text(const Sexp& e) {
  if (!e.list) {
    return e.quoted ? fmt::format("\"{}\"", e.atom) : e.atom;
  }
  std::string out = "(";
  for (size_t k = 0; k < e.items.size(); k++) {
    out += (k ? " " : "") + sexp_text(e.items[k]);
  }
  return out + ")";
}

json sexp_value(const Sexp& e) {
  if (e.list) {
    return sexp_text(e);
  }
  if (e.quoted) {
    return e.atom;
  }
  if (e.atom == "#t") {
    return true;
  }
  if (e.atom == "#f") {
    return false;
  }
  const char* c = e.atom.c_str();
  char* end = nullptr;
  if (e.atom.size() > 2 && e.atom.rfind("#x", 0) == 0) {
    const long long v = std::strtoll(c + 2, &end, 16);
    if (*end == 0) {
      return v;
    }
  } else if (!e.atom.empty()) {
    const long long v = std::strtoll(c, &end, 10);
    if (*end == 0) {
      return v;
    }
    const double d = std::strtod(c, &end);
    if (*end == 0) {
      return d;
    }
  }
  return e.atom;
}

/// {"version": v, "values": {key: value}} of a settings file, or null if it cannot be read.
json settings_file(const fs::path& path) {
  std::string text;
  try {
    if (!fs::exists(path)) {
      return nullptr;
    }
    text = file_util::read_text_file(path);
  } catch (const std::exception&) {
    return nullptr;
  }
  size_t i = 0;
  auto root = parse_sexp(text, i);
  if (!root || !root->list || root->items.size() < 2) {
    return json{{"error", "unreadable"}};
  }
  json values = json::object();
  for (size_t k = 2; k < root->items.size(); k++) {
    const auto& item = root->items[k];
    if (!item.list || item.items.empty() || item.items[0].list) {
      continue;
    }
    if (item.items.size() == 2) {
      values[item.items[0].atom] = sexp_value(item.items[1]);
    } else {
      json arr = json::array();
      for (size_t m = 1; m < item.items.size(); m++) {
        arr.push_back(sexp_value(item.items[m]));
      }
      values[item.items[0].atom] = arr;
    }
  }
  return json{{"version", sexp_value(root->items[1])}, {"values", values}, {"text", text}};
}

// ---- machine and build ----

std::string os_version() {
#ifdef _WIN32
  OSVERSIONINFOW v{};
  v.dwOSVersionInfoSize = sizeof(v);
  using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
  if (auto fn = (RtlGetVersionFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")) {
    fn(&v);
  }
  char disp[64] = {0};
  DWORD size = sizeof(disp);
  RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
               "DisplayVersion", RRF_RT_REG_SZ, nullptr, disp, &size);
  DWORD ubr = 0;
  size = sizeof(ubr);
  RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "UBR",
               RRF_RT_REG_DWORD, nullptr, &ubr, &size);
  const int major = (v.dwMajorVersion == 10 && v.dwBuildNumber >= 22000) ? 11 : v.dwMajorVersion;
  return fmt::format("Windows {}{}{} ({}.{}.{}.{})", major, disp[0] ? " " : "", disp,
                     v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber, ubr);
#else
  utsname u{};
  if (uname(&u) != 0) {
    return "unknown";
  }
  return fmt::format("{} {} {}", u.sysname, u.release, u.machine);
#endif
}

json build_info() {
  const fs::path exe_dir = fs::path(file_util::get_current_executable_path()).parent_path();
  json info = json::object();
  try {
    if (fs::exists(exe_dir / "build.json")) {
      info = json::parse(file_util::read_text_file(exe_dir / "build.json"));
    }
  } catch (const std::exception&) {
    info = json::object();
  }
  auto str = [&](const char* k) -> json {
    return info.contains(k) && info[k].is_string() ? info[k] : json(nullptr);
  };
  json commit = str("engine_sha");
  if (commit.is_null()) {
    commit = str("head");
  }
  std::string id = "dev-" + build_revision();
  if (info.contains("bin") && info["bin"].is_string()) {
    id = info["bin"].get<std::string>();
  } else if (!info.empty()) {
    id = exe_dir.filename().string();
  }
  return {{"id", id},
          {"commit", commit.is_null() ? json(build_revision()) : commit},
          {"branch", str("branch")},
          {"built", str("gk_built")},
          {"compiled_revision", build_revision()}};
}

json display_info(int width, int height) {
  const auto& gs = Gfx::g_global_settings;
  const auto& hp = Gfx::g_hdr_present;
  json d = json::object();
  auto main = Display::GetMainDisplay();
  auto dm = main ? main->get_display_manager() : nullptr;
  if (dm) {
    static const char* kModes[] = {"windowed", "fullscreen", "borderless"};
    const int mode = (int)dm->get_display_mode();
    d["mode"] = (mode >= 0 && mode < 3) ? kModes[mode] : "unknown";
    d["window"] = {dm->get_window_width(), dm->get_window_height()};
    d["refresh_hz"] = dm->get_active_display_refresh_rate();
  }
  d["game_resolution"] = {gs.game_res_w, gs.game_res_h};
  d["screenshot"] = {width, height};
  d["render_scale"] = gs.render_scale;
  d["upscaler"] = Gfx::fsr3_on() ? "fsr3" : "none";
  d["msaa"] = gs.msaa_samples;
  d["aa_mode"] = gs.aa_mode;
  d["vsync"] = gs.vsync;
  d["hdr"] = {
      {"fp16_target", gs.hdr_target},
      {"output_requested", gs.hdr_output},
      {"format", gs.hdr_format == (int)HdrOutputFormat::SCRGB ? "scrgb" : "pq"},
      {"presenting", hp.active && hp.hdr},
      {"peak_nits", hp.peak_nits},
      {"paper_white_nits", hp.paper_white_nits},
      {"black_nits", hp.black_nits},
      {"display_peak_nits", hp.display_peak_nits},
      {"display_black_nits", hp.display_black_nits},
      {"desktop_white_nits", hp.desktop_white_nits},
  };
  return d;
}

fs::path reports_dir() {
  if (const std::string run = env("SANDOVER_RUN_DIR"); !run.empty()) {
    return fs::path(run) / "bug-reports";
  }
  if (const auto o = file_util::get_user_config_dir_override(); !o.empty()) {
    return o / "bug-reports";
  }
  return file_util::get_user_config_dir() / version_to_game_name(g_game_version) / "bug-reports";
}

std::tm local_now() {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  return tm;
}

std::string utc_now() {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

// ---- zip (stored entries) ----

u32 crc32(const std::string& data) {
  static const auto table = [] {
    std::array<u32, 256> t{};
    for (u32 n = 0; n < 256; n++) {
      u32 c = n;
      for (int k = 0; k < 8; k++) {
        c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      }
      t[n] = c;
    }
    return t;
  }();
  u32 c = 0xFFFFFFFFu;
  for (unsigned char b : data) {
    c = table[(c ^ b) & 0xFF] ^ (c >> 8);
  }
  return c ^ 0xFFFFFFFFu;
}

void put16(std::string& out, u32 v) {
  out.push_back(char(v & 0xFF));
  out.push_back(char((v >> 8) & 0xFF));
}

void put32(std::string& out, u32 v) {
  put16(out, v & 0xFFFF);
  put16(out, v >> 16);
}

/// Writes `files` (name in the zip, path on disk) uncompressed: the PNGs are compressed already.
bool write_zip(const fs::path& zip, const std::vector<std::pair<std::string, fs::path>>& files) {
  const std::tm tm = local_now();
  const u32 dos_time = (tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2);
  const u32 dos_date = ((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday;
  std::string body, central;
  for (const auto& [name, path] : files) {
    std::string data;
    try {
      auto bytes = file_util::read_binary_file(path);
      data.assign(bytes.begin(), bytes.end());
    } catch (const std::exception&) {
      return false;
    }
    const u32 crc = crc32(data), offset = (u32)body.size();
    auto header = [&](std::string& out, bool is_central) {
      put32(out, is_central ? 0x02014b50 : 0x04034b50);
      if (is_central) {
        put16(out, 20);
      }
      put16(out, 20);
      put16(out, 0x0800);  // UTF-8 names
      put16(out, 0);       // stored
      put16(out, dos_time);
      put16(out, dos_date);
      put32(out, crc);
      put32(out, (u32)data.size());
      put32(out, (u32)data.size());
      put16(out, (u32)name.size());
      put16(out, 0);
      if (is_central) {
        put16(out, 0);
        put16(out, 0);
        put16(out, 0);
        put32(out, 0);
        put32(out, offset);
      }
      out += name;
    };
    header(body, false);
    body += data;
    header(central, true);
  }
  std::string end;
  put32(end, 0x06054b50);
  put16(end, 0);
  put16(end, 0);
  put16(end, (u32)files.size());
  put16(end, (u32)files.size());
  put32(end, (u32)central.size());
  put32(end, (u32)body.size());
  put16(end, 0);
  const fs::path tmp = zip.string() + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary);
    f << body << central << end;
    if (!f) {
      return false;
    }
  }
  std::error_code ec;
  fs::rename(tmp, zip, ec);
  return !ec;
}

void write_text_atomic(const fs::path& path, const std::string& text) {
  const fs::path tmp = path.string() + ".tmp";
  file_util::write_text_file(tmp, text);
  fs::rename(tmp, path);
}

std::vector<u32> downscale(const std::vector<u32>& px, int w, int h, int f, int& ow, int& oh) {
  ow = w / f;
  oh = h / f;
  std::vector<u32> out((size_t)ow * oh);
  for (int y = 0; y < oh; y++) {
    for (int x = 0; x < ow; x++) {
      u32 sum[4] = {0, 0, 0, 0};
      for (int dy = 0; dy < f; dy++) {
        for (int dx = 0; dx < f; dx++) {
          const u32 p = px[(size_t)(y * f + dy) * w + (x * f + dx)];
          for (int c = 0; c < 4; c++) {
            sum[c] += (p >> (8 * c)) & 0xFF;
          }
        }
      }
      u32 v = 0;
      for (int c = 0; c < 4; c++) {
        v |= (sum[c] / (f * f)) << (8 * c);
      }
      out[(size_t)y * ow + x] = v;
    }
  }
  return out;
}

void finish(Job job, std::vector<u32> rgba, int w, int h, json report) {
  bool ok = true;
  std::vector<std::pair<std::string, fs::path>> files;
  const std::string top = "sandover-bug-" + job.id;
  try {
    file_util::write_rgba_png(job.png, rgba.data(), w, h);
    files.emplace_back(top + "/screenshot.png", job.png);
    if (w > kPreviewWidth) {
      int pw, ph;
      const int f = (w + kPreviewWidth - 1) / kPreviewWidth;
      auto small = downscale(rgba, w, h, f, pw, ph);
      file_util::write_rgba_png(job.dir / "preview.png", small.data(), pw, ph);
      report["files"]["preview"] = "preview.png";
    }
    write_text_atomic(job.dir / "report.json", report.dump(2) + "\n");
    files.emplace_back(top + "/report.json", job.dir / "report.json");
    ok = write_zip(job.dir.parent_path() / (top + ".zip"), files);
  } catch (const std::exception& e) {
    lg::error("bug report {}: {}", job.id, e.what());
    ok = false;
  }
  if (ok) {
    lg::info("bug report saved: {}", sanitize((job.dir.parent_path() / (top + ".zip")).string()));
  } else {
    lg::error("bug report {} could not be written to {}", job.id, sanitize(job.dir.string()));
  }
  {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_job.reset();
  }
  g_result |= ok ? 2 : 4;
}

}  // namespace

std::string sanitize(const std::string& text) {
  return sanitize_with(text, rules());
}

void request(Source source) {
  g_request = (int)source;
}

u64 poll() {
  u64 r = 0;
  const int source = g_request.exchange(-1);
  std::lock_guard<std::mutex> lk(g_mutex);
  if (g_job && !g_job->delivered &&
      std::chrono::steady_clock::now() - g_job->started > kShotTimeout) {
    lg::error("bug report {}: no frame was rendered for it", g_job->id);
    g_job.reset();
    g_pending.clear();
    g_result |= 4;
  }
  if (source >= 0) {
    if (g_job) {
      lg::warn("bug report: still writing {}, request ignored", g_job->id);
    } else {
      r |= 1;
      g_request = -1;
      Job job;
      job.source = (Source)source;
      job.started = std::chrono::steady_clock::now();
      g_job = std::move(job);
    }
  }
  return r | g_result.exchange(0);
}

void save(const GoalState& s, const std::string& level, const std::string& cont) {
  std::lock_guard<std::mutex> lk(g_mutex);
  if (!g_job || !g_job->id.empty()) {
    return;
  }
  const std::tm tm = local_now();
  char stamp[32];
  std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
  const fs::path base = reports_dir();
  std::string id = stamp;
  for (int k = 2; fs::exists(base / id) || fs::exists(base / ("sandover-bug-" + id + ".zip"));
       k++) {
    id = fmt::format("{}-{}", stamp, k);
  }
  std::error_code ec;
  fs::create_directories(base / id, ec);
  if (ec) {
    lg::error("bug report: cannot create {}: {}", sanitize((base / id).string()), ec.message());
    g_job.reset();
    g_result |= 4;
    return;
  }
  auto vec = [&](int first, int n) {
    json a = json::array();
    for (int k = 0; k < n; k++) {
      a.push_back(s.data[first + k]);
    }
    return a;
  };
  auto name = [](const std::string& v) {
    return v == "none" || v.empty() ? json(nullptr) : json(v);
  };
  g_job->id = id;
  g_job->dir = base / id;
  g_job->png = base / id / "screenshot.png";
  g_job->started = std::chrono::steady_clock::now();
  g_job->game = {
      {"level", name(level)},
      {"continue", name(cont)},
      {"jak_pos", vec(0, 3)},
      {"jak_quat", vec(3, 4)},
      {"cam_pos", vec(7, 3)},
      {"cam_rows", {vec(10, 3), vec(13, 3), vec(16, 3)}},
      {"fov", s.data[19] * 360.0 / 65536.0},
      {"hour", s.data[20]},
      {"mood_weights", vec(21, 8)},
  };
  g_job->profile = s.data[29] > 0.5f ? "vr" : "flat";
  g_pending = g_job->png.string();
  lg::info("bug report {} requested ({})", id, level);
}

std::string take_pending_path() {
  std::lock_guard<std::mutex> lk(g_mutex);
  return std::exchange(g_pending, {});
}

bool owns(const std::string& path) {
  std::lock_guard<std::mutex> lk(g_mutex);
  return g_job && !g_job->delivered && !g_job->id.empty() && g_job->png.string() == path;
}

void deliver(std::vector<u32>&& rgba, int width, int height, const GpuInfo& gpu) {
  Job job;
  {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g_job || g_job->delivered) {
      return;
    }
    g_job->delivered = true;
    job = *g_job;
  }
  static const char* kSources[] = {"key", "menu", "repl"};
  const auto settings_dir = file_util::get_user_settings_dir(g_game_version);
  // GOAL writes the settings in effect to sandover-report.gc just before save
  json sandover = settings_file(settings_dir / "sandover-report.gc");
  std::error_code ec;
  if (sandover.is_object() && !sandover.contains("error")) {
    sandover["source"] = "live";
    fs::remove(settings_dir / "sandover-report.gc", ec);
  } else {
    sandover = settings_file(settings_dir / "sandover.gc");
    if (sandover.is_object()) {
      sandover["source"] = "file";
    }
  }
  if (sandover.is_object()) {
    sandover["profile"] = job.profile;
  }
  json pc = settings_file(settings_dir / "pc-settings.gc");
  if (pc.is_object()) {
    pc.erase("text");
  }
  json report = {
      {"schema", "sandover-bug-report"},
      {"schema_version", kSchema},
      {"id", job.id},
      {"source", kSources[std::clamp((int)job.source, 0, 2)]},
      {"index", 1},
      {"time_utc", utc_now()},
  };
  report.update(job.game);
  report["toggles"] = json::array();
  report["hdr"] = false;
  report["status"] = "kept";
  report["note"] = nullptr;
  report["tags"] = json::array();
  report["annotations"] = json::array();
  report["build"] = build_info();
  report["build_sha"] = report["build"]["commit"];
  report["system"] = {
      {"os", os_version()},
      {"cpu_threads", std::thread::hardware_concurrency()},
      {"gpu", {{"vendor", gpu.vendor}, {"renderer", gpu.renderer}, {"driver", gpu.version}}},
  };
  report["display"] = display_info(width, height);
  report["settings"] = {{"sandover", sandover}, {"pc", pc}};
  report["files"] = {{"screenshot", "screenshot.png"}};
  report["log_tail"] = lg::recent_lines(kLogLines);
  report = sanitize_json(report, rules());
  std::thread(finish, std::move(job), std::move(rgba), width, height, std::move(report)).detach();
}

}  // namespace bug_report
