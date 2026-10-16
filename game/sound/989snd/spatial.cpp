// SPDX-License-Identifier: ISC
#include "spatial.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "common/log/log.h"

#ifdef SANDOVER_SPATIAL_AUDIO
#define AL_ALEXT_PROTOTYPES
#include "AL/al.h"
#include "AL/alc.h"
#include "AL/alext.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <cstdlib>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <spatialaudioclient.h>
#include <windows.h>
#endif
#endif

namespace snd {

SpatialMode g_spatial_mode_request = SpatialMode::Off;

const char* SpatialModeName(SpatialMode mode) {
  switch (mode) {
    case SpatialMode::Off:
      return "off";
    case SpatialMode::Binaural:
      return "binaural";
    case SpatialMode::Surround:
      return "surround";
    case SpatialMode::Objects:
      return "objects";
  }
  return "?";
}

std::optional<SpatialMode> SpatialModeFromName(std::string_view name) {
  for (auto m :
       {SpatialMode::Off, SpatialMode::Binaural, SpatialMode::Surround, SpatialMode::Objects}) {
    if (name == SpatialModeName(m)) {
      return m;
    }
  }
  return std::nullopt;
}

namespace {
// Overlord sound units (game units / 16) per metre; only directions matter (distance model off).
constexpr float kUnitsPerMetre = 256.f;
// Bound on queued samples for a source that is not being pulled yet (100 ms).
constexpr size_t kMaxQueue = SpatialOutput::kRate / 10;
}  // namespace

SpatialOutput::SpatialOutput(SpatialMode mode, std::recursive_mutex& lock, TickFn tick)
    : mMode(mode), mLock(lock), mTick(std::move(tick)) {
  for (int i = 0; i < kSlots; i++) {
    mCtx[i] = {this, i};
  }
  mCtx[kSlots] = {this, -1};
}

bool SpatialOutput::Route(u32 owner, s16Output sample) {
  for (auto& s : mSlots) {
    if (s.owner == owner) {
      s.acc += (sample.left + sample.right) >> 1;
      return true;
    }
  }
  return false;
}

void SpatialOutput::EndSample(s16Output bed, s32 master_volume) {
  for (auto& s : mSlots) {
    if (s.owner == 0) {
      continue;
    }
    s32 v = std::clamp<s32>(s.acc, INT16_MIN, INT16_MAX);
    s.queue.push_back(static_cast<s16>((v * master_volume) >> 15));
    s.acc = 0;
    if (s.queue.size() > kMaxQueue) {
      s.queue.pop_front();
    }
  }
  mBed.push_back(bed.left);
  mBed.push_back(bed.right);
  if (mBed.size() > 2 * kMaxQueue) {
    mBed.pop_front();
    mBed.pop_front();
  }
}

bool SpatialOutput::Claim(u32 owner, const SpatialVec& pos) {
  if (owner == 0) {
    return false;
  }
  for (auto& s : mSlots) {
    if (s.owner == 0) {
      s.owner = owner;
      s.acc = 0;
      s.queue.clear();
      s.pos = pos;
      s.pos_dirty = true;
      s.start_pending = true;
      s.first_fill = true;
      mTotalClaims++;
      return true;
    }
  }
  return false;
}

void SpatialOutput::Release(u32 owner) {
  for (auto& s : mSlots) {
    if (s.owner == owner) {
      s.owner = 0;
      s.acc = 0;
      s.queue.clear();
      s.start_pending = false;
    }
  }
}

bool SpatialOutput::Owns(u32 owner) const {
  if (owner == 0) {
    return false;
  }
  return std::any_of(mSlots.begin(), mSlots.end(), [&](const Slot& s) { return s.owner == owner; });
}

void SpatialOutput::SetPosition(u32 owner, const SpatialVec& pos) {
  for (auto& s : mSlots) {
    if (s.owner == owner && owner != 0) {
      s.pos = pos;
      s.pos_dirty = true;
    }
  }
}

void SpatialOutput::SetListener(const SpatialVec& pos, float yaw_deg, bool mirror) {
  mListenerPos = pos;
  mListenerYaw = yaw_deg;
  mListenerMirror = mirror;
  mListenerDirty = true;
}

void SpatialOutput::Prune(const std::function<bool(u32)>& alive) {
  for (auto& s : mSlots) {
    if (s.owner != 0 && !alive(s.owner)) {
      Release(s.owner);
    }
  }
}

int SpatialOutput::ActiveSlots() const {
  return (int)std::count_if(mSlots.begin(), mSlots.end(),
                            [](const Slot& s) { return s.owner != 0; });
}

int SpatialOutput::Fill(int slot, s16* dst, int frames) {
  std::scoped_lock lock(mLock);
  if (slot >= 0 && mSlots[slot].owner == 0) {
    return 0;  // ends the source until it is claimed again
  }
  auto& q = slot < 0 ? mBed : mSlots[slot].queue;
  if (slot >= 0 && mSlots[slot].first_fill) {
    // Drop what queued up before the source started, to line up with the bed.
    mSlots[slot].first_fill = false;
    while (q.size() > mBed.size() / 2) {
      q.pop_front();
    }
  }
  const size_t per_frame = slot < 0 ? 2 : 1;
  const size_t need = frames * per_frame;
  // Bounded: a slot released by the tick stops growing its queue.
  for (int guard = 0; q.size() < need && guard < frames; guard++) {
    mTick(*this);
    if (slot >= 0 && mSlots[slot].owner == 0) {
      break;
    }
  }
  size_t n = std::min(need, q.size());
  std::copy_n(q.begin(), n, dst);
  q.erase(q.begin(), q.begin() + n);
  std::fill(dst + n, dst + need, 0);
  return frames;
}

#ifdef SANDOVER_SPATIAL_AUDIO

namespace {
bool g_openal_used = false;

// Objects mode needs OpenAL Soft's WASAPI spatial stream, a config option read once per
// process, so it must be the first mode opened.
#ifdef _WIN32
// Dynamic-object budget of the default render endpoint; 0 when no spatial format is enabled.
u32 EndpointObjectBudget() {
  if (const char* force = std::getenv("SANDOVER_SPATIAL_OBJECT_BUDGET")) {
    return static_cast<u32>(std::strtoul(force, nullptr, 10));
  }
  bool co = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
  UINT32 budget = 0;
  IMMDeviceEnumerator* en = nullptr;
  IMMDevice* dev = nullptr;
  ISpatialAudioClient* sac = nullptr;
  if (SUCCEEDED(
          CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) &&
      SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) &&
      SUCCEEDED(dev->Activate(__uuidof(ISpatialAudioClient), CLSCTX_INPROC_SERVER, nullptr,
                              reinterpret_cast<void**>(&sac)))) {
    sac->GetMaxDynamicObjectCount(&budget);
  }
  if (sac) {
    sac->Release();
  }
  if (dev) {
    dev->Release();
  }
  if (en) {
    en->Release();
  }
  if (co) {
    CoUninitialize();
  }
  return budget;
}
#endif

// Returns false when the endpoint has no spatial format; the caller then opens the bed.
bool ConfigureForObjects() {
#ifdef _WIN32
  u32 budget = EndpointObjectBudget();
  lg::info("[spatial] endpoint dynamic-object budget {}", budget);
  if (budget == 0) {
    lg::warn("[spatial] no Windows Spatial Sound format on this endpoint; using the channel bed");
    return false;
  }
  if (g_openal_used) {
    lg::warn(
        "[spatial] objects mode must be the first spatial mode in a process "
        "(start with --audio-spatial objects); OpenAL is already configured");
    return false;
  }
  wchar_t exe[MAX_PATH];
  DWORD len = GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring path(exe, len);
  path = path.substr(0, path.find_last_of(L"\\/") + 1) + L"sandover-alsoft-objects.ini";
  _wputenv_s(L"ALSOFT_CONF", path.c_str());
  return true;
#else
  return false;
#endif
}

std::string OutputModeName(ALCint mode) {
  switch (mode) {
    case ALC_MONO_SOFT:
      return "mono";
    case ALC_STEREO_SOFT:
      return "stereo";
    case ALC_STEREO_BASIC_SOFT:
      return "stereo-basic";
    case ALC_STEREO_UHJ_SOFT:
      return "stereo-uhj";
    case ALC_STEREO_HRTF_SOFT:
      return "stereo-hrtf";
    case ALC_QUAD_SOFT:
      return "quad";
    case ALC_SURROUND_5_1_SOFT:
      return "5.1";
    case ALC_SURROUND_6_1_SOFT:
      return "6.1";
    case ALC_SURROUND_7_1_SOFT:
      return "7.1";
    case ALC_ANY_SOFT:
      return "device layout (spatial stream)";
    default:
      return fmt::format("0x{:x}", mode);
  }
}

int ChannelCount(ALCint mode) {
  switch (mode) {
    case ALC_MONO_SOFT:
      return 1;
    case ALC_QUAD_SOFT:
      return 4;
    case ALC_SURROUND_5_1_SOFT:
      return 6;
    case ALC_SURROUND_6_1_SOFT:
      return 7;
    case ALC_SURROUND_7_1_SOFT:
      return 8;
    default:
      return 2;
  }
}
}  // namespace

