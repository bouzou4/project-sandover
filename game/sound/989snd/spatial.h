// SPDX-License-Identifier: ISC
#pragma once

#include <array>
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/common_types.h"

#include "game/sound/common/synth.h"

namespace snd {

enum class SpatialMode : int { Off = 0, Binaural = 1, Surround = 2, Objects = 3 };

const char* SpatialModeName(SpatialMode mode);
std::optional<SpatialMode> SpatialModeFromName(std::string_view name);

/// Mode requested before the sound system starts (command line); applied when the player is made.
extern SpatialMode g_spatial_mode_request;
/// The command line named a mode (--audio-spatial); the settings take it as their starting value.
extern bool g_spatial_mode_from_cli;

/// OpenAL Soft reads its config once per process: true when that config is the Windows Spatial
/// Sound one (objects), set before OpenAL's first call. Fixed for the rest of the process.
bool ObjectsConfigured();
/// Points OpenAL Soft at the objects config (sandover-alsoft-objects.ini next to gk) when
/// `device` has a spatial format and OpenAL has not been called yet; returns whether it did.
bool ConfigureForObjects(const std::string& device);
/// At start, before OpenAL's first call: the saved Output is Spatial on `device` (an OpenAL
/// Soft name, "" the Windows default). Sets the objects config when that endpoint has a spatial
/// format; returns whether it did.
bool ConfigureObjectsAtStart(const std::string& device);
/// The settings file whose Output row gk applies at start: the VR profile's under --xr.
const char* SavedAudioFileName(bool xr);

/// Speaker layout a surround device is opened with: as Windows reports it, or forced.
enum class SpatialLayout : int { Device = 0, Stereo = 1, X51 = 2, X71 = 3 };

/// The Audio menu's Output row (pc-set-audio-output). Objects is the row's Spatial entry; it is
/// applied when the game starts (OpenAL Soft's config), so it is saved as `spatial`.
enum class AudioOutput : int {
  Auto = 0,
  Stereo = 1,
  X51 = 2,
  X71 = 3,
  Headphones = 4,
  Objects = 5
};
const char* AudioOutputName(AudioOutput out);
std::optional<AudioOutput> AudioOutputFromName(std::string_view name);

/// The audio keys of a sandover.gc file, as the game writes them: `(audio-output 5.1)` and
/// `(audio-device "OpenAL Soft on ...")`. A missing or unknown key reads as the game's default.
struct SavedAudio {
  AudioOutput output{AudioOutput::Auto};
  std::string device;
};
SavedAudio ParseSavedAudio(std::string_view text);
/// The file's name for an output (`auto`, `stereo`, `5.1`, `7.1`, `headphones`, `spatial`).
const char* SavedOutputName(AudioOutput out);

/// How Player::SetAudioOutput / SetSpatialMode apply a request. Spatial is a property of the
/// process (OpenAL Soft's config, read once), so a request may wait for the next start.
enum class OutputAction : int {
  Open = 0,      // the request's own plan (Spatial: the objects stream)
  Keep = 1,      // the running output stays, reopened on the requested device
  Fallback = 2,  // first output of a session, Spatial not set up at start: Auto's plan
};
/// `configured`: this process runs the objects config; `first`: no output applied yet this
/// session; `cli`: the request is the command line's or the REPL's mode (SetSpatialMode).
OutputAction PlanOutputRequest(bool configured, bool first, AudioOutput requested, bool cli);
/// The page's restart note: the request differs from the process, and a restart would apply
/// it. `budget` is the requested endpoint's dynamic-object budget (nullopt: unknown); with no
/// spatial format (<= 0) a restart changes nothing and the enable note says why.
bool RestartPending(AudioOutput requested, bool configured, std::optional<int> budget);

/// How an Output setting opens: Auto is surround at the device's layout, falling back to the
/// vanilla stream on a stereo default device; Stereo is the vanilla stream (OpenAL stereo on a
/// chosen device); 5.1 and 7.1 force the layout; Headphones is binaural.
struct OutputPlan {
  SpatialMode mode{SpatialMode::Off};
  SpatialLayout layout{SpatialLayout::Device};
  bool stereo_to_vanilla{false};
};
OutputPlan PlanOutput(AudioOutput out, const std::string& device);

/// The spatial sound format Windows has enabled on an endpoint (Sound settings > Spatial audio),
/// as Windows.Media.Audio.SpatialAudioDeviceConfiguration names it.
enum class SpatialFormat : int {
  None = 0,
  WindowsSonic = 1,
  DolbyAtmosHeadphones = 2,
  DolbyAtmosHomeTheater = 3,
  DolbyAtmosSpeakers = 4,
  DtsHeadphoneX = 5,
  DtsXUltra = 6,
  DtsXHomeTheater = 7,
  Other = 8,       // a format this build does not know by name
  Unreadable = -1  // the WinRT query failed (older Windows, or not Windows)
};
const char* SpatialFormatName(SpatialFormat f);
/// The spatial stream's static bed carries the four height channels (7.1.4) for this mask.
bool SpatialMaskHasHeights(u32 static_mask);

/// What Windows reports for a render endpoint (its shared-mode mix format).
struct EndpointInfo {
  bool found{false};
  bool is_default{false};
  std::string name;  // friendly name, as OpenAL Soft lists it without its prefix
  int channels{0};
  u32 mask{0};
  // Windows Spatial Sound on this endpoint: the dynamic-object budget (0: no spatial format,
  // -1: no spatial audio client) and the bed channels the format renders natively
  int spatial_objects{-1};
  u32 spatial_static_mask{0};
  SpatialFormat spatial_format{SpatialFormat::Unreadable};
  std::string spatial_format_id;  // Windows' subtype id for it, as read
};
/// `device` is an OpenAL Soft device name; empty for the Windows default (multimedia role).
EndpointInfo QueryEndpoint(const std::string& device);
/// "5.1 (back)", "7.1", "stereo", ... for a channel count and WAVEFORMATEXTENSIBLE mask.
std::string EndpointLayoutName(int channels, u32 mask);
/// OpenAL Soft's output devices (ALC_ENUMERATE_ALL_EXT); empty when it cannot enumerate.
std::vector<std::string> ListOutputDevices();

/// An output speaker by name. A layout's channels are listed in the order of OpenAL Soft's
/// buffer formats for it (the speaker test's and the bed's), which direct channels match by name.
enum class Speaker : u8 { FL, FR, FC, LFE, BL, BR, SL, SR, BC, TFL, TFR, TBL, TBR };
/// The speakers of a layout of `channels` (1, 2, 4: quad, 6: 5.1, 7: 6.1, 8: 7.1, 12: 7.1.4).
std::vector<Speaker> LayoutSpeakers(int channels);

/// Sample rate of the spatial outputs (OpenAL's device and every source).
constexpr int kSpatialRate = 48000;

/**
 * How the surround outputs place what is not positional: a gain from each input to each output
 * channel, resolved by speaker name from the output layout (no per-layout code).
 * - front pair: the authored stereo mix (effects, music, ambience) as on the stereo output;
 * - dialog: the part common to L and R (min(L, R) of a panned mono line) to the centre at equal
 *   power (L = R = v becomes C = v * sqrt 2, what an ITU downmix folds back to v on each side),
 *   only the louder side's excess to its front speaker; the line unchanged on the front pair
 *   without a centre. Every gain is >= 0, so no speaker plays the line inverted;
 * - music and ambience: also to the surround pairs, delayed (kBleedDelay) and lowered: side
 *   or rear pair at kMusicBleedDb / kAmbientBleedDb, the back pair kBackExtraDb lower when a
 *   side pair exists too, back centre the same from L+R; nothing to the heights or the LFE.
 * Inactive (the stereo bed as before) on layouts with neither a centre nor a surround channel.
 */
struct BedRouting {
  enum Input : int {
    FrontL,
    FrontR,
    DialogCentre,  // SplitDialog: the part common to L and R
    DialogL,       // L minus that
    DialogR,       // R minus that
    MusicL,        // delayed
    MusicR,
    AmbientL,  // delayed
    AmbientR,
    kInputs
  };
  static constexpr float kMusicBleedDb = -8.f;
  static constexpr float kAmbientBleedDb = -3.f;
  static constexpr float kBackExtraDb = -3.f;
  static constexpr int kBleedDelay = kSpatialRate * 15 / 1000;  // 15 ms

