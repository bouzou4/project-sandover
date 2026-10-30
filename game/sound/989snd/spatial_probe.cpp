// SPDX-License-Identifier: ISC
// Spatial audio test scene: noise at fixed positions around a fixed listener, rendered through
// SpatialOutput on an OpenAL Soft loopback device; one WAV per layout plus a level table.
//   spatial_probe <out_dir>            test scene + cost table
//   spatial_probe --endpoint           Windows Spatial Sound state of the default endpoint
//   spatial_probe --device <mode>      opens the real device in <mode> for 1 s of silence
//   spatial_probe --list               Windows' endpoints (layout, spatial format) and OpenAL
//                                      Soft's device list
//   spatial_probe --saved <file>       the audio keys of a sandover.gc as gk reads them at start
//   spatial_probe --outputs [device]   what each Audio menu Output setting opens on a device
//   spatial_probe --play-test <output> [device]   the speaker test, audible, on the real device
//   spatial_probe --speaker-test <dir> the speaker test and bed on loopback, per-burst levels
//   spatial_probe --routing <dir>      each role (dialog, music, ambience, positional width)
//                                      per layout, before and after the surround routing
//   spatial_probe --census <SBK dir>   sound banks: volume groups, falloff, authored pan

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

#include "lfo.h"
#include "loader.h"
#include "sfxblock.h"
#include "spatial.h"

#include "common/log/log.h"

#define AL_ALEXT_PROTOTYPES
#include "AL/al.h"
#include "AL/alc.h"
#include "AL/alext.h"

#include "third-party/openal-soft/alc/inprogext.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <initguid.h>
#include <mmdeviceapi.h>
#include <spatialaudioclient.h>
#include <windows.h>
#endif

using namespace snd;

namespace {

constexpr int kRate = SpatialOutput::kRate;
constexpr float kDist = 4.f * 256.f;  // 4 m in overlord sound units

struct Noise {
  u32 state{0x12345678};
  s16 Next() {
    state = state * 1664525u + 1013904223u;
    return static_cast<s16>(static_cast<s32>(state >> 16) - 32768) / 4;
  }
};

struct Placement {
  const char* name;
  float yaw;
  SpatialVec offset;  // from the listener, game units
};

// Vanilla pan convention: at yaw 0 the camera's left is +x, so it faces +z; yaw 90 faces +x.
const Placement kPlacements[] = {
    {"front", 0, {0, 0, kDist}},        {"left", 0, {kDist, 0, 0}},
    {"right", 0, {-kDist, 0, 0}},       {"behind", 0, {0, 0, -kDist}},
    {"above", 0, {0, kDist, 0}},        {"yaw90-front", 90, {kDist, 0, 0}},
    {"yaw90-left", 90, {0, 0, -kDist}},
};

const SpatialVec kListener{10000, 2000, -5000};

struct Layout {
  const char* name;
  SpatialMode mode;
  int channels;  // ALC_*_SOFT
  int count;
  u32 mask;
  std::vector<const char*> labels;
};

void WriteWav(const std::string& path, const std::vector<float>& data, int ch, u32 mask) {
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) {
    lg::error("cannot write {}", path);
    return;
  }
  auto w32 = [&](u32 v) { fwrite(&v, 4, 1, f); };
  auto w16 = [&](u16 v) { fwrite(&v, 2, 1, f); };
  const u32 bytes = static_cast<u32>(data.size() * 4);
  fwrite("RIFF", 1, 4, f);
  w32(4 + 8 + 40 + 8 + bytes);
  fwrite("WAVEfmt ", 1, 8, f);
  w32(40);
  w16(0xFFFE);
  w16(ch);
  w32(kRate);
  w32(kRate * ch * 4);
  w16(ch * 4);
  w16(32);
  w16(22);
  w16(32);
  w32(mask);
  // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
  const u8 guid[16] = {0x03, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71};
  fwrite(guid, 1, 16, f);
  fwrite("data", 1, 4, f);
  w32(bytes);
  fwrite(data.data(), 4, data.size(), f);
  fclose(f);
}

float Db(double rms) {
  return rms > 1e-9 ? static_cast<float>(20 * std::log10(rms)) : -200.f;
}

struct Scene {
  std::recursive_mutex lock;
  Noise noise;
  int sources{1};

  SpatialOutput::TickFn Tick() {
    return [this](SpatialOutput& out) {
      for (int i = 1; i <= sources; i++) {
        s16 s = noise.Next();
        out.Route(i, {s, s});
      }
      out.EndSample({0, 0}, 0x7fff);
    };
  }
};

std::vector<float> RenderFrames(SpatialOutput& out, int frames) {
  std::vector<float> buf(static_cast<size_t>(frames) * out.OutputChannels());
  const int block = 512;
  for (int done = 0; done < frames; done += block) {
    out.Render(buf.data() + static_cast<size_t>(done) * out.OutputChannels(),
               std::min(block, frames - done));
  }
  return buf;
}

// Binaural: lag (samples, + = right ear later) at the cross-correlation peak within 1 ms.
int InterauralLag(const float* d, int frames) {
  const int max_lag = kRate / 1000;
  double best = -1e30;
  int best_lag = 0;
  for (int lag = -max_lag; lag <= max_lag; lag++) {
    double acc = 0;
    for (int i = max_lag; i < frames - max_lag; i++) {
      acc += d[2 * i] * d[2 * (i + lag) + 1];
    }
    if (acc > best) {
      best = acc;
      best_lag = lag;
    }
  }
  return best_lag;
}

