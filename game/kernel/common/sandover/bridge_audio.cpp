#include "game/kernel/common/sandover/bridge.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "common/log/log.h"
#include "common/util/font/font_utils.h"
#include "common/util/string_util.h"

#include "game/kernel/common/Ptr.h"
#include "game/kernel/common/kernel_types.h"
#include "game/runtime.h"
#include "game/sound/989snd/spatial.h"
#include "game/sound/sndshim.h"

void post_audio_spatial(s32 mode);

void pc_set_audio_spatial(u32 mode_str) {
  if (g_game_version != GameVersion::Jak1) {
    return;
  }
  std::string name = Ptr<String>(mode_str).c()->data();
  auto mode = snd::SpatialModeFromName(name);
  if (!mode) {
    lg::error("pc-set-audio-spatial: unknown mode '{}' (off, binaural, surround, objects)", name);
    return;
  }
  post_audio_spatial(static_cast<s32>(*mode));
}

namespace {
// The Audio menu's sound control. Opening a device, listing devices and asking Windows about an
// endpoint all run on this thread, never on the GOAL stack: OpenAL Soft and COM may throw and
// catch C++ exceptions inside, which Windows cannot unwind on GOAL's own stack (the game hung in
// alcOpenDevice when a device failed to open). GOAL posts requests and reads what was last cached.
class AudioControl {
 public:
  void PostOutput(s32 output, std::string device) {
    std::scoped_lock l(m_lock);
    m_output = {output, std::move(device)};
    Wake();
  }
  void PostTest() {
    std::scoped_lock l(m_lock);
    m_test = true;
    Wake();
  }
  void PostSpatial(s32 mode) {
    std::scoped_lock l(m_lock);
    m_spatial = mode;
    Wake();
  }
  void PostDevices() {
    std::scoped_lock l(m_lock);
    m_list = true;
    Wake();
  }
  /// The menu reads the status: keep it fresh (about every 100 ms) for the next two seconds.
  void Poll() {
    std::scoped_lock l(m_lock);
    m_polled = std::chrono::steady_clock::now();
    Wake();
  }

  /// Before the Player goes (sound shutdown, process exit): no snd_* call runs after this.
  void Stop() {
    {
      std::scoped_lock l(m_lock);
      m_stopped = true;
    }
    m_cv.notify_one();
    std::scoped_lock j(m_join_lock);  // sound shutdown and exit may both get here
    if (m_thread.joinable() && m_thread.get_id() != std::this_thread::get_id()) {
      m_thread.join();
      lg::info("[audio] control thread joined");
    }
  }
  ~AudioControl() { Stop(); }

  std::mutex m_lock;  // guards the requests and everything below
  std::vector<std::string> devices;
  snd::AudioStatus status;
  bool status_ok{false};
  snd::EndpointInfo endpoint;

 private:
  void Wake() {
    if (m_stopped) {
      return;
    }
    if (!m_started) {
      m_started = true;
      m_thread = std::thread([this] { Run(); });
      // joined before the sound system stops, and at exit before any static destructor runs
      snd_SetBeforeStop(&StopAudioControl);
      std::atexit(&StopAudioControl);
    }
    m_cv.notify_one();
  }
  static void StopAudioControl();

  void Run() {
    std::string endpoint_for = "\x01";
    auto endpoint_time = std::chrono::steady_clock::time_point{};
    std::unique_lock l(m_lock);
    for (;;) {
      m_cv.wait_for(l, std::chrono::milliseconds(100));
      if (m_stopped) {
        return;  // the Player is going: Stop() joins before it is reset
      }
      auto output = std::move(m_output);
      m_output.reset();
      auto spatial = m_spatial;
      m_spatial.reset();
      const bool test = std::exchange(m_test, false);
      const auto now = std::chrono::steady_clock::now();
      const bool polled = now - m_polled < std::chrono::seconds(2);
      // while the page is up the list is re-read every 3 s, so the Device row opens on a fresh one
      const bool list =
          std::exchange(m_list, false) || (polled && now - m_list_time > std::chrono::seconds(3));
      if (list) {
        m_list_time = now;
      }
      l.unlock();
      if (spatial) {
        snd_SetSpatialMode(*spatial);
      }
      if (output) {
        snd_SetAudioOutput(output->first, output->second.c_str());
      }
      if (test) {
        snd_StartSpeakerTest();
      }
      std::vector<std::string> new_devices;
      if (list) {
        new_devices = snd::ListOutputDevices();
      }
      snd::AudioStatus st;
      bool ok = false;
      std::optional<snd::EndpointInfo> ep;
      if (polled || output || test) {
        ok = snd_GetAudioStatus(&st);
        if (st.device != endpoint_for || now - endpoint_time > std::chrono::milliseconds(1500)) {
          ep = snd::QueryEndpoint(st.device);
          endpoint_for = st.device;
          endpoint_time = now;
        }
      }
      l.lock();
      if (list) {
        devices = std::move(new_devices);
      }
      if (polled || output || test) {
        status = std::move(st);
        status_ok = ok;
        if (ep) {
          endpoint = *ep;
        }
        // the running device is the requested one; without a spatial format a restart won't help
        status.restart_pending = snd::RestartPending(
            status.output, status.objects_process,
            endpoint.found ? std::optional<int>(endpoint.spatial_objects) : std::nullopt);
      }
    }
  }