  int channels{2};
  bool active{false};
  std::vector<Speaker> speakers;
  std::vector<std::array<float, kInputs>> gain;  // [output channel][input]
  std::string Describe() const;
  /// Output channel `ch` for one frame of inputs.
  float Channel(size_t ch, const std::array<float, kInputs>& in) const;
  /// A dialog sample's (DialogCentre, DialogL, DialogR): the common part is the side nearer
  /// zero when L and R share a sign (a mono line at any pan), else nothing.
  static std::array<float, 3> SplitDialog(float l, float r);
};
BedRouting ResolveBedRouting(int channels);

/// Loudness of a positional sound on the surround outputs, from the vanilla pan law: the pan
/// table's L^2 + R^2 at this bearing (0 ahead, horizontal plane), relative to the sides. Ahead
/// and behind are +2.9 dB, as in the original's stereo mix.
float VanillaPanPowerGain(float bearing_deg);

/**
 * Speaker test signal: one burst per output channel in turn (front left, front right, centre,
 * LFE, surround/side left and right, back left and right as present, then the four height
 * channels of a 7.1.4 spatial bed), 0.7 s on and 0.3 s off.
 * Mains get band-limited noise, the LFE a 50 Hz tone. Render() fills interleaved frames in the
 * output's own channel order; each burst goes to exactly one channel.
 */
class SpeakerTest {
 public:
  static constexpr int kOn = 48000 * 7 / 10;
  static constexpr int kOff = 48000 * 3 / 10;
  /// Sets up the sequence for an output of `channels` (1, 2, 4, 6, 7, 8, or 12: 7.1.4).
  void Start(int channels, std::string context);
  void Stop() { mActive = false; }
  /// Adds the next `frames` frames to `out` (`stride` floats per frame, `channels` used).
  void Render(float* out, int frames, int stride);
  /// Burst being played (index into Labels()), or -1 when idle.
  int Current() const { return mActive ? mSegment.load() : -1; }
  int Count() const { return (int)mSlots.size(); }
  const std::vector<std::string>& Labels() const { return mLabels; }
  static std::vector<int> Slots(int channels, std::vector<std::string>* labels);

