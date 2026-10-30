// SPDX-License-Identifier: ISC
#include "spatial.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "common/log/log.h"

#ifdef SANDOVER_SPATIAL_AUDIO
#define AL_ALEXT_PROTOTYPES
#include "AL/al.h"
#include "AL/alc.h"
#include "AL/alext.h"

#include "third-party/openal-soft/alc/inprogext.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <audioclient.h>
#include <initguid.h>
#include <inspectable.h>
#include <mmdeviceapi.h>
#include <spatialaudioclient.h>
#include <windows.h>
#endif
#endif

namespace snd {

SpatialMode g_spatial_mode_request = SpatialMode::Off;
bool g_spatial_mode_from_cli = false;

const char* AudioOutputName(AudioOutput out) {
  switch (out) {
    case AudioOutput::Auto:
      return "auto";
    case AudioOutput::Stereo:
      return "stereo";
    case AudioOutput::X51:
      return "5.1";
    case AudioOutput::X71:
      return "7.1";
    case AudioOutput::Headphones:
      return "headphones";
    case AudioOutput::Objects:
      return "objects";
  }
  return "?";
}

std::optional<AudioOutput> AudioOutputFromName(std::string_view name) {
  for (int i = 0; i <= (int)AudioOutput::Objects; i++) {
    if (name == AudioOutputName((AudioOutput)i)) {
      return (AudioOutput)i;
    }
  }
  return std::nullopt;
}

const char* SavedOutputName(AudioOutput out) {
  return out == AudioOutput::Objects ? "spatial" : AudioOutputName(out);
}

namespace {
// The value after `(key` in a sandover.gc text: a bare word, or a quoted string without quotes.
// With `quoted_only` a bare word reads as "" (sandover-read-quoted in pckernel.gc).
std::optional<std::string> SavedValue(std::string_view text,
                                      std::string_view key,
                                      bool quoted_only = false) {
  const std::string open = "(" + std::string(key);
  for (size_t at = text.find(open); at != std::string_view::npos; at = text.find(open, at + 1)) {
    size_t i = at + open.size();
    if (i >= text.size() || (text[i] != ' ' && text[i] != '\t')) {
      continue;  // a longer key with the same start
    }
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) {
      i++;
    }
    if (i < text.size() && text[i] == '"') {
      const size_t end = text.find('"', i + 1);
      if (end == std::string_view::npos) {
        return std::nullopt;
      }
      return std::string(text.substr(i + 1, end - i - 1));
    }
    if (quoted_only) {
      return std::string();
    }
    size_t end = i;
    while (end < text.size() && text[end] != ')' && text[end] != ' ' && text[end] != '\t' &&
           text[end] != '\r' && text[end] != '\n') {
      end++;
    }
    return std::string(text.substr(i, end - i));
  }
  return std::nullopt;
}
}  // namespace

SavedAudio ParseSavedAudio(std::string_view text) {
  SavedAudio saved;
  if (auto out = SavedValue(text, "audio-output")) {
    for (int i = 0; i <= (int)AudioOutput::Objects; i++) {
      if (*out == SavedOutputName((AudioOutput)i)) {
        saved.output = (AudioOutput)i;
      }
    }
  }
  if (auto dev = SavedValue(text, "audio-device", true)) {
    saved.device = *dev;
  }
  return saved;
}

const char* SavedAudioFileName(bool xr) {
  return xr ? "sandover-vr.gc" : "sandover.gc";
}

OutputAction PlanOutputRequest(bool configured, bool first, AudioOutput requested, bool cli) {
  const bool spatial = requested == AudioOutput::Objects;
  if (configured) {
    // every device opens through the spatial config: other outputs wait for a restart
    return spatial ? OutputAction::Open : OutputAction::Keep;
  }
  if (!spatial || cli) {
    return OutputAction::Open;  // command-line objects: decoded to the device's bed
  }
  return first ? OutputAction::Fallback : OutputAction::Keep;
}

bool RestartPending(AudioOutput requested, bool configured, std::optional<int> budget) {
  const bool spatial = requested == AudioOutput::Objects;
  if (spatial == configured) {
    return false;
  }
  return configured || !budget || *budget > 0;
}

const char* SpatialFormatName(SpatialFormat f) {
  switch (f) {
    case SpatialFormat::None:
      return "off";
    case SpatialFormat::WindowsSonic:
      return "Windows Sonic for Headphones";
    case SpatialFormat::DolbyAtmosHeadphones:
      return "Dolby Atmos for Headphones";
    case SpatialFormat::DolbyAtmosHomeTheater:
      return "Dolby Atmos for home theater";
    case SpatialFormat::DolbyAtmosSpeakers:
      return "Dolby Atmos for built-in speakers";
    case SpatialFormat::DtsHeadphoneX:
      return "DTS Headphone:X";
    case SpatialFormat::DtsXUltra:
      return "DTS:X Ultra";
    case SpatialFormat::DtsXHomeTheater:
      return "DTS:X for home theater";
    case SpatialFormat::Other:
      return "a spatial format (unnamed)";
    case SpatialFormat::Unreadable:
      return "unknown (Windows did not say)";
  }
  return "?";
}

bool SpatialMaskHasHeights(u32 static_mask) {
  // AudioObjectType_TopFrontLeft .. TopBackRight (spatialaudioclient.h)
  constexpr u32 kTops = 0x400 | 0x800 | 0x1000 | 0x2000;
  return (static_mask & kTops) == kTops;
}

OutputPlan PlanOutput(AudioOutput out, const std::string& device) {
  switch (out) {
    case AudioOutput::Auto:
      return {SpatialMode::Surround, SpatialLayout::Device, true};
    case AudioOutput::Stereo:
      // the vanilla stream plays on the Windows default; another device needs OpenAL
      if (device.empty()) {
        return {SpatialMode::Off, SpatialLayout::Device, false};
      }
      return {SpatialMode::Surround, SpatialLayout::Stereo, false};
    case AudioOutput::X51:
      return {SpatialMode::Surround, SpatialLayout::X51, false};
    case AudioOutput::X71:
      return {SpatialMode::Surround, SpatialLayout::X71, false};
    case AudioOutput::Headphones:
      return {SpatialMode::Binaural, SpatialLayout::Device, false};
    case AudioOutput::Objects:
      return {SpatialMode::Objects, SpatialLayout::Device, false};
  }
  return {};
}

std::string EndpointLayoutName(int channels, u32 mask) {
  switch (mask) {
    case 0x4:
      return "mono";
    case 0x3:
      return "stereo";
    case 0x33:
      return "quad";
    case 0x3F:
      return "5.1 (back)";
    case 0x60F:
      return "5.1 (side)";
    case 0x70F:
      return "6.1";
    case 0x63F:
      return "7.1";
    case 0x2D63F:
      return "7.1.4";
    default:
      break;
  }
  if (mask == 0) {
    return channels == 1 ? "mono" : channels == 2 ? "stereo" : fmt::format("{} ch", channels);
  }
  return fmt::format("{} ch (mask 0x{:x})", channels, mask);
}