void RunScene(const std::string& out_dir, const Layout& lay, FILE* table) {
  Scene scene;
  auto out = SpatialOutput::OpenLoopback(lay.mode, lay.channels, scene.lock, scene.Tick());
  if (!out) {
    lg::error("{}: loopback open failed", lay.name);
    return;
  }
  lg::info("{}: {}", lay.name, out->Describe());
  fprintf(table, "\n### %s (%s)\n\n| position |", lay.name, out->Describe().c_str());
  for (auto* l : lay.labels) {
    fprintf(table, " %s |", l);
  }
  if (lay.mode == SpatialMode::Binaural) {
    fprintf(table, " ILD L-R dB | ITD us (+: right ear later) |");
  }
  fprintf(table, "\n|---|");
  for (size_t i = 0; i < lay.labels.size() + (lay.mode == SpatialMode::Binaural ? 2 : 0); i++) {
    fprintf(table, "---|");
  }
  fprintf(table, "\n");

  const int seg = kRate * 3 / 4;
  const int gap = kRate / 4;
  const int skip = kRate / 20;
  std::vector<float> all;
  for (auto& p : kPlacements) {
    {
      std::scoped_lock l(scene.lock);
      out->SetListener(kListener, p.yaw, false);
      out->Claim(1, {kListener.x + p.offset.x, kListener.y + p.offset.y, kListener.z + p.offset.z});
    }
    out->Flush();
    auto buf = RenderFrames(*out, seg);
    {
      std::scoped_lock l(scene.lock);
      out->Release(1);
    }
    auto silence = RenderFrames(*out, gap);
    all.insert(all.end(), buf.begin(), buf.end());
    all.insert(all.end(), silence.begin(), silence.end());

    const int ch = out->OutputChannels();
    fprintf(table, "| %s |", p.name);
    std::vector<double> rms(ch, 0);
    for (int i = skip; i < seg; i++) {
      for (int c = 0; c < ch; c++) {
        double v = buf[static_cast<size_t>(i) * ch + c];
        rms[c] += v * v;
      }
    }
    for (int c = 0; c < ch; c++) {
      rms[c] = std::sqrt(rms[c] / (seg - skip));
      fprintf(table, " %.1f |", Db(rms[c]));
    }
    if (lay.mode == SpatialMode::Binaural) {
      int lag = InterauralLag(buf.data() + static_cast<size_t>(skip) * 2, seg - skip);
      fprintf(table, " %+.1f | %+.0f |", Db(rms[0]) - Db(rms[1]), lag * 1e6 / kRate);
    }
    fprintf(table, "\n");
  }
  WriteWav(out_dir + "/" + lay.name + ".wav", all, out->OutputChannels(), lay.mask);
}

// Wall time to render 10 s with `sources` moving sources, as a share of real time.
double Cost(const Layout& lay, int sources) {
  Scene scene;
  scene.sources = sources;
  auto out = SpatialOutput::OpenLoopback(lay.mode, lay.channels, scene.lock, scene.Tick());
  if (!out) {
    return -1;
  }
  for (int i = 1; i <= sources; i++) {
    std::scoped_lock l(scene.lock);
    out->Claim(i, kListener);
  }
  std::vector<float> buf(static_cast<size_t>(800) * out->OutputChannels());
  const int frames_total = kRate * 10;
  auto t0 = std::chrono::steady_clock::now();
  for (int done = 0, frame = 0; done < frames_total; done += 800, frame++) {
    {
      std::scoped_lock l(scene.lock);
      out->SetListener(kListener, static_cast<float>(frame % 360), false);
      for (int i = 1; i <= sources; i++) {
        float a = (frame + i * 37) * 0.05f;
        out->SetPosition(i, {kListener.x + kDist * std::cos(a), kListener.y + 100.f * i,
                             kListener.z + kDist * std::sin(a)});
      }
    }
    out->Flush();
    out->Render(buf.data(), 800);
  }
  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  return secs / 10.0;
}

#ifdef _WIN32
int Endpoint() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  IMMDeviceEnumerator* en = nullptr;
  IMMDevice* dev = nullptr;
  ISpatialAudioClient* sac = nullptr;
  if (FAILED(
          CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) ||
      FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) {
    printf("no default render endpoint\n");
    return 1;
  }
  HRESULT hr = dev->Activate(__uuidof(ISpatialAudioClient), CLSCTX_INPROC_SERVER, nullptr,
                             reinterpret_cast<void**>(&sac));
  if (FAILED(hr)) {
    printf("ISpatialAudioClient: not available (0x%08lx)\n", hr);
    return 0;
  }
  UINT32 objects = 0;
  sac->GetMaxDynamicObjectCount(&objects);
  AudioObjectType mask{};
  sac->GetNativeStaticObjectTypeMask(&mask);
  printf("ISpatialAudioClient: max dynamic objects %u, native static mask 0x%x\n", objects,
         static_cast<unsigned>(mask));
  printf("spatial sound format %s\n",
         objects > 0 ? "enabled (Windows Sonic, Dolby Atmos or DTS)" : "off for this endpoint");
  sac->Release();
  dev->Release();
  en->Release();
  return 0;
}
#endif

int Device(const std::string& name) {
  auto mode = SpatialModeFromName(name);
  if (!mode || *mode == SpatialMode::Off) {
    printf("mode: binaural, surround or objects\n");
    return 1;
  }
  std::recursive_mutex lock;
  auto out =
      SpatialOutput::OpenDevice(*mode, lock, [](SpatialOutput& o) { o.EndSample({0, 0}, 0x7fff); });
  if (!out) {
    printf("open failed\n");
    return 1;
  }
  printf("%s\n", out->Describe().c_str());
  Sleep(1000);
  return 0;
}

