#include "kmachine.h"

#include <algorithm>
#include <cmath>
#include <random>

#include "common/global_profiler/GlobalProfiler.h"
#include "common/log/log.h"
#include "common/symbols.h"
#include "common/util/FileUtil.h"
#include "common/util/Timer.h"
#include "common/util/font/font_utils.h"
#include "common/util/string_util.h"

#include "game/external/discord.h"
#include "game/graphics/bug_report.h"
#include "game/graphics/display.h"
#include "game/graphics/gfx.h"
#include "game/graphics/opengl_renderer/LavaField.h"
#include "game/graphics/opengl_renderer/Materials.h"
#include "game/graphics/opengl_renderer/Neighbours.h"
#include "game/graphics/opengl_renderer/WorldLights.h"
#include "game/graphics/screenshot.h"
#include "game/kernel/common/Ptr.h"
#include "game/kernel/common/kernel_types.h"
#include "game/kernel/common/kprint.h"
#include "game/kernel/common/kscheme.h"
#include "game/mips2c/mips2c_table.h"
#include "game/runtime.h"
#include "game/sce/libcdvd_ee.h"
#include "game/sce/libpad.h"
#include "game/sce/libscf.h"
#include "game/sce/sif_ee.h"
#include "game/sound/989snd/spatial.h"
#include "game/sound/sndshim.h"

/*!
 * Where does OVERLORD load its data from?
 */
OverlordDataSource isodrv;

// Get IOP modules from DVD or from dsefilesv
u32 modsrc;

// Reboot IOP with IOP kernel from DVD/CD on boot
u32 reboot_iop;

const char* init_types[] = {"fakeiso", "deviso", "iso_cd"};
u8 pad_dma_buf[2 * SCE_PAD_DMA_BUFFER_SIZE];

// added
u32 vif1_interrupt_handler = 0;
u32 vblank_interrupt_handler = 0;

Timer ee_clock_timer;

void kmachine_init_globals_common() {
  memset(pad_dma_buf, 0, sizeof(pad_dma_buf));
  isodrv = fakeiso;  // changed. fakeiso is the only one that works in opengoal.
  modsrc = 1;
  reboot_iop = 1;
  vif1_interrupt_handler = 0;
  vblank_interrupt_handler = 0;
  ee_clock_timer = Timer();
}

/*!
 * Initialize the CD Drive
 * DONE, EXACT
 */
void InitCD() {
  lg::info("Initializing CD drive. This may take a while...");
  ee::sceCdInit(SCECdINIT);
  ee::sceCdMmode(SCECdDVD);
  while (ee::sceCdDiskReady(0) == SCECdNotReady) {
    lg::debug("Drive not ready... insert a disk!");
  }
  lg::debug("Disk type {}\n", ee::sceCdGetDiskType());
}

/*!
 * Initialize the GS and display the splash screen.
 */
