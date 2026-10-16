// SPDX-License-Identifier: ISC
#pragma once

#include <array>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "common/common_types.h"

#include "game/sound/common/synth.h"

namespace snd {

enum class SpatialMode : int { Off = 0, Binaural = 1, Surround = 2, Objects = 3 };

const char* SpatialModeName(SpatialMode mode);
std::optional<SpatialMode> SpatialModeFromName(std::string_view name);

/// Mode requested before the sound system starts (command line); applied when the player is made.
extern SpatialMode g_spatial_mode_request;

/// A point in overlord sound units (game units / 16, so 256 per metre; y up).
struct SpatialVec {
  float x{0}, y{0}, z{0};
};

/**
 * Positional-voice output through OpenAL Soft.
 *
 * Each spatialized sound (one 989snd sound handle, all its voices summed to mono) gets one
 * OpenAL source at its world position; OpenAL Soft encodes every source into its ambisonic mix
 * and decodes that for the device: HRTF (binaural), the endpoint's speaker layout (surround), or
 * a Windows Spatial Sound static bed (objects). Voices of sounds without a position are mixed to
 * stereo exactly as on the cubeb path and played on the front pair.
 *
 * OpenAL pulls samples from its mixer thread (or from Render() on a loopback device); every pull
 * runs the synth forward through the tick function under the synth lock, so the synth is clocked
 * by this device and cubeb is stopped while it is open.
 */
class SpatialOutput : public VoiceRouter {
 public:
  static constexpr int kSlots = 32;
  static constexpr int kRate = 48000;
  using TickFn = std::function<void(SpatialOutput&)>;

  /// Opens the default output device. `lock` guards the synth, `tick` advances it one sample.
  static std::unique_ptr<SpatialOutput> OpenDevice(SpatialMode mode,
                                                   std::recursive_mutex& lock,
                                                   TickFn tick);
  /// Opens a loopback device for capture; `channels` is ALC_STEREO_SOFT, ALC_5POINT1_SOFT or
  /// ALC_7POINT1_SOFT (binaural needs stereo). Output is interleaved float.
  static std::unique_ptr<SpatialOutput> OpenLoopback(SpatialMode mode,
                                                     int channels,
                                                     std::recursive_mutex& lock,
                                                     TickFn tick);
  ~SpatialOutput() override;
  SpatialOutput(const SpatialOutput&) = delete;
  SpatialOutput& operator=(const SpatialOutput&) = delete;

  // Synth side: call with the synth lock held.
  bool Route(u32 owner, s16Output sample) override;
  void EndSample(s16Output bed, s32 master_volume) override;
  bool Claim(u32 owner, const SpatialVec& pos);
  void Release(u32 owner);
  bool Owns(u32 owner) const;
  void SetPosition(u32 owner, const SpatialVec& pos);
  void SetListener(const SpatialVec& pos, float yaw_deg, bool mirror);
  /// Releases every slot whose owner fails `alive`.
  void Prune(const std::function<bool(u32)>& alive);
  int ActiveSlots() const;
  u64 TotalClaims() const { return mTotalClaims; }

  /// Without the synth lock: applies pending starts and positions to OpenAL.
  void Flush();

  /// Loopback only: renders `frames` frames into `out` (OutputChannels() floats per frame).
  void Render(float* out, int frames);
  int OutputChannels() const { return mOutChannels; }
  SpatialMode Mode() const { return mMode; }
  /// What the device ended up as (output mode, HRTF, rate), for the log.
  const std::string& Describe() const { return mDescription; }

 private:
  struct Slot {
    u32 owner{0};
    s32 acc{0};
    std::deque<s16> queue;
    SpatialVec pos;
    bool pos_dirty{false};
    bool start_pending{false};
    bool first_fill{false};
    unsigned source{0};
    unsigned buffer{0};
  };
  struct CallbackCtx {
    SpatialOutput* self;
    int slot;  // -1: the stereo bed
  };

  SpatialOutput(SpatialMode mode, std::recursive_mutex& lock, TickFn tick);
  bool InitContext(void* device, bool loopback, int channels);
  static int Callback(void* user, void* data, int bytes);
  int Fill(int slot, s16* dst, int frames);

  SpatialMode mMode;
  std::recursive_mutex& mLock;
  TickFn mTick;

  void* mDevice{nullptr};
  void* mContext{nullptr};
  int mOutChannels{2};
  std::string mDescription;

  std::array<Slot, kSlots> mSlots{};
  std::array<CallbackCtx, kSlots + 1> mCtx{};
  std::deque<s16> mBed;  // interleaved stereo
  unsigned mBedSource{0};
  unsigned mBedBuffer{0};

  SpatialVec mListenerPos;
  float mListenerYaw{0};
  bool mListenerMirror{false};
  bool mListenerDirty{true};
  u64 mTotalClaims{0};
};

}  // namespace snd
