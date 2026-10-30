// Copyright: 2021 - 2024, Ziemas
// SPDX-License-Identifier: ISC
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

#include "ame_handler.h"
#include "handle_allocator.h"
#include "loader.h"
#include "sound_handler.h"

#include "common/common_types.h"

#include "../common/synth.h"
#include "game/sound/989snd/spatial.h"
#include "game/sound/989snd/vagvoice.h"

#include "third-party/cubeb/cubeb/include/cubeb/cubeb.h"

namespace snd {

class Player {
 public:
  Player();
  ~Player();
  Player(const Player&) = delete;
  Player operator=(const Player&) = delete;

  // player(player&& other) noexcept = default;
  // player& operator=(player&& other) noexcept = default;

  BankHandle LoadBank(std::span<u8> bank);

  u32 PlaySound(BankHandle bank, u32 sound, s32 vol, s32 pan, s32 pm, s32 pb);
  u32 PlaySoundByName(BankHandle bank,
                      char* bank_name,
                      char* sound_name,
                      s32 vol,
                      s32 pan,
                      s32 pm,
                      s32 pb);
  void DebugPrintAllSoundsInBank(BankHandle bank);
  void SetSoundReg(u32 sound_id, u8 reg, u8 value);
  u8 GetSoundGroup(u32 sound_id);
  void SetGlobalExcite(u8 value) { GlobalExcite = value; };
  bool SoundStillActive(u32 sound_id);
  void SetMasterVolume(u32 group, s32 volume);
  void UnloadBank(BankHandle bank_handle);
  void StopSound(u32 sound_handle);
  u32 GetSoundID(u32 sound_handle);
  void SetPanTable(VolPair* pantable);
  void SetPlaybackMode(s32 mode);
  void PauseSound(s32 sound_handle);
  void ContinueSound(s32 sound_handle);
  void PauseAllSoundsInGroup(u8 group);
  void ContinueAllSoundsInGroup(u8 group);
  void SetSoundVolPan(s32 sound_handle, s32 vol, s32 pan);
  void SubmitVoice(std::shared_ptr<Voice>& voice) { mSynth.AddVoice(voice); };
  void SetSoundPmod(s32 sound_handle, s32 mod);
  void InitCubeb();
  void DestroyCubeb();
  s32 GetTick() { return mTick; };
  void StopAllSounds();
  s32 GetSoundUserData(BankHandle block_handle,
                       char* block_name,
                       s32 sound_id,
                       char* sound_name,
                       SFXUserData* dst);

  /// Switches the output between cubeb (off) and the OpenAL spatial backend.
  void SetSpatialMode(SpatialMode mode);
  /// The Audio menu's output and device (empty: Windows default); reopens only on a change.
  void SetAudioOutput(AudioOutput output, const std::string& device);
  AudioStatus GetAudioStatus();
  /// One burst per channel of the running output (cubeb: front left, front right).
  void StartSpeakerTest();
  SpatialMode GetSpatialMode();
  /// Position for the next PlaySound; it is spatialized if the backend has a free source.
  void SetNextSoundPosition(const SpatialVec& pos);
  void SetSoundPosition(u32 sound_handle, const SpatialVec& pos);
  void SetListener(const SpatialVec& pos, float yaw_deg, bool mirror);
  /// Pushes pending spatial changes to OpenAL; call without holding any sound lock.
  void FlushSpatial();

 private:
  std::recursive_mutex mTickLock;  // TODO does not need to recursive with some light restructuring
  IdAllocator mHandleAllocator;
  std::map<u32, std::unique_ptr<SoundHandler>> mHandlers;

  void Tick(s16Output* stream, int samples);
  void SpatialTick(SpatialOutput& out);

#ifdef _WIN32
  bool m_coinitialized = false;
#endif

  Loader mLoader;
  Synth mSynth;
  VoiceManager mVmanager;
  s32 mTick{0};

  void OpenOutput(SpatialMode mode,
                  SpatialLayout layout,
                  const std::string& device,
                  bool stereo_to_vanilla);

  std::mutex mSpatialSwap;  // held while the backend is replaced or flushed, never by the mixer
  std::unique_ptr<SpatialOutput> mSpatial;
  std::string mSpatialDevice;  // the device mSpatial was asked for ("" default)
  AudioOutput mOutputRequest{AudioOutput::Stereo};
  std::string mDeviceRequest;
  bool mOutputApplied{false};
  AudioOutput mRunningOutput{AudioOutput::Auto};  // whose plan runs (Spatial: Auto's)
  std::string mNote;
  SpeakerTest mCubebTest;  // under mTickLock
  std::vector<float> mTestScratch;
  std::optional<SpatialVec> mPendingPos;
  int mSpatialHtick{200};
  bool mCubebRunning{false};

  cubeb* mCtx{nullptr};
  cubeb_stream* mStream{nullptr};

  static long sound_callback(cubeb_stream* stream,
                             void* user,
                             const void* input,
                             void* output_buffer,
                             long len);
  static void state_callback(cubeb_stream* stream, void* user, cubeb_state state);
};
}  // namespace snd