std::vector<Speaker> LayoutSpeakers(int channels) {
  using S = Speaker;
  switch (channels) {
    case 1:
      return {S::FC};
    case 4:
      return {S::FL, S::FR, S::BL, S::BR};
    case 6:
      return {S::FL, S::FR, S::FC, S::LFE, S::SL, S::SR};
    case 7:
      return {S::FL, S::FR, S::FC, S::LFE, S::BC, S::SL, S::SR};
    case 8:
      return {S::FL, S::FR, S::FC, S::LFE, S::BL, S::BR, S::SL, S::SR};
    case 12:
      return {S::FL, S::FR, S::FC,  S::LFE, S::BL,  S::BR,
              S::SL, S::SR, S::TFL, S::TFR, S::TBL, S::TBR};
    default:
      return {S::FL, S::FR};
  }
}

namespace {
const char* SpeakerLabel(Speaker sp) {
  switch (sp) {
    case Speaker::FL:
      return "FRONT LEFT";
    case Speaker::FR:
      return "FRONT RIGHT";
    case Speaker::FC:
      return "CENTER";
    case Speaker::LFE:
      return "SUBWOOFER (LFE)";
    case Speaker::SL:
      return "SURROUND LEFT";
    case Speaker::SR:
      return "SURROUND RIGHT";
    case Speaker::BL:
      return "BACK LEFT";
    case Speaker::BR:
      return "BACK RIGHT";
    case Speaker::BC:
      return "BACK CENTER";
    case Speaker::TFL:
      return "TOP FRONT LEFT";
    case Speaker::TFR:
      return "TOP FRONT RIGHT";
    case Speaker::TBL:
      return "TOP BACK LEFT";
    case Speaker::TBR:
      return "TOP BACK RIGHT";
  }
  return "?";
}

const char* SpeakerShort(Speaker sp) {
  static const char* kNames[] = {"FL", "FR", "FC",  "LFE", "BL",  "BR", "SL",
                                 "SR", "BC", "TFL", "TFR", "TBL", "TBR"};
  return kNames[(int)sp];
}

float DbGain(float db) {
  return std::pow(10.f, db / 20.f);
}
}  // namespace

std::vector<int> SpeakerTest::Slots(int channels, std::vector<std::string>* labels) {
  // test order: FL FR FC LFE, the side (surround) pair, the back pair or centre, the heights
  static const Speaker kOrder[] = {
      Speaker::FL, Speaker::FR, Speaker::FC,  Speaker::LFE, Speaker::SL,  Speaker::SR, Speaker::BL,
      Speaker::BR, Speaker::BC, Speaker::TFL, Speaker::TFR, Speaker::TBL, Speaker::TBR};
  const auto layout = LayoutSpeakers(channels);
  std::vector<int> slots;
  if (labels) {
    labels->clear();
  }
  if (channels <= 1) {
    slots.push_back(0);
    if (labels) {
      labels->push_back("MONO");
    }
    return slots;
  }
  for (auto sp : kOrder) {
    auto it = std::find(layout.begin(), layout.end(), sp);
    if (it == layout.end()) {
      continue;
    }
    slots.push_back((int)(it - layout.begin()));
    if (labels) {
      labels->push_back(SpeakerLabel(sp));
    }
  }
  return slots;
}

BedRouting ResolveBedRouting(int channels) {
  BedRouting r;
  r.channels = channels;
  r.speakers = LayoutSpeakers(channels);
  r.gain.assign(r.speakers.size(), {});
  auto has = [&](Speaker sp) {
    return std::find(r.speakers.begin(), r.speakers.end(), sp) != r.speakers.end();
  };
  const bool centre = has(Speaker::FC) && channels > 1;
  const bool side = has(Speaker::SL) && has(Speaker::SR);
  const bool back = has(Speaker::BL) && has(Speaker::BR);
  const bool back_centre = has(Speaker::BC);
  r.active = centre || side || back || back_centre;
  if (!r.active) {
    return r;
  }
  const float music = DbGain(BedRouting::kMusicBleedDb);
  const float ambient = DbGain(BedRouting::kAmbientBleedDb);
  const float back_extra = side ? DbGain(BedRouting::kBackExtraDb) : 1.f;
  const float sqrt2 = std::sqrt(2.f);
  using I = BedRouting::Input;
  for (size_t ch = 0; ch < r.speakers.size(); ch++) {
    auto& g = r.gain[ch];
    switch (r.speakers[ch]) {
      case Speaker::FL:
        g[I::FrontL] = 1.f;
        g[I::DialogCentre] = centre ? 0.f : 1.f;
        g[I::DialogL] = 1.f;
        break;
      case Speaker::FR:
        g[I::FrontR] = 1.f;
        g[I::DialogCentre] = centre ? 0.f : 1.f;
        g[I::DialogR] = 1.f;
        break;
      case Speaker::FC:
        g[I::DialogCentre] = sqrt2;
        break;
      case Speaker::SL:
        g[I::MusicL] = music;
        g[I::AmbientL] = ambient;
        break;
      case Speaker::SR:
        g[I::MusicR] = music;
        g[I::AmbientR] = ambient;
        break;
      case Speaker::BL:
        g[I::MusicL] = music * back_extra;
        g[I::AmbientL] = ambient * back_extra;
        break;
      case Speaker::BR:
        g[I::MusicR] = music * back_extra;
        g[I::AmbientR] = ambient * back_extra;
        break;
      case Speaker::BC: {
        const float bc = 0.5f * (side ? DbGain(BedRouting::kBackExtraDb) : 1.f);
        g[I::MusicL] = g[I::MusicR] = music * bc;
        g[I::AmbientL] = g[I::AmbientR] = ambient * bc;
      } break;
      default:  // LFE and the heights get nothing
        break;
    }
  }
  return r;
}

std::string BedRouting::Describe() const {
  if (!active) {
    return "stereo bed on the front pair";
  }
  static const char* kInputNames[] = {"front L", "front R", "dialog common", "dialog L", "dialog R",
                                      "music L", "music R", "ambient L",     "ambient R"};
  std::string out;
  for (size_t ch = 0; ch < speakers.size(); ch++) {
    std::string row;
    for (int i = 0; i < kInputs; i++) {
      if (gain[ch][i] != 0.f) {
        row += fmt::format("{}{} {:+.1f} dB", row.empty() ? "" : ", ", kInputNames[i],
                           20.f * std::log10(std::abs(gain[ch][i])));
      }
    }
    out += fmt::format("{}{}: {}", out.empty() ? "" : "; ", SpeakerShort(speakers[ch]),
                       row.empty() ? std::string("-") : row);
  }
  return out;
}

