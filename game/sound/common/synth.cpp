// Copyright: 2021 - 2024, Ziemas
// SPDX-License-Identifier: ISC
#include "synth.h"

#include <stdexcept>

namespace snd {

static s16 ApplyVolume(s16 sample, s32 volume) {
  return (sample * volume) >> 15;
}

s16Output Synth::Tick() {
  s16Output out{};

  mVoices.remove_if([](std::shared_ptr<Voice>& v) { return v->Dead(); });
  for (auto& v : mVoices) {
    out += v->Run();
  }

  out.left = ApplyVolume(out.left, mVolume.left.Get());
  out.right = ApplyVolume(out.right, mVolume.right.Get());

  mVolume.Run();

  return out;
}

void Synth::TickRouted(VoiceRouter& router) {
  s16Output out{};

  mVoices.remove_if([](std::shared_ptr<Voice>& v) { return v->Dead(); });
  for (auto& v : mVoices) {
    s16Output s = v->Run();
    if (v->Owner() != 0 && router.Route(v->Owner(), s)) {
      continue;
    }
    if (v->Role() != VoiceRole::Effect && router.RouteRole(v->Role(), s)) {
      continue;
    }
    out += s;
  }

  out.left = ApplyVolume(out.left, mVolume.left.Get());
  out.right = ApplyVolume(out.right, mVolume.right.Get());
  router.EndSample(out, mVolume.left.Get());

  mVolume.Run();
}

void Synth::AddVoice(std::shared_ptr<Voice> voice) {
  mVoices.emplace_front(voice);
}

void Synth::SetMasterVol(u32 volume) {
  mVolume.left.Set(volume);
  mVolume.right.Set(volume);
}
}  // namespace snd