// Windows' render endpoints next to OpenAL Soft's device list.
std::string SpatialSummary(const EndpointInfo& ep) {
  return fmt::format("spatial format {} ('{}'), dynamic objects {}, native static mask 0x{:x}{}",
                     SpatialFormatName(ep.spatial_format), ep.spatial_format_id, ep.spatial_objects,
                     ep.spatial_static_mask,
                     ep.spatial_objects > 0 && SpatialMaskHasHeights(ep.spatial_static_mask)
                         ? " (7.1.4 heights)"
                         : "");
}

int List() {
  auto def = QueryEndpoint("");
  printf("Windows default render device (multimedia): '%s', %s (%d ch, mask 0x%x); %s\n",
         def.name.c_str(), EndpointLayoutName(def.channels, def.mask).c_str(), def.channels,
         def.mask, SpatialSummary(def).c_str());
  auto devices = ListOutputDevices();
  printf("OpenAL Soft devices (%s): %zu\n",
         alcIsExtensionPresent(nullptr, "ALC_ENUMERATE_ALL_EXT") ? "ALC_ENUMERATE_ALL_EXT"
                                                                 : "ALC_DEVICE_SPECIFIER",
         devices.size());
  for (auto& d : devices) {
    auto ep = QueryEndpoint(d);
    printf("  '%s': Windows reports %s (%d ch, mask 0x%x)%s; %s\n", d.c_str(),
           ep.found ? EndpointLayoutName(ep.channels, ep.mask).c_str() : "nothing", ep.channels,
           ep.mask, ep.is_default ? ", default" : "", SpatialSummary(ep).c_str());
  }
  return 0;
}

// Opens the real device the way each Output setting does (Player::SetAudioOutput) and reports
// what OpenAL Soft ended up with; 0.3 s of silence each.
int Outputs(const std::string& device) {
  std::recursive_mutex lock;
  auto tick = [](SpatialOutput& o) { o.EndSample({0, 0}, 0x7fff); };
  for (auto out : {AudioOutput::Auto, AudioOutput::Stereo, AudioOutput::X51, AudioOutput::X71,
                   AudioOutput::Headphones}) {
    auto plan = PlanOutput(out, device);
    printf("output %-10s -> ", AudioOutputName(out));
    if (plan.mode == SpatialMode::Off) {
      printf("vanilla stereo stream (cubeb, Windows default device)\n");
      continue;
    }
    auto o = SpatialOutput::OpenDevice(plan.mode, lock, tick, plan.layout, device);
    if (!o) {
      printf("open failed\n");
      continue;
    }
    if (plan.stereo_to_vanilla && o->OutputChannels() <= 2 && device.empty()) {
      printf("%s; stereo, so the vanilla stereo stream plays\n", o->Describe().c_str());
    } else {
      printf("%s\n", o->Describe().c_str());
    }
    fflush(stdout);
    Sleep(300);
  }
  return 0;
}

// The audio keys of a settings file, as main.cpp reads them before the sound system starts.
int Saved(const std::string& path) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) {
    printf("cannot read %s\n", path.c_str());
    return 1;
  }
  std::string text;
  char buf[4096];
  for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;) {
    text.append(buf, n);
  }
  fclose(f);
  const auto saved = ParseSavedAudio(text);
  printf("audio-output %s, audio-device '%s'\n", SavedOutputName(saved.output),
         saved.device.c_str());
  return 0;
}

// Plays the speaker test on the real device for one Output setting.
int PlayTest(const std::string& name, const std::string& device) {
  auto out = AudioOutputFromName(name);
  if (!out) {
    printf("output: auto, stereo, 5.1, 7.1, headphones or objects\n");
    return 1;
  }
  auto plan = PlanOutput(*out, device);
  if (plan.mode == SpatialMode::Off) {
    plan = {SpatialMode::Surround, SpatialLayout::Stereo, false};  // stands in for the stream
  }
  std::recursive_mutex lock;
  auto o = SpatialOutput::OpenDevice(
      plan.mode, lock, [](SpatialOutput& s) { s.EndSample({0, 0}, 0x7fff); }, plan.layout, device);
  if (!o) {
    printf("open failed\n");
    return 1;
  }
  printf("%s\n", o->Describe().c_str());
  o->StartSpeakerTest(o->Describe());
  while (o->Test().Current() >= 0) {
    Sleep(50);
  }
  Sleep(200);
  return 0;
}