int SpatialOutput::Callback(void* user, void* data, int bytes) {
  auto* ctx = static_cast<CallbackCtx*>(user);
  const int frame_bytes = ctx->slot < 0 ? 4 : 2;
  int frames = ctx->self->Fill(ctx->slot, static_cast<s16*>(data), bytes / frame_bytes);
  return frames * frame_bytes;
}

std::unique_ptr<SpatialOutput> SpatialOutput::OpenDevice(SpatialMode mode,
                                                         std::recursive_mutex& lock,
                                                         TickFn tick) {
  if (mode == SpatialMode::Off) {
    return nullptr;
  }
  if (mode == SpatialMode::Objects && !ConfigureForObjects()) {
    lg::info("[spatial] objects: decoding to the speaker layout instead");
  }
  g_openal_used = true;
  ALCdevice* dev = alcOpenDevice(nullptr);
  if (!dev) {
    lg::error("[spatial] alcOpenDevice failed");
    return nullptr;
  }
  auto out = std::unique_ptr<SpatialOutput>(new SpatialOutput(mode, lock, std::move(tick)));
  if (!out->InitContext(dev, false, 0)) {
    return nullptr;
  }
  return out;
}

std::unique_ptr<SpatialOutput> SpatialOutput::OpenLoopback(SpatialMode mode,
                                                           int channels,
                                                           std::recursive_mutex& lock,
                                                           TickFn tick) {
  g_openal_used = true;
  ALCdevice* dev = alcLoopbackOpenDeviceSOFT(nullptr);
  if (!dev) {
    lg::error("[spatial] alcLoopbackOpenDeviceSOFT failed");
    return nullptr;
  }
  auto out = std::unique_ptr<SpatialOutput>(new SpatialOutput(mode, lock, std::move(tick)));
  if (!out->InitContext(dev, true, channels)) {
    return nullptr;
  }
  return out;
}