float BedRouting::Channel(size_t ch, const std::array<float, kInputs>& in) const {
  float v = 0.f;
  for (int i = 0; i < kInputs; i++) {
    v += gain[ch][i] * in[i];
  }
  return v;
}

std::array<float, 3> BedRouting::SplitDialog(float l, float r) {
  const float c = l * r > 0.f ? (std::abs(l) < std::abs(r) ? l : r) : 0.f;
  return {c, l - c, r - c};
}

float VanillaPanPowerGain(float bearing_deg) {
  // the overlord's gPanTable (ssound.cpp): near side 0x3fff, far side linear from 0x3fff ahead
  // and behind to 0xc00 at the side
  float a = std::fmod(std::abs(bearing_deg), 360.f);
  if (a > 180.f) {
    a = 360.f - a;
  }
  const float off_axis = std::min(a, 180.f - a);  // 0 ahead or behind, 90 at the side
  auto far_side = [](float d) { return (0xc00 + (90.f - d) * 0x33ff / 90.f) / 0x3fff; };
  const float side_power = 1.f + far_side(90.f) * far_side(90.f);
  return std::sqrt((1.f + far_side(off_axis) * far_side(off_axis)) / side_power);
}

void SpeakerTest::Start(int channels, std::string context) {
  std::scoped_lock l(mLock);
  mSlots = Slots(channels, &mLabels);
  mContext = std::move(context);
  mFrame = 0;
  mLogged = -1;
  mLow = mBand = 0;
  // test hook: quieter bursts for loopback proofs on a device someone may be listening to
  const char* gain_db = std::getenv("SANDOVER_SPEAKER_TEST_GAIN_DB");
  mGain = gain_db ? std::pow(10.f, (float)std::atof(gain_db) / 20.f) : 1.f;
  mSegment = 0;
  mActive = true;
  lg::info("[audio] speaker test start: {} bursts; {}{}", mSlots.size(), mContext,
           gain_db ? fmt::format(" (test gain {} dB)", gain_db) : std::string());
}

void SpeakerTest::Render(float* out, int frames, int stride) {
  std::scoped_lock l(mLock);
  if (!mActive) {
    return;
  }
  constexpr int period = kOn + kOff;
  constexpr int fade = 480;  // 10 ms
  const int n = (int)mSlots.size();
  for (int i = 0; i < frames; i++, mFrame++) {
    const int seg = mFrame / period;
    if (seg >= n) {
      mActive = false;
      mSegment = -1;
      lg::info("[audio] speaker test done");
      return;
    }
    if (seg != mLogged) {
      mLogged = seg;
      mSegment = seg;
      lg::info("[audio] speaker test {}/{}: {} (output channel {})", seg + 1, n, mLabels[seg],
               mSlots[seg]);
    }
    const int pos = mFrame % period;
    if (pos >= kOn) {
      continue;
    }
    const float env = std::min({1.f, pos / (float)fade, (kOn - pos) / (float)fade});
    float v;
    if (mSlots[seg] == 3 && n >= 6) {
      v = 0.35f * std::sin(2.f * 3.14159265f * 50.f * pos / 48000.f);  // LFE
    } else {
      // white noise through a state-variable band-pass near 1 kHz: easy to place by ear
      mNoise = mNoise * 1664525u + 1013904223u;
      const float white = ((mNoise >> 9) / 4194304.f) - 1.f;
      mLow += 0.13f * mBand;
      const float high = white - mLow - 0.7f * mBand;
      mBand += 0.13f * high;
      v = 0.5f * mBand;
    }
    out[(size_t)i * stride + mSlots[seg]] += v * env * mGain;
  }
}

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
    mCtx[i] = {this, i, false};
    mCtx[kSlots + 1 + i] = {this, i, true};
  }
  mCtx[kSlots] = {this, -1, false};
}

bool SpatialOutput::Route(u32 owner, s16Output sample) {
  for (auto& s : mSlots) {
    if (s.owner == owner) {
      s.acc += (sample.left + sample.right) >> 1;
      s.acc_side += (sample.left - sample.right) >> 1;
      return true;
    }
  }
  return false;
}

bool SpatialOutput::RouteRole(VoiceRole role, s16Output sample) {
  if (!mRouting.active) {
    return false;
  }
  auto& acc = mRoleAcc[(int)role];
  acc[0] += sample.left;
  acc[1] += sample.right;
  return true;
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
    if (mRouting.active) {
      const s32 w = std::clamp<s32>(s.acc_side, INT16_MIN, INT16_MAX);
      const s16 side = static_cast<s16>((w * master_volume) >> 15);
      if (!s.side_live && side != 0) {
        // first width: the side source starts here, its queue lined up with the mid's
        s.side.assign(s.queue.size() - 1, 0);
        s.side_live = true;
        s.side_start_pending = true;
      }
      if (s.side_live) {
        s.side.push_back(side);
        if (s.side.size() > kMaxQueue) {
          s.side.pop_front();
        }
      }
    }
    s.acc_side = 0;
  }
  if (!mRouting.active) {
    mBed.push_back(bed.left);
    mBed.push_back(bed.right);
    if (mBed.size() > 2 * kMaxQueue) {
      mBed.pop_front();
      mBed.pop_front();
    }
    return;
  }

  // the role sums take the master volume the synth gave the bed
  std::array<std::array<float, 2>, 4> role{};
  for (int r = 1; r < 4; r++) {
    for (int c = 0; c < 2; c++) {
      const s32 v = std::clamp<s32>(mRoleAcc[r][c], INT16_MIN, INT16_MAX);
      role[r][c] = (float)((v * master_volume) >> 15) / 32768.f;
      mRoleAcc[r][c] = 0;
    }
  }
  const auto& music = role[(int)VoiceRole::Music];
  const auto& dialog = role[(int)VoiceRole::Dialog];
  const auto& ambient = role[(int)VoiceRole::Ambient];

  using I = BedRouting::Input;
  std::array<float, BedRouting::kInputs> in{};
  in[I::FrontL] = bed.left / 32768.f + music[0] + ambient[0];
  in[I::FrontR] = bed.right / 32768.f + music[1] + ambient[1];
  const auto split = BedRouting::SplitDialog(dialog[0], dialog[1]);
  in[I::DialogCentre] = split[0];
  in[I::DialogL] = split[1];
  in[I::DialogR] = split[2];
  float* line = &mBleedLine[(size_t)mBleedPos * 4];
  in[I::MusicL] = line[0];
  in[I::MusicR] = line[1];
  in[I::AmbientL] = line[2];
  in[I::AmbientR] = line[3];
  line[0] = music[0];
  line[1] = music[1];
  line[2] = ambient[0];
  line[3] = ambient[1];
  mBleedPos = (mBleedPos + 1) % BedRouting::kBleedDelay;

  const size_t ch = mRouting.gain.size();
  for (size_t c = 0; c < ch; c++) {
    mBedOut.push_back(mRouting.Channel(c, in));
  }
  if (mBedOut.size() > ch * kMaxQueue) {
    mBedOut.erase(mBedOut.begin(), mBedOut.begin() + (std::ptrdiff_t)ch);
  }
}

