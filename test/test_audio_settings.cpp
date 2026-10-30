// The audio keys gk reads from sandover.gc before the sound system starts (main.cpp), in the form
// the game's writer (sandover-settings-write) produces.

#include <cmath>
#include <string>
#include <vector>

#include "game/sound/989snd/spatial.h"
#include "gtest/gtest.h"

using snd::AudioOutput;

namespace {
// what sandover-settings-write prints, around the two audio keys
std::string settings_file(const std::string& output, const std::string& device) {
  return "(sandover 4\n  (vanilla-rendering? #f)\n  (camera-blur vehicles)\n  (audio-output " +
         output + ")\n  (audio-device \"" + device + "\")\n  )\n";
}
}  // namespace

TEST(AudioSettings, EveryOutputRoundTrips) {
  for (int i = 0; i <= (int)AudioOutput::Objects; i++) {
    const auto out = (AudioOutput)i;
    const auto saved = snd::ParseSavedAudio(settings_file(snd::SavedOutputName(out), ""));
    EXPECT_EQ(saved.output, out) << snd::SavedOutputName(out);
    EXPECT_EQ(saved.device, "");
  }
}

TEST(AudioSettings, SpatialIsSavedAsSpatial) {
  EXPECT_STREQ(snd::SavedOutputName(AudioOutput::Objects), "spatial");
  const auto saved = snd::ParseSavedAudio(
      settings_file("spatial", "OpenAL Soft on Speakers (Steam Streaming Speakers)"));
  EXPECT_EQ(saved.output, AudioOutput::Objects);
  EXPECT_EQ(saved.device, "OpenAL Soft on Speakers (Steam Streaming Speakers)");
}

TEST(AudioSettings, DeviceNamesKeepSpacesParenthesesAndNumbers) {
  const std::string name = "OpenAL Soft on Digital Output (USB Audio DAC) #2";
  const auto saved = snd::ParseSavedAudio(settings_file("5.1", name));
  EXPECT_EQ(saved.output, AudioOutput::X51);
  EXPECT_EQ(saved.device, name);
}

TEST(AudioSettings, MissingOrUnknownReadsAsTheDefault) {
  auto saved = snd::ParseSavedAudio("(sandover 4\n  (ssao? #t)\n  )\n");
  EXPECT_EQ(saved.output, AudioOutput::Auto);
  EXPECT_EQ(saved.device, "");
  saved = snd::ParseSavedAudio(settings_file("objects", ""));  // the engine's name, never saved
  EXPECT_EQ(saved.output, AudioOutput::Auto);
  saved = snd::ParseSavedAudio("");
  EXPECT_EQ(saved.output, AudioOutput::Auto);
}

TEST(AudioSettings, OnlyTheExactKeyCounts) {
  const auto saved = snd::ParseSavedAudio(
      "(sandover 4\n  (audio-output-old spatial)\n  (audio-output\t7.1)\n  (audio-device \"\")\n)");
  EXPECT_EQ(saved.output, AudioOutput::X71);
}

TEST(AudioSettings, HeightsNeedAllFourTopChannels) {
  EXPECT_TRUE(
      snd::SpatialMaskHasHeights(0xffffe));  // a native mask Windows reports with a format on
  EXPECT_TRUE(snd::SpatialMaskHasHeights(0x3c00));
  EXPECT_FALSE(snd::SpatialMaskHasHeights(0x3fe));  // 7.1 bed only
  EXPECT_FALSE(snd::SpatialMaskHasHeights(0x1c00));
}

TEST(AudioSettings, AnUnquotedDeviceReadsAsTheWindowsDefault) {
  // sandover-read-quoted (pckernel.gc): a bare word is the empty name
  auto saved =
      snd::ParseSavedAudio("(sandover 4\n  (audio-output spatial)\n  (audio-device #f)\n)");
  EXPECT_EQ(saved.output, AudioOutput::Objects);
  EXPECT_EQ(saved.device, "");
  saved = snd::ParseSavedAudio("(sandover 4\n  (audio-device Speakers)\n)");
  EXPECT_EQ(saved.device, "");
}