  bool m_started{false};
  bool m_stopped{false};  // the Player is gone or going
  std::thread m_thread;
  std::mutex m_join_lock;
  std::condition_variable m_cv;
  std::optional<std::pair<s32, std::string>> m_output;
  std::optional<s32> m_spatial;
  bool m_test{false};
  bool m_list{false};
  std::chrono::steady_clock::time_point m_polled{};
  std::chrono::steady_clock::time_point m_list_time{};
};
AudioControl g_audio;
void AudioControl::StopAudioControl() {
  g_audio.Stop();
}

std::string audio_display_name(std::string name) {
  constexpr std::string_view prefix = "OpenAL Soft on ";
  if (name.rfind(prefix, 0) == 0) {
    name = name.substr(prefix.size());
  }
  return name;
}

// "Speakers (Steam Streaming Speakers)" -> "Steam Streaming Speakers": fits a menu row
std::string audio_short_name(std::string name) {
  name = audio_display_name(name);
  auto open = name.find(" (");
  if (open != std::string::npos && name.back() == ')') {
    name = name.substr(open + 2, name.size() - open - 3);
  }
  return name;
}

// OpenAL Soft's output mode names as the menu shows them
std::string audio_layout_name(const std::string& layout) {
  if (layout == "stereo-basic" || layout == "stereo") {
    return "stereo";
  }
  if (layout == "stereo-hrtf") {
    return "stereo, headphone HRTF";
  }
  if (layout == "7.1.4 bed (Windows Spatial Sound stream)") {
    return "7.1.4 bed (spatial)";  // the format is on its own line
  }
  return layout;
}

u32 audio_copy_string(const std::string& text, u32 dst, bool game_font) {
  std::string s = text.substr(0, 100);
  if (game_font) {
    // the Jak 1 font has only caps
    s = get_font_bank_from_game_version(g_game_version)
            ->convert_utf8_to_game(str_util::to_upper(s).c_str());
  }
  s = s.substr(0, 127);  // the GOAL strings are 128 bytes
  strcpy(Ptr<String>(dst).c()->data(), s.c_str());
  return bool_to_symbol(true);
}
}  // namespace

/// Audio menu: output 0 auto, 1 stereo, 2 5.1, 3 7.1, 4 headphones (binaural), 5 spatial
/// (objects: runs only in a process that set it up at start), on an OpenAL Soft device name
/// ("" = the Windows default device). Applied on the audio thread.
void pc_set_audio_output(s64 output, u32 device_str) {
  if (g_game_version != GameVersion::Jak1 || output < 0 || output > 5) {
    return;
  }
  g_audio.PostOutput((s32)output, Ptr<String>(device_str).c()->data());
}