size_t SpatialOutput::BedFrames() const {
  return mRouting.active ? mBedOut.size() / mRouting.gain.size() : mBed.size() / 2;
}

bool SpatialOutput::Claim(u32 owner, const SpatialVec& pos) {
  if (owner == 0) {
    return false;
  }
  for (auto& s : mSlots) {
    if (s.owner == 0) {
      s.owner = owner;
      s.acc = 0;
      s.acc_side = 0;
      s.queue.clear();
      s.side.clear();
      s.pos = pos;
      s.pos_dirty = true;
      s.start_pending = true;
      s.first_fill = true;
      s.side_live = false;
      s.side_start_pending = false;
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
      s.acc_side = 0;
      s.queue.clear();
      s.side.clear();
      s.start_pending = false;
      s.side_live = false;
      s.side_start_pending = false;
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

void SpatialOutput::TrimFirstFill(Slot& s) {
  // Drop what queued up before the sources started, to line up with the bed; the side queue
  // loses the same samples, so the pair stays aligned whichever source is pulled first.
  if (!s.first_fill) {
    return;
  }
  s.first_fill = false;
  const size_t bed = BedFrames();
  if (s.queue.size() > bed) {
    const size_t drop = s.queue.size() - bed;
    s.queue.erase(s.queue.begin(), s.queue.begin() + (std::ptrdiff_t)drop);
    s.side.erase(s.side.begin(), s.side.begin() + (std::ptrdiff_t)std::min(drop, s.side.size()));
  }
}

int SpatialOutput::Fill(int slot, s16* dst, int frames) {
  std::scoped_lock lock(mLock);
  if (slot >= 0 && mSlots[slot].owner == 0) {
    return 0;  // ends the source until it is claimed again
  }
  auto& q = slot < 0 ? mBed : mSlots[slot].queue;
  if (slot >= 0) {
    TrimFirstFill(mSlots[slot]);
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

int SpatialOutput::FillSide(int slot, s16* dst, int frames) {
  // stereo frames: +S on the channel left of the sound's direction, -S on the right one
  std::scoped_lock lock(mLock);
  auto& s = mSlots[slot];
  if (s.owner == 0 || !s.side_live) {
    return 0;
  }
  TrimFirstFill(s);
  for (int guard = 0; s.side.size() < (size_t)frames && guard < frames; guard++) {
    mTick(*this);
    if (s.owner == 0) {
      break;
    }
  }
  const size_t n = std::min((size_t)frames, s.side.size());
  for (size_t i = 0; i < n; i++) {
    const s16 v = s.side[i];
    dst[2 * i] = v;
    dst[2 * i + 1] = static_cast<s16>(std::max<s32>(-v, INT16_MIN + 1));
  }
  s.side.erase(s.side.begin(), s.side.begin() + (std::ptrdiff_t)n);
  std::fill(dst + 2 * n, dst + 2 * (size_t)frames, 0);
  return frames;
}

int SpatialOutput::FillBed(float* dst, int frames) {
  std::scoped_lock lock(mLock);
  const size_t ch = mRouting.gain.size();
  const size_t need = (size_t)frames * ch;
  for (int guard = 0; mBedOut.size() < need && guard < frames; guard++) {
    mTick(*this);
  }
  const size_t n = std::min(need, mBedOut.size());
  std::copy_n(mBedOut.begin(), n, dst);
  mBedOut.erase(mBedOut.begin(), mBedOut.begin() + (std::ptrdiff_t)n);
  std::fill(dst + n, dst + need, 0.f);
  return frames;
}

#ifdef SANDOVER_SPATIAL_AUDIO

namespace {
bool g_openal_used = false;

// Objects mode needs OpenAL Soft's WASAPI spatial stream, a config option read once per
// process, so it must be set before OpenAL's first call.
bool g_objects_configured = false;

#ifdef _WIN32
std::string Utf8(const wchar_t* w) {
  if (!w) {
    return {};
  }
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  std::string out(n > 1 ? n - 1 : 0, ' ');
  if (n > 1) {
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
  }
  return out;
}

// PKEY_Device_FriendlyName (functiondiscoverykeys_devpkey.h needs the full windows.h)
const PROPERTYKEY kFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}},
    14};

// Windows.Media.Audio's spatial format classes, declared here (vtable order as in the SDK's
// windows.media.audio.h) so that this file needs neither C++/WinRT nor runtimeobject.lib.
struct IWinSpatialDeviceConfig : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_DeviceId(HSTRING* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_IsSpatialAudioSupported(boolean* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE IsSpatialAudioFormatSupported(HSTRING subtype,
                                                                  boolean* result) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_ActiveSpatialAudioFormat(HSTRING* value) = 0;
};
struct IWinSpatialDeviceConfigStatics : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE GetForDeviceId(HSTRING device_id,
                                                   IWinSpatialDeviceConfig** result) = 0;
};
struct IWinSpatialFormatSubtypeStatics : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_WindowsSonic(HSTRING* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_DolbyAtmosForHeadphones(HSTRING* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_DolbyAtmosForHomeTheater(HSTRING* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_DolbyAtmosForSpeakers(HSTRING* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_DTSHeadphoneX(HSTRING* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_DTSXUltra(HSTRING* value) = 0;
};
struct IWinSpatialFormatSubtypeStatics2 : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_DTSXForHomeTheater(HSTRING* value) = 0;
};
constexpr IID kIidDeviceConfigStatics = {0x3ec37f7b,
                                         0x936d,
                                         0x4e04,
                                         {0x97, 0x28, 0x28, 0x27, 0xd9, 0xf7, 0x58, 0xc4}};
constexpr IID kIidSubtypeStatics = {0xb3de8a47,
                                    0x83ee,
                                    0x4266,
                                    {0xa9, 0x45, 0xbe, 0xdf, 0x50, 0x7a, 0xfe, 0xed}};
constexpr IID kIidSubtypeStatics2 = {0x4565e6cb,
                                     0xd95b,
                                     0x5621,
                                     {0xb6, 0xaf, 0x0e, 0x88, 0x49, 0xc5, 0x7c, 0x80}};

// combase's WinRT string and activation entry points, looked up at run time
struct WinRt {
  using CreateFn = HRESULT(WINAPI*)(PCWSTR, UINT32, HSTRING*);
  using DeleteFn = HRESULT(WINAPI*)(HSTRING);
  using RawFn = PCWSTR(WINAPI*)(HSTRING, UINT32*);
  using FactoryFn = HRESULT(WINAPI*)(HSTRING, REFIID, void**);
  CreateFn create{nullptr};
  DeleteFn del{nullptr};
  RawFn raw{nullptr};
  FactoryFn factory{nullptr};
  WinRt() {
    if (HMODULE m = LoadLibraryW(L"combase.dll")) {
      create = reinterpret_cast<CreateFn>(GetProcAddress(m, "WindowsCreateString"));
      del = reinterpret_cast<DeleteFn>(GetProcAddress(m, "WindowsDeleteString"));
      raw = reinterpret_cast<RawFn>(GetProcAddress(m, "WindowsGetStringRawBuffer"));
      factory = reinterpret_cast<FactoryFn>(GetProcAddress(m, "RoGetActivationFactory"));
    }
  }
  bool ok() const { return create && del && raw && factory; }
  std::wstring Take(HSTRING s) const {
    if (!s) {
      return {};
    }
    UINT32 n = 0;
    PCWSTR p = raw(s, &n);
    std::wstring out(p ? p : L"", n);
    del(s);
    return out;
  }
  void* Factory(const wchar_t* cls, const IID& iid) const {
    HSTRING name = nullptr;
    void* out = nullptr;
    if (SUCCEEDED(create(cls, (UINT32)wcslen(cls), &name))) {
      if (FAILED(factory(name, iid, &out))) {
        out = nullptr;
      }
      del(name);
    }
    return out;
  }
};

// The format Windows has enabled on an endpoint (`mmdevice_id` as IMMDevice::GetId gives it).
SpatialFormat ActiveSpatialFormat(const std::wstring& mmdevice_id, std::string* raw) {
  static const WinRt rt;
  if (!rt.ok()) {
    return SpatialFormat::Unreadable;
  }
  auto* statics = static_cast<IWinSpatialDeviceConfigStatics*>(
      rt.Factory(L"Windows.Media.Audio.SpatialAudioDeviceConfiguration", kIidDeviceConfigStatics));
  auto* subtypes = static_cast<IWinSpatialFormatSubtypeStatics*>(
      rt.Factory(L"Windows.Media.Audio.SpatialAudioFormatSubtype", kIidSubtypeStatics));
  SpatialFormat result = SpatialFormat::Unreadable;
  if (statics && subtypes) {
    // the WinRT id of an endpoint is its audio-render device interface path
    const std::wstring id =
        L"\\\\?\\SWD#MMDEVAPI#" + mmdevice_id + L"#{e6327cad-dcec-4949-ae8a-991e976a79d2}";
    HSTRING hid = nullptr;
    IWinSpatialDeviceConfig* cfg = nullptr;
    if (SUCCEEDED(rt.create(id.c_str(), (UINT32)id.size(), &hid)) &&
        SUCCEEDED(statics->GetForDeviceId(hid, &cfg)) && cfg) {
      HSTRING active = nullptr;
      if (SUCCEEDED(cfg->get_ActiveSpatialAudioFormat(&active))) {
        std::wstring fmt = rt.Take(active);
        *raw = Utf8(fmt.c_str());
        // "Off" in Windows' Sound settings reads as the null GUID
        if (fmt == L"{00000000-0000-0000-0000-000000000000}") {
          fmt.clear();
        }
        result = fmt.empty() ? SpatialFormat::None : SpatialFormat::Other;
        using Getter = HRESULT (STDMETHODCALLTYPE IWinSpatialFormatSubtypeStatics::*)(HSTRING*);
        const std::pair<Getter, SpatialFormat> known[] = {
            {&IWinSpatialFormatSubtypeStatics::get_WindowsSonic, SpatialFormat::WindowsSonic},
            {&IWinSpatialFormatSubtypeStatics::get_DolbyAtmosForHeadphones,
             SpatialFormat::DolbyAtmosHeadphones},
            {&IWinSpatialFormatSubtypeStatics::get_DolbyAtmosForHomeTheater,
             SpatialFormat::DolbyAtmosHomeTheater},
            {&IWinSpatialFormatSubtypeStatics::get_DolbyAtmosForSpeakers,
             SpatialFormat::DolbyAtmosSpeakers},
            {&IWinSpatialFormatSubtypeStatics::get_DTSHeadphoneX, SpatialFormat::DtsHeadphoneX},
            {&IWinSpatialFormatSubtypeStatics::get_DTSXUltra, SpatialFormat::DtsXUltra},
        };
        for (auto& [get, f] : known) {
          HSTRING s = nullptr;
          if (!fmt.empty() && SUCCEEDED((subtypes->*get)(&s)) &&
              _wcsicmp(rt.Take(s).c_str(), fmt.c_str()) == 0) {
            result = f;
          }
        }
        IWinSpatialFormatSubtypeStatics2* subtypes2 = nullptr;
        if (!fmt.empty() && SUCCEEDED(subtypes->QueryInterface(
                                kIidSubtypeStatics2, reinterpret_cast<void**>(&subtypes2)))) {
          HSTRING s = nullptr;
          if (SUCCEEDED(subtypes2->get_DTSXForHomeTheater(&s)) &&
              _wcsicmp(rt.Take(s).c_str(), fmt.c_str()) == 0) {
            result = SpatialFormat::DtsXHomeTheater;
          }
          subtypes2->Release();
        }
      }
      cfg->Release();
    }
    if (hid) {
      rt.del(hid);
    }
  }
  if (statics) {
    statics->Release();
  }
  if (subtypes) {
    subtypes->Release();
  }
  return result;
}

std::string FriendlyName(IMMDevice* dev) {
  IPropertyStore* props = nullptr;
  std::string name;
  if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
    PROPVARIANT v;
    PropVariantInit(&v);
    if (SUCCEEDED(props->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR) {
      name = Utf8(v.pwszVal);
    }
    PropVariantClear(&v);
    props->Release();
  }
  return name;
}
#endif

// OpenAL Soft lists "OpenAL Soft on <friendly name>", with " #2" on duplicates.
std::string StripAlName(std::string name) {
  constexpr std::string_view prefix = "OpenAL Soft on ";
  if (name.rfind(prefix, 0) == 0) {
    name = name.substr(prefix.size());
  }
  auto hash = name.rfind(" #");
  if (hash != std::string::npos && hash + 2 < name.size() &&
      std::all_of(name.begin() + hash + 2, name.end(),
                  [](char c) { return c >= '0' && c <= '9'; })) {
    name = name.substr(0, hash);
  }
  return name;
}

}  // namespace

// Returns false when the process cannot run the spatial stream (OpenAL Soft was configured
// without it, or the endpoint has no spatial format); the caller then opens the bed.
bool ConfigureForObjects(const std::string& device) {
#ifdef _WIN32
  if (g_objects_configured) {
    return true;
  }
  if (g_openal_used) {
    lg::warn(
        "[spatial] objects mode must be set up before OpenAL's first call (the saved Spatial "
        "output at start, or --audio-spatial objects); OpenAL is already configured");
    return false;
  }
  int budget = QueryEndpoint(device).spatial_objects;
  if (const char* force = std::getenv("SANDOVER_SPATIAL_OBJECT_BUDGET")) {
    budget = (int)std::strtol(force, nullptr, 10);
  }
  lg::info("[spatial] endpoint dynamic-object budget {}", budget);
  if (budget <= 0) {
    lg::warn("[spatial] no Windows Spatial Sound format on this endpoint; using the channel bed");
    return false;
  }
  wchar_t exe[MAX_PATH];
  DWORD len = GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring path(exe, len);
  path = path.substr(0, path.find_last_of(L"\\/") + 1) + L"sandover-alsoft-objects.ini";
  if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    lg::warn("[spatial] {} is missing next to gk; Windows Spatial Sound not set up",
             Utf8(path.c_str()));
    return false;
  }
  if (const wchar_t* own = _wgetenv(L"ALSOFT_CONF"); own && *own) {
    lg::info("[spatial] ALSOFT_CONF was {}; replaced with {} for Spatial", Utf8(own),
             Utf8(path.c_str()));
  }
  _wputenv_s(L"ALSOFT_CONF", path.c_str());
  g_objects_configured = true;
  return true;
#else
  (void)device;
  return false;
#endif
}

bool ObjectsConfigured() {
  return g_objects_configured;
}

bool ConfigureObjectsAtStart(const std::string& device) {
  const bool ok = ConfigureForObjects(device);
  lg::info("[audio] saved output spatial on {}: {}",
           device.empty() ? std::string("the Windows default device") : "'" + device + "'",
           ok ? "Windows Spatial Sound set up for this process"
              : "not set up (no spatial format on the device); surround until a restart");
  return ok;
}

namespace {

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
      return "7.1.4 bed (Windows Spatial Sound stream)";
    default:
      return fmt::format("0x{:x}", mode);
  }
}

// A float buffer format in the layout of `channels` (LayoutSpeakers' order); stereo otherwise.
ALenum BufferFormat(int channels) {
  switch (channels) {
    case 1:
      return AL_FORMAT_MONO_FLOAT32;
    case 4:
      return AL_FORMAT_QUAD32;
    case 6:
      return AL_FORMAT_51CHN32;
    case 7:
      return AL_FORMAT_61CHN32;
    case 8:
      return AL_FORMAT_71CHN32;
    case 12:
      return AL_FORMAT_714CHN_FLOAT32_SANDOVER;
    default:
      return AL_FORMAT_STEREO_FLOAT32;
  }
}

int ChannelCount(ALCint mode) {
  switch (mode) {
    case ALC_7POINT1POINT4_SANDOVER:
      return 12;
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

EndpointInfo QueryEndpoint(const std::string& device) {
  EndpointInfo info;
#ifdef _WIN32
  const std::string want = StripAlName(device);
  bool co = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
  IMMDeviceEnumerator* en = nullptr;
  IMMDevice* def = nullptr;
  IMMDevice* found = nullptr;
  std::wstring def_id;
  if (SUCCEEDED(
          CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) {
    // OpenAL Soft and cubeb follow the multimedia role's default
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eMultimedia, &def))) {
      LPWSTR id = nullptr;
      if (SUCCEEDED(def->GetId(&id))) {
        def_id = id;
        CoTaskMemFree(id);
      }
    }
    if (want.empty()) {
      found = def;
      if (found) {
        found->AddRef();
      }
    } else {
      IMMDeviceCollection* list = nullptr;
      UINT count = 0;
      if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &list))) {
        list->GetCount(&count);
        for (UINT i = 0; i < count && !found; i++) {
          IMMDevice* d = nullptr;
          if (SUCCEEDED(list->Item(i, &d))) {
            if (FriendlyName(d) == want) {
              found = d;
            } else {
              d->Release();
            }
          }
        }
        list->Release();
      }
    }
  }
  if (found) {
    info.found = true;
    info.name = FriendlyName(found);
    LPWSTR id = nullptr;
    if (SUCCEEDED(found->GetId(&id))) {
      info.is_default = def_id == id;
      CoTaskMemFree(id);
    }
    IAudioClient* client = nullptr;
    if (SUCCEEDED(found->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void**>(&client)))) {
      WAVEFORMATEX* wfx = nullptr;
      if (SUCCEEDED(client->GetMixFormat(&wfx)) && wfx) {
        info.channels = wfx->nChannels;
        if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wfx->cbSize >= 22) {
          info.mask = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(wfx)->dwChannelMask;
        }
        CoTaskMemFree(wfx);
      }
      client->Release();
    }
    ISpatialAudioClient* sac = nullptr;
    if (SUCCEEDED(found->Activate(__uuidof(ISpatialAudioClient), CLSCTX_INPROC_SERVER, nullptr,
                                  reinterpret_cast<void**>(&sac)))) {
      UINT32 objects = 0;
      AudioObjectType mask = AudioObjectType_None;
      if (SUCCEEDED(sac->GetMaxDynamicObjectCount(&objects))) {
        info.spatial_objects = (int)objects;
      }
      if (SUCCEEDED(sac->GetNativeStaticObjectTypeMask(&mask))) {
        info.spatial_static_mask = (u32)mask;
      }
      sac->Release();
    }
    if (SUCCEEDED(found->GetId(&id))) {
      info.spatial_format = ActiveSpatialFormat(id, &info.spatial_format_id);
      CoTaskMemFree(id);
    }
    found->Release();
  }
  if (def) {
    def->Release();
  }
  if (en) {
    en->Release();
  }
  if (co) {
    CoUninitialize();
  }