void InitVideo() {
  if (!SplashScreen) {
    lg::info("InitVideo: skipping splash!\n");
    return;
  }
  std::map<int, std::string> lang_to_splash_map{
      {SCE_JAPANESE_LANGUAGE, "JAP"},   {SCE_ENGLISH_LANGUAGE, "USA"},
      {SCE_FRENCH_LANGUAGE, "FRE"},     {SCE_SPANISH_LANGUAGE, "SPA"},
      {SCE_GERMAN_LANGUAGE, "GER"},     {SCE_ITALIAN_LANGUAGE, "ITA"},
      {SCE_PORTUGUESE_LANGUAGE, "POR"}, {SCE_KOREAN_LANGUAGE, "KOR"},
  };
  auto lang = ee::sceScfGetLanguage();
  if (!lang_to_splash_map.contains(lang)) {
    lg::warn("InitVideo: no splash for lang {}, falling back to english...\n", lang);
    lang = SCE_ENGLISH_LANGUAGE;
  }
  auto filename = "SCREEN1." + lang_to_splash_map.at(lang);
  auto path = file_util::get_jak_project_dir() / "out" / game_version_names[g_game_version] /
              "iso" / filename;
  if (lang != SCE_ENGLISH_LANGUAGE && !fs::exists(path)) {
    lg::warn("InitVideo: file {} not found, falling back to english...\n", filename);
    path = file_util::get_jak_project_dir() / "out" / game_version_names[g_game_version] / "iso" /
           "SCREEN1.USA";
  }
  if (!fs::exists(path)) {
    lg::warn("InitVideo: splash screen not found!\n");
    return;
  }
  auto data = file_util::read_binary_file(path);
  // width is always 512, height is sometimes different (e.g. demo screens), so we infer from file
  // size
  constexpr int kWidth = 512;
  if (data.size() % (kWidth * 4) != 0) {
    lg::error("InitVideo: splash size {} not divisible by stride {}", data.size(), kWidth * 4);
    return;
  }
  int kHeight = data.size() / (kWidth * 4);
  if ((int)data.size() != kWidth * kHeight * 4) {
    lg::error("InitVideo: unexpected size {}, expected {} for splash screen", data.size(),
              kWidth * kHeight * 4);
    return;
  }
  Gfx::g_splash.data = std::move(data);
  Gfx::g_splash.width = kWidth;
  Gfx::g_splash.height = kHeight;
  Gfx::g_splash.ready.store(true);
  SplashTimer.start();
  while (SplashTimer.getSeconds() < SPLASH_SCREEN_TIME) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

/*!
 * Flush caches.  Does all the memory, regardless of what you specify
 */
void CacheFlush(void* mem, int size) {
  (void)mem;
  (void)size;
  // FlushCache(0);
  // FlushCache(2);
}

/*!
 * Open a new controller pad.
 * Set the new_pad flag to 1 and state to 0.
 * Prints an error if it fails to open.
 */
u64 CPadOpen(u64 cpad_info, s32 pad_number) {
  auto cpad = Ptr<CPadInfo>(cpad_info).c();
  if (cpad->cpad_file == 0) {
    // not open, so we will open it
    cpad->cpad_file =
        ee::scePadPortOpen(pad_number, 0, pad_dma_buf + pad_number * SCE_PAD_DMA_BUFFER_SIZE);
    if (cpad->cpad_file < 1) {
      MsgErr("dkernel: !open cpad #%d (%d)\n", pad_number, cpad->cpad_file);
    }
    cpad->new_pad = 1;
    cpad->state = 0;
  }
  return cpad_info;
}

/*!
 * Not checked super carefully for jak 2, but looks the same
 */
u64 CPadGetData(u64 cpad_info) {
  using namespace ee;
  auto cpad = Ptr<CPadInfo>(cpad_info).c();
  auto pad_state = scePadGetState(cpad->number, 0);
  if (pad_state == scePadStateDiscon) {
    cpad->state = 0;
  }
  cpad->valid = pad_state | 0x80;
  switch (cpad->state) {
    // case 99: // functional
    default:  // controller is functioning as normal
      if (pad_state == scePadStateStable || pad_state == scePadStateFindCTP1) {
        scePadRead(cpad->number, 0, (u8*)cpad);
        // ps2 controllers would send an enabled bit if the button was NOT pressed, but we don't do
        // that here. removed code that flipped the bits.

        if (cpad->change_time) {
          scePadSetActDirect(cpad->number, 0, cpad->direct);
        }
        cpad->valid = pad_state;
      }
      break;
    case 0:  // unavailable
      if (pad_state == scePadStateStable || pad_state == scePadStateFindCTP1) {
        auto pad_mode = scePadInfoMode(cpad->number, 0, InfoModeCurID, 0);
        if (pad_mode != 0) {
          auto vibration_mode = scePadInfoMode(cpad->number, 0, InfoModeCurExID, 0);
          if (vibration_mode > 0) {
            // vibration supported
            pad_mode = vibration_mode;
          }
          if (pad_mode == 4) {
            // controller mode
            cpad->state = 40;
          } else if (pad_mode == 7) {
            // dualshock mode
            cpad->state = 70;
          } else {
            // who knows mode
            cpad->state = 90;
          }
        }
      }
      break;
    case 40:  // controller mode - check for extra modes
      // cpad->change_time = 0;
      cpad->change_time = 0;
      if (scePadInfoMode(cpad->number, 0, InfoModeIdTable, -1) == 0) {
        // no controller modes
        cpad->state = 90;
        return cpad_info;
      }
      cpad->state = 41;
    case 41:  // controller mode - change to dualshock mode!
      // try to enter the 2nd controller mode (dualshock for ds2's)
      if (scePadSetMainMode(cpad->number, 0, 1, 3) == 1) {
        cpad->state = 42;
      }
      break;
    case 42:  // controller mode change check
      if (scePadGetReqState(cpad->number, 0) == scePadReqStateFailed) {
        // failed to change to DS2
        cpad->state = 41;
      }
      if (scePadGetReqState(cpad->number, 0) == scePadReqStateComplete) {
        // change successful. go back to the beginning.
        cpad->state = 0;
      }
      break;
    case 70:  // dualshock mode - check vibration
      // get number of actuators (2 for DS2)
      if (scePadInfoAct(cpad->number, 0, -1, 0) < 1) {
        // no actuators means no vibration. skip to end!
        // cpad->change_time = 0;
        cpad->change_time = 0;
        cpad->state = 99;
      } else {
        // we have actuators to use.
        // cpad->change_time = 1;  // remember to update pad times.
        cpad->change_time = 1;
        cpad->state = 75;
      }
      break;
    case 75:  // set actuator vib param info
      if (scePadSetActAlign(cpad->number, 0, cpad->align) != 0) {
        if (scePadInfoPressMode(cpad->number, 0) == 1) {
          // pressure buttons supported
          cpad->state = 76;
        } else {
          // no pressure buttons, done with controller setup
          cpad->state = 99;
        }
      }
      break;
    case 76:  // enter pressure mode
      if (scePadEnterPressMode(cpad->number, 0) == 1) {
        cpad->state = 78;
      }
      break;
    case 78:  // pressure mode request check
      if (scePadGetReqState(cpad->number, 0) == scePadReqStateFailed) {
        cpad->state = 76;
      }
      if (scePadGetReqState(cpad->number, 0) == scePadReqStateComplete) {
        cpad->state = 99;
      }
      break;
    case 90:
      break;  // unsupported controller. too bad!
  }
  return cpad_info;
}

// should make sure this works the same way in jak 2
void InstallHandler(u32 handler_idx, u32 handler_func) {
  switch (handler_idx) {
    case 3:
      vblank_interrupt_handler = handler_func;
      break;
    case 5:
      vif1_interrupt_handler = handler_func;
      break;
    default:
      lg::error("unknown handler: {}\n", handler_idx);
      ASSERT(false);
  }
}

// nothing used this in jak1, hopefully same for 2
void InstallDebugHandler() {
  ASSERT(false);
}

/*!
 * Get length of a file.
 */
s32 klength(u64 fs) {
  auto file_stream = Ptr<FileStream>(fs).c();
  if ((file_stream->flags ^ 1) & 1) {
    // first flag bit not set. This means no errors
    auto end_seek = ee::sceLseek(file_stream->file, 0, SCE_SEEK_END);
    auto reset_seek = ee::sceLseek(file_stream->file, 0, SCE_SEEK_SET);
    if (reset_seek < 0 || end_seek < 0) {
      // seeking failed, flag it
      file_stream->flags |= 1;
    }
    return end_seek;
  } else {
    return 0;
  }
}

/*!
 * Seek a file stream.
 */
s32 kseek(u64 fs, s32 offset, s32 where) {
  s32 result = -1;
  auto file_stream = Ptr<FileStream>(fs).c();
  if ((file_stream->flags ^ 1) & 1) {
    result = ee::sceLseek(file_stream->file, offset, where);
    if (result < 0) {
      file_stream->flags |= 1;
    }
  }
  return result;
}

/*!
 * Read from a file stream.
 */
s32 kread(u64 fs, u64 buffer, s32 size) {
  s32 result = -1;
  auto file_stream = Ptr<FileStream>(fs).c();
  if ((file_stream->flags ^ 1) & 1) {
    result = ee::sceRead(file_stream->file, Ptr<u8>(buffer).c(), size);
    if (result < 0) {
      file_stream->flags |= 1;
    }
  }
  return result;
}

/*!
 * Write to a file stream.
 */
s32 kwrite(u64 fs, u64 buffer, s32 size) {
  s32 result = -1;
  auto file_stream = Ptr<FileStream>(fs).c();
  if ((file_stream->flags ^ 1) & 1) {
    result = ee::sceWrite(file_stream->file, Ptr<u8>(buffer).c(), size);
    if (result < 0) {
      file_stream->flags |= 1;
    }
  }
  return result;
}

s32 kmkdir(u64 name) {
  char acStack_90[128];
  if (Ptr<String>(name)->data()[4] == '/') {  // start from the fourth character?
    sprintf(acStack_90, "%s", Ptr<String>(name)->data() + 5);
  } else {
    sprintf(acStack_90, "host:%s", Ptr<String>(name)->data() + 4);
  }
  return ee::sceMkDir(acStack_90, 0x1fd);
}

/*!
 * Close a file stream.
 */
u64 kclose(u64 fs) {
  auto file_stream = Ptr<FileStream>(fs).c();
  if ((file_stream->flags ^ 1) & 1) {
    ee::sceClose(file_stream->file);
    file_stream->file = -1;
  }
  file_stream->flags = 0;
  return fs;
}

// TODO dma_to_iop
void dma_to_iop() {
  ASSERT(false);
}

u64 DecodeLanguage() {
  return masterConfig.language;
}

u64 DecodeAspect() {
  return masterConfig.aspect;
}

u64 DecodeVolume() {
  return masterConfig.volume;
}

// NOTE: this is originally hardcoded, and returns different values depending on the disc region.
// it returns 0 for NTSC-U, 1 for PAL and 2 for NTSC-J
u64 DecodeTerritory() {
  return GAME_TERRITORY_SCEA;
}

u64 DecodeTimeout() {
  return masterConfig.timeout;
}

u64 DecodeInactiveTimeout() {
  return masterConfig.inactive_timeout;
}

void DecodeTime(u32 ptr) {
  Ptr<ee::sceCdCLOCK> clock(ptr);
  // in jak2, if this fails, they do a sceScfGetLocalTimefromRTC
  sceCdReadClock(clock.c());
}

void vif_interrupt_callback(int bucket_id) {
  // added for the PC port for faking VIF interrupts from the graphics system.
  if (vif1_interrupt_handler && MasterExit == RuntimeExitStatus::RUNNING) {
    call_goal(Ptr<Function>(vif1_interrupt_handler), bucket_id, 0, 0, s7.offset, g_ee_main_mem);
  }
}

/// PC PORT FUNCTIONS BEGIN

u32 offset_of_s7() {
  return s7.offset;
}

inline bool symbol_to_bool(const u32 symptr) {
  return symptr != s7.offset;
}

inline u64 bool_to_symbol(const bool val) {
  return val ? static_cast<u64>(s7.offset) + true_symbol_offset(g_game_version) : s7.offset;
}

u64 pc_filter_debug_string(u32 str_ptr, u32 dist_ptr) {
  auto str = std::string(Ptr<String>(str_ptr).c()->data());
  float dist;
  memcpy(&dist, &dist_ptr, 4);

  // Check distance first
  if (Gfx::g_debug_settings.text_check_range) {
    if (dist / 4096.F > Gfx::g_debug_settings.text_max_range) {
      return bool_to_symbol(true);
    }
  }

  // Get the current filters
  const auto& filters = Gfx::g_debug_settings.text_filters;
  if (filters.empty()) {
    // there are no filters, exit early
    return bool_to_symbol(false);
  }

  // Currently very dumb contains check
  for (const auto& filter : filters) {
    if (filter.type == DebugTextFilter::Type::CONTAINS) {
      if (!str.empty() && !filter.content.empty() && !str_util::contains(str, filter.content)) {
        return bool_to_symbol(true);
      }
    } else if (filter.type == DebugTextFilter::Type::NOT_CONTAINS) {
      if (!str.empty() && !filter.content.empty() && str_util::contains(str, filter.content)) {
        return bool_to_symbol(true);
      }
    } else if (filter.type == DebugTextFilter::Type::REGEX) {
      if (str_util::valid_regex(filter.content) &&
          std::regex_match(str, std::regex(filter.content))) {
        return bool_to_symbol(true);
      }
    }
  }
  return bool_to_symbol(false);
}

CommonPCPortFunctionWrappers g_pc_port_funcs;

u64 read_ee_timer() {
  u64 ns = ee_clock_timer.getNs();
  return (ns * 3) / 10;
}

void pc_memmove(u32 dst, u32 src, u32 size) {
  memmove(Ptr<u8>(dst).c(), Ptr<u8>(src).c(), size);
}

void send_gfx_dma_chain(u32 /*bank*/, u32 chain) {
  if (Gfx::GetCurrentRenderer()) {
    Gfx::GetCurrentRenderer()->send_chain(g_ee_main_mem, chain);
  }
}

void pc_texture_upload_now(u32 page, u32 mode) {
  if (Gfx::GetCurrentRenderer()) {
    Gfx::GetCurrentRenderer()->texture_upload_now(Ptr<u8>(page).c(), mode, s7.offset);
  }
}

void pc_force_reload_all() {
  if (Gfx::GetCurrentRenderer()) {
    Gfx::GetCurrentRenderer()->force_reload_all();
  }
}

void pc_force_reload_level(u32 name) {
  if (Gfx::GetCurrentRenderer()) {
    Gfx::GetCurrentRenderer()->force_reload_level(std::string(Ptr<String>(name).c()->data()));
  }
}

void pc_force_reload_common() {
  if (Gfx::GetCurrentRenderer()) {
    Gfx::GetCurrentRenderer()->force_reload_common();
  }
}

void pc_texture_relocate(u32 dst, u32 src, u32 format) {
  if (Gfx::GetCurrentRenderer()) {
    Gfx::GetCurrentRenderer()->texture_relocate(dst, src, format);
  }
}

u64 pc_get_mips2c(u32 name) {
  const char* n = Ptr<String>(name).c()->data();
  return Mips2C::gLinkedFunctionTable.get(n);
}

u64 pc_get_display_id() {
  if (Display::GetMainDisplay()) {
    return Display::GetMainDisplay()->get_display_manager()->get_active_display_index();
  }
  return 0;
}

void pc_set_display_id(u64 display_id) {
  if (Display::GetMainDisplay()) {
    Display::GetMainDisplay()->get_display_manager()->enqueue_set_display_id(display_id);
  }
}

u64 pc_get_display_name(u32 id, u32 str_dest_ptr) {
  std::string name = "";
  if (Display::GetMainDisplay()) {
    name = Display::GetMainDisplay()->get_display_manager()->get_connected_display_name(id);
  }
  if (name.empty()) {
    return bool_to_symbol(false);
  }
  if (g_game_version == GameVersion::Jak1) {
    // The Jak 1 font has only caps
    name = str_util::to_upper(name).c_str();
  }
  // Encode the string to the game font
  const auto encoded_name = get_font_bank_from_game_version(g_game_version)
                                ->convert_utf8_to_game(str_util::titlize(name).c_str());
  strcpy(Ptr<String>(str_dest_ptr).c()->data(), encoded_name.c_str());
  strcpy(Ptr<String>(str_dest_ptr).c()->data(), str_util::titlize(encoded_name).c_str());
  return bool_to_symbol(true);
}

u32 pc_get_display_mode() {
  auto display_mode = game_settings::DisplaySettings::DisplayMode::Windowed;
  if (Display::GetMainDisplay()) {
    display_mode = Display::GetMainDisplay()->get_display_manager()->get_display_mode();
  }
  switch (display_mode) {
    case game_settings::DisplaySettings::DisplayMode::Borderless:
      return g_pc_port_funcs.intern_from_c("borderless").offset;
    case game_settings::DisplaySettings::DisplayMode::Fullscreen:
      return g_pc_port_funcs.intern_from_c("fullscreen").offset;
    default:
      return g_pc_port_funcs.intern_from_c("windowed").offset;
  }
}

void pc_set_display_mode(u32 symptr, u64 window_width, u64 window_height) {
  if (!Display::GetMainDisplay()) {
    return;
  }
  if (symptr == g_pc_port_funcs.intern_from_c("windowed").offset || symptr == s7.offset) {
    Display::GetMainDisplay()->get_display_manager()->enqueue_set_window_display_mode(
        game_settings::DisplaySettings::DisplayMode::Windowed, window_width, window_height);
  } else if (symptr == g_pc_port_funcs.intern_from_c("borderless").offset) {
    Display::GetMainDisplay()->get_display_manager()->enqueue_set_window_display_mode(
        game_settings::DisplaySettings::DisplayMode::Borderless, window_width, window_height);
  } else if (symptr == g_pc_port_funcs.intern_from_c("fullscreen").offset) {
    Display::GetMainDisplay()->get_display_manager()->enqueue_set_window_display_mode(
        game_settings::DisplaySettings::DisplayMode::Fullscreen, window_width, window_height);
  }
}

u64 pc_get_display_count() {
  if (Display::GetMainDisplay()) {
    return Display::GetMainDisplay()->get_display_manager()->num_connected_displays();
  }
  return 0;
}

void pc_get_active_display_size(u32 w_ptr, u32 h_ptr) {
  if (!Display::GetMainDisplay()) {
    return;
  }
  if (w_ptr) {
    auto w_out = Ptr<s64>(w_ptr).c();
    if (w_out) {
      *w_out = Display::GetMainDisplay()->get_display_manager()->get_screen_width();
    }
  }
  if (h_ptr) {
    auto h_out = Ptr<s64>(h_ptr).c();
    if (h_out) {
      *h_out = Display::GetMainDisplay()->get_display_manager()->get_screen_height();
    }
  }
}

s64 pc_get_active_display_refresh_rate() {
  if (Display::GetMainDisplay()) {
    return Display::GetMainDisplay()->get_display_manager()->get_active_display_refresh_rate();
  }
  return 0;
}

void pc_get_window_size(u32 w_ptr, u32 h_ptr) {
  if (!Display::GetMainDisplay()) {
    return;
  }
  if (w_ptr) {
    auto w = Ptr<s64>(w_ptr).c();
    if (w) {
      *w = Display::GetMainDisplay()->get_display_manager()->get_window_width();
    }
  }
  if (h_ptr) {
    auto h = Ptr<s64>(h_ptr).c();
    if (h) {
      *h = Display::GetMainDisplay()->get_display_manager()->get_window_height();
    }
  }
}

void pc_get_window_scale(u32 x_ptr, u32 y_ptr) {
  if (!Display::GetMainDisplay()) {
    return;
  }
  if (x_ptr) {
    auto x = Ptr<float>(x_ptr).c();
    if (x) {
      *x = Display::GetMainDisplay()->get_display_manager()->get_window_scale_x();
    }
  }
  if (y_ptr) {
    auto y = Ptr<float>(y_ptr).c();
    if (y) {
      *y = Display::GetMainDisplay()->get_display_manager()->get_window_scale_y();
    }
  }
}

void pc_set_window_size(u64 width, u64 height) {
  if (Display::GetMainDisplay()) {
    Display::GetMainDisplay()->get_display_manager()->enqueue_set_window_size(width, height);
  }
}

s64 pc_get_num_resolutions(u32 for_windowed) {
  if (Display::GetMainDisplay()) {
    return Display::GetMainDisplay()->get_display_manager()->get_num_resolutions(
        symbol_to_bool(for_windowed));
  }
  return 0;
}

void pc_get_resolution(u32 id, u32 for_windowed, u32 w_ptr, u32 h_ptr) {
  if (Display::GetMainDisplay()) {
    auto res = Display::GetMainDisplay()->get_display_manager()->get_resolution(
        id, symbol_to_bool(for_windowed));
    auto w = Ptr<s64>(w_ptr).c();
    if (w) {
      *w = res.width;
    }
    auto h = Ptr<s64>(h_ptr).c();
    if (h) {
      *h = res.height;
    }
  }
}

u64 pc_is_supported_resolution(u64 width, u64 height) {
  if (Display::GetMainDisplay()) {
    return bool_to_symbol(
        Display::GetMainDisplay()->get_display_manager()->is_supported_resolution(width, height));
  }
  return bool_to_symbol(false);
}

u64 pc_get_controller_name(u32 id, u32 str_dest_ptr) {
  std::string name = "";
  if (Display::GetMainDisplay()) {
    name = Display::GetMainDisplay()->get_input_manager()->get_controller_name(id);
  }
  if (name.empty()) {
    return bool_to_symbol(false);
  }

  if (g_game_version == GameVersion::Jak1) {
    // The Jak 1 font has only caps
    name = str_util::to_upper(name).c_str();
  }
  // Encode the string to the game font
  const auto encoded_name = get_font_bank_from_game_version(g_game_version)
                                ->convert_utf8_to_game(str_util::titlize(name).c_str());
  strcpy(Ptr<String>(str_dest_ptr).c()->data(), encoded_name.c_str());
  strcpy(Ptr<String>(str_dest_ptr).c()->data(), str_util::titlize(encoded_name).c_str());
  return bool_to_symbol(true);
}

u64 pc_get_current_bind(s32 bind_assignment_info, u32 str_dest_ptr) {
  if (!Display::GetMainDisplay()) {
    // TODO - return something that lets the runtime use a translatable string if unknown
    strcpy(Ptr<String>(str_dest_ptr).c()->data(), str_util::to_upper("unknown").c_str());
    return bool_to_symbol(true);
  }

  auto info = bind_assignment_info ? Ptr<BindAssignmentInfo>(bind_assignment_info).c() : NULL;
  auto port = (int)info->port;
  auto device_type = (int)info->device_type;
  auto for_button = info->buttons != s7.offset;
  auto input_idx = (int)info->input_idx;
  auto analog_min_range = info->analog_min_range != s7.offset;

  if (Display::GetMainDisplay()) {
    auto name = Display::GetMainDisplay()->get_input_manager()->get_current_bind(
        port, (InputDeviceType)device_type, for_button, input_idx, analog_min_range);
    if (name.empty()) {
      return bool_to_symbol(false);
    }
    if (g_game_version == GameVersion::Jak1) {
      // The Jak 1 font has only caps
      name = str_util::to_upper(name).c_str();
    }
    // Encode the string to the game font
    const auto encoded_name = get_font_bank_from_game_version(g_game_version)
                                  ->convert_utf8_to_game(str_util::titlize(name).c_str());
    strcpy(Ptr<String>(str_dest_ptr).c()->data(), encoded_name.c_str());
    return bool_to_symbol(true);
  }
  // TODO - return something that lets the runtime use a translatable string if unknown
  strcpy(Ptr<String>(str_dest_ptr).c()->data(), str_util::to_upper("unknown").c_str());
  return bool_to_symbol(true);
}

u64 pc_get_controller_count() {
  if (Display::GetMainDisplay()) {
    return Display::GetMainDisplay()->get_input_manager()->get_num_controllers();
  }
  return 0;
}

u64 pc_get_controller_index(u32 port) {
  if (Display::GetMainDisplay()) {
    return Display::GetMainDisplay()->get_input_manager()->get_controller_index(port);
  }
  return 0;
}

void pc_set_controller(u32 controller_id, u32 port) {
  if (Display::GetMainDisplay()) {
    Display::GetMainDisplay()->get_input_manager()->set_controller_for_port(controller_id, port);
  }
}

u32 pc_get_keyboard_enabled() {
  if (Display::GetMainDisplay()) {
    return bool_to_symbol(Display::GetMainDisplay()->get_input_manager()->is_keyboard_enabled());
  }
  return bool_to_symbol(false);
}

void pc_set_keyboard_enabled(u32 sym_val) {
  if (Display::GetMainDisplay()) {
    Display::GetMainDisplay()->get_input_manager()->enable_keyboard(symbol_to_bool(sym_val));
  }
}

void pc_set_mouse_options(u32 enabled, u32 control_camera, u32 control_movement) {
  if (Display::GetMainDisplay()) {
    Display::GetMainDisplay()->get_input_manager()->enqueue_update_mouse_options(
        symbol_to_bool(enabled), symbol_to_bool(control_camera), symbol_to_bool(control_movement));
  }
}

void pc_set_mouse_camera_sens(u32 xsens, u32 ysens) {
  float xsens_val;
  memcpy(&xsens_val, &xsens, 4);
  float ysens_val;
  memcpy(&ysens_val, &ysens, 4);
  if (Display::GetMainDisplay()) {
    Display::GetMainDisplay()->get_input_manager()->set_camera_sens(xsens_val, ysens_val);
  }
}

void pc_ignore_background_controller_events(u32 sym_val) {
  if (Display::GetMainDisplay()) {
    Display::GetMainDisplay()->get_input_manager()->enqueue_ignore_background_controller_events(
        symbol_to_bool(sym_val));
  }
}

u64 pc_current_controller_has_led() {
  if (Display::GetMainDisplay()) {
    return bool_to_symbol(Display::GetMainDisplay()->get_input_manager()->controller_has_led(0));
  }
  return bool_to_symbol(false);
}

u64 pc_current_controller_has_rumble() {
  if (Display::GetMainDisplay()) {
    return bool_to_symbol(Display::GetMainDisplay()->get_input_manager()->controller_has_rumble(0));
  }
  return bool_to_symbol(false);
}

void pc_set_controller_led(const int port, const u8 red, const u8 green, const u8 blue) {
  if (Display::GetMainDisplay()) {
    Display::GetMainDisplay()->get_input_manager()->enqueue_set_controller_led(port, red, green,
                                                                               blue);
  }
}

u64 pc_waiting_for_bind() {
  if (Display::GetMainDisplay()) {
    return bool_to_symbol(Display::GetMainDisplay()->get_input_manager()->get_waiting_for_bind());
  }
  return bool_to_symbol(false);
}

void pc_set_waiting_for_bind(InputDeviceType device_type,
                             u32 for_analog,
                             u32 for_minimum_analog,
                             s32 input_idx) {
  if (Display::GetMainDisplay()) {
    Display::GetMainDisplay()->get_input_manager()->set_wait_for_bind(
        device_type, symbol_to_bool(for_analog), symbol_to_bool(for_minimum_analog), input_idx);
  }
}

void pc_stop_waiting_for_bind() {
  if (Display::GetMainDisplay()) {
    return Display::GetMainDisplay()->get_input_manager()->stop_waiting_for_bind();
  }
}

void pc_reset_bindings_to_defaults(const int port, const InputDeviceType device_type) {
  if (Display::GetMainDisplay()) {
    Display::GetMainDisplay()->get_input_manager()->reset_input_bindings_to_defaults(port,
                                                                                     device_type);
  }
}

void pc_set_auto_hide_cursor(u32 val) {
  if (Display::GetMainDisplay()) {
    Display::GetMainDisplay()->get_input_manager()->enqueue_set_auto_hide_mouse(
        symbol_to_bool(val));
  }
}

u64 pc_get_pressure_sensitivity_enabled() {
  if (Display::GetMainDisplay()) {
    return bool_to_symbol(
        Display::GetMainDisplay()->get_input_manager()->is_pressure_sensitivity_enabled());
  }
  return bool_to_symbol(false);
}

void pc_set_pressure_sensitivity_enabled(u32 val) {
  if (Display::GetMainDisplay()) {
    Display::GetMainDisplay()->get_input_manager()->set_pressure_sensitivity_enabled(
        symbol_to_bool(val));
  }
}

void pc_set_axis_scale(u32 val) {
  if (Display::GetMainDisplay()) {
    // wow dangerous i guess
    Display::GetMainDisplay()->get_input_manager()->set_axis_scale(*((float*)&val));
  }
}

u32 pc_get_axis_scale() {
  float out = 1.33f;
  if (Display::GetMainDisplay()) {
    out = Display::GetMainDisplay()->get_input_manager()->axis_scale();
  }
  // wow dangerous i guess
  return *((u32*)&out);
}

u64 pc_current_controller_has_pressure_sensitivity() {
  if (Display::GetMainDisplay()) {
    return bool_to_symbol(
        Display::GetMainDisplay()->get_input_manager()->controller_has_pressure_sensitivity_support(
            0));
  }
  return bool_to_symbol(false);
}

u64 pc_current_controller_has_trigger_effect_support() {
  if (Display::GetMainDisplay()) {
    return bool_to_symbol(
        Display::GetMainDisplay()->get_input_manager()->controller_has_trigger_effect_support(0));
  }
  return bool_to_symbol(false);
}

u64 pc_get_trigger_effects_enabled() {
  if (Display::GetMainDisplay()) {
    return bool_to_symbol(
        Display::GetMainDisplay()->get_input_manager()->are_trigger_effects_enabled());
  }
  return bool_to_symbol(false);
}

void pc_set_trigger_effects_enabled(u32 val) {
  if (Display::GetMainDisplay()) {
    Display::GetMainDisplay()->get_input_manager()->enqueue_set_trigger_effects_enabled(
        symbol_to_bool(val));
  }
}

void pc_clear_trigger_effect(dualsense_effects::TriggerEffectOption option) {
  if (Display::GetMainDisplay()) {
    Display::GetMainDisplay()->get_input_manager()->enqueue_controller_clear_trigger_effect(0,
                                                                                            option);
  }
}

void pc_send_trigger_effect_feedback(dualsense_effects::TriggerEffectOption option,
                                     u8 position,
                                     u8 strength) {
  if (Display::GetMainDisplay()) {
    Display::GetMainDisplay()->get_input_manager()->enqueue_controller_send_trigger_effect_feedback(
        0, option, position, strength);
  }
}

void pc_send_trigger_effect_vibrate(dualsense_effects::TriggerEffectOption option,
                                    u8 position,
                                    u8 amplitude,
                                    u8 frequency) {
  if (Display::GetMainDisplay()) {
    Display::GetMainDisplay()->get_input_manager()->enqueue_controller_send_trigger_effect_vibrate(
        0, option, position, amplitude, frequency);
  }
}

void pc_send_trigger_effect_weapon(dualsense_effects::TriggerEffectOption option,
                                   u8 start_position,
                                   u8 end_position,
                                   u8 strength) {
  if (Display::GetMainDisplay()) {
    Display::GetMainDisplay()->get_input_manager()->enqueue_controller_send_trigger_effect_weapon(
        0, option, start_position, end_position, strength);
  }
}

void pc_send_trigger_rumble(u16 left_rumble, u16 right_rumble, u32 duration_ms) {
  if (Display::GetMainDisplay()) {
    Display::GetMainDisplay()->get_input_manager()->enqueue_controller_send_trigger_rumble(
        0, left_rumble, right_rumble, duration_ms);
  }
}

void pc_set_vsync(u32 sym_val) {
  Gfx::g_global_settings.vsync = symbol_to_bool(sym_val);
}

void pc_set_msaa(int samples) {
  Gfx::g_global_settings.msaa_samples = samples;
}

void pc_set_frame_rate(int rate) {
  Gfx::g_global_settings.target_fps = rate;
}

void pc_set_game_resolution(int w, int h) {
  Gfx::g_global_settings.game_res_w = w;
  Gfx::g_global_settings.game_res_h = h;
}

void pc_set_letterbox(int w, int h) {
  Gfx::g_global_settings.lbox_w = w;
  Gfx::g_global_settings.lbox_h = h;
}

void pc_set_brightness_contrast(s32 color, s32 alpha) {
  Gfx::g_global_settings.brightness_contrast_color = color;
  Gfx::g_global_settings.brightness_contrast_alpha = alpha;
}

void pc_renderer_tree_set_lod(Gfx::RendererTreeType tree, int lod) {
  switch (tree) {
    case Gfx::RendererTreeType::TFRAG3:
      Gfx::g_global_settings.lod_tfrag = lod;
      break;
    case Gfx::RendererTreeType::TIE3:
      Gfx::g_global_settings.lod_tie = lod;
      break;
    default:
      lg::error("Invalid tree {} specified for SetLod ({})", fmt::underlying(tree), lod);
      break;
  }
}

void pc_set_collision_mask(GfxGlobalSettings::CollisionRendererMode mode, s64 mask, u32 symptr) {
  if (symbol_to_bool(symptr)) {
    Gfx::CollisionRendererSetMask(mode, mask);
  } else {
    Gfx::CollisionRendererClearMask(mode, mask);
  }
}

u32 pc_get_collision_mask(GfxGlobalSettings::CollisionRendererMode mode, s64 mask) {
  return Gfx::CollisionRendererGetMask(mode, mask) ? s7.offset + true_symbol_offset(g_game_version)
                                                   : s7.offset;
}

void pc_set_collision_wireframe(u32 symptr) {
  Gfx::g_global_settings.collision_wireframe = symbol_to_bool(symptr);
}

void pc_set_collision(u32 symptr) {
  Gfx::g_global_settings.collision_enable = symbol_to_bool(symptr);
}

void pc_set_gfx_hack(u64 which, u32 symptr) {
  switch (which) {
    case 0:  // no tex
      Gfx::g_global_settings.hack_no_tex = symbol_to_bool(symptr);
      break;
  }
}

void pc_set_ssao_debug(u32 symptr) {
  Gfx::g_global_settings.ssao_depth_debug = symbol_to_bool(symptr);
}

void pc_set_ssao_depth_capture(u32 symptr) {
  Gfx::g_global_settings.ssao_depth_capture = symbol_to_bool(symptr);
}

void pc_set_ssao_option(u64 which, u64 value) {
  auto& s = Gfx::g_global_settings;
  switch (which) {
    case 0:
      s.ssao_capture_point = value ? 1 : 0;
      break;
    case 1:
      s.ssao_msaa_mode = value ? 1 : 0;
      break;
    case 2:
      s.ssao_gpu_timing = value != 0;
      break;
    case 3:
      s.ssao_msaa_probe_request = true;
      break;
    default:
      lg::error("pc-set-ssao-option: unknown option {}", which);
      break;
  }
}

void pc_set_ssao_preset(s64 index) {
  if (!Gfx::SsaoApplyPreset((int)index)) {
    Gfx::g_global_settings.ssao_enable = false;
  }
}

void pc_set_inspect(u64 enable) {
  Gfx::g_global_settings.inspect_enable = enable != 0;
}

void pc_inspect_pick(u64 x, u64 y, u64 verdict) {
  auto& s = Gfx::g_global_settings;
  auto& p = s.inspect_picks[s.inspect_pick_seq % 16];
  p[0] = x / 1000000.f;
  p[1] = y / 1000000.f;
  p[2] = (float)verdict;
  s.inspect_pick_seq++;
}

void pc_set_perf_timing(s64 frames) {
  Gfx::g_global_settings.perf_timing_window = (int)std::clamp<s64>(frames, 0, 100000);
}

void pc_set_perf_opt(s64 mask) {
  Gfx::g_global_settings.perf_opt_mask = (u32)(mask & 0xffffffff);
}

void pc_set_smaa(u64 which, s64 value) {
  auto& s = Gfx::g_global_settings;
  const int n = (int)(value / 1000000);
  switch (which) {
    case 0:
      s.aa_mode = std::clamp(n, (int)AA_MSAA, (int)AA_MSAA_SMAA);
      break;
    case 1:
      s.smaa_threshold = std::clamp(value / 1000000.f, 0.01f, 0.5f);
      break;
    case 2:
      s.smaa_luma = value != 0;
      break;
    case 3:
      s.smaa_debug = std::clamp(n, 0, 2);
      break;
    case 4: {
      // threshold, luma edges and debug view (1-3) back to the engine defaults
      static const GfxGlobalSettings d{};
      s.smaa_threshold = d.smaa_threshold;
      s.smaa_luma = d.smaa_luma;
      s.smaa_debug = d.smaa_debug;
      break;
    }
    default:
      lg::error("pc-set-smaa: unknown option {}", which);
  }
}

void pc_set_render_scale(u64 which, s64 value) {
  auto& s = Gfx::g_global_settings;
  switch (which) {
    case 0:
      s.render_scale = std::clamp(value / 1000000.f, 0.25f, 1.f);
      break;
    case 1:
      s.upscaler = std::clamp((int)(value / 1000000), (int)UPSCALE_BILINEAR, (int)UPSCALE_FSR1);
      break;
    case 2:
      s.fsr_sharpness = std::clamp(value / 1000000.f, 0.f, 4.f);
      break;
    case 3: {
      // sharpness (2) back to the engine default; 0 and 1 are settings fields
      static const GfxGlobalSettings d{};
      s.fsr_sharpness = d.fsr_sharpness;
      break;
    }
    default:
      lg::error("pc-set-render-scale: unknown option {}", which);
  }
}

void pc_set_bloom(u64 which, s64 value) {
  auto& s = Gfx::g_global_settings;
  const float v = value / 1000000.f;
  switch (which) {
    case 0:
      s.bloom_enable = value != 0;
      break;
    case 1:
      s.bloom_strength = v;
      break;
    case 2:
      s.bloom_radius = v;
      break;
    case 3:
      s.bloom_threshold = v;
      break;
    case 4:
      s.bloom_hue_clip = value != 0;
      break;
    default:
      lg::error("pc-set-bloom: unknown option {}", which);
  }
}

void pc_set_water(u64 which, s64 value) {
  auto& s = Gfx::g_global_settings;
  const float v = value / 1000000.f;
  switch ((WaterOption)which) {
    case WaterOption::ENABLE:
      s.water_enable = value != 0;
      break;
    case WaterOption::WAVE:
      s.water_wave = std::max(v, 0.f);
      break;
    case WaterOption::GLINT:
      s.water_glint = std::max(v, 0.f);
      break;
    case WaterOption::GLINT_EXP:
      s.water_glint_exp = std::max(v, 1.f);
      break;
    case WaterOption::FRESNEL:
      s.water_fresnel = std::clamp(v, 0.f, 1.f);
      break;
    case WaterOption::SPARKLE:
      s.water_sparkle = std::max(v, 0.f);
      break;
    case WaterOption::VIEW:
      s.water_view = (int)std::lround(v);
      break;
    case WaterOption::DISPLACE:
      s.water_displace = std::max(v, 0.f);
      break;
    case WaterOption::SUBDIV:
      s.water_subdiv = std::clamp((int)std::lround(v), 1, 32);
      break;
    case WaterOption::RANGE_FULL_M:
      s.water_range_full_m = std::max(v, 0.f);
      break;
    case WaterOption::RANGE_ZERO_M:
      s.water_range_zero_m = std::max(v, 1.f);
      break;
    case WaterOption::CREST_FOAM:
      s.water_crest_foam = std::clamp(v, 0.f, 1.f);
      break;
    case WaterOption::CREST_J:
      s.water_crest_j = v;
      break;
    case WaterOption::SHORE_FOAM:
      s.water_shore_foam = std::clamp(v, 0.f, 1.f);
      break;
    case WaterOption::SHORE_M:
      s.water_shore_m = std::max(v, 0.01f);
      break;
    case WaterOption::SHALLOW:
      s.water_shallow = std::clamp(v, 0.f, 1.f);
      break;
    case WaterOption::SHALLOW_M:
      s.water_shallow_m = std::max(v, 0.01f);
      break;
    case WaterOption::SSS:
      s.water_sss = std::max(v, 0.f);
      break;
    case WaterOption::CREST_LIGHT:
      s.water_crest_light = std::clamp(v, 0.f, 1.f);
      break;
    case WaterOption::ABSORB:
      s.water_absorb = std::max(v, 0.f);
      break;
    case WaterOption::SKY_REFL:
      s.water_sky_refl = std::clamp(v, 0.f, 1.f);
      break;
    case WaterOption::SKY_HAZE_M:
      s.water_sky_haze_m = std::max(v, 1.f);
      break;
    case WaterOption::AMP_NOISE:
      s.water_amp_noise = std::clamp(v, 0.f, 1.f);
      break;
    case WaterOption::PHASE_NOISE:
      s.water_phase_noise = std::max(v, 0.f);
      break;
    case WaterOption::NOISE_M:
      s.water_noise_m = std::max(v, 1.f);
      break;
    case WaterOption::SHALLOW_DEPTH_M:
      s.water_shallow_depth_m = std::max(v, 0.05f);
      break;
    case WaterOption::SHORE_FADE_NEAR_M:
      s.water_shore_fade_near_m = std::max(v, 0.f);
      break;
    case WaterOption::SHORE_FADE_FAR_M:
      s.water_shore_fade_far_m = std::max(v, 1.f);
      break;
    case WaterOption::SHOAL_M:
      s.water_shoal_m = std::max(v, 0.f);
      break;
    case WaterOption::TILE_HEX:
      s.water_tile_hex = std::clamp(v, 0.f, 1.f);
      break;
    case WaterOption::TILE_HEX_M:
      s.water_tile_hex_m = std::max(v, 1.f);
      break;
    case WaterOption::TILE_HEX_ROT_DEG:
      s.water_tile_hex_rot_deg = std::clamp(v, 0.f, 180.f);
      break;
    case WaterOption::TILE_WARP_M:
      s.water_tile_warp_m = std::max(v, 0.f);
      break;
    case WaterOption::TILE_WARP_CELL_M:
      s.water_tile_warp_cell_m = std::max(v, 1.f);
      break;
    case WaterOption::AA_LOD_START:
      s.water_aa_lod_start = v;
      break;
    case WaterOption::AA_LOD_END:
      s.water_aa_lod_end = v;
      break;
    case WaterOption::AA_FOAM_EDGE_PX:
      s.water_aa_foam_edge_px = std::max(v, 0.f);
      break;
    case WaterOption::AA_WAVE_PX_FULL:
      s.water_aa_wave_px_full = std::max(v, 0.f);
      break;
    case WaterOption::AA_WAVE_PX_ZERO:
      s.water_aa_wave_px_zero = std::max(v, 0.f);
      break;
    case WaterOption::CLOCK_S:
      s.water_clock_s = v;
      break;
    case WaterOption::SHOAL_OCC:
      s.water_shoal_occ = std::min(v, 0.f);
      break;
    case WaterOption::DEPTH_BLUR_M:
      s.water_depth_blur_m = std::max(v, 0.f);
      break;
    case WaterOption::TESS_SEG_M:
      s.water_tess_seg_m = std::max(v, 0.01f);
      break;
    case WaterOption::SUN_GLINT:
      s.water_sun_glint = value != 0;
      break;
    case WaterOption::SUN_GLINT_STRENGTH:
      s.water_sun_glint_strength = std::max(v, 0.f);
      break;
    case WaterOption::SUN_GLINT_ROUGH:
      s.water_sun_glint_rough = std::clamp(v, 0.01f, 0.5f);
      break;
    case WaterOption::SUN_GLINT_PEAK:
      s.water_sun_glint_peak = std::max(v, 0.01f);
      break;
    case WaterOption::LOD_START_M:
      s.water_lod_start_m = std::max(v, 0.f);
      break;
    case WaterOption::LOD_END_M:
      s.water_lod_end_m = std::max(v, 1.f);
      break;
    case WaterOption::LOD_PROBE:
      s.water_lod_probe = (int)std::llround(v);
      break;
    case WaterOption::SUN_GLINT_RESET: {
      static const GfxGlobalSettings d{};
      s.water_sun_glint_strength = d.water_sun_glint_strength;
      s.water_sun_glint_rough = d.water_sun_glint_rough;
      s.water_sun_glint_peak = d.water_sun_glint_peak;
      break;
    }
    default: {
      int i = (int)which - (int)WaterOption::WAVE0;
      WaterWave* set = s.water_waves;
      if (i < 0 || i >= kWaterWaves * 5) {
        i = (int)which - (int)WaterOption::LAKE_WAVE0;
        set = s.water_lake_waves;
      }
      if (i < 0 || i >= kWaterWaves * 5) {
        lg::error("pc-set-water: unknown option {}", which);
        break;
      }
      auto& w = set[i / 5];
      switch ((WaterWaveField)(i % 5)) {
        case WaterWaveField::DIR_DEG:
          w.dir_deg = v;
          break;
        case WaterWaveField::AMP_M:
          w.amp_m = std::max(v, 0.f);
          break;
        case WaterWaveField::LENGTH_M:
          w.length_m = std::max(v, 0.1f);
          break;
        case WaterWaveField::SPEED:
          w.speed = v;
          break;
        case WaterWaveField::STEEP:
          w.steep = std::clamp(v, 0.f, 1.f);
          break;
      }
    }
  }
}

void pc_set_swamp(u64 which, s64 value) {
  auto& s = Gfx::g_global_settings;
  const float v = value / 1000000.f;
  const float v01 = std::clamp(v, 0.f, 1.f);
  const float vpos = std::max(v, 0.f);
  switch ((SwampOption)which) {
    case SwampOption::WATER:
      s.swamp_water = value != 0;
      break;
    case SwampOption::WATER_DARK:
      s.swamp_water_dark = v01;
      break;
    case SwampOption::WATER_REFL:
      s.swamp_water_refl = vpos;
      break;
    case SwampOption::WATER_RIPPLE:
      s.swamp_water_ripple = vpos;
      break;
    case SwampOption::WATER_GLINT:
      s.swamp_water_glint = vpos;
      break;
    case SwampOption::WET:
      s.swamp_wet = value != 0;
      break;
    case SwampOption::WET_DARK:
      s.swamp_wet_dark = v01;
      break;
    case SwampOption::WET_COAT:
      s.swamp_wet_coat = vpos;
      break;
    case SwampOption::WET_PUDDLE:
      s.swamp_wet_puddle = v01;
      break;
    case SwampOption::MIST:
      s.swamp_mist = value != 0;
      break;
    case SwampOption::MIST_DENSITY:
      s.swamp_mist_density = vpos;
      break;
    case SwampOption::MIST_HEIGHT_M:
      s.swamp_mist_height_m = std::max(v, 0.05f);
      break;
    case SwampOption::MIST_START_M:
      s.swamp_mist_start_m = vpos;
      break;
    case SwampOption::MIST_MAX:
      s.swamp_mist_max = v01;
      break;
    case SwampOption::MIST_SUN:
      s.swamp_mist_sun = vpos;
      break;
    case SwampOption::MIST_BRIGHT:
      s.swamp_mist_bright = v01;
      break;
    case SwampOption::SHORE:
      s.shore_wet = value != 0;
      break;
    case SwampOption::SHORE_BAND_M:
      s.shore_wet_band_m = std::max(v, 0.05f);
      break;
    case SwampOption::SHORE_DARK:
      s.shore_wet_dark = v01;
      break;
    case SwampOption::SHORE_FILM:
      s.shore_wet_film = vpos;
      break;
    case SwampOption::SHORE_CELL_M:
      s.shore_wet_cell_m = std::max(v, 0.1f);
      break;
    case SwampOption::MISTY_MIST:
      s.misty_mist = value != 0;
      break;
    case SwampOption::MISTY_MIST_DENSITY:
      s.misty_mist_density = vpos;
      break;
    case SwampOption::MISTY_MIST_HEIGHT_M:
      s.misty_mist_height_m = std::max(v, 0.05f);
      break;
    case SwampOption::MISTY_MIST_START_M:
      s.misty_mist_start_m = vpos;
      break;
    case SwampOption::MISTY_MIST_MAX:
      s.misty_mist_max = v01;
      break;
    case SwampOption::MISTY_MIST_BRIGHT:
      s.misty_mist_bright = v01;
      break;
    case SwampOption::MISTY_MIST_STRENGTH:
      s.misty_mist_strength = std::clamp(v, 0.f, 4.f);
      break;
    case SwampOption::WET_PBR:
      s.wet_pbr = value != 0;
      break;
    case SwampOption::WET_PBR_GLOSS:
      s.wet_pbr_gloss = v01;
      break;
    case SwampOption::WET_PBR_ROUGH:
      s.wet_pbr_rough = std::clamp(v, 0.04f, 1.f);
      break;
    case SwampOption::WET_PBR_F0:
      s.wet_pbr_f0 = v01;
      break;
    case SwampOption::WET_PBR_DARK:
      s.wet_pbr_dark = v01;
      break;
    case SwampOption::WET_PBR_SPLASH_FULL_M:
      s.wet_pbr_splash_full_m = vpos;
      break;
    case SwampOption::WET_PBR_SPLASH_TOP_M:
      s.wet_pbr_splash_top_m = vpos;
      break;
    default:
      lg::error("pc-set-swamp: unknown option {}", which);
  }
}

void pc_set_lava(u64 which, s64 value) {
  auto& s = Gfx::g_global_settings;
  const float v = value / 1000000.f;
  const float vpos = std::max(v, 0.f);
  switch ((LavaOption)which) {
    case LavaOption::GLOW:
      s.lava_glow = value != 0;
      break;
    case LavaOption::GLOW_GAIN:
      s.lava_glow_gain = vpos;
      break;
    case LavaOption::GLOW_MASK:
      s.lava_glow_mask = std::clamp(v, 0.f, 1.f);
      break;
    case LavaOption::GLOW_KEY_LO:
      s.lava_glow_key_lo = v;
      break;
    case LavaOption::GLOW_KEY_HI:
      s.lava_glow_key_hi = v;
      break;
    case LavaOption::UNDER:
      s.lava_under = value != 0;
      break;
    case LavaOption::UNDER_GAIN:
      s.lava_under_gain = vpos;
      break;
    case LavaOption::UNDER_REACH_M:
      s.lava_under_reach_m = std::max(v, 1.f);
      break;
    case LavaOption::UNDER_REFL:
      s.lava_under_refl = vpos;
      break;
    case LavaOption::UNDER_BAKE:
      s.lava_under_bake = std::clamp(v, 0.f, 1.f);
      break;
    case LavaOption::GLOW_CORE:
      s.lava_glow_core = vpos;
      break;
    case LavaOption::GLOW_CRUST:
      s.lava_glow_crust = std::clamp(v, 0.f, 1.f);
      break;
    case LavaOption::UNDER_CHAR:
      s.lava_under_char = vpos;
      break;
    case LavaOption::PIN:
      s.lava_pin = value != 0;
      break;
    case LavaOption::RESET: {
      static const GfxGlobalSettings d{};
      s.lava_glow = d.lava_glow;
      s.lava_glow_gain = d.lava_glow_gain;
      s.lava_glow_mask = d.lava_glow_mask;
      s.lava_glow_key_lo = d.lava_glow_key_lo;
      s.lava_glow_key_hi = d.lava_glow_key_hi;
      s.lava_glow_core = d.lava_glow_core;
      s.lava_glow_crust = d.lava_glow_crust;
      s.lava_under = d.lava_under;
      s.lava_under_gain = d.lava_under_gain;
      s.lava_under_reach_m = d.lava_under_reach_m;
      s.lava_under_refl = d.lava_under_refl;
      s.lava_under_bake = d.lava_under_bake;
      s.lava_under_char = d.lava_under_char;
      s.lava_view = d.lava_view;
      s.lava_pin = d.lava_pin;
    } break;
    case LavaOption::VIEW:
      s.lava_view = (int)std::clamp<s64>(value / 1000000, 0, 2);
      break;
    default:
      lg::error("pc-set-lava: unknown option {}", which);
  }
}

/// A level's lava light this frame (mood light-group 0 dir2 while it points down; level 0 when
/// it does not) and the lava clock's factor in millionths; colour packed 10 bits per channel,
/// bit 30: vanilla lights the characters with it.
void pc_lava_light(u32 level_str, s64 level, s64 clock, u64 rgb) {
  lava_field::goal_light(Ptr<String>(level_str).c()->data(), level / 1000000.f,
                         clock / 1000000.f, (rgb & 1023) / 1000.f, ((rgb >> 10) & 1023) / 1000.f,
                         ((rgb >> 20) & 1023) / 1000.f, (rgb >> 30) & 1);
}

void pc_set_caustics(u64 which, s64 value) {
  auto& s = Gfx::g_global_settings;
  const float v = value / 1000000.f;
  const float vpos = std::max(v, 0.f);
  switch ((CausticOption)which) {
    case CausticOption::MODE:
      s.caustic_mode = value != 0 ? 1 : 0;
      break;
    case CausticOption::CELL_SCALE:
      s.caustic_cell_scale = std::max(v, 0.01f);
      break;
    case CausticOption::LOOP_HZ:
      s.caustic_loop_hz = vpos;
      break;
    case CausticOption::DRIFT_MPS:
      s.caustic_drift_mps = vpos;
      break;
    case CausticOption::AMP:
      s.caustic_amp = vpos;
      break;
    case CausticOption::WEB:
      s.caustic_web = vpos;
      break;
    case CausticOption::WEB_MEAN:
      s.caustic_web_mean = std::clamp(v, 0.f, 1.f);
      break;
    case CausticOption::CLOCK_S:
      s.caustic_clock_hold_s = v;
      break;
    default:
      lg::error("pc-set-caustics: unknown option {}", which);
  }
}

void pc_set_fur(u64 which, s64 value) {
  auto& s = Gfx::g_global_settings;
  const float v = value / 1000000.f;
  switch ((FurOption)which) {
    case FurOption::ENABLE:
      s.fur_enable = value != 0;
      break;
    case FurOption::SHELLS:
      s.fur_shells = std::clamp((int)std::lround(v), 1, 64);
      break;
    case FurOption::LENGTH_M:
      s.fur_length_m = v;
      break;
    case FurOption::DENSITY:
      s.fur_density = v;
      break;
    case FurOption::GRAVITY:
      s.fur_gravity = v;
      break;
    case FurOption::CURL:
      s.fur_curl = v;
      break;
    case FurOption::ROOT_DARK:
      s.fur_root_dark = v;
      break;
    case FurOption::VIEW:
      s.fur_view = (int)std::lround(v);
      break;
    case FurOption::SIZE_EXP:
      s.fur_size_exp = std::max(v, 0.f);
      break;
    case FurOption::SIZE_REF_M:
      s.fur_size_ref_m = std::max(v, 1e-3f);
      break;
    default:
      lg::error("pc-set-fur: unknown option {}", which);
  }
}

void pc_set_inland_water(u64 which, s64 value) {
  auto& s = Gfx::g_global_settings;
  const float v = value / 1000000.f;
  const float v01 = std::clamp(v, 0.f, 1.f);
  const float vpos = std::max(v, 0.f);
  switch ((InlandWaterOption)which) {
    case InlandWaterOption::WATER:
      s.inland_water = value != 0;
      break;
    case InlandWaterOption::DEPTH_M:
      s.inland_water_depth_m = std::max(v, 0.01f);
      break;
    case InlandWaterOption::REFL:
      s.inland_water_refl = vpos;
      break;
    case InlandWaterOption::REFL_CAP:
      s.inland_water_refl_cap = v01;
      break;
    case InlandWaterOption::GLINT:
      s.inland_water_glint = vpos;
      break;
    case InlandWaterOption::MUD:
      s.inland_mud = value != 0;
      break;
    case InlandWaterOption::MUD_DARK:
      s.inland_mud_dark = v01;
      break;
    case InlandWaterOption::MUD_REFL_CAP:
      s.inland_mud_refl_cap = v01;
      break;
    case InlandWaterOption::MUD_ROUGH:
      s.inland_mud_rough = std::clamp(v, 0.05f, 1.f);
      break;
    case InlandWaterOption::MUD_GLINT:
      s.inland_mud_glint = vpos;
      break;
    case InlandWaterOption::VIEW:
      s.inland_water_view = (int)std::lround(v);
      break;
    case InlandWaterOption::PICKED:
      s.inland_water_picked = value != 0;
      break;
    default:
      lg::error("pc-set-inland-water: unknown option {}", which);
  }
}

void pc_set_emissive_lights(u64 which, s64 value) {
  auto& s = Gfx::g_global_settings;
  const float v = value / 1000000.f;
  switch (which) {
    case 0:
      s.emissive_lights_enable = value != 0;
      break;
    case 1:
      s.emissive_lights_intensity = v;
      break;
    case 2:
      s.emissive_lights_radius = v;
      break;
    case 3:
      s.emissive_lights_falloff = v;
      break;
    case 4:
      s.emissive_lights_threshold = v;
      break;
    case 5:
      s.world_lights_count = (int)std::lround(v);
      break;
    case 6:
      s.world_lights_strong = v;
      break;
    case 7:
      s.world_lights_weak = v;
      break;
    case 8:
      s.world_lights_arc = v;
      break;
    case 9:
      s.world_lights_particle = v;
      break;
    case 10:
      s.world_lights_radius_scale = v;
      break;
    case 11:
      s.world_lights_radius_max = v;
      break;
    case 12:
      s.world_lights_lamp = v;
      break;
    case 13:
      s.emissive_lights_blend = v;
      break;
    case 14:
      s.emissive_lights_frames = std::max(1, (int)std::lround(v));
      break;
    case 15:
      s.emissive_lights_fade = std::max(1, (int)std::lround(v));
      break;
    case 16:
      s.world_lights_suppress = v;
      break;
    case 17:
      // stability burst: the next N frames are saved as <name>-fNN.png, lights logged
      g_screenshot_burst = std::max(0, (int)std::lround(v));
      break;
    case 18:
      s.world_lights_glow = v;
      break;
    case 19:
      s.world_lights_props = value != 0;
      break;
    case 27:
      s.eco_lights = value != 0;
      break;
    case 28:
      s.eco_lights_strength = std::max(v, 0.f);
      break;
    case 20:
      s.light_highlights = value != 0;
      break;
    case 21:
      s.light_highlights_strength = std::max(v, 0.f);
      break;
    case 22:
      s.light_highlights_max = std::max(v, 0.01f);
      break;
    case 23:
      s.light_highlights_damp = std::clamp(v, 0.f, 1.f);
      break;
    case 35:
      s.world_lights_glow_fade_m = std::max(v, 0.f);
      break;
    case 36:
      s.world_lights_glow_slots = std::max(0, (int)std::lround(v));
      break;
    case 37: {
      // glow prop ranking (35-36) and eco shot lights (27-28) back to the engine defaults
      static const GfxGlobalSettings d{};
      s.world_lights_glow_fade_m = d.world_lights_glow_fade_m;
      s.world_lights_glow_slots = d.world_lights_glow_slots;
      s.eco_lights = d.eco_lights;
      s.eco_lights_strength = d.eco_lights_strength;
      s.world_lights_launch = d.world_lights_launch;
      break;
    }
    case 38:
      s.world_lights_launch = value != 0;
      break;
    default:
      lg::error("pc-set-emissive-lights: unknown option {}", which);
  }
}

static bool world_lights_running() {
  const auto& s = Gfx::g_global_settings;
  return (s.ssao_enable && s.emissive_lights_enable && s.world_lights_count > 0) ||
         (s.shadow_mode != 0 && (s.shadow_lights > 0 || s.shadow_cave_dark < 1.f));
}

u64 pc_world_lights_begin(s64 hour) {
  return world_lights_running() ? world_lights::goal_begin((int)hour) : 0;
}

u64 pc_world_lights_query(s64 i) {
  return world_lights::goal_query((int)i);
}

void pc_world_lights_set(s64 i, s64 level_milli) {
  world_lights::goal_set((int)i, (int)level_milli);
}

u64 pc_eco_lights_begin() {
  const auto& s = Gfx::g_global_settings;
  const bool tiles = s.ssao_enable && s.emissive_lights_enable && s.world_lights_count > 0;
  const bool actors = s.shadow_mode != 0 && s.shadow_lights > 0;
  return world_lights::eco_begin() && (tiles || actors) ? 1 : 0;
}

// pos: a GOAL vector (game units); size_level = collision radius (game units) x 1024 + level
// 0..1000. Four arguments: on Windows the GOAL to C call passes no more.
void pc_eco_lights_push(u32 group, u32 pos, s64 size_level, s64 id) {
  constexpr float kM = 4096.f;
  float v[3];
  memcpy(v, Ptr<u8>(pos).c(), sizeof(v));
  const float pos_m[3] = {v[0] / kM, v[1] / kM, v[2] / kM};
  world_lights::eco_push(Ptr<String>(group).c()->data(), pos_m, (float)(size_level >> 10) / kM,
                         (float)(size_level & 1023) / 1000.f, (u32)id);
}

// one sparticle-launch-control spawn: group name, origin (GOAL vector, game units), entity aid
// (0: none), launcher quaternion (0: none)
void pc_launch_lights_push(u32 group, u32 pos, s64 aid, u32 quat) {
  constexpr float kM = 4096.f;
  if (!group || !pos) {
    return;
  }
  float v[3], q[4];
  memcpy(v, Ptr<u8>(pos).c(), sizeof(v));
  if (quat) {
    memcpy(q, Ptr<u8>(quat).c(), sizeof(q));
  }
  const float pos_m[3] = {v[0] / kM, v[1] / kM, v[2] / kM};
  world_lights::launch_push(Ptr<String>(group).c()->data(), pos_m, (u32)aid, quat ? q : nullptr);
}

void pc_launch_lights_end(s64 paused) {
  world_lights::launch_end(paused != 0);
}

u64 pc_eco_lights_known(u32 group) {
  return world_lights::eco_known(Ptr<String>(group).c()->data()) ? 1 : 0;
}

void pc_eco_lights_end() {
  world_lights::eco_end();
}

void pc_set_ssao_param(u64 which, s64 value) {
  auto& s = Gfx::g_global_settings;
  auto& p = s.ssao_params;
  const float v = value / 1000000.f;
  const int iv = (int)std::lround(v);
  switch ((SsaoParam)which) {
    case SsaoParam::ENABLE:
      s.ssao_enable = value != 0;
      return;
    case SsaoParam::VIEW:
      s.ssao_view = iv;
      return;
    case SsaoParam::GPU_TIMING:
      s.ssao_gpu_timing = value != 0;
      return;
    case SsaoParam::NORMAL_TARGET:
      s.ssao_normal_target = value != 0;
      return;
    case SsaoParam::EMISSIVE:
      s.ssao_emissive = value != 0;
      return;
    case SsaoParam::FOG_WEIGHT:
      s.ssao_fog_weight = std::max(v, 0.f);
      return;
    case SsaoParam::EMISSIVE_LO:
      s.ssao_emissive_lo = v;
      return;
    case SsaoParam::EMISSIVE_HI:
      s.ssao_emissive_hi = v;
      return;
    case SsaoParam::RADIUS:
      p.radius_m = v;
      break;
    case SsaoParam::INTENSITY:
      p.intensity = v;
      break;
    case SsaoParam::BIAS:
      p.bias_m = v;
      break;
    case SsaoParam::BLUR_RADIUS:
      p.blur_radius = iv;
      break;
    case SsaoParam::BLUR_SHARPNESS:
      p.blur_sharpness = v;
      break;
    case SsaoParam::MERC_WEIGHT:
      p.merc_weight = v;
      break;
    case SsaoParam::LUMA_FLOOR:
      p.luma_floor = v;
      break;
    case SsaoParam::CAVE_SCALE:
      p.cave_scale = v;
      break;
    case SsaoParam::TAPS:
      p.taps = iv;
      break;
    case SsaoParam::CREASE_ANGLE:
      p.crease_deg = v;
      break;
    case SsaoParam::CREASE_FLOOR:
      p.crease_floor = v;
      break;
    default:
      lg::error("pc-set-ssao-param: unknown param {}", which);
      return;
  }
  s.ssao_preset = -1;
}

void pc_set_hdr_target(u32 symptr) {
  Gfx::g_global_settings.hdr_target = symbol_to_bool(symptr);
}

void pc_set_tonemap(u64 which, s64 value) {
  auto& s = Gfx::g_global_settings;
  const float v = value / 1000000.f;
  switch ((TonemapOption)which) {
    case TonemapOption::MODE:
      s.tonemap_mode = (int)std::lround(v);
      break;
    case TonemapOption::EXPOSURE:
      s.tonemap_exposure = v;
      break;
    case TonemapOption::KNEE:
      s.tonemap_knee = v;
      break;
    case TonemapOption::PAPER_WHITE:
      s.paper_white_nits = v;
      break;
    case TonemapOption::TOON_KNEE:
      s.tonemap_toon_knee = v;
      break;
    case TonemapOption::TOON_RANGE:
      s.tonemap_toon_range = v;
      break;
    case TonemapOption::WHITE_START:
      s.tonemap_white_start = v;
      break;
    case TonemapOption::WHITE_FULL:
      s.tonemap_white_full = v;
      break;
    case TonemapOption::HALO_START:
      s.tonemap_halo_start = v;
      break;
    case TonemapOption::HALO_FULL:
      s.tonemap_halo_full = v;
      break;
    default:
      lg::error("pc-set-tonemap: unknown option {}", which);
      break;
  }
}

void pc_set_hdr_output(u64 which, s64 value) {
  auto& s = Gfx::g_global_settings;
  const float v = value / 1000000.f;
  switch ((HdrOutputOption)which) {
    case HdrOutputOption::ENABLE:
      s.hdr_output = v != 0.f;
      break;
    case HdrOutputOption::FORMAT:
      s.hdr_format = (int)std::lround(v);
      break;
    case HdrOutputOption::PAPER_WHITE:
      s.paper_white_nits = v;
      break;
    case HdrOutputOption::PEAK:
      s.hdr_peak_nits = v;
      break;
    case HdrOutputOption::BLACK:
      s.hdr_black_nits = v;
      break;
    case HdrOutputOption::SATURATION:
      s.hdr_saturation = v;
      break;
    case HdrOutputOption::KNEE:
      s.hdr_knee = v;
      break;
    case HdrOutputOption::OS_SWITCH:
      s.hdr_os_switch = v != 0.f;
      break;
    case HdrOutputOption::PEAK_MILLI:
      s.hdr_peak_nits = value / 1000.f;
      break;
    case HdrOutputOption::CAL_PATTERN:
      s.hdr_cal_pattern = (int)std::lround(v);
      break;
    default:
      lg::error("pc-set-hdr-output: unknown option {}", which);
      break;
  }
}

/// pc-get-hdr-output, in thousandths (GOAL's int-to-float is 32-bit, millionths overflow above
/// 2147 nits): 0 HDR output presenting (1000) or not (0), 1 display peak and 2 display black in
/// nits (negative until the display was queried), 3 effective peak, 4 effective black, 5 the peak
/// setting as last sent (negative = from the display), 6 Windows' SDR white (negative if unknown).
s64 pc_get_hdr_output(u64 which) {
  const auto& h = Gfx::g_hdr_present;
  switch (which) {
    case 0:
      return h.active && h.hdr ? 1000 : 0;
    case 1:
      return std::llround(h.display_peak_nits * 1000.0);
    case 2:
      return std::llround(h.display_black_nits * 1000.0);
    case 3:
      return std::llround(h.peak_nits * 1000.0);
    case 4:
      return std::llround(h.black_nits * 1000.0);
    case 5:
      return std::llround(Gfx::g_global_settings.hdr_peak_nits * 1000.0);
    case 6:
      return std::llround(h.desktop_white_nits * 1000.0);
    default:
      lg::error("pc-get-hdr-output: unknown value {}", which);
      return 0;
  }
}

u64 pc_get_sky_bodies() {
  const auto& s = Gfx::g_global_settings;
  return s.sun_disc ? (u64)s.drawn_sky_bodies : 0;
}

void pc_set_sun_option(u64 which, s64 value) {
  auto& s = Gfx::g_global_settings;
  const float v = value / 1000000.f;
  const int iv = (int)std::lround(v);
  switch ((SunOption)which) {
    case SunOption::ENABLE:
      s.sun_enable = value != 0;
      break;
    case SunOption::MODE:
      s.sun_mode = iv;
      break;
    case SunOption::OFFSET_DEG:
      s.sun_offset_deg = v;
      break;
    case SunOption::NIGHT:
      s.sun_night = iv;
      break;
    case SunOption::STRENGTH:
      s.sun_strength = v;
      break;
    case SunOption::VIEW:
      s.sun_view = iv;
      break;
    case SunOption::ARC_POS:
      s.sun_arc_pos = v;
      break;
    case SunOption::DISC:
      s.sun_disc = value != 0;
      break;
    case SunOption::DISC_SIZE_DEG:
      s.sun_disc_size_deg = std::clamp(v, 0.05f, 20.f);
      break;
    case SunOption::DISC_GLARE:
      s.sun_disc_glare = std::clamp(v, 0.f, 1.f);
      break;
    case SunOption::DISC_CORE:
      s.sun_disc_core = std::max(v, 0.f);
      break;
    case SunOption::DISC_RIM:
      s.sun_disc_rim = std::max(v, 0.f);
      break;
    case SunOption::DISC_SDR_CORE:
      s.sun_disc_sdr_core = std::max(v, 0.f);
      break;
    case SunOption::DISC_SDR_RIM:
      s.sun_disc_sdr_rim = std::max(v, 0.f);
      break;
    case SunOption::DISC_FADE:
      s.sun_disc_fade = std::clamp(v, 0.01f, 1.f);
      break;
    case SunOption::DISC_HDR:
      s.sun_disc_hdr = std::clamp(iv, 0, 2);
      break;
    case SunOption::DISC_RESET: {
      static const GfxGlobalSettings d{};
      s.sun_disc_size_deg = d.sun_disc_size_deg;
      s.sun_disc_glare = d.sun_disc_glare;
      s.sun_disc_core = d.sun_disc_core;
      s.sun_disc_rim = d.sun_disc_rim;
      s.sun_disc_sdr_core = d.sun_disc_sdr_core;
      s.sun_disc_sdr_rim = d.sun_disc_sdr_rim;
      s.sun_disc_fade = d.sun_disc_fade;
      s.sun_disc_hdr = d.sun_disc_hdr;
      break;
    }
    case SunOption::SKY_STYLE:
      s.sky_style = std::clamp(iv, 0, 1);
      break;
    case SunOption::SKY_BURST:
      s.sky_burst = std::clamp(v, 0.f, 4.f);
      break;
    case SunOption::SKY_WASH:
      s.sky_wash = std::clamp(v, 0.f, 4.f);
      break;
    case SunOption::SKY_GLOW:
      s.sky_glow = std::max(v, 0.f);
      break;
    case SunOption::SKY_MOON:
      s.sky_moon = std::max(v, 0.f);
      break;
    case SunOption::SKY_MOON_SDR:
      s.sky_moon_sdr = std::max(v, 0.f);
      break;
    case SunOption::SKY_STARS:
      s.sky_stars = std::clamp(v, 0.f, 4.f);
      break;
    case SunOption::SKY_MOON_DEG:
      s.sky_moon_deg = std::clamp(v, 0.1f, 20.f);
      break;
    case SunOption::SKY_GLOW_SDR:
      s.sky_glow_sdr = std::max(v, 0.f);
      break;
    case SunOption::SKY_RESET: {
      static const GfxGlobalSettings d{};
      s.sky_style = d.sky_style;
      s.sky_burst = d.sky_burst;
      s.sky_wash = d.sky_wash;
      s.sky_glow = d.sky_glow;
      s.sky_moon = d.sky_moon;
      s.sky_moon_sdr = d.sky_moon_sdr;
      s.sky_stars = d.sky_stars;
      s.sky_moon_deg = d.sky_moon_deg;
      s.sky_glow_sdr = d.sky_glow_sdr;
      break;
    }
    default:
      lg::error("pc-set-sun-option: unknown option {}", which);
  }
}

// world sun map size and cascades per shadow quality tier (option 40): low, medium, high
constexpr int kWorldShadowTier[3][2] = {{1024, 2}, {2048, 3}, {4096, 3}};

void pc_set_shadow_option(u64 which, s64 value) {
  auto& s = Gfx::g_global_settings;
  const float v = value / 1000000.f;
  const int iv = (int)std::lround(v);
  switch ((ShadowOption)which) {
    case ShadowOption::MODE:
      s.shadow_mode = std::clamp(iv, 0, 1);
      break;
    case ShadowOption::STRENGTH:
      s.shadow_strength = v;
      break;
    case ShadowOption::VIEW:
      s.shadow_view = iv;
      break;
    case ShadowOption::RANGE_M:
      s.shadow_range_m = std::max(v, 1.f);
      break;
    case ShadowOption::LAMP_SCALE:
      s.shadow_lamp_scale = v;
      break;
    case ShadowOption::MARKER:
      s.shadow_marker = std::max(v, 0.f);
      break;
    case ShadowOption::REACH_M:
      s.shadow_reach_m = std::max(v, 0.1f);
      break;
    case ShadowOption::LIGHTS:
      s.shadow_lights = std::clamp(iv, 0, 4);
      break;
    case ShadowOption::LIGHT_R0_M:
      s.shadow_light_r0_m = std::max(v, 0.1f);
      break;
    case ShadowOption::LIGHT_REACH_M:
      s.shadow_light_reach_m = std::max(v, 1.f);
      break;
    case ShadowOption::CAVE_DARK:
      s.shadow_cave_dark = std::clamp(v, 0.f, 1.f);
      break;
    case ShadowOption::TERRAIN_SUN:
      s.shadow_terrain_sun = std::clamp(v, 0.f, 1.f);
      break;
    case ShadowOption::SKY_HUE:
      s.shadow_sky_hue = v != 0.f;
      break;
    case ShadowOption::SKY_HUE_STRENGTH:
      s.shadow_sky_hue_strength = std::clamp(v, 0.f, 2.f);
      break;
    case ShadowOption::WORLD:
      s.shadow_world = v != 0.f;
      break;
    case ShadowOption::WORLD_ACTORS:
      s.shadow_world_actors = v != 0.f;
      break;
    case ShadowOption::WORLD_WATER:
      s.shadow_world_water = v != 0.f;
      break;
    case ShadowOption::WORLD_SIZE:
      s.shadow_world_size = iv >= 4096 ? 4096 : (iv >= 2048 ? 2048 : 1024);
      break;
    case ShadowOption::WORLD_CASCADES:
      s.shadow_world_cascades = std::clamp(iv, 1, 3);
      break;
    case ShadowOption::WORLD_PENUMBRA:
      s.shadow_world_penumbra = std::clamp(v, 0.f, 0.1f);
      break;
    case ShadowOption::WORLD_RANGE_M:
      s.shadow_world_range_m = std::clamp(v, 20.f, 400.f);
      break;
    case ShadowOption::WORLD_RESET: {
      static const GfxGlobalSettings d{};
      s.shadow_world = d.shadow_world;
      s.shadow_world_actors = d.shadow_world_actors;
      s.shadow_world_water = d.shadow_world_water;
      s.shadow_world_size = kWorldShadowTier[std::clamp(s.shadow_quality, 0, 2)][0];
      s.shadow_world_cascades = kWorldShadowTier[std::clamp(s.shadow_quality, 0, 2)][1];
      s.shadow_world_penumbra = d.shadow_world_penumbra;
      s.shadow_world_range_m = d.shadow_world_range_m;
      break;
    }
    case ShadowOption::QUALITY:
      s.shadow_quality = std::clamp(iv, 0, 2);
      s.shadow_world_size = kWorldShadowTier[s.shadow_quality][0];
      s.shadow_world_cascades = kWorldShadowTier[s.shadow_quality][1];
      break;
    case ShadowOption::CANOPY:
      s.shadow_canopy = std::clamp(v, 0.f, 1.f);
      break;
    case ShadowOption::CANOPY_AMBIENT:
      s.shadow_canopy_ambient = std::clamp(v, 0.f, 0.8f);
      break;
    case ShadowOption::CANOPY_PATCH:
      s.shadow_canopy_patch = std::clamp(v, 0.f, 0.5f);
      break;
    case ShadowOption::CANOPY_SHARP:
      s.shadow_canopy_sharp = std::clamp(v, 0.f, 1.f);
      break;
    case ShadowOption::CANOPY_PENUMBRA:
      s.shadow_canopy_penumbra = std::clamp(v, 0.f, 0.2f);
      break;
    case ShadowOption::CANOPY_TINT:
      s.shadow_canopy_tint = std::clamp(v, 0.f, 1.f);
      break;
    default:
      lg::error("pc-set-shadow-option: unknown option {}", which);
  }
}

void pc_set_pbr_param(u64 which, s64 value) {
  auto& s = Gfx::g_global_settings;
  const float v = value / 1000000.f;
  switch ((PbrParam)which) {
    case PbrParam::ENABLE:
      s.pbr_enable = value != 0;
      break;
    case PbrParam::STRENGTH:
      s.pbr_strength = v;
      break;
    case PbrParam::DETAIL:
      s.pbr_detail = v;
      break;
    case PbrParam::SPEC:
      s.pbr_spec = v;
      break;
    case PbrParam::VIEW:
      s.pbr_view = (int)std::lround(v);
      break;
    case PbrParam::MERC:
      s.pbr_merc = value != 0;
      break;
    case PbrParam::MERC_VSIGN:
      s.pbr_merc_vsign = v < 0 ? -1.f : 1.f;
      break;
    case PbrParam::BACKGROUND:
      s.pbr_background = value != 0;
      break;
    case PbrParam::KEY:
      s.pbr_key = v;
      break;
    case PbrParam::SOURCE:
      s.pbr_source = value != 0 ? 1 : 0;
      break;
    case PbrParam::TIMING:
      s.pbr_timing = value != 0;
      break;
    case PbrParam::LOOK:
      s.pbr_look = (int)std::lround(v);
      pbr_load_preset();
      break;
    case PbrParam::METALLIC:
      s.pbr_metallic = v;
      break;
    case PbrParam::ENV:
      s.pbr_env = v;
      break;
    case PbrParam::FRESNEL:
      s.pbr_fresnel = v;
      break;
    case PbrParam::AMBIENT:
      s.pbr_ambient = v;
      break;
    case PbrParam::ROUGH_CONTRAST:
      s.pbr_rough_contrast = v;
      break;
    case PbrParam::BAND:
      s.pbr_band = v;
      break;
    case PbrParam::LOBE:
      s.pbr_lobe = v;
      break;
    case PbrParam::METAL_DARKEN:
      s.pbr_metal_darken = v;
      break;
    case PbrParam::MERC_UP:
      s.pbr_merc_up = v < 0 ? -1.f : 1.f;
      break;
    case PbrParam::SPECULAR:
      s.pbr_specular = v;
      break;
    case PbrParam::PARALLAX:
      s.pbr_parallax = value != 0;
      break;
    case PbrParam::PARALLAX_DEPTH:
      s.pbr_px_depth = v;
      break;
    case PbrParam::PARALLAX_BASIS:
      s.pbr_px_basis = value != 0;
      break;
    case PbrParam::METAL_HUE:
      s.pbr_metal_hue = std::clamp(v, 0.f, 1.f);
      break;
    case PbrParam::TRANSMIT:
      s.pbr_transmit = std::max(v, 0.f);
      break;
    case PbrParam::WRAP:
      s.pbr_wrap = std::max(v, 0.f);
      break;
    case PbrParam::WIND_TIE:
      s.pbr_wind_tie = value != 0;
      break;
    case PbrParam::BAKE_WRAP:
      s.pbr_bake_wrap = std::max(v, 0.f);
      break;
    case PbrParam::SUN_OCCLUSION:
      s.pbr_transmit_occl = value != 0;
      break;
    case PbrParam::LEAF_DENSITY:
      s.pbr_transmit_density = std::clamp(v, 0.f, 4.f);
      break;
    case PbrParam::ICE:
      s.pbr_ice = std::max(v, 0.f);
      break;
    case PbrParam::GLINT:
      s.pbr_glint = std::max(v, 0.f);
      break;
    default:
      lg::error("pc-set-pbr-param: unknown param {}", which);
  }
}

void pc_set_sun_dir(s64 x, s64 y, s64 z) {
  auto& d = Gfx::g_global_settings.sun_dir;
  d[0] = x / 1000000.f;
  d[1] = y / 1000000.f;
  d[2] = z / 1000000.f;
}

void pc_set_stereo(u32 symptr) {
  Gfx::g_global_settings.stereo_enable = symbol_to_bool(symptr);
}

void pc_set_stereo_option(u64 which, u64 value) {
  auto& s = Gfx::g_global_settings;
  switch (which) {
    case 0:
      s.stereo_ipd_m = (s64)value / 1000000.f;  // micrometres
      break;
    case 1:
      s.stereo_world_scale = std::max((s64)value, (s64)1) / 10000.f;  // 1/10000ths
      break;
    case 2:
      s.stereo_swap_eyes = value != 0;
      break;
    default:
      lg::error("pc-set-stereo-option: unknown option {}", which);
      break;
  }
}

void pc_set_xr(u32 symptr) {
  Gfx::g_global_settings.xr_enable = symbol_to_bool(symptr);
}

void pc_set_xr_recentre() {
  Gfx::g_global_settings.xr_recentre_requests++;
}

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
  snd_SetSpatialMode(static_cast<s32>(*mode));
}