bool SpatialOutput::InitContext(void* device, bool loopback, int channels) {
  auto* dev = static_cast<ALCdevice*>(device);
  mDevice = dev;

  std::vector<ALCint> attrs = {ALC_FREQUENCY,      kRate, ALC_MONO_SOURCES, kSlots + 4,
                               ALC_STEREO_SOURCES, 4};
  if (loopback) {
    attrs.insert(attrs.end(),
                 {ALC_FORMAT_CHANNELS_SOFT, channels, ALC_FORMAT_TYPE_SOFT, ALC_FLOAT_SOFT});
  }
  if (mMode == SpatialMode::Binaural) {
    attrs.insert(attrs.end(), {ALC_HRTF_SOFT, ALC_TRUE});
    if (!loopback) {
      attrs.insert(attrs.end(), {ALC_OUTPUT_MODE_SOFT, ALC_STEREO_HRTF_SOFT});
    }
  } else {
    attrs.insert(attrs.end(), {ALC_HRTF_SOFT, ALC_FALSE});
  }
  attrs.push_back(0);

  auto* ctx = alcCreateContext(dev, attrs.data());
  if (!ctx || !alcMakeContextCurrent(ctx)) {
    lg::error("[spatial] context creation failed: 0x{:x}", alcGetError(dev));
    if (ctx) {
      alcDestroyContext(ctx);
    }
    alcCloseDevice(dev);
    mDevice = nullptr;
    return false;
  }
  mContext = ctx;

  ALCint out_mode = 0, hrtf = 0, freq = 0;
  alcGetIntegerv(dev, ALC_OUTPUT_MODE_SOFT, 1, &out_mode);
  alcGetIntegerv(dev, ALC_HRTF_STATUS_SOFT, 1, &hrtf);
  alcGetIntegerv(dev, ALC_FREQUENCY, 1, &freq);
  mOutChannels = loopback ? ChannelCount(channels) : ChannelCount(out_mode);
  const char* name = alcGetString(dev, ALC_ALL_DEVICES_SPECIFIER);
  mDescription = fmt::format("mode {} on '{}': output {}, hrtf {}, {} Hz", SpatialModeName(mMode),
                             name ? name : "?", OutputModeName(out_mode),
                             hrtf == ALC_HRTF_ENABLED_SOFT ? "on" : "off", freq);

  alDistanceModel(AL_NONE);

  for (int i = 0; i < kSlots; i++) {
    auto& s = mSlots[i];
    alGenBuffers(1, &s.buffer);
    alBufferCallbackSOFT(s.buffer, AL_FORMAT_MONO16, kRate,
                         reinterpret_cast<ALBUFFERCALLBACKTYPESOFT>(&Callback), &mCtx[i]);
    alGenSources(1, &s.source);
    alSourcei(s.source, AL_BUFFER, (ALint)s.buffer);
  }
  alGenBuffers(1, &mBedBuffer);
  alBufferCallbackSOFT(mBedBuffer, AL_FORMAT_STEREO16, kRate,
                       reinterpret_cast<ALBUFFERCALLBACKTYPESOFT>(&Callback), &mCtx[kSlots]);
  alGenSources(1, &mBedSource);
  alSourcei(mBedSource, AL_BUFFER, (ALint)mBedBuffer);
  alSourcei(mBedSource, AL_DIRECT_CHANNELS_SOFT, AL_DROP_UNMATCHED_SOFT);
  alSourcePlay(mBedSource);

  if (ALenum err = alGetError(); err != AL_NO_ERROR) {
    lg::error("[spatial] source setup failed: 0x{:x}", err);
    return false;
  }
  lg::info("[spatial] {}", mDescription);
  return true;
}