// The speaker test and the vanilla bed through the loopback device: per-burst RMS per channel.
int SpeakerTestLevels(const std::string& out_dir) {
  struct Case {
    const char* name;
    SpatialMode mode;
    int channels;
    std::vector<const char*> labels;
  };
  const std::vector<Case> cases = {
      {"stereo-hrtf", SpatialMode::Binaural, ALC_STEREO_SOFT, {"L", "R"}},
      {"5.1", SpatialMode::Surround, ALC_5POINT1_SOFT, {"FL", "FR", "FC", "LFE", "SL", "SR"}},
      {"7.1",
       SpatialMode::Surround,
       ALC_7POINT1_SOFT,
       {"FL", "FR", "FC", "LFE", "BL", "BR", "SL", "SR"}},
  };
  FILE* table = fopen((out_dir + "/speaker-test.md").c_str(), "w");
  if (!table) {
    lg::error("cannot write to {}", out_dir);
    return 1;
  }
  fprintf(table,
          "# Speaker test through OpenAL Soft's loopback device\n\nPer-channel RMS (dBFS) over "
          "each burst's 0.7 s; the bed row is the vanilla stereo mix (noise on left and right).\n");
  for (auto& c : cases) {
    std::recursive_mutex lock;
    Noise noise;
    bool bed = false;
    auto out = SpatialOutput::OpenLoopback(c.mode, c.channels, lock, [&](SpatialOutput& o) {
      s16 v = bed ? noise.Next() : s16(0);
      o.EndSample({v, v}, 0x7fff);
    });
    if (!out) {
      lg::error("{}: loopback open failed", c.name);
      continue;
    }
    const int ch = out->OutputChannels();
    fprintf(table, "\n### %s (%s)\n\n| burst |", c.name, out->Describe().c_str());
    for (auto* l : c.labels) {
      fprintf(table, " %s |", l);
    }
    fprintf(table, "\n|---|");
    for (size_t i = 0; i < c.labels.size(); i++) {
      fprintf(table, "---|");
    }
    fprintf(table, "\n");
    auto row = [&](const std::string& label, const std::vector<float>& buf, int from, int to) {
      fprintf(table, "| %s |", label.c_str());
      for (int k = 0; k < ch; k++) {
        double acc = 0;
        for (int i = from; i < to; i++) {
          double v = buf[(size_t)i * ch + k];
          acc += v * v;
        }
        fprintf(table, " %.1f |", Db(std::sqrt(acc / std::max(1, to - from))));
      }
      fprintf(table, "\n");
    };
    out->StartSpeakerTest(std::string("loopback ") + c.name);
    const int period = SpeakerTest::kOn + SpeakerTest::kOff;
    const int n = out->Test().Count();
    auto buf = RenderFrames(*out, period * n + kRate / 10);
    std::vector<float> all = buf;
    for (int s = 0; s < n; s++) {
      row(fmt::format("{} {}", s + 1, out->Test().Labels()[s]), buf, s * period,
          s * period + SpeakerTest::kOn);
    }
    bed = true;
    auto bedbuf = RenderFrames(*out, kRate);
    row("bed (vanilla mix)", bedbuf, kRate / 10, kRate);
    all.insert(all.end(), bedbuf.begin(), bedbuf.end());
    const u32 mask = ch == 2 ? 0x3 : ch == 6 ? 0x60F : 0x63F;
    WriteWav(out_dir + "/speaker-test-" + c.name + ".wav", all, ch, mask);
  }
  fclose(table);
  printf("wrote %s/speaker-test.md\n", out_dir.c_str());
  return 0;
}

// The surround routing on loopback: each role's signal through the real router (RouteRole,
// Route, EndSample), before (routing off: the stereo bed and the mono positional sum) and after.
constexpr const char* kSpeakerNames[] = {"FL", "FR", "FC",  "LFE", "BL",  "BR", "SL",
                                         "SR", "BC", "TFL", "TFR", "TBL", "TBR"};

// The 989 pan law as the overlord sets it (jak1 ssound.cpp, vagvoice.cpp MakeVolume): pan 0
// ahead, 90 right, 270 left. Returns left and right gains.
std::pair<float, float> VanillaPan(int pan) {
  static std::array<std::pair<float, float>, 361> table = [] {
    std::array<std::pair<float, float>, 361> t{};
    for (int i = 0; i < 91; i++) {
      const float opposing_front = (float)(((i * 0x33ff) / 0x5a) + 0xc00);
      t[90 - i].first = 0x3fff;
      t[180 - i].first = opposing_front;
      t[i].second = opposing_front;
      t[90 + i].second = 0x3fff;
    }
    return t;
  }();
  int p = ((pan % 360) + 360) % 360;
  p = p >= 270 ? p - 270 : p + 90;
  if (p < 180) {
    return {table[p].first / 0x3fff, table[p].second / 0x3fff};
  }
  return {table[p - 180].second / 0x3fff, table[p - 180].first / 0x3fff};
}

struct RoutingLayout {
  const char* name;
  SpatialMode mode;
  int channels;  // ALC_*_SOFT
  int count;
  u32 mask;
};

struct Rendered {
  std::vector<float> buf;
  int ch{0};
  std::vector<Speaker> speakers;
  std::string routing;
};

constexpr int kRouteFrames = kRate * 6 / 5;
constexpr int kRouteSkip = kRate / 5;

double ChannelPower(const Rendered& r, int c) {
  double acc = 0;
  for (int i = kRouteSkip; i < kRouteFrames; i++) {
    const double v = r.buf[(size_t)i * r.ch + c];
    acc += v * v;
  }
  return acc / (kRouteFrames - kRouteSkip);
}

double TotalPower(const Rendered& r) {
  double p = 0;
  for (int c = 0; c < r.ch; c++) {
    p += ChannelPower(r, c);
  }
  return p;
}

// Lag (samples) of channel b behind channel a at the peak of the cross-correlation.
int LagBehind(const Rendered& r, int a, int b, int max_lag) {
  double best = -1e30;
  int best_lag = 0;
  for (int lag = 0; lag <= max_lag; lag++) {
    double acc = 0;
    for (int i = kRouteSkip; i + lag < kRouteFrames; i++) {
      acc += r.buf[(size_t)i * r.ch + a] * r.buf[(size_t)(i + lag) * r.ch + b];
    }
    if (acc > best) {
      best = acc;
      best_lag = lag;
    }
  }
  return best_lag;
}