void pc_set_vista(u32 level_str) {
  auto& s = Gfx::g_global_settings;
  std::string name = Ptr<String>(level_str).c()->data();
  s.vista_enable = !name.empty() && name != "none";
  s.vista_neighbours = name == "neighbours";
  if (s.vista_enable) {
    strncpy(s.vista_level, name.c_str(), sizeof(s.vista_level) - 1);
  }
}

void pc_set_vista_hide(u32 filter_str) {
  auto& s = Gfx::g_global_settings;
  strncpy(s.vista_hide, Ptr<String>(filter_str).c()->data(), sizeof(s.vista_hide) - 1);
}

void pc_set_vista_option(u64 which, u64 value) {
  auto& s = Gfx::g_global_settings;
  switch (which) {
    case 0:
      s.vista_hide_standins = value != 0;
      break;
    case 1:
      s.vista_fog_mode = value ? VISTA_FOG_MAIN : VISTA_FOG_OWN;
      break;
    case 2:
      s.vista_fog_end_m = (float)std::max((s64)value, (s64)1);
      break;
    case 3:
      s.vista_draw_shrub = value != 0;
      break;
    case 4:
      s.vista_gpu_timing = value != 0;
      break;
    case 5:
      s.vista_tod_addr = (u32)value;
      break;
    case 6:
      s.vista_hour_override = (s64)value < 0 ? -1.f : (float)value / 100.f;
      break;
    case 7:
      s.vista_own_mood = value != 0;
      break;
    case 8:
      s.vista_draw_alpha = value != 0;
      break;
    case 9:
      s.vista_hide_medres = value != 0;
      break;
    case 10:
      s.vista_hide_neighbour_lowres = value != 0;
      break;
    case 11:
      s.vista_landmarks = value != 0;
      break;
    case 12:
      s.vista_standin_neighbours = value != 0;
      break;
    case 13:
      s.vista_fog_floor = value != 0;
      break;
    default:
      lg::error("pc-set-vista-option: unknown option {}", which);
      break;
  }
}