void SpatialOutput::Flush() {
  struct Pending {
    unsigned source;
    SpatialVec pos;
    bool pos_dirty;
    bool start;
  };
  std::array<Pending, kSlots> pending{};
  int count = 0;
  SpatialVec lpos;
  float yaw;
  bool mirror;
  {
    std::scoped_lock lock(mLock);
    for (auto& s : mSlots) {
      if (s.owner != 0 && (s.pos_dirty || s.start_pending || mListenerDirty)) {
        pending[count++] = {s.source, s.pos, s.pos_dirty || mListenerDirty, s.start_pending};
        s.pos_dirty = false;
        s.start_pending = false;
      }
    }

    mListenerDirty = false;
    lpos = mListenerPos;
    yaw = mListenerYaw;
    mirror = mListenerMirror;
  }

  // Sources go in listener space (OpenAL's default listener: right +x, up +y, forward -z).
  // The vanilla pan puts the camera's left on (cos a, 0, -sin a); forward is left x up.
  const float rad = yaw * 3.14159265f / 180.f;
  const float lx = std::cos(rad), lz = -std::sin(rad);
  const float fx = std::sin(rad), fz = std::cos(rad);
  const float right_sign = mirror ? 1.f : -1.f;
  auto to_listener = [&](const SpatialVec& p) {
    const float dx = p.x - lpos.x, dy = p.y - lpos.y, dz = p.z - lpos.z;
    return SpatialVec{right_sign * (dx * lx + dz * lz) / kUnitsPerMetre, dy / kUnitsPerMetre,
                      -(dx * fx + dz * fz) / kUnitsPerMetre};
  };
  for (int i = 0; i < count; i++) {
    auto& p = pending[i];
    if (p.pos_dirty) {
      const SpatialVec v = to_listener(p.pos);
      alSource3f(p.source, AL_POSITION, v.x, v.y, v.z);
    }
    if (p.start) {
      alSourcePlay(p.source);
    }
  }
}

void SpatialOutput::Render(float* out, int frames) {
  alcRenderSamplesSOFT(static_cast<ALCdevice*>(mDevice), out, frames);
}

SpatialOutput::~SpatialOutput() {
  if (mContext) {
    for (auto& s : mSlots) {
      alSourceStop(s.source);
      alDeleteSources(1, &s.source);
      alDeleteBuffers(1, &s.buffer);
    }
    alSourceStop(mBedSource);
    alDeleteSources(1, &mBedSource);
    alDeleteBuffers(1, &mBedBuffer);
    alcMakeContextCurrent(nullptr);
    alcDestroyContext(static_cast<ALCcontext*>(mContext));
  }
  if (mDevice) {
    alcCloseDevice(static_cast<ALCdevice*>(mDevice));
  }
}

#else

int SpatialOutput::Callback(void*, void*, int) {
  return 0;
}
std::unique_ptr<SpatialOutput> SpatialOutput::OpenDevice(SpatialMode,
                                                         std::recursive_mutex&,
                                                         TickFn) {
  lg::warn("[spatial] not built on this platform");
  return nullptr;
}
std::unique_ptr<SpatialOutput> SpatialOutput::OpenLoopback(SpatialMode,
                                                           int,
                                                           std::recursive_mutex&,
                                                           TickFn) {
  return nullptr;
}
bool SpatialOutput::InitContext(void*, bool, int) {
  return false;
}
void SpatialOutput::Flush() {}
void SpatialOutput::Render(float*, int) {}
SpatialOutput::~SpatialOutput() = default;

#endif

}  // namespace snd