// One scene: `voice` gives the voices of one sample as (role, owner, sample); owner != 0 is
// positional (claimed at `pos` when given).
using VoiceFn = std::function<void(std::vector<std::tuple<VoiceRole, u32, s16Output>>&)>;

Rendered RenderRoles(const RoutingLayout& lay,
                     bool routing,
                     const VoiceFn& voices,
                     const SpatialVec* pos) {
  std::recursive_mutex lock;
  std::vector<std::tuple<VoiceRole, u32, s16Output>> vs;
  auto tick = [&](SpatialOutput& o) {
    vs.clear();
    voices(vs);
    s16Output bed{};
    for (auto& [role, owner, s] : vs) {
      // the synth's TickRouted order: a claimed owner, then the role, else the stereo mix
      if (owner != 0 && o.Route(owner, s)) {
        continue;
      }
      if (role != VoiceRole::Effect && o.RouteRole(role, s)) {
        continue;
      }
      bed += s;
    }
    o.EndSample(bed, 0x7fff);
  };
  auto out = SpatialOutput::OpenLoopback(lay.mode, lay.channels, lock, tick, routing);
  Rendered r;
  if (!out) {
    lg::error("{}: loopback open failed", lay.name);
    return r;
  }
  r.ch = out->OutputChannels();
  r.speakers = LayoutSpeakers(r.ch);
  r.routing = out->Routing().Describe();
  if (pos) {
    std::scoped_lock l(lock);
    out->SetListener(kListener, 0, false);
    out->Claim(1, {kListener.x + pos->x, kListener.y + pos->y, kListener.z + pos->z});
  }
  // as in game: the sound claims, the bed runs the synth, the game frame's Flush starts it
  constexpr int kGameFrame = kRate / 60;
  for (int done = 0; done < kRouteFrames; done += kGameFrame) {
    auto part = RenderFrames(*out, std::min(kGameFrame, kRouteFrames - done));
    r.buf.insert(r.buf.end(), part.begin(), part.end());
    out->Flush();
  }
  return r;
}