/// True while neighbour mode wants landmark actors of undisplayed game levels (vista-landmarks.gc).
u32 pc_vista_landmarks() {
  const auto& s = Gfx::g_global_settings;
  return bool_to_symbol(s.vista_enable && s.vista_neighbours && s.vista_landmarks &&
                        neighbours::is_outdoor(neighbours::viewer()));
}

/// The level the player is in (level-get-target-inside), sent every frame; gates neighbour mode.
void pc_set_vista_viewer(u32 level_str) {
  const std::string name = Ptr<String>(level_str).c()->data();
  neighbours::set_viewer(name == "none" ? "" : name);
}

/// True while the loader keeps `level_str` resident as a neighbour the game has not loaded.
u32 pc_vista_neighbour(u32 level_str) {
  const auto& s = Gfx::g_global_settings;
  if (!(s.vista_enable && s.vista_neighbours)) {
    return bool_to_symbol(false);
  }
  std::vector<std::string> game, adj;
  neighbours::get_resident(&game, &adj);
  const std::string name = Ptr<String>(level_str).c()->data();
  return bool_to_symbol(std::find(adj.begin(), adj.end(), name) != adj.end());
}

/*!
 * Vista fog policy: mode is "vanilla", "push_out", "own" or "unified". push_out and own take
 * a = fog end in m; unified takes a = start in m, b = density in 0.001/km, c = scale height
 * in m (0: no height falloff). Negative values keep the current setting.
 */