#else
  (void)device;
#endif
  return info;
}

std::vector<std::string> ListOutputDevices() {
  if (!g_openal_used && g_spatial_mode_request == SpatialMode::Objects) {
    ConfigureForObjects("");  // OpenAL Soft reads its config on its first call
  }
  g_openal_used = true;
  std::vector<std::string> out;
  const bool all = alcIsExtensionPresent(nullptr, "ALC_ENUMERATE_ALL_EXT");
  const char* list = alcGetString(nullptr, all ? ALC_ALL_DEVICES_SPECIFIER : ALC_DEVICE_SPECIFIER);
  while (list && *list) {
    out.emplace_back(list);
    list += out.back().size() + 1;
  }
  return out;
}

int SpatialOutput::TestCallback(void* user, void* data, int bytes) {
  auto* self = static_cast<SpatialOutput*>(user);
  if (self->mTest.Current() < 0) {
    return 0;  // ends the source until the next test
  }
  const int ch = self->mTestChannels;
  const int frames = bytes / (4 * ch);
  auto* out = static_cast<float*>(data);
  std::fill(out, out + (size_t)frames * ch, 0.f);
  self->mTest.Render(out, frames, ch);
  return frames * 4 * ch;
}

void SpatialOutput::StartSpeakerTest(const std::string& context) {
  alSourceStop(mTestSource);
  mTest.Start(mTestChannels, context);
  alSourcePlay(mTestSource);
}