 private:
  std::mutex mLock;  // Start (game thread) against Render (mixer thread)
  std::atomic<bool> mActive{false};
  std::atomic<int> mSegment{-1};
  int mFrame{0};
  int mLogged{-1};
  u32 mNoise{0x2545F491};
  float mLow{0}, mBand{0};
  float mGain{1};
  std::vector<int> mSlots;
  std::vector<std::string> mLabels;
  std::string mContext;
};

/// What runs now, for the Audio menu's status lines (Player::GetAudioStatus).
struct AudioStatus {
  AudioOutput output{AudioOutput::Stereo};  // as requested
  std::string device_request;               // "" = the Windows default
  SpatialMode mode{SpatialMode::Off};       // what runs
  std::string device;                       // the OpenAL device; "" for the vanilla stream
  std::string layout;                       // OpenAL Soft's output layout, or stereo
  int channels{2};
  std::string note;             // why auto picked the vanilla stream
  bool objects_process{false};  // OpenAL Soft runs the Windows Spatial Sound config
  bool restart_pending{false};  // the requested output differs from what this process can run
  int test_current{-1};
  std::vector<std::string> test_labels;
};

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
  static constexpr int kRate = kSpatialRate;
  using TickFn = std::function<void(SpatialOutput&)>;

  /// Opens `device` (an OpenAL Soft name; empty: the Windows default). Surround uses `layout`
  /// (Device: what Windows reports). `lock` guards the synth, `tick` advances it one sample.
  static std::unique_ptr<SpatialOutput> OpenDevice(SpatialMode mode,
                                                   std::recursive_mutex& lock,
                                                   TickFn tick,
                                                   SpatialLayout layout = SpatialLayout::Device,
                                                   const std::string& device = "");
  /// Opens a loopback device for capture; `channels` is ALC_STEREO_SOFT, ALC_5POINT1_SOFT or
  /// ALC_7POINT1_SOFT (binaural needs stereo). Output is interleaved float.
  /// `routing` false keeps the pre-routing behaviour (mono positional sum, stereo bed): the
  /// probe's before/after comparison. ALC_7POINT1POINT4_SANDOVER renders the 7.1.4 bed.
  static std::unique_ptr<SpatialOutput> OpenLoopback(SpatialMode mode,
                                                     int channels,
                                                     std::recursive_mutex& lock,
                                                     TickFn tick,
                                                     bool routing = true);
  ~SpatialOutput() override;
  SpatialOutput(const SpatialOutput&) = delete;
  SpatialOutput& operator=(const SpatialOutput&) = delete;