void pc_set_vista_fog(u32 mode_str, u64 a, u64 b, u64 c) {
  auto& s = Gfx::g_global_settings;
  const std::string mode = Ptr<String>(mode_str).c()->data();
  const s64 pa = (s64)a, pb = (s64)b, pc = (s64)c;
  if (mode == "vanilla") {
    s.vista_fog_mode = VISTA_FOG_MAIN;
  } else if (mode == "push_out" || mode == "own") {
    s.vista_fog_mode = mode == "own" ? VISTA_FOG_OWN : VISTA_FOG_PUSH_OUT;
    if (pa > 0) {
      s.vista_fog_end_m = (float)pa;
    }
  } else if (mode == "unified") {
    s.vista_fog_mode = VISTA_FOG_UNIFIED;
    if (pa >= 0) {
      s.vista_atmo_start_m = (float)pa;
    }
    if (pb >= 0) {
      s.vista_atmo_density_per_km = pb / 1000.f;
    }
    if (pc >= 0) {
      s.vista_atmo_height_m = (float)pc;
    }
  } else {
    lg::error("pc-set-vista-fog: unknown mode {}", mode);
  }
}

namespace {
VistaNeighbourFog* vista_nb_fog_entry(const std::string& level, const char* who) {
  auto& s = Gfx::g_global_settings;
  for (int i = 0; i < s.vista_nb_fog_count; i++) {
    if (level == s.vista_nb_fog[i].level) {
      return &s.vista_nb_fog[i];
    }
  }
  if (s.vista_nb_fog_count >= (int)s.vista_nb_fog.size() ||
      level.size() >= sizeof(VistaNeighbourFog::level)) {
    lg::error("{}: cannot add {}", who, level);
    return nullptr;
  }
  VistaNeighbourFog* f = &s.vista_nb_fog[s.vista_nb_fog_count++];
  *f = VistaNeighbourFog();
  strncpy(f->level, level.c_str(), sizeof(f->level) - 1);
  return f;
}
}  // namespace