int SpatialOutput::Callback(void* user, void* data, int bytes) {
  auto* ctx = static_cast<CallbackCtx*>(user);
  auto* self = ctx->self;
  if (ctx->side) {
    return self->FillSide(ctx->slot, static_cast<s16*>(data), bytes / 4) * 4;
  }
  if (ctx->slot < 0 && self->mRouting.active) {
    const int frame_bytes = 4 * (int)self->mRouting.gain.size();
    return self->FillBed(static_cast<float*>(data), bytes / frame_bytes) * frame_bytes;
  }
  const int frame_bytes = ctx->slot < 0 ? 4 : 2;
  int frames = self->Fill(ctx->slot, static_cast<s16*>(data), bytes / frame_bytes);
  return frames * frame_bytes;
}

std::unique_ptr<SpatialOutput> SpatialOutput::OpenDevice(SpatialMode mode,
                                                         std::recursive_mutex& lock,
                                                         TickFn tick,
                                                         SpatialLayout layout,
                                                         const std::string& device) {
  if (mode == SpatialMode::Off) {
    return nullptr;
  }
  if (mode == SpatialMode::Objects && !ConfigureForObjects(device)) {
    lg::info("[spatial] objects: decoding to the speaker layout instead");
  }
  g_openal_used = true;
  ALCdevice* dev = alcOpenDevice(device.empty() ? nullptr : device.c_str());
  if (!dev && !device.empty()) {
    lg::warn("[spatial] could not open '{}', using the Windows default device", device);
    dev = alcOpenDevice(nullptr);
  }
  if (!dev) {
    lg::error("[spatial] alcOpenDevice failed");
    return nullptr;
  }
  auto out = std::unique_ptr<SpatialOutput>(new SpatialOutput(mode, lock, std::move(tick)));
  out->mLayout = mode == SpatialMode::Surround ? layout : SpatialLayout::Device;
  if (!out->InitContext(dev, false, 0)) {
    return nullptr;
  }
  return out;
}