/// 0 running mode (0 off, 1 binaural, 2 surround, 3 objects; -1 no sound system; keeps the
/// cached status fresh for two seconds), 1 output channels, 2 and 3 Windows' channel count and
/// mask for the running device, 4 the command line's mode as an output (-1 not given), 5 read
/// the device list again and return the length of the last one read, 6 speaker test burst (-1
/// idle), 7 bursts in the test, 8 the requested output, 9 this process runs the Windows Spatial
/// Sound config (1/0), 10 the requested output needs a restart (1/0), 11 the spatial format
/// Windows has enabled on the running device (snd::SpatialFormat, -1 unreadable), 12 its
/// dynamic-object budget (0: no format, -1: no spatial audio client).
s64 pc_get_audio_state(u64 which) {
  if (which == 0) {
    g_audio.Poll();
  } else if (which == 5) {
    g_audio.PostDevices();
  } else if (which == 4) {
    if (!snd::g_spatial_mode_from_cli) {
      return -1;
    }
    switch (snd::g_spatial_mode_request) {
      case snd::SpatialMode::Off:
        return (s64)snd::AudioOutput::Stereo;
      case snd::SpatialMode::Binaural:
        return (s64)snd::AudioOutput::Headphones;
      case snd::SpatialMode::Objects:
        return (s64)snd::AudioOutput::Objects;
      default:
        return (s64)snd::AudioOutput::Auto;
    }
  }
  std::scoped_lock l(g_audio.m_lock);
  const auto& st = g_audio.status;
  switch (which) {
    case 0:
      return g_audio.status_ok ? (s64)st.mode : -1;
    case 1:
      return st.channels;
    case 2:
      return g_audio.endpoint.channels;
    case 3:
      return g_audio.endpoint.mask;
    case 5:
      return (s64)g_audio.devices.size();
    case 6:
      return st.test_current;
    case 7:
      return (s64)st.test_labels.size();
    case 8:
      return (s64)st.output;
    case 9:
      return st.objects_process ? 1 : 0;
    case 10:
      return st.restart_pending ? 1 : 0;
    case 11:
      return g_audio.endpoint.found ? (s64)g_audio.endpoint.spatial_format : -1;
    case 12:
      return g_audio.endpoint.found ? g_audio.endpoint.spatial_objects : -1;
    default:
      lg::error("pc-get-audio-state: unknown value {}", which);
      return 0;
  }
}

/// Into `dst` (128 bytes): 0 device `idx` of the list in the game font, 1 the same as saved
/// (OpenAL Soft's name), 2 the running device, 3 its output layout, 4 the layout Windows reports
/// for it, 5 speaker test burst `idx`, 6 why auto chose the vanilla stream, 7 the running mode,
/// 8 the running device's short name (0 is short too), 9 the spatial format Windows has enabled
/// on the running device. Returns #f when there is nothing to say.
u32 pc_get_audio_string(u64 which, u64 idx, u32 dst) {
  std::scoped_lock l(g_audio.m_lock);
  const auto& st = g_audio.status;
  const auto& ep = g_audio.endpoint;
  switch (which) {
    case 0:
    case 1:
      if (idx >= g_audio.devices.size()) {
        return bool_to_symbol(false);
      }
      return which == 0 ? audio_copy_string(audio_short_name(g_audio.devices[idx]), dst, true)
                        : audio_copy_string(g_audio.devices[idx], dst, false);
    case 2:
      if (!st.device.empty()) {
        return audio_copy_string(audio_display_name(st.device), dst, true);
      }
      return ep.found ? audio_copy_string(ep.name, dst, true) : bool_to_symbol(false);
    case 3:
      return audio_copy_string(st.mode == snd::SpatialMode::Off
                                   ? std::string("stereo (original mix)")
                                   : audio_layout_name(st.layout),
                               dst, true);
    case 4:
      if (!ep.found) {
        return bool_to_symbol(false);
      }
      return audio_copy_string(snd::EndpointLayoutName(ep.channels, ep.mask), dst, true);
    case 5:
      if (idx >= st.test_labels.size()) {
        return bool_to_symbol(false);
      }
      return audio_copy_string(st.test_labels[idx], dst, true);
    case 6:
      if (st.note.empty()) {
        return bool_to_symbol(false);
      }
      return audio_copy_string(st.note, dst, true);
    case 7:
      return audio_copy_string(snd::SpatialModeName(st.mode), dst, true);
    case 8:
      if (!st.device.empty()) {
        return audio_copy_string(audio_short_name(st.device), dst, true);
      }
      return ep.found ? audio_copy_string(audio_short_name(ep.name), dst, true)
                      : bool_to_symbol(false);
    case 9:
      if (!ep.found || ep.spatial_format == snd::SpatialFormat::Unreadable) {
        return bool_to_symbol(false);
      }
      return audio_copy_string(snd::SpatialFormatName(ep.spatial_format), dst, true);
    default:
      lg::error("pc-get-audio-string: unknown value {}", which);
      return bool_to_symbol(false);
  }
}

void pc_audio_speaker_test() {
  g_audio.PostTest();
}

void post_audio_spatial(s32 mode) {
  g_audio.PostSpatial(mode);
}