/*!
 * Per-neighbour unified-fog override: density scale in thousandths, fog colour tint packed as
 * 0xRRGGBB (128 per channel = 1.0) and fog cap (0-1) in thousandths. A negative value keeps the
 * default. Level "clear" drops every override. Kernel functions take at most four arguments.
 */
void pc_set_vista_neighbour_fog(u32 level_str, u64 density, u64 tint, u64 cap) {
  auto& s = Gfx::g_global_settings;
  const std::string level = Ptr<String>(level_str).c()->data();
  if (level == "clear") {
    s.vista_nb_fog_count = 0;
    return;
  }
  VistaNeighbourFog* f = vista_nb_fog_entry(level, "pc-set-vista-neighbour-fog");
  if (!f) {
    return;
  }
  auto milli = [](u64 v, float dflt) { return (s64)v < 0 ? dflt : (s64)v / 1000.f; };
  f->density_scale = milli(density, 1.f);
  for (int c = 0; c < 3; c++) {
    f->tint[c] = (s64)tint < 0 ? 1.f : ((tint >> (16 - 8 * c)) & 0xff) / 128.f;
  }
  f->max_fog = milli(cap, -1.f);
}

/*!
 * Haze in vista neighbour mode (unified fog), on every background draw past a start distance:
 * density in thousandths of 1/km (0: off), start distance in m, the haze colour as a tint of the
 * viewer's fog colour packed as 0xRRGGBB (128 per channel = 1.0), and the cap on the haze amount
 * in thousandths. A negative value keeps the current setting.
 */
