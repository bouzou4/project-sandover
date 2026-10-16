#pragma once

#include <string>

#include "common/common_types.h"
#include "common/versions/versions.h"

class Shader {
 public:
  static constexpr char shader_folder[] = "game/graphics/opengl_renderer/shaders/";
  /// frag_name: fragment shader file to use instead of shader_name.frag (empty: same name).
  Shader(const std::string& shader_name, GameVersion version, const std::string& frag_name = "");
  Shader() = default;
  void activate() const;
  bool okay() const { return m_is_okay; }
  u64 id() const { return m_program; }

 private:
  std::string m_name;
  u64 m_frag_shader = 0;
  u64 m_vert_shader = 0;
  u64 m_program = 0;
  bool m_is_okay = false;
};

// note: update the constructor in Shader.cpp
enum class ShaderId {
  SOLID_COLOR = 0,
  DIRECT_BASIC = 1,
  DIRECT_BASIC_TEXTURED = 2,
  DEBUG_RED = 3,
  SKY = 4,
  SKY_BLEND = 5,
  TFRAG3 = 6,
  TFRAG3_NO_TEX = 7,
  SPRITE = 8,
  SPRITE3 = 9,
  DIRECT2 = 10,
  EYE = 11,
  GENERIC = 12,
  OCEAN_TEXTURE = 13,
  OCEAN_TEXTURE_MIPMAP = 14,
  OCEAN_COMMON = 15,
  SHADOW = 16,
  SHRUB = 17,
  COLLISION = 18,
  MERC2 = 19,
  SPRITE_DISTORT = 20,
  SPRITE_DISTORT_INSTANCED = 21,
  POST_PROCESSING = 22,
  DEPTH_CUE = 23,
  EMERC = 24,
  GLOW_PROBE = 25,
  GLOW_PROBE_READ = 26,
  GLOW_PROBE_READ_DEBUG = 27,
  GLOW_PROBE_DOWNSAMPLE = 28,
  GLOW_DRAW = 29,
  ETIE_BASE = 30,
  ETIE = 31,
  SHADOW2 = 32,
  DIRECT_BASIC_TEXTURED_MULTI_UNIT = 33,
  TEX_ANIM = 34,
  GLOW_DEPTH_COPY = 35,
  GLOW_PROBE_ON_GRID = 36,
  HFRAG = 37,
  HFRAG_MONTAGE = 38,
  PLAIN_TEXTURE = 39,
  TIE_WIND = 40,
  SIMPLE_TEXTURE = 41,
  SLOW_TIME = 42,
  OCEAN_ENVMAP = 43,
  OCEAN_ENVMAP_HAZE = 44,
  SSAO_DEPTH_DEBUG = 45,
  SSAO_MSAA_PROBE = 46,
  SSAO_LINEARIZE = 47,
  SSAO_MINIFY = 48,
  SSAO_AO = 49,
  SSAO_BLUR = 50,
  SSAO_COMPOSITE = 51,
  SSAO_VIEW = 52,
  TFRAG3_SUN = 53,
  BLOOM_BRIGHT = 54,
  BLOOM_BLUR = 55,
  BLOOM_ADD = 56,
  MERC2_SHADOW = 57,
  GENERIC_SHADOW = 58,
  EMISSIVE_TILES = 59,
  EMISSIVE_SELECT = 60,
  EMISSIVE_LIGHT = 61,
  OCEAN_WATER = 62,  // ocean shading with tessellation (ocean_water.vert/.tesc/.tese)
  SWAMP_MIST = 63,
  SMAA_EDGES = 64,
  SMAA_WEIGHTS = 65,
  SMAA_BLEND = 66,
  FSR_EASU = 67,  // optional, built by fsr_ready(): bilinear if it fails to build
  FSR_RCAS = 68,
  SUN_VIS = 69,
  SUN_DISC = 70,
  SKY_BODY = 71,
  SKY_STARS = 72,
  MAX_SHADERS
};

class ShaderLibrary {
 public:
  ShaderLibrary(GameVersion version);
  Shader& operator[](ShaderId id) { return m_shaders[(int)id]; }
  Shader& at(ShaderId id) { return m_shaders[(int)id]; }

  /// The tessellated ocean program (ShaderId::OCEAN_WATER), compiled on the first call so a
  /// vanilla boot never touches the tessellation stages. False, logged once, where the context
  /// has no tessellation stages or the program fails to build.
  bool ocean_water_ready();
  /// The FSR 1 programs (FSR_EASU, FSR_RCAS), compiled on the first call, so they cost nothing
  /// until a render scale below 1 asks for them. False, logged once, if either fails to build.
  bool fsr_ready();

 private:
  Shader m_shaders[(int)ShaderId::MAX_SHADERS];
  GameVersion m_version;
  int m_ocean_water_state = 0;  // 0 not tried, 1 ready, -1 unavailable
  int m_fsr_state = 0;          // same states
};