std::unique_ptr<SpatialOutput> SpatialOutput::OpenLoopback(SpatialMode mode,
                                                           int channels,
                                                           std::recursive_mutex& lock,
                                                           TickFn tick,
                                                           bool routing) {
  g_openal_used = true;
  ALCdevice* dev = alcLoopbackOpenDeviceSOFT(nullptr);
  if (!dev) {
    lg::error("[spatial] alcLoopbackOpenDeviceSOFT failed");
    return nullptr;
  }
  auto out = std::unique_ptr<SpatialOutput>(new SpatialOutput(mode, lock, std::move(tick)));
  out->mRoutingAllowed = routing;
  if (!out->InitContext(dev, true, channels)) {
    return nullptr;
  }
  return out;
}

bool SpatialOutput::InitContext(void* device, bool loopback, int channels) {
  auto* dev = static_cast<ALCdevice*>(device);
  mDevice = dev;

  std::vector<ALCint> attrs = {ALC_FREQUENCY,      kRate,     ALC_MONO_SOURCES, kSlots + 4,
                               ALC_STEREO_SOURCES, kSlots + 4};
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
    // a forced layout; Windows' shared mode still caps it at the endpoint's own layout
    if (!loopback && mLayout != SpatialLayout::Device) {
      const ALCint forced = mLayout == SpatialLayout::Stereo ? ALC_STEREO_BASIC_SOFT
                            : mLayout == SpatialLayout::X51  ? ALC_SURROUND_5_1_SOFT
                                                             : ALC_SURROUND_7_1_SOFT;
      attrs.insert(attrs.end(), {ALC_OUTPUT_MODE_SOFT, forced});
    }
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
  // the spatial stream's bed (ALC_ANY_SOFT) is 7.1.4: the test adds its four height channels
  // when the endpoint's format renders them
  std::string spatial;
  mTestChannels = mOutChannels == 12 ? 12 : std::min(mOutChannels, 8);
  if (!loopback && out_mode == ALC_ANY_SOFT) {
    const auto ep = QueryEndpoint(name ? name : "");
    mOutChannels = 12;
    mTestChannels = SpatialMaskHasHeights(ep.spatial_static_mask) ? 12 : 8;
    spatial = fmt::format(", format {}, native static mask 0x{:x}, heights {}",
                          SpatialFormatName(ep.spatial_format), ep.spatial_static_mask,
                          mTestChannels == 12 ? "yes" : "no");
  }
  mDeviceName = name ? name : "";
  mLayoutName = OutputModeName(loopback ? 0 : out_mode);
  if (loopback) {
    mLayoutName = fmt::format("loopback {} ch", mOutChannels);
  }
  mDescription = fmt::format(
      "mode {} on '{}': output {}{}, hrtf {}, {} Hz", SpatialModeName(mMode), name ? name : "?",
      loopback && mOutChannels == 12 ? std::string("7.1.4") : OutputModeName(out_mode), spatial,
      hrtf == ALC_HRTF_ENABLED_SOFT ? "on" : "off", freq);
  if (mLayout != SpatialLayout::Device) {
    static const char* kLayoutNames[] = {"device", "stereo", "5.1", "7.1"};
    mDescription += fmt::format(" ({} requested)", kLayoutNames[(int)mLayout]);
  }

  alDistanceModel(AL_NONE);

  // the non-positional routing, by speaker name from the layout the speaker test plays in;
  // headphones keep the stereo bed
  mRouting = ResolveBedRouting(mTestChannels);
  if (mMode == SpatialMode::Binaural || !mRoutingAllowed) {
    mRouting.active = false;
  }
  mBleedLine.assign((size_t)BedRouting::kBleedDelay * 4, 0.f);
  mBleedPos = 0;
  lg::info("[spatial] routing ({} ch): {}", mTestChannels, mRouting.Describe());

  for (int i = 0; i < kSlots; i++) {
    auto& s = mSlots[i];
    alGenBuffers(1, &s.buffer);
    alBufferCallbackSOFT(s.buffer, AL_FORMAT_MONO16, kRate,
                         reinterpret_cast<ALBUFFERCALLBACKTYPESOFT>(&Callback), &mCtx[i]);
    alGenSources(1, &s.source);
    alSourcei(s.source, AL_BUFFER, (ALint)s.buffer);
    if (mRouting.active) {
      // room for the vanilla loudness gain (+2.9 dB ahead), above AL_MAX_GAIN's default 1
      alSourcef(s.source, AL_MAX_GAIN, 2.f);
      // the authored width: a stereo pair (+S, -S) either side of the sound's direction
      alGenBuffers(1, &s.side_buffer);
      alBufferCallbackSOFT(s.side_buffer, AL_FORMAT_STEREO16, kRate,
                           reinterpret_cast<ALBUFFERCALLBACKTYPESOFT>(&Callback),
                           &mCtx[kSlots + 1 + i]);
      alGenSources(1, &s.side_source);
      alSourcei(s.side_source, AL_BUFFER, (ALint)s.side_buffer);
    }
  }
  alGenBuffers(1, &mBedBuffer);
  alBufferCallbackSOFT(mBedBuffer,
                       mRouting.active ? BufferFormat(mTestChannels) : AL_FORMAT_STEREO16, kRate,
                       reinterpret_cast<ALBUFFERCALLBACKTYPESOFT>(&Callback), &mCtx[kSlots]);
  alGenSources(1, &mBedSource);
  alSourcei(mBedSource, AL_BUFFER, (ALint)mBedBuffer);
  alSourcei(mBedSource, AL_DIRECT_CHANNELS_SOFT, AL_DROP_UNMATCHED_SOFT);
  alSourcePlay(mBedSource);

  // speaker test: a buffer in the output's own layout, each channel written straight to the
  // output channel of the same name (direct channels)
  const ALenum test_fmt = BufferFormat(mTestChannels);
  if (test_fmt == AL_FORMAT_STEREO_FLOAT32) {
    mTestChannels = 2;  // a layout without a buffer format: the front pair
  }
  alGenBuffers(1, &mTestBuffer);
  alBufferCallbackSOFT(mTestBuffer, test_fmt, kRate,
                       reinterpret_cast<ALBUFFERCALLBACKTYPESOFT>(&TestCallback), this);
  alGenSources(1, &mTestSource);
  alSourcei(mTestSource, AL_BUFFER, (ALint)mTestBuffer);
  alSourcei(mTestSource, AL_DIRECT_CHANNELS_SOFT, AL_DROP_UNMATCHED_SOFT);

  if (ALenum err = alGetError(); err != AL_NO_ERROR) {
    lg::error("[spatial] source setup failed: 0x{:x}", err);
    return false;
  }
  lg::info("[spatial] {}", mDescription);
  return true;
}

