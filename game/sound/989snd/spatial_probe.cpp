// SPDX-License-Identifier: ISC
// Spatial audio test scene: noise at fixed positions around a fixed listener, rendered through
// SpatialOutput on an OpenAL Soft loopback device; one WAV per layout plus a level table.
//   spatial_probe <out_dir>            test scene + cost table
//   spatial_probe --endpoint           Windows Spatial Sound state of the default endpoint
//   spatial_probe --device <mode>      opens the real device in <mode> for 1 s of silence

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "spatial.h"

#include "common/log/log.h"

#define AL_ALEXT_PROTOTYPES
#include "AL/al.h"
#include "AL/alc.h"
#include "AL/alext.h"

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

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::string(argv[1]) == "--endpoint") {
    return Endpoint();
  }
  if (argc > 2 && std::string(argv[1]) == "--device") {
    return Device(argv[2]);
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