int RoutingLevels(const std::string& out_dir) {
  const std::vector<RoutingLayout> layouts = {
      {"stereo", SpatialMode::Surround, ALC_STEREO_SOFT, 2, 0x3},
      {"binaural", SpatialMode::Binaural, ALC_STEREO_SOFT, 2, 0x3},
      {"5.1", SpatialMode::Surround, ALC_5POINT1_SOFT, 6, 0x60F},
      {"7.1", SpatialMode::Surround, ALC_7POINT1_SOFT, 8, 0x63F},
      {"7.1.4", SpatialMode::Surround, ALC_7POINT1POINT4_SANDOVER, 12, 0x2D63F},
  };
  FILE* t = fopen((out_dir + "/routing.md").c_str(), "w");
  if (!t) {
    lg::error("cannot write to {}", out_dir);
    return 1;
  }
  const auto music_pan = VanillaPan(315);  // authored 45 deg left
  const auto wide_a = VanillaPan(315);
  const auto wide_b = VanillaPan(45);
  const auto hard_left = VanillaPan(270);  // a positioned stream line or tone hard left
  auto scale = [](s16 v, float g) { return static_cast<s16>(std::lround(v * g)); };

  struct Scene {
    const char* name;
    std::function<VoiceFn()> make;
  };
  const SpatialVec front{0, 0, kDist}, left{kDist, 0, 0}, right{-kDist, 0, 0};
  auto noise_pair = [] {
    auto a = std::make_shared<Noise>();
    auto b = std::make_shared<Noise>();
    b->state = 0x9e3779b9;
    return std::make_pair(a, b);
  };
  std::vector<Scene> scenes;
  scenes.push_back({"effect (non-positional, L = R)", [&] {
                      auto n = noise_pair();
                      return VoiceFn([n](auto& vs) {
                        s16 s = n.first->Next();
                        vs.push_back({VoiceRole::Effect, 0u, s16Output{s, s}});
                      });
                    }});
  scenes.push_back({"dialog (L = R)", [&] {
                      auto n = noise_pair();
                      return VoiceFn([n](auto& vs) {
                        s16 s = n.first->Next();
                        vs.push_back({VoiceRole::Dialog, 0u, s16Output{s, s}});
                      });
                    }});
  scenes.push_back({"music (authored pan 45 deg left)", [&] {
                      auto n = noise_pair();
                      return VoiceFn([n, music_pan, scale](auto& vs) {
                        s16 s = n.first->Next();
                        vs.push_back(
                            {VoiceRole::Music, 0u,
                             s16Output{scale(s, music_pan.first), scale(s, music_pan.second)}});
                      });
                    }});
  scenes.push_back({"ambient (stereo, L and R uncorrelated)", [&] {
                      auto n = noise_pair();
                      return VoiceFn([n](auto& vs) {
                        vs.push_back(
                            {VoiceRole::Ambient, 0u, s16Output{n.first->Next(), n.second->Next()}});
                      });
                    }});
  const size_t kPositionedDialog = scenes.size();
  scenes.push_back({"dialog positioned hard left (L 1.0, R 0.19)", [&] {
                      auto n = noise_pair();
                      return VoiceFn([n, hard_left, scale](auto& vs) {
                        s16 s = n.first->Next();
                        vs.push_back(
                            {VoiceRole::Dialog, 0u,
                             s16Output{scale(s, hard_left.first), scale(s, hard_left.second)}});
                      });
                    }});
  struct Place {
    const char* name;
    SpatialVec pos;
    int bearing;
  };
  const Place places[] = {{"left", left, 270}, {"centre", front, 0}, {"right", right, 90}};
  auto pos_scene = [&](int kind) {
    auto n = noise_pair();
    auto tick = std::make_shared<int>(0);
    return VoiceFn([n, tick, kind, wide_a, wide_b, hard_left, scale](auto& vs) {
      s16 a = n.first->Next();
      s16 b = n.second->Next();
      if (kind == 5) {
        // centred for 0.1 s, then hard left: the width starts after the source is playing
        const bool wide = (*tick)++ >= kRate / 10;
        vs.push_back({VoiceRole::Effect, 1u,
                      s16Output{wide ? scale(a, hard_left.first) : a,
                                wide ? scale(a, hard_left.second) : a}});
        return;
      }
      if (kind == 0) {
        vs.push_back({VoiceRole::Effect, 1u, s16Output{a, a}});
        return;
      }
      if (kind == 4) {
        vs.push_back({VoiceRole::Effect, 1u,
                      s16Output{scale(a, hard_left.first), scale(a, hard_left.second)}});
        return;
      }
      if (kind != 3) {
        vs.push_back(
            {VoiceRole::Effect, 1u, s16Output{scale(a, wide_a.first), scale(a, wide_a.second)}});
      }
      if (kind != 2) {
        vs.push_back(
            {VoiceRole::Effect, 1u, s16Output{scale(b, wide_b.first), scale(b, wide_b.second)}});
      }
    });
  };

  fprintf(t,
          "# Surround routing through OpenAL Soft's loopback device\n\n"
          "Per-channel RMS (dBFS) over 1.0 s after 0.2 s of start-up, noise at -16.8 dBFS per "
          "voice channel. `before` = routing off (the stereo bed on the front pair, positional "
          "voices summed to mono), `after` = this branch. `total` = power summed over all "
          "channels (dB).\n");
  for (auto& lay : layouts) {
    fprintf(t, "\n## %s\n", lay.name);
    bool described = false;
    double max_diff = 0;
    auto header = [&](const Rendered& r, const char* first) {
      fprintf(t, "\n| %s |", first);
      for (auto sp : r.speakers) {
        fprintf(t, " %s |", kSpeakerNames[(int)sp]);
      }
      fprintf(t, " total |\n|---|");
      for (size_t i = 0; i <= r.speakers.size(); i++) {
        fprintf(t, "---|");
      }
      fprintf(t, "\n");
    };
    auto row = [&](const std::string& label, const Rendered& r) {
      fprintf(t, "| %s |", label.c_str());
      for (int c = 0; c < r.ch; c++) {
        fprintf(t, " %.1f |", 10 * std::log10(std::max(ChannelPower(r, c), 1e-20)));
      }
      fprintf(t, " %.2f |\n", 10 * std::log10(std::max(TotalPower(r), 1e-20)));
    };
    auto diff = [&](const Rendered& a, const Rendered& b) {
      for (size_t i = 0; i < a.buf.size() && i < b.buf.size(); i++) {
        max_diff = std::max(max_diff, (double)std::abs(a.buf[i] - b.buf[i]));
      }
    };
    std::vector<float> wav;
    // non-positional roles
    for (size_t k = 0; k < scenes.size(); k++) {
      auto before = RenderRoles(lay, false, scenes[k].make(), nullptr);
      auto after = RenderRoles(lay, true, scenes[k].make(), nullptr);
      if (!described) {
        fprintf(t, "\nRouting: %s\n", after.routing.c_str());
        header(after, "non-positional");
        described = true;
      }
      row(std::string(scenes[k].name) + ", before", before);
      row(std::string(scenes[k].name) + ", after", after);
      if (lay.mode == SpatialMode::Binaural) {
        diff(before, after);
      }
      wav.insert(wav.end(), after.buf.begin(), after.buf.end());
      if (k == kPositionedDialog && lay.mode != SpatialMode::Binaural) {
        // polarity of each channel against the line (before's FL is the line's L) and the
        // ITU fold-back L' = FL + 0.707 FC, R' = FR + 0.707 FC against before's FL / FR
        auto at = [](const Rendered& r, int i, int c) {
          return (double)r.buf[(size_t)i * r.ch + c];
        };
        int fc = -1;
        for (int c = 0; c < after.ch; c++) {
          fc = after.speakers[c] == Speaker::FC ? c : fc;
        }
        std::string signs;
        for (int c = 0; c < after.ch; c++) {
          double xy = 0, xx = 0, yy = 0;
          for (int i = kRouteSkip; i < kRouteFrames; i++) {
            xy += at(after, i, c) * at(before, i, 0);
            xx += at(after, i, c) * at(after, i, c);
            yy += at(before, i, 0) * at(before, i, 0);
          }
          signs += fmt::format(
              "{}{} {}", signs.empty() ? "" : ", ", kSpeakerNames[(int)after.speakers[c]],
              xx > 1e-12 ? fmt::format("{:+.3f}", xy / std::sqrt(xx * yy)) : std::string("silent"));
        }
        double fold = 0, ref = 0;
        for (int i = kRouteSkip; i < kRouteFrames; i++) {
          for (int side = 0; side < 2; side++) {
            const double c = fc >= 0 ? at(after, i, fc) * 0.70710678 : 0;
            const double e = at(after, i, side) + c - at(before, i, side);
            fold += e * e;
            ref += at(before, i, side) * at(before, i, side);
          }
        }
        fprintf(t, "|   correlation with the line: %s | |\n", signs.c_str());
        fprintf(t, "|   ITU fold-back vs the stereo line: error %.1f dB under the signal | |\n",
                10 * std::log10(std::max(ref, 1e-20) / std::max(fold, 1e-20)));
      }
      // delay and level of the bleed against the front pair
      for (int c = 0; c < after.ch; c++) {
        const auto sp = after.speakers[c];
        if ((sp == Speaker::SL || sp == Speaker::BL || sp == Speaker::BC) && k >= 2 &&
            ChannelPower(after, c) > 1e-12) {
          const int lag = LagBehind(after, 0, c, kRate / 40);
          fprintf(t, "|   %s vs FL: %+.2f dB, %d samples (%.1f ms) later | |\n",
                  kSpeakerNames[(int)sp],
                  10 * std::log10(ChannelPower(after, c) / ChannelPower(after, 0)), lag,
                  lag * 1000.0 / kRate);
        }
      }
    }
    // positional
    header(RenderRoles(lay, true, pos_scene(0), &front), "positional");
    const char* kinds[] = {"mono-authored",
                           "wide (voice A pan 45 L + voice B pan 45 R)",
                           "voice A only (pan 45 L)",
                           "voice B only (pan 45 R)",
                           "hard-panned tone (pan 270)",
                           "hard-panned tone, width from 0.1 s"};
    double total_after[6][3] = {};
    for (int kind = 0; kind < 6; kind++) {
      for (auto& p : places) {
        auto before = RenderRoles(lay, false, pos_scene(kind), &p.pos);
        auto after = RenderRoles(lay, true, pos_scene(kind), &p.pos);
        row(fmt::format("{} at {}, before", kinds[kind], p.name), before);
        row(fmt::format("{} at {}, after", kinds[kind], p.name), after);
        total_after[kind][&p - places] = TotalPower(after);
        if (lay.mode == SpatialMode::Binaural) {
          diff(before, after);
        }
        wav.insert(wav.end(), after.buf.begin(), after.buf.end());
      }
    }
    {
      // overhead: no bearing, so the sides' 0 dB rather than the ahead gain
      const SpatialVec above{0, kDist, 0};
      auto before = RenderRoles(lay, false, pos_scene(0), &above);
      auto after = RenderRoles(lay, true, pos_scene(0), &above);
      row("mono-authored above, before", before);
      row("mono-authored above, after", after);
      if (lay.mode == SpatialMode::Binaural) {
        diff(before, after);
      }
    }
    if (lay.mode != SpatialMode::Binaural && lay.channels != ALC_STEREO_SOFT) {
      // against the original's stereo power: the voice's noise power times the pan table's
      // L^2 + R^2 at (bearing + authored pan)
      Noise noise;
      double np = 0;
      for (int i = 0; i < kRouteFrames; i++) {
        const double v = noise.Next() / 32768.0;
        np += i >= kRouteSkip ? v * v : 0;
      }
      np /= kRouteFrames - kRouteSkip;
      fprintf(t,
              "\n| ahead (bearing 0) | after total | original | after minus original |\n"
              "|---|---|---|---|\n");
      for (int kind : {0, 4}) {
        const auto g = VanillaPan(kind == 0 ? 0 : 270);
        const double orig = np * (g.first * g.first + g.second * g.second);
        fprintf(t, "| %s | %.2f | %.2f | %+.2f dB |\n", kinds[kind],
                10 * std::log10(total_after[kind][1]), 10 * std::log10(orig),
                10 * std::log10(total_after[kind][1] / orig));
      }
    }
    if (lay.mode == SpatialMode::Binaural) {
      fprintf(t, "\nBinaural, after vs before: max sample difference %.3g over every scene.\n",
              max_diff);
    }
    WriteWav(out_dir + "/routing-" + lay.name + ".wav", wav, lay.count, lay.mask);
  }

  // the original's stereo loudness per bearing, for the positional rows
  fprintf(t,
          "\n## Vanilla stereo reference (pan table)\n\n| voices | bearing | L | R | total "
          "(dB, rel. one voice at full scale) |\n|---|---|---|---|---|\n");
  for (int kind = 0; kind < 2; kind++) {
    for (auto& p : places) {
      double l = 0, r = 0;
      for (int vp : kind == 0 ? std::vector<int>{0} : std::vector<int>{315, 45}) {
        auto g = VanillaPan(p.bearing + vp);
        l += g.first * g.first;
        r += g.second * g.second;
      }
      fprintf(t, "| %s | %s (%d) | %.1f | %.1f | %.2f |\n", kind == 0 ? "mono" : "wide A + B",
              p.name, p.bearing, 10 * std::log10(l), 10 * std::log10(r), 10 * std::log10(l + r));
    }
  }
  fclose(t);
  printf("wrote %s/routing.md\n", out_dir.c_str());
  return 0;
}