void pc_set_vista_haze(u64 density, u64 start, u64 tint, u64 cap) {
  auto& s = Gfx::g_global_settings;
  if ((s64)density >= 0) {
    s.vista_haze_density_per_km = (s64)density / 1000.f;
  }
  if ((s64)start >= 0) {
    s.vista_haze_start_m = (float)(s64)start;
  }
  if ((s64)tint >= 0) {
    for (int c = 0; c < 3; c++) {
      s.vista_haze_tint[c] = ((tint >> (16 - 8 * c)) & 0xff) / 128.f;
    }
  }
  if ((s64)cap >= 0) {
    s.vista_haze_max = std::min((s64)cap / 1000.f, 1.f);
  }
}

/*!
 * Per-neighbour haze multiplier in thousandths (1000 = the global haze density).
 */
void pc_set_vista_neighbour_haze(u32 level_str, u64 scale) {
  const std::string level = Ptr<String>(level_str).c()->data();
  VistaNeighbourFog* f = vista_nb_fog_entry(level, "pc-set-vista-neighbour-haze");
  if (f) {
    f->haze_scale = (s64)scale < 0 ? 1.f : (s64)scale / 1000.f;
  }
}

u32 pc_get_os() {
#ifdef _WIN32
  return g_pc_port_funcs.intern_from_c("windows").offset;
#elif __linux__
  return g_pc_port_funcs.intern_from_c("linux").offset;
#elif __APPLE__
  return g_pc_port_funcs.intern_from_c("darwin").offset;
#else
  return s7.offset;
#endif
}

time_t pc_get_unix_timestamp() {
  return std::time(nullptr);
}

u64 pc_filepath_exists(u32 filepath) {
  auto filepath_str = std::string(Ptr<String>(filepath).c()->data());
  return bool_to_symbol(fs::exists(filepath_str));
}

u64 pc_mkdir_filepath(u32 filepath) {
  auto filepath_str = std::string(Ptr<String>(filepath).c()->data());
  return bool_to_symbol(file_util::create_dir_if_needed_for_file(filepath_str));
}

void pc_prof(u32 name, ProfNode::Kind kind) {
  prof().event(Ptr<String>(name).c()->data(), kind);
}

std::mt19937 extra_random_generator;
u32 pc_rand() {
  return (u32)extra_random_generator();
}

void pc_treat_pad0_as_pad1(u32 symptr) {
  Gfx::g_debug_settings.treat_pad0_as_pad1 = symbol_to_bool(symptr);
}

u32 pc_is_imgui_visible() {
  return bool_to_symbol(Gfx::g_debug_settings.show_imgui);
}

void pc_screen_shot() {
  g_want_screenshot = true;
}

u64 pc_snap_poll() {
  return snap::poll();
}

u64 pc_bug_report_poll() {
  return bug_report::poll();
}

void pc_bug_report_request(u32 source) {
  bug_report::request((bug_report::Source)std::clamp<int>((s32)source, 0, 2));
}

void pc_bug_report_save(u32 state) {
  const auto* s = Ptr<bug_report::GoalState>(state).c();
  auto str = [](u32 p) { return p ? std::string(Ptr<String>(p).c()->data()) : std::string(); };
  bug_report::save(*s, str(s->level), str(s->cont));
}

void pc_register_screen_shot_settings(u32 ptr) {
  register_screen_shot_settings(Ptr<ScreenShotSettings>(ptr).c());
}

void pc_encode_utf8_string(u32 src_str_ptr, u32 str_dest_ptr) {
  auto str = std::string(Ptr<String>(src_str_ptr).c()->data());
  std::string version = version_to_game_name(g_game_version);
  const std::string font_bank_name = version == "jak1" ? "jak1-v2" : version;
  std::string converted =
      get_font_bank(get_text_version_from_name(font_bank_name))->convert_utf8_to_game(str);
  strcpy(Ptr<String>(str_dest_ptr).c()->data(), converted.c_str());
}