TEST(AudioSettings, TheStartReadsTheProfileOfTheMode) {
  // GOAL's sandover-profile-file: the flat screen's file, the VR profile's under --xr
  EXPECT_STREQ(snd::SavedAudioFileName(false), "sandover.gc");
  EXPECT_STREQ(snd::SavedAudioFileName(true), "sandover-vr.gc");
}

namespace {
void expect_step(bool configured,
                 bool first,
                 AudioOutput requested,
                 bool cli,
                 snd::OutputAction action,
                 bool pending) {
  EXPECT_EQ(snd::PlanOutputRequest(configured, first, requested, cli), action)
      << configured << first << (int)requested << cli;
  // the restart note, before the endpoint's budget is known
  EXPECT_EQ(snd::RestartPending(requested, configured, std::nullopt), pending)
      << configured << first << (int)requested << cli;
}
}  // namespace

TEST(AudioSettings, RestartStateMachine) {
  using A = snd::OutputAction;
  for (bool first : {true, false}) {
    for (bool cli : {true, false}) {
      // a spatial process: Spatial runs, every other output waits for a restart
      expect_step(true, first, AudioOutput::Objects, cli, A::Open, false);
      expect_step(true, first, AudioOutput::X51, cli, A::Keep, true);
      expect_step(true, first, AudioOutput::Stereo, cli, A::Keep, true);
      // any other process opens the other outputs
      expect_step(false, first, AudioOutput::Auto, cli, A::Open, false);
      expect_step(false, first, AudioOutput::Headphones, cli, A::Open, false);
    }
  }
  // Spatial picked in a process that was not set up at start
  expect_step(false, true, AudioOutput::Objects, false, A::Fallback, true);  // saved, at boot
  expect_step(false, false, AudioOutput::Objects, false, A::Keep, true);     // during a session
  // the command line's or REPL's objects opens the mode (decoded to the bed)
  expect_step(false, true, AudioOutput::Objects, true, A::Open, true);
  expect_step(false, false, AudioOutput::Objects, true, A::Open, true);
  // a CLI push while no row output is applied yet (mOutputApplied false)
  expect_step(false, true, AudioOutput::Auto, true, A::Open, false);
}

TEST(AudioSettings, NoRestartNoteWithoutASpatialFormat) {
  // Spatial asked for in a process without it: a restart helps only with a format on the device
  EXPECT_TRUE(snd::RestartPending(AudioOutput::Objects, false, 128));
  EXPECT_TRUE(snd::RestartPending(AudioOutput::Objects, false, std::nullopt));
  EXPECT_FALSE(snd::RestartPending(AudioOutput::Objects, false, 0));
  EXPECT_FALSE(snd::RestartPending(AudioOutput::Objects, false, -1));
  // a spatial process: another output always needs the restart
  EXPECT_TRUE(snd::RestartPending(AudioOutput::X51, true, 0));
  EXPECT_FALSE(snd::RestartPending(AudioOutput::Objects, true, 0));
  EXPECT_FALSE(snd::RestartPending(AudioOutput::Auto, false, 0));
}

namespace {
using snd::BedRouting;
using snd::Speaker;

int Index(const BedRouting& r, Speaker sp) {
  for (size_t i = 0; i < r.speakers.size(); i++) {
    if (r.speakers[i] == sp) {
      return (int)i;
    }
  }
  return -1;
}

float Db(float g) {
  return 20.f * std::log10(std::abs(g));
}
}  // namespace

TEST(AudioRouting, StereoKeepsTheStereoBed) {
  EXPECT_FALSE(snd::ResolveBedRouting(2).active);
  EXPECT_FALSE(snd::ResolveBedRouting(1).active);
}

