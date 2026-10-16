#pragma once

#include "common/listener_common.h"
#include "common/versions/versions.h"

//! Supported languages.
enum class Language {
  English = 0,
  French = 1,
  German = 2,
  Spanish = 3,
  Italian = 4,
  Japanese = 5,
  UK_English = 6,
  Portuguese = 9
};

struct GameLaunchOptions {
  GameVersion game_version = GameVersion::Jak1;
  bool disable_display = false;
  int server_port = -1;  // -1: DECI2_PORT - 1 + game_version
};