void SpatialOutput::Flush() {
  struct Pending {
    int slot;
    u32 owner;
    unsigned source;
    unsigned side_source;
    SpatialVec pos;
    bool pos_dirty;
    bool start;
    bool start_side;
  };
  std::array<Pending, kSlots> pending{};
  int count = 0;
  SpatialVec lpos;
  float yaw;
  bool mirror;
  {
    std::scoped_lock lock(mLock);
    for (int i = 0; i < kSlots; i++) {
      auto& s = mSlots[i];
      if (s.owner != 0 &&
          (s.pos_dirty || s.start_pending || s.side_start_pending || mListenerDirty)) {
        pending[count++] = {i,
                            s.owner,
                            s.source,
                            s.side_source,
                            s.pos,
                            s.pos_dirty || mListenerDirty,
                            s.start_pending,
                            s.side_start_pending};
        s.pos_dirty = false;
        s.start_pending = false;
        s.side_start_pending = false;
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
  // one batch: the mid and side of a sound change in the same mixer update
  alDeferUpdatesSOFT();
  for (int i = 0; i < count; i++) {
    auto& p = pending[i];
    if (p.pos_dirty) {
      const SpatialVec v = to_listener(p.pos);
      alSource3f(p.source, AL_POSITION, v.x, v.y, v.z);
      if (p.side_source) {
        // vanilla's loudness per bearing on the mid (faded to the sides' 0 dB toward overhead
        // and underfoot); the width pair at unity gain, +-90 deg from the direction
        const float horiz = std::sqrt(v.x * v.x + v.z * v.z);
        const float dist = std::sqrt(horiz * horiz + v.y * v.y);
        const float az = horiz > 0.f ? std::atan2(v.x, -v.z) : 0.f;
        const float flat = dist > 0.f ? horiz / dist : 0.f;
        const float gain = 1.f + (VanillaPanPowerGain(az * 180.f / 3.14159265f) - 1.f) * flat;
        alSourcef(p.source, AL_GAIN, gain);
        const ALfloat angles[2] = {-az + 1.57079633f, -az - 1.57079633f};
        alSourcefv(p.side_source, AL_STEREO_ANGLES, angles);
      }
    }
  }
  alProcessUpdatesSOFT();
  for (int i = 0; i < count; i++) {
    auto& p = pending[i];
    if (!p.start_side || p.start || !p.side_source) {
      continue;
    }
    // width that began after the mid started: restart the pair from aligned queues, so both
    // sources start in the same mixer update as on a claim
    alSourceStop(p.source);
    std::scoped_lock lock(mLock);
    auto& s = mSlots[p.slot];
    if (s.owner != p.owner) {
      p.start_side = false;
      continue;
    }
    if (s.side.size() > s.queue.size()) {
      s.side.erase(s.side.begin(),
                   s.side.begin() + (std::ptrdiff_t)(s.side.size() - s.queue.size()));
    }
    s.first_fill = true;
    p.start = true;
  }
  for (int i = 0; i < count; i++) {
    auto& p = pending[i];
    if (p.start && p.start_side && p.side_source) {
      const ALuint both[2] = {p.source, p.side_source};
      alSourcePlayv(2, both);
    } else if (p.start) {
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
      if (s.side_source) {
        alSourceStop(s.side_source);
        alDeleteSources(1, &s.side_source);
        alDeleteBuffers(1, &s.side_buffer);
      }
    }
    alSourceStop(mBedSource);
    alDeleteSources(1, &mBedSource);
    alDeleteBuffers(1, &mBedBuffer);
    alSourceStop(mTestSource);
    alDeleteSources(1, &mTestSource);
    alDeleteBuffers(1, &mTestBuffer);
    alcMakeContextCurrent(nullptr);
    alcDestroyContext(static_cast<ALCcontext*>(mContext));
  }
  if (mDevice) {
    alcCloseDevice(static_cast<ALCdevice*>(mDevice));
  }
}

#else

bool ObjectsConfigured() {
  return false;
}
bool ConfigureForObjects(const std::string&) {
  return false;
}
bool ConfigureObjectsAtStart(const std::string&) {
  return false;
}
int SpatialOutput::Callback(void*, void*, int) {
  return 0;
}
EndpointInfo QueryEndpoint(const std::string&) {
  return {};
}
std::vector<std::string> ListOutputDevices() {
  return {};
}
int SpatialOutput::TestCallback(void*, void*, int) {
  return 0;
}
void SpatialOutput::StartSpeakerTest(const std::string&) {}
std::unique_ptr<SpatialOutput> SpatialOutput::OpenDevice(SpatialMode,
                                                         std::recursive_mutex&,
                                                         TickFn,
                                                         SpatialLayout,
                                                         const std::string&) {
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