TEST(AudioRouting, DialogToTheCentreAtEqualPower) {
  for (int ch : {6, 7, 8, 12}) {
    auto r = snd::ResolveBedRouting(ch);
    ASSERT_TRUE(r.active) << ch;
    const int fc = Index(r, Speaker::FC);
    EXPECT_NEAR(r.gain[fc][BedRouting::DialogCentre], std::sqrt(2.f), 1e-6) << ch;
    EXPECT_EQ(r.gain[Index(r, Speaker::FL)][BedRouting::DialogCentre], 0.f) << ch;
    EXPECT_EQ(r.gain[Index(r, Speaker::FR)][BedRouting::DialogCentre], 0.f) << ch;
  }
  // no centre (quad): the phantom centre on the front pair, as on the stereo output
  auto quad = snd::ResolveBedRouting(4);
  ASSERT_TRUE(quad.active);
  EXPECT_EQ(quad.gain[Index(quad, Speaker::FL)][BedRouting::DialogCentre], 1.f);
  EXPECT_EQ(quad.gain[Index(quad, Speaker::FR)][BedRouting::DialogCentre], 1.f);
}

TEST(AudioRouting, PositionedDialogNeverInvertedAndFoldsBack) {
  // a positioned stream line hard left (the overlord's gPanTable: L 1, R 0xc00 / 0x3fff)
  const float pan_l = 1.f, pan_r = (float)0xc00 / 0x3fff;
  for (int ch : {4, 6, 7, 8, 12}) {
    auto r = snd::ResolveBedRouting(ch);
    const int fl = Index(r, Speaker::FL), fr = Index(r, Speaker::FR), fc = Index(r, Speaker::FC);
    for (float s : {0.5f, -0.5f, 0.001f}) {
      for (bool mirror : {false, true}) {
        const float l = s * (mirror ? pan_r : pan_l), rr = s * (mirror ? pan_l : pan_r);
        std::array<float, BedRouting::kInputs> in{};
        const auto split = BedRouting::SplitDialog(l, rr);
        in[BedRouting::DialogCentre] = split[0];
        in[BedRouting::DialogL] = split[1];
        in[BedRouting::DialogR] = split[2];
        for (size_t c = 0; c < r.speakers.size(); c++) {
          // every channel's gain for the line (out / s) is >= 0: no speaker plays it inverted
          EXPECT_GE(r.Channel(c, in) / s, -1e-7f) << ch << " channel " << c;
        }
        // ITU downmix: L' = FL + 0.707 C, R' = FR + 0.707 C gives back the stereo line
        const float c = fc >= 0 ? r.Channel(fc, in) * std::sqrt(0.5f) : 0.f;
        EXPECT_NEAR(r.Channel(fl, in) + c, l, 1e-6f) << ch;
        EXPECT_NEAR(r.Channel(fr, in) + c, rr, 1e-6f) << ch;
        if (fc >= 0) {
          // the common part at equal power in the centre, the far side silent
          EXPECT_NEAR(r.Channel(fc, in), std::sqrt(2.f) * s * pan_r, 1e-6f) << ch;
          EXPECT_EQ(r.Channel(mirror ? fl : fr, in), 0.f) << ch;
        }
      }
    }
  }
  // L = R is unchanged: all of it in the centre at sqrt 2
  const auto mid = BedRouting::SplitDialog(0.25f, 0.25f);
  EXPECT_EQ(mid[0], 0.25f);
  EXPECT_EQ(mid[1], 0.f);
  EXPECT_EQ(mid[2], 0.f);
  // opposite signs share nothing: each side stays on its own speaker
  const auto apart = BedRouting::SplitDialog(0.25f, -0.1f);
  EXPECT_EQ(apart[0], 0.f);
}

TEST(AudioRouting, BleedsFollowTheLayoutByName) {
  auto x51 = snd::ResolveBedRouting(6);
  EXPECT_NEAR(Db(x51.gain[Index(x51, Speaker::SL)][BedRouting::MusicL]), -8.f, 1e-4);
  EXPECT_NEAR(Db(x51.gain[Index(x51, Speaker::SR)][BedRouting::AmbientR]), -3.f, 1e-4);
  for (int ch : {8, 12}) {
    auto r = snd::ResolveBedRouting(ch);
    EXPECT_NEAR(Db(r.gain[Index(r, Speaker::SL)][BedRouting::MusicL]), -8.f, 1e-4) << ch;
    EXPECT_NEAR(Db(r.gain[Index(r, Speaker::BL)][BedRouting::MusicL]), -11.f, 1e-4) << ch;
    EXPECT_NEAR(Db(r.gain[Index(r, Speaker::BR)][BedRouting::AmbientR]), -6.f, 1e-4) << ch;
    EXPECT_EQ(r.gain[Index(r, Speaker::SL)][BedRouting::MusicR], 0.f) << ch;
  }
  // quad has only the back pair: the full bleed there
  auto quad = snd::ResolveBedRouting(4);
  EXPECT_NEAR(Db(quad.gain[Index(quad, Speaker::BL)][BedRouting::MusicL]), -8.f, 1e-4);
}