  // Synth side: call with the synth lock held.
  bool Route(u32 owner, s16Output sample) override;
  bool RouteRole(VoiceRole role, s16Output sample) override;
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
  /// The opened device's OpenAL Soft name and output layout ("5.1", "stereo-hrtf", ...).
  const std::string& DeviceName() const { return mDeviceName; }
  const std::string& LayoutName() const { return mLayoutName; }
  SpatialLayout RequestedLayout() const { return mLayout; }
  /// The non-positional routing in use (inactive: the stereo bed on the front pair).
  const BedRouting& Routing() const { return mRouting; }

  /// Plays the speaker test on this device (a direct-channel source, one burst per channel).
  void StartSpeakerTest(const std::string& context);
  const SpeakerTest& Test() const { return mTest; }

 private:
  struct Slot {
    u32 owner{0};
    s32 acc{0};
    s32 acc_side{0};
    std::deque<s16> queue;
    std::deque<s16> side;  // (L - R) / 2 of the voices: the authored width, routed outputs
    SpatialVec pos;
    bool pos_dirty{false};
    bool start_pending{false};
    bool first_fill{false};
    bool side_live{false};  // the side source plays: some S was non-zero since the claim
    bool side_start_pending{false};
    unsigned source{0};
    unsigned buffer{0};
    unsigned side_source{0};
    unsigned side_buffer{0};
  };
  struct CallbackCtx {
    SpatialOutput* self;
    int slot;  // -1: the bed
    bool side;
  };

  SpatialOutput(SpatialMode mode, std::recursive_mutex& lock, TickFn tick);
  static int TestCallback(void* user, void* data, int bytes);
  bool InitContext(void* device, bool loopback, int channels);
  static int Callback(void* user, void* data, int bytes);
  int Fill(int slot, s16* dst, int frames);
  int FillSide(int slot, s16* dst, int frames);
  int FillBed(float* dst, int frames);
  void TrimFirstFill(Slot& s);
  size_t BedFrames() const;

  SpatialMode mMode;
  std::recursive_mutex& mLock;
  TickFn mTick;

  void* mDevice{nullptr};
  void* mContext{nullptr};
  int mOutChannels{2};
  SpatialLayout mLayout{SpatialLayout::Device};
  std::string mDescription;
  std::string mDeviceName;
  std::string mLayoutName;

  SpeakerTest mTest;
  int mTestChannels{2};
  unsigned mTestSource{0};
  unsigned mTestBuffer{0};

  std::array<Slot, kSlots> mSlots{};
  std::array<CallbackCtx, 2 * kSlots + 1> mCtx{};
  std::deque<s16> mBed;  // interleaved stereo (routing inactive)
  unsigned mBedSource{0};
  unsigned mBedBuffer{0};

  // routing active: the bed in the output layout (interleaved float) and the role sums
  bool mRoutingAllowed{true};
  BedRouting mRouting;
  std::deque<float> mBedOut;
  std::array<std::array<s32, 2>, 4> mRoleAcc{};  // by VoiceRole
  std::vector<float> mBleedLine;                 // kBleedDelay frames of music L R, ambient L R
  int mBleedPos{0};

  SpatialVec mListenerPos;
  float mListenerYaw{0};
  bool mListenerMirror{false};
  bool mListenerDirty{true};
  u64 mTotalClaims{0};
};

}  // namespace snd