/// Initializes all functions that are common across all game versions
/// These functions have the same implementation and do not use any game specific functions (other
/// than the one to create a function in the first place)
void init_common_pc_port_functions(
    std::function<Ptr<Function>(const char*, void*)> make_func_symbol_func,
    std::function<InternFromCInfo(const char*)> intern_from_c_func,
    std::function<u64(const char*)> make_string_from_c_func) {
  g_pc_port_funcs.intern_from_c = intern_from_c_func;
  g_pc_port_funcs.make_string_from_c = make_string_from_c_func;
  // Get a 300MHz timer value. Called from EE thread
  make_func_symbol_func("__read-ee-timer", (void*)read_ee_timer);
  // Do a fast memory copy.
  make_func_symbol_func("__mem-move", (void*)pc_memmove);
  // Called from game thread to submit rendering DMA chain.
  make_func_symbol_func("__send-gfx-dma-chain", (void*)send_gfx_dma_chain);
  // Called from game thread to upload a texture outside of the main DMA chain.
  make_func_symbol_func("__pc-texture-upload-now", (void*)pc_texture_upload_now);
  make_func_symbol_func("__pc-texture-relocate", (void*)pc_texture_relocate);
  // Called from the game thread at initialization. The game thread is the only one to touch the
  // mips2c function table (through the linker and ugh this function), so no locking is needed.
  make_func_symbol_func("__pc-get-mips2c", (void*)pc_get_mips2c);
  make_func_symbol_func("__pc-force-reload-all-levels", (void*)pc_force_reload_all);
  make_func_symbol_func("__pc-force-reload-level", (void*)pc_force_reload_level);
  make_func_symbol_func("__pc-force-reload-common-level", (void*)pc_force_reload_common);

  // -- DISPLAY RELATED --
  // Returns the name of the display with the given id or #f if not found / empty
  make_func_symbol_func("pc-get-display-id", (void*)pc_get_display_id);
  make_func_symbol_func("pc-set-display-id!", (void*)pc_set_display_id);
  make_func_symbol_func("pc-get-display-name", (void*)pc_get_display_name);
  make_func_symbol_func("pc-get-display-mode", (void*)pc_get_display_mode);
  make_func_symbol_func("pc-set-display-mode!", (void*)pc_set_display_mode);
  make_func_symbol_func("pc-get-display-count", (void*)pc_get_display_count);
  // Returns resolution of the monitor's current display mode
  make_func_symbol_func("pc-get-active-display-size", (void*)pc_get_active_display_size);
  // Returns the current refresh rate of the currently selected monitor's display mode.
  make_func_symbol_func("pc-get-active-display-refresh-rate",
                        (void*)pc_get_active_display_refresh_rate);
  // Returns size of window. Called from game thread
  make_func_symbol_func("pc-get-window-size", (void*)pc_get_window_size);
  // Returns scale of window. This is for DPI stuff.
  make_func_symbol_func("pc-get-window-scale", (void*)pc_get_window_scale);
  make_func_symbol_func("pc-set-window-size!", (void*)pc_set_window_size);
  make_func_symbol_func("pc-get-num-resolutions", (void*)pc_get_num_resolutions);
  make_func_symbol_func("pc-get-resolution", (void*)pc_get_resolution);
  make_func_symbol_func("pc-is-supported-resolution?", (void*)pc_is_supported_resolution);

  // -- INPUT RELATED --
  // Returns the name of the display with the given id or #f if not found / empty
  make_func_symbol_func("pc-get-controller-name", (void*)pc_get_controller_name);
  make_func_symbol_func("pc-get-current-bind", (void*)pc_get_current_bind);
  make_func_symbol_func("pc-get-controller-count", (void*)pc_get_controller_count);
  make_func_symbol_func("pc-get-controller-index", (void*)pc_get_controller_index);
  make_func_symbol_func("pc-set-controller!", (void*)pc_set_controller);
  make_func_symbol_func("pc-get-keyboard-enabled?", (void*)pc_get_keyboard_enabled);
  make_func_symbol_func("pc-set-keyboard-enabled!", (void*)pc_set_keyboard_enabled);
  make_func_symbol_func("pc-set-mouse-options!", (void*)pc_set_mouse_options);
  make_func_symbol_func("pc-set-mouse-camera-sens!", (void*)pc_set_mouse_camera_sens);
  make_func_symbol_func("pc-ignore-background-controller-events!",
                        (void*)pc_ignore_background_controller_events);
  make_func_symbol_func("pc-current-controller-has-led?", (void*)pc_current_controller_has_led);
  make_func_symbol_func("pc-current-controller-has-rumble?",
                        (void*)pc_current_controller_has_rumble);
  make_func_symbol_func("pc-set-controller-led!", (void*)pc_set_controller_led);
  make_func_symbol_func("pc-waiting-for-bind?", (void*)pc_waiting_for_bind);
  make_func_symbol_func("pc-set-waiting-for-bind!", (void*)pc_set_waiting_for_bind);
  make_func_symbol_func("pc-stop-waiting-for-bind!", (void*)pc_stop_waiting_for_bind);
  make_func_symbol_func("pc-reset-bindings-to-defaults!", (void*)pc_reset_bindings_to_defaults);
  make_func_symbol_func("pc-set-auto-hide-cursor!", (void*)pc_set_auto_hide_cursor);
  make_func_symbol_func("pc-get-pressure-sensitivity-enabled?",
                        (void*)pc_get_pressure_sensitivity_enabled);
  make_func_symbol_func("pc-set-pressure-sensitivity-enabled!",
                        (void*)pc_set_pressure_sensitivity_enabled);
  make_func_symbol_func("pc-set-axis-scale!", (void*)pc_set_axis_scale);
  make_func_symbol_func("pc-get-axis-scale", (void*)pc_get_axis_scale);
  make_func_symbol_func("pc-current-controller-has-pressure-sensitivity?",
                        (void*)pc_current_controller_has_pressure_sensitivity);
  make_func_symbol_func("pc-current-controller-has-trigger-effect-support?",
                        (void*)pc_current_controller_has_trigger_effect_support);
  make_func_symbol_func("pc-get-trigger-effects-enabled?", (void*)pc_get_trigger_effects_enabled);
  make_func_symbol_func("pc-set-trigger-effects-enabled!", (void*)pc_set_trigger_effects_enabled);
  make_func_symbol_func("pc-clear-trigger-effect!", (void*)pc_clear_trigger_effect);
  make_func_symbol_func("pc-send-trigger-effect-feedback!", (void*)pc_send_trigger_effect_feedback);
  make_func_symbol_func("pc-send-trigger-effect-vibrate!", (void*)pc_send_trigger_effect_vibrate);
  make_func_symbol_func("pc-send-trigger-effect-weapon!", (void*)pc_send_trigger_effect_weapon);
  make_func_symbol_func("pc-send-trigger-rumble!", (void*)pc_send_trigger_rumble);

  // graphics things
  make_func_symbol_func("pc-set-vsync", (void*)pc_set_vsync);
  make_func_symbol_func("pc-set-msaa", (void*)pc_set_msaa);
  make_func_symbol_func("pc-set-frame-rate", (void*)pc_set_frame_rate);
  make_func_symbol_func("pc-set-game-resolution", (void*)pc_set_game_resolution);
  make_func_symbol_func("pc-set-brightness-contrast", (void*)pc_set_brightness_contrast);
  make_func_symbol_func("pc-set-letterbox", (void*)pc_set_letterbox);
  make_func_symbol_func("pc-renderer-tree-set-lod", (void*)pc_renderer_tree_set_lod);
  make_func_symbol_func("pc-set-collision-mode", (void*)Gfx::CollisionRendererSetMode);
  make_func_symbol_func("pc-set-collision-mask", (void*)pc_set_collision_mask);
  make_func_symbol_func("pc-get-collision-mask", (void*)pc_get_collision_mask);
  make_func_symbol_func("pc-set-collision-wireframe", (void*)pc_set_collision_wireframe);
  make_func_symbol_func("pc-set-collision", (void*)pc_set_collision);
  make_func_symbol_func("pc-set-gfx-hack", (void*)pc_set_gfx_hack);
  if (g_game_version == GameVersion::Jak1) {
    make_func_symbol_func("pc-set-ssao-debug", (void*)pc_set_ssao_debug);
    make_func_symbol_func("pc-set-ssao-depth-capture", (void*)pc_set_ssao_depth_capture);
    make_func_symbol_func("pc-set-ssao-option", (void*)pc_set_ssao_option);
    make_func_symbol_func("pc-set-ssao-preset", (void*)pc_set_ssao_preset);
    make_func_symbol_func("pc-set-ssao-param", (void*)pc_set_ssao_param);
    make_func_symbol_func("pc-set-hdr-target", (void*)pc_set_hdr_target);
    make_func_symbol_func("pc-set-tonemap", (void*)pc_set_tonemap);
    make_func_symbol_func("pc-set-hdr-output", (void*)pc_set_hdr_output);
    make_func_symbol_func("pc-get-hdr-output", (void*)pc_get_hdr_output);
    make_func_symbol_func("pc-set-bloom", (void*)pc_set_bloom);
    make_func_symbol_func("pc-set-smaa", (void*)pc_set_smaa);
    make_func_symbol_func("pc-set-render-scale", (void*)pc_set_render_scale);
    make_func_symbol_func("pc-set-perf-timing", (void*)pc_set_perf_timing);
    make_func_symbol_func("pc-set-inland-water", (void*)pc_set_inland_water);
    make_func_symbol_func("pc-set-perf-opt", (void*)pc_set_perf_opt);
    make_func_symbol_func("pc-set-fur", (void*)pc_set_fur);
    make_func_symbol_func("pc-snap-poll", (void*)pc_snap_poll);
    make_func_symbol_func("pc-bug-report-poll", (void*)pc_bug_report_poll);
    make_func_symbol_func("pc-bug-report-request", (void*)pc_bug_report_request);
    make_func_symbol_func("pc-bug-report-save", (void*)pc_bug_report_save);
    make_func_symbol_func("pc-set-water", (void*)pc_set_water);
    make_func_symbol_func("pc-set-swamp", (void*)pc_set_swamp);
    make_func_symbol_func("pc-set-lava", (void*)pc_set_lava);
    make_func_symbol_func("pc-lava-light", (void*)pc_lava_light);
    make_func_symbol_func("pc-set-caustics", (void*)pc_set_caustics);
    make_func_symbol_func("pc-set-emissive-lights", (void*)pc_set_emissive_lights);
    make_func_symbol_func("pc-world-lights-begin", (void*)pc_world_lights_begin);
    make_func_symbol_func("pc-world-lights-query", (void*)pc_world_lights_query);
    make_func_symbol_func("pc-world-lights-set", (void*)pc_world_lights_set);
    make_func_symbol_func("pc-eco-lights-begin", (void*)pc_eco_lights_begin);
    make_func_symbol_func("pc-eco-lights-push", (void*)pc_eco_lights_push);
    make_func_symbol_func("pc-eco-lights-known", (void*)pc_eco_lights_known);
    make_func_symbol_func("pc-eco-lights-end", (void*)pc_eco_lights_end);
    make_func_symbol_func("pc-launch-lights-push", (void*)pc_launch_lights_push);
    make_func_symbol_func("pc-launch-lights-end", (void*)pc_launch_lights_end);
    make_func_symbol_func("pc-set-inspect", (void*)pc_set_inspect);
    make_func_symbol_func("pc-inspect-pick", (void*)pc_inspect_pick);
    make_func_symbol_func("pc-set-sun-option", (void*)pc_set_sun_option);
    make_func_symbol_func("pc-get-sky-bodies", (void*)pc_get_sky_bodies);
    make_func_symbol_func("pc-set-pbr-param", (void*)pc_set_pbr_param);
    make_func_symbol_func("pc-set-sun-dir", (void*)pc_set_sun_dir);
    make_func_symbol_func("pc-set-shadow-option", (void*)pc_set_shadow_option);
    make_func_symbol_func("pc-set-stereo", (void*)pc_set_stereo);
    make_func_symbol_func("pc-set-stereo-option", (void*)pc_set_stereo_option);
    make_func_symbol_func("pc-set-xr", (void*)pc_set_xr);
    make_func_symbol_func("pc-set-xr-recentre", (void*)pc_set_xr_recentre);
    make_func_symbol_func("pc-set-audio-spatial", (void*)pc_set_audio_spatial);
    make_func_symbol_func("pc-set-vista", (void*)pc_set_vista);
    make_func_symbol_func("pc-set-vista-hide", (void*)pc_set_vista_hide);
    make_func_symbol_func("pc-set-vista-option", (void*)pc_set_vista_option);
    make_func_symbol_func("pc-set-vista-fog", (void*)pc_set_vista_fog);
    make_func_symbol_func("pc-vista-landmarks?", (void*)pc_vista_landmarks);
    make_func_symbol_func("pc-set-vista-neighbour-fog", (void*)pc_set_vista_neighbour_fog);
    make_func_symbol_func("pc-set-vista-haze", (void*)pc_set_vista_haze);
    make_func_symbol_func("pc-set-vista-neighbour-haze", (void*)pc_set_vista_neighbour_haze);
    make_func_symbol_func("pc-vista-neighbour?", (void*)pc_vista_neighbour);
    make_func_symbol_func("pc-set-vista-viewer", (void*)pc_set_vista_viewer);
  }

  // -- OTHER --
  // Return the current OS as a symbol. Actually returns what it was compiled for!
  make_func_symbol_func("pc-get-os", (void*)pc_get_os);
  make_func_symbol_func("pc-get-unix-timestamp", (void*)pc_get_unix_timestamp);
  make_func_symbol_func("pc-treat-pad0-as-pad1", (void*)pc_treat_pad0_as_pad1);
  make_func_symbol_func("pc-is-imgui-visible?", (void*)pc_is_imgui_visible);

  // file related functions
  make_func_symbol_func("pc-filepath-exists?", (void*)pc_filepath_exists);
  make_func_symbol_func("pc-mkdir-file-path", (void*)pc_mkdir_filepath);

  // discord rich presence
  make_func_symbol_func("pc-discord-rpc-set", (void*)set_discord_rpc);

  // profiler
  make_func_symbol_func("pc-prof", (void*)pc_prof);

  // RNG
  make_func_symbol_func("pc-rand", (void*)pc_rand);

  // text
  make_func_symbol_func("pc-encode-utf8-string", (void*)pc_encode_utf8_string);

  // debugging tools
  make_func_symbol_func("pc-filter-debug-string?", (void*)pc_filter_debug_string);
  make_func_symbol_func("pc-screen-shot", (void*)pc_screen_shot);
  make_func_symbol_func("pc-register-screen-shot-settings",
                        (void*)pc_register_screen_shot_settings);
}
