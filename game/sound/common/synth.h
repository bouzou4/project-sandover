// Copyright: 2021 - 2024, Ziemas
// SPDX-License-Identifier: ISC
#pragma once
#include <forward_list>
#include <memory>
#include <unordered_map>
#include <vector>

#include "envelope.h"
#include "sound_types.h"
#include "voice.h"

#include "common/common_types.h"

namespace snd {
struct SpuVolume {
  /*   0 */ s16 left;
  /*   2 */ s16 right;
};

/// Receives voices that belong to spatialized sounds instead of the stereo mix.
class VoiceRouter {
 public:
  virtual ~VoiceRouter() = default;
  /// Takes the voice's output if its owner is spatialized; returns false to mix it normally.
  virtual bool Route(u32 owner, s16Output sample) = 0;
  /// End of one output sample: the stereo mix of unrouted voices and the master volume.
  virtual void EndSample(s16Output bed, s32 master_volume) = 0;
};

class Synth {
 public:
  Synth() {
    mVolume.left.Set(0x3FFF);
    mVolume.right.Set(0x3FFF);
  }

  s16Output Tick();
  /// Same as Tick, but voices the router claims are handed to it instead of the mix.
  void TickRouted(VoiceRouter& router);
  void AddVoice(std::shared_ptr<Voice> voice);
  void SetMasterVol(u32 volume);

 private:
  std::forward_list<std::shared_ptr<Voice>> mVoices;

  VolumePair mVolume{};
};
}  // namespace snd