// Sound bank census: per SBK, the sounds by volume group, falloff curve (0: not positional),
// and whether a tone or an LFO pans (authored width).
int Census(const std::string& dir) {
  namespace fs = std::filesystem;
  int total = 0, ambient = 0, ambient_np = 0, wide = 0, wide_pos = 0;
  std::map<int, int> groups;
  for (auto& e : fs::directory_iterator(dir)) {
    if (e.path().extension() != ".SBK" && e.path().extension() != ".sbk") {
      continue;
    }
    std::ifstream f(e.path(), std::ios::binary);
    std::vector<u8> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data.size() < 24) {
      continue;
    }
    u32 count = 0;
    memcpy(&count, data.data() + 20, 4);
    std::vector<std::pair<std::string, u32>> names(count);
    for (u32 i = 0; i < count && 24 + i * 20 + 20 <= data.size(); i++) {
      char nm[17] = {};
      memcpy(nm, data.data() + 24 + i * 20, 16);
      u32 fo = 0;
      memcpy(&fo, data.data() + 24 + i * 20 + 16, 4);
      names[i] = {nm, fo};
    }
    // the bank follows the name list: one sector for a level bank, ten for the common one
    u32 type1 = 0;
    if (data.size() > 2052) {
      memcpy(&type1, data.data() + 2048, 4);
    }
    const size_t offset = type1 == 3 ? 2048 : 10 * 2048;
    if (data.size() <= offset) {
      continue;
    }
    Loader loader;
    auto handle = loader.BankLoad(std::span<u8>(data).subspan(offset));
    auto* bank = dynamic_cast<SFXBlock*>(loader.GetBankByHandle(handle));
    if (!bank) {
      continue;
    }
    for (size_t i = 0; i < bank->Sounds.size(); i++) {
      auto& sfx = bank->Sounds[i];
      int curve = i < names.size() ? (int)(names[i].second >> 28) : -1;
      bool pans = false;
      std::string how;
      for (auto& g : sfx.Grains) {
        if (auto* tone = std::get_if<Tone>(&g.data); tone && tone->Pan != 0) {
          pans = true;
          how += fmt::format(" tone pan {}", tone->Pan);
        }
        if (auto* lfo = std::get_if<LFOParams>(&g.data);
            lfo && g.Type == GrainType::LFO_SETTINGS && lfo->target == (u8)LFOTarget::PAN) {
          pans = true;
          how += fmt::format(" LFO pan depth {}", lfo->depth);
        }
      }
      total++;
      groups[sfx.VolGroup]++;
      if (sfx.VolGroup == 4) {
        ambient++;
        if (curve == 0) {
          ambient_np++;
          printf("ambient, not positional: %s %s\n", e.path().filename().string().c_str(),
                 i < names.size() ? names[i].first.c_str() : "?");
        }
      }
      if (pans) {
        wide++;
        if (curve != 0) {
          wide_pos++;
          printf("positional with authored pan: %s %s (group %d, curve %d, min %u max %u m):%s\n",
                 e.path().filename().string().c_str(),
                 i < names.size() ? names[i].first.c_str() : "?", sfx.VolGroup, curve,
                 i < names.size() ? (names[i].second & 0x3fff) : 0u,
                 i < names.size() ? ((names[i].second >> 14) & 0x3fff) : 0u, how.c_str());
        }
      }
    }
  }
  printf("sounds %d; by group:", total);
  for (auto& [g, n] : groups) {
    printf(" %d:%d", g, n);
  }
  printf("\nambient (group 4) %d, of which not positional %d\n", ambient, ambient_np);
  printf("authored pan (tone or LFO) %d, of which positional %d\n", wide, wide_pos);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::string(argv[1]) == "--endpoint") {
    return Endpoint();
  }
  if (argc > 2 && std::string(argv[1]) == "--device") {
    return Device(argv[2]);
  }
  if (argc > 1 && std::string(argv[1]) == "--list") {
    return List();
  }
  if (argc > 2 && std::string(argv[1]) == "--saved") {
    return Saved(argv[2]);
  }
  if (argc > 1 && std::string(argv[1]) == "--outputs") {
    return Outputs(argc > 2 ? argv[2] : "");
  }
  if (argc > 2 && std::string(argv[1]) == "--play-test") {
    return PlayTest(argv[2], argc > 3 ? argv[3] : "");
  }
  if (argc > 2 && std::string(argv[1]) == "--speaker-test") {
    return SpeakerTestLevels(argv[2]);
  }
  if (argc > 2 && std::string(argv[1]) == "--routing") {
    return RoutingLevels(argv[2]);
  }
  if (argc > 2 && std::string(argv[1]) == "--census") {
    return Census(argv[2]);
  }
  const std::string out_dir = argc > 1 ? argv[1] : ".";

  const std::vector<Layout> layouts = {
      {"binaural", SpatialMode::Binaural, ALC_STEREO_SOFT, 2, 0x3, {"L", "R"}},
      {"surround-5.1",
       SpatialMode::Surround,
       ALC_5POINT1_SOFT,
       6,
       0x60F,
       {"FL", "FR", "FC", "LFE", "SL", "SR"}},
      {"surround-7.1",
       SpatialMode::Surround,
       ALC_7POINT1_SOFT,
       8,
       0x63F,
       {"FL", "FR", "FC", "LFE", "BL", "BR", "SL", "SR"}},
  };

  FILE* table = fopen((out_dir + "/levels.md").c_str(), "w");
  if (!table) {
    lg::error("cannot write to {}", out_dir);
    return 1;
  }
  fprintf(table, "# Spatial probe: per-channel RMS (dBFS), noise source 4 m from the listener\n");
  for (auto& lay : layouts) {
    RunScene(out_dir, lay, table);
  }

  fprintf(table,
          "\n### Cost: render time / audio time, one thread, sources moving every 800 "
          "frames\n\n| layout | 0 sources | 8 | 32 |\n|---|---|---|---|\n");
  for (auto& lay : layouts) {
    fprintf(table, "| %s | %.2f%% | %.2f%% | %.2f%% |\n", lay.name, Cost(lay, 0) * 100,
            Cost(lay, 8) * 100, Cost(lay, 32) * 100);
  }
  fclose(table);
  printf("wrote %s/levels.md\n", out_dir.c_str());
  return 0;
}