TEST(AudioRouting, NothingToTheHeightsOrTheLfe) {
  for (int ch : {6, 7, 8, 12}) {
    auto r = snd::ResolveBedRouting(ch);
    for (auto sp : {Speaker::LFE, Speaker::TFL, Speaker::TFR, Speaker::TBL, Speaker::TBR}) {
      const int i = Index(r, sp);
      if (i < 0) {
        continue;
      }
      for (float g : r.gain[i]) {
        EXPECT_EQ(g, 0.f) << ch;
      }
    }
  }
}

TEST(AudioRouting, FrontPairCarriesTheAuthoredMix) {
  for (int ch : {4, 6, 7, 8, 12}) {
    auto r = snd::ResolveBedRouting(ch);
    EXPECT_EQ(r.gain[Index(r, Speaker::FL)][BedRouting::FrontL], 1.f) << ch;
    EXPECT_EQ(r.gain[Index(r, Speaker::FL)][BedRouting::FrontR], 0.f) << ch;
    EXPECT_EQ(r.gain[Index(r, Speaker::FR)][BedRouting::FrontR], 1.f) << ch;
  }
}

TEST(AudioRouting, VanillaLoudnessPerBearing) {
  // the original's pan law: centre (and behind) +2.86 dB over the sides
  EXPECT_NEAR(Db(snd::VanillaPanPowerGain(0.f)), 2.86f, 0.02f);
  EXPECT_NEAR(Db(snd::VanillaPanPowerGain(180.f)), 2.86f, 0.02f);
  EXPECT_NEAR(Db(snd::VanillaPanPowerGain(90.f)), 0.f, 1e-4);
  EXPECT_NEAR(Db(snd::VanillaPanPowerGain(-90.f)), 0.f, 1e-4);
  EXPECT_NEAR(snd::VanillaPanPowerGain(30.f), snd::VanillaPanPowerGain(-150.f), 1e-6);
}

TEST(AudioRouting, SpeakerTestOrderUnchanged) {
  std::vector<std::string> labels;
  EXPECT_EQ(snd::SpeakerTest::Slots(6, &labels), (std::vector<int>{0, 1, 2, 3, 4, 5}));
  EXPECT_EQ(labels[4], "SURROUND LEFT");
  EXPECT_EQ(snd::SpeakerTest::Slots(7, &labels), (std::vector<int>{0, 1, 2, 3, 5, 6, 4}));
  EXPECT_EQ(labels[6], "BACK CENTER");
  EXPECT_EQ(snd::SpeakerTest::Slots(8, &labels), (std::vector<int>{0, 1, 2, 3, 6, 7, 4, 5}));
  EXPECT_EQ(labels[6], "BACK LEFT");
  EXPECT_EQ(snd::SpeakerTest::Slots(12, &labels),
            (std::vector<int>{0, 1, 2, 3, 6, 7, 4, 5, 8, 9, 10, 11}));
  EXPECT_EQ(labels[11], "TOP BACK RIGHT");
  EXPECT_EQ(snd::SpeakerTest::Slots(4, &labels), (std::vector<int>{0, 1, 2, 3}));
  EXPECT_EQ(labels[2], "BACK LEFT");
  EXPECT_EQ(snd::SpeakerTest::Slots(2, &labels), (std::vector<int>{0, 1}));
  EXPECT_EQ(snd::SpeakerTest::Slots(1, &labels), (std::vector<int>{0}));
  EXPECT_EQ(labels[0], "MONO");
}
