#include "Shader.h"

#include "common/log/log.h"
#include "common/util/Assert.h"
#include "common/util/FileUtil.h"

#include <cstring>
#include <tuple>
#include <utility>

#include "game/graphics/pipelines/opengl.h"

Shader::Shader(const std::string& shader_name, GameVersion version, const std::string& frag_name)
    : m_name(shader_name) {
  const std::string height_scale = version == GameVersion::Jak1 ? "1.0" : "0.5";
  const std::string scissor_height = version == GameVersion::Jak1 ? "448.0" : "416.0";
  const std::string scissor_adjust = "512.0 / " + scissor_height;

  // read the shader source
  auto vert_src =
      file_util::read_text_file(file_util::get_file_path({shader_folder, shader_name + ".vert"}));
  auto frag_src = file_util::read_text_file(file_util::get_file_path(
      {shader_folder, (frag_name.empty() ? shader_name : frag_name) + ".frag"}));

  // shared snippets: SSAO normal target, actor shadow receivers
  auto include = [](std::string& src, const std::string& token, const std::string& file) {
    auto pos = src.find(token);
    if (pos != std::string::npos) {
      src.replace(pos, token.size(),
                  file_util::read_text_file(file_util::get_file_path({shader_folder, file})));
    }
  };
  include(vert_src, "SSAO_NORMAL_VERT", "ssao_normal_vert.glsl");
  include(frag_src, "SSAO_NORMAL_FRAG", "ssao_normal_frag.glsl");
  include(frag_src, "SSAO_FOG_T", "ssao_fog_t.glsl");
  include(frag_src, "ACTOR_SHADOW_FRAG", "actor_shadow_frag.glsl");
  {
    // canopy shade, at every use (receivers, wind TIE, generic); the file has an include guard
    // and is read once and spliced only where the token stood, never rescanned
    const std::string token = "CANOPY_FRAG";
    if (frag_src.find(token) != std::string::npos) {
      const std::string body =
          file_util::read_text_file(file_util::get_file_path({shader_folder, "canopy_frag.glsl"}));
      std::string out;
      size_t from = 0, at;
      while ((at = frag_src.find(token, from)) != std::string::npos) {
        out.append(frag_src, from, at - from).append(body);
        from = at + token.size();
      }
      frag_src = out.append(frag_src, from, std::string::npos);
    }
  }
  // world sun map receivers (WorldShadows.h), at every use; include guards in both files
  for (auto [src, token, file] :
       {std::tuple<std::string*, const char*, const char*>{&frag_src, "WORLD_SHADOW_FRAG",
                                                           "world_shadow_frag.glsl"},
        std::tuple<std::string*, const char*, const char*>{&vert_src, "WORLD_SHADOW_VERT",
                                                           "world_shadow_vert.glsl"}}) {
    if (src->find(token) == std::string::npos) {
      continue;
    }
    const std::string body = file_util::read_text_file(file_util::get_file_path({shader_folder, file}));
    std::string out;
    size_t from = 0, at;
    const size_t len = strlen(token);
    while ((at = src->find(token, from)) != std::string::npos) {
      out.append(*src, from, at - from).append(body);
      from = at + len;
    }
    *src = out.append(*src, from, std::string::npos);
  }
  include(frag_src, "CAVE_DARK_FRAG", "cave_dark_frag.glsl");
  include(vert_src, "INSPECT_VERT", "inspect_vert.glsl");
  include(frag_src, "INSPECT_FRAG", "inspect_frag.glsl");
  include(vert_src, "PBR_BG_VERT", "pbr_bg_vert.glsl");
  include(frag_src, "PBR_BG_FRAG", "pbr_bg_frag.glsl");
  include(frag_src, "PBR_COMMON_FRAG", "pbr_common_frag.glsl");
  include(frag_src, "SWAMP_MIST_GLSL", "swamp_mist.glsl");
  include(vert_src, "WATER_WAVES", "water_waves.glsl");
  include(frag_src, "WATER_WAVES", "water_waves.glsl");
  include(frag_src, "WATER_SUN_GLINT", "water_sun_glint.glsl");
  include(vert_src, "XR_GS_VERT", "xr_gs.glsl");
  // SMAA: the config first, then the reference it ends with
  for (auto* s : {&vert_src, &frag_src}) {
    include(*s, "SANDOVER_SMAA_CONFIG", "smaa_config.glsl");
    include(*s, "SANDOVER_SMAA_HLSL", "SMAA.hlsl");
  }
  // AMD FidelityFX FSR 1, the headers as published (shaders/ffx)
  include(frag_src, "SANDOVER_FFX_A", "ffx/ffx_a.h");
  include(frag_src, "SANDOVER_FFX_FSR1", "ffx/ffx_fsr1.h");
  // last, at every use, so the snippets above may use it too (the file has an include guard);
  // the text is read once and spliced only where the token stood, never rescanned
  for (auto* src : {&frag_src, &vert_src}) {
    const std::string token = "LEVEL_HEIGHT_GLSL";
    if (src->find(token) == std::string::npos) {
      continue;
    }
    const std::string body =
        file_util::read_text_file(file_util::get_file_path({shader_folder, "level_height.glsl"}));
    std::string out;
    size_t from = 0, at;
    while ((at = src->find(token, from)) != std::string::npos) {
      out.append(*src, from, at - from).append(body);
      from = at + token.size();
    }
    *src = out.append(*src, from, std::string::npos);
  }

  // optional tessellation stages: <name>.tesc and <name>.tese next to the vertex shader
  std::string tesc_src, tese_src;
  const auto tesc_path = file_util::get_file_path({shader_folder, shader_name + ".tesc"});
  const auto tese_path = file_util::get_file_path({shader_folder, shader_name + ".tese"});
  if (file_util::file_exists(tesc_path) && file_util::file_exists(tese_path)) {
    tesc_src = file_util::read_text_file(tesc_path);
    tese_src = file_util::read_text_file(tese_path);
    include(tesc_src, "WATER_WAVES", "water_waves.glsl");
    include(tese_src, "WATER_WAVES", "water_waves.glsl");
    include(tesc_src, "XR_GS_VERT", "xr_gs.glsl");
    include(tese_src, "XR_GS_VERT", "xr_gs.glsl");
    for (auto* s : {&tesc_src, &tese_src}) {
      *s = std::regex_replace(*s, std::regex("HEIGHT_SCALE"), height_scale);
      *s = std::regex_replace(*s, std::regex("SCISSOR_ADJUST"), "(" + scissor_adjust + ")");
    }
  }

  vert_src = std::regex_replace(vert_src, std::regex("HEIGHT_SCALE"), height_scale);
  vert_src = std::regex_replace(vert_src, std::regex("SCISSOR_HEIGHT"), scissor_height);
  frag_src = std::regex_replace(frag_src, std::regex("SCISSOR_HEIGHT"), scissor_height);
  vert_src = std::regex_replace(vert_src, std::regex("SCISSOR_ADJUST"), "(" + scissor_adjust + ")");
  // water_waves.glsl is also included in fragment shaders
  frag_src = std::regex_replace(frag_src, std::regex("HEIGHT_SCALE"), height_scale);
  frag_src = std::regex_replace(frag_src, std::regex("SCISSOR_ADJUST"), "(" + scissor_adjust + ")");

  m_vert_shader = glCreateShader(GL_VERTEX_SHADER);
  const char* src = vert_src.c_str();
  glShaderSource(m_vert_shader, 1, &src, nullptr);
  glCompileShader(m_vert_shader);

  constexpr int len = 1024;
  int compile_ok;
  char err[len];

  glGetShaderiv(m_vert_shader, GL_COMPILE_STATUS, &compile_ok);
  if (!compile_ok) {
    glGetShaderInfoLog(m_vert_shader, len, nullptr, err);
    lg::error("Failed to compile vertex shader {}:\n{}", shader_name.c_str(), err);
    m_is_okay = false;
    return;
  }

  m_frag_shader = glCreateShader(GL_FRAGMENT_SHADER);
  src = frag_src.c_str();
  glShaderSource(m_frag_shader, 1, &src, nullptr);
  glCompileShader(m_frag_shader);

  glGetShaderiv(m_frag_shader, GL_COMPILE_STATUS, &compile_ok);
  if (!compile_ok) {
    glGetShaderInfoLog(m_frag_shader, len, nullptr, err);
    lg::error("Failed to compile fragment shader {}:\n{}", shader_name.c_str(), err);
    m_is_okay = false;
    return;
  }

  GLuint tess[2] = {0, 0};
  if (!tesc_src.empty()) {
    const GLenum kinds[2] = {GL_TESS_CONTROL_SHADER, GL_TESS_EVALUATION_SHADER};
    const std::string* srcs[2] = {&tesc_src, &tese_src};
    for (int i = 0; i < 2; i++) {
      tess[i] = glCreateShader(kinds[i]);
      src = srcs[i]->c_str();
      glShaderSource(tess[i], 1, &src, nullptr);
      glCompileShader(tess[i]);
      glGetShaderiv(tess[i], GL_COMPILE_STATUS, &compile_ok);
      if (!compile_ok) {
        glGetShaderInfoLog(tess[i], len, nullptr, err);
        lg::error("Failed to compile tessellation shader {}:\n{}", shader_name.c_str(), err);
        m_is_okay = false;
        return;
      }
    }
  }

  m_program = glCreateProgram();
  glAttachShader(m_program, m_vert_shader);
  glAttachShader(m_program, m_frag_shader);
  for (auto t : tess) {
    if (t) {
      glAttachShader(m_program, t);
    }
  }
  glLinkProgram(m_program);

  glGetProgramiv(m_program, GL_LINK_STATUS, &compile_ok);
  if (!compile_ok) {
    glGetProgramInfoLog(m_program, len, nullptr, err);
    lg::error("Failed to link shader {}:\n{}", shader_name.c_str(), err);
    m_is_okay = false;
    return;
  }

  // uniform samplers must be named matching the texture unit
  glUseProgram(m_program);
  for (int i = 1; i < 30; ++i) {
    std::string uniformName = "tex_T" + std::to_string(i);
    GLint texLoc = glGetUniformLocation(m_program, uniformName.c_str());
    if (texLoc != -1) {
      glUniform1i(texLoc, i);
    }
  }
  // samplers on units 30+ (scratchpad ALLOCATIONS), named rather than tex_T<i>
  for (const auto& [name, unit] :
       {std::pair<const char*, int>{"canopy_tex", 30}, std::pair<const char*, int>{"lh_tex", 32},
        std::pair<const char*, int>{"canopy_vol", 43}, std::pair<const char*, int>{"ws_map", 49},
        std::pair<const char*, int>{"ws_raw", 50}}) {
    const GLint loc = glGetUniformLocation(m_program, name);
    if (loc != -1) {
      glUniform1i(loc, unit);
    }
  }
  // assuming that the bones uniform block is always using binding point 1
  GLint bonesLoc = glGetUniformBlockIndex(m_program, "ub_bones");
  if (bonesLoc != -1) {
    glUniformBlockBinding(m_program, bonesLoc, 1);
  }

  glDeleteShader(m_vert_shader);
  glDeleteShader(m_frag_shader);
  for (auto t : tess) {
    if (t) {
      glDeleteShader(t);
    }
  }
  m_is_okay = true;
}

void Shader::activate() const {
  ASSERT(m_is_okay);
  glUseProgram(m_program);
}

ShaderLibrary::ShaderLibrary(GameVersion version) : m_version(version) {
  at(ShaderId::SOLID_COLOR) = {"solid_color", version};
  at(ShaderId::DIRECT_BASIC) = {"direct_basic", version};
  at(ShaderId::DIRECT_BASIC_TEXTURED) = {"direct_basic_textured", version};
  at(ShaderId::DIRECT_BASIC_TEXTURED_MULTI_UNIT) = {"direct_basic_textured_multi_unit", version};
  at(ShaderId::DEBUG_RED) = {"debug_red", version};
  at(ShaderId::SPRITE) = {"sprite_3d", version};
  at(ShaderId::SKY) = {"sky", version};
  at(ShaderId::SKY_BLEND) = {"sky_blend", version};
  at(ShaderId::TFRAG3) = {"tfrag3", version};
  at(ShaderId::TFRAG3_NO_TEX) = {"tfrag3_no_tex", version};
  at(ShaderId::SPRITE3) = {"sprite3_3d", version};
  at(ShaderId::DIRECT2) = {"direct2", version};
  at(ShaderId::EYE) = {"eye", version};
  at(ShaderId::GENERIC) = {"generic", version};
  at(ShaderId::OCEAN_TEXTURE) = {"ocean_texture", version};
  at(ShaderId::OCEAN_TEXTURE_MIPMAP) = {"ocean_texture_mipmap", version};
  at(ShaderId::OCEAN_COMMON) = {"ocean_common", version};
  at(ShaderId::SHRUB) = {"shrub", version};
  at(ShaderId::SHADOW) = {"shadow", version};
  at(ShaderId::COLLISION) = {"collision", version};
  at(ShaderId::MERC2) = {"merc2", version};
  at(ShaderId::SPRITE_DISTORT) = {"sprite_distort", version};
  at(ShaderId::SPRITE_DISTORT_INSTANCED) = {"sprite_distort_instanced", version};
  at(ShaderId::POST_PROCESSING) = {"post_processing", version};
  at(ShaderId::DEPTH_CUE) = {"depth_cue", version};
  at(ShaderId::EMERC) = {"emerc", version};
  at(ShaderId::GLOW_PROBE) = {"glow_probe", version};
  at(ShaderId::GLOW_PROBE_READ) = {"glow_probe_read", version};
  at(ShaderId::GLOW_PROBE_READ_DEBUG) = {"glow_probe_read_debug", version};
  at(ShaderId::GLOW_PROBE_DOWNSAMPLE) = {"glow_probe_downsample", version};
  at(ShaderId::GLOW_DRAW) = {"glow_draw", version};
  at(ShaderId::ETIE_BASE) = {"etie_base", version};
  at(ShaderId::ETIE) = {"etie", version};
  at(ShaderId::SHADOW2) = {"shadow2", version};
  at(ShaderId::TEX_ANIM) = {"tex_anim", version};
  at(ShaderId::GLOW_DEPTH_COPY) = {"glow_depth_copy", version};
  at(ShaderId::GLOW_PROBE_ON_GRID) = {"glow_probe_on_grid", version};
  at(ShaderId::HFRAG) = {"hfrag", version};
  at(ShaderId::HFRAG_MONTAGE) = {"hfrag_montage", version};
  at(ShaderId::PLAIN_TEXTURE) = {"plain_texture", version};
  at(ShaderId::TIE_WIND) = {"tie_wind", version};
  at(ShaderId::SIMPLE_TEXTURE) = {"simple_texture", version};
  at(ShaderId::SLOW_TIME) = {"slow_time", version};
  at(ShaderId::OCEAN_ENVMAP) = {"ocean_envmap", version};
  at(ShaderId::OCEAN_ENVMAP_HAZE) = {"ocean_envmap_haze", version};
  at(ShaderId::SSAO_DEPTH_DEBUG) = {"ssao_depth_debug", version};
  at(ShaderId::SSAO_MSAA_PROBE) = {"ssao_msaa_probe", version};
  at(ShaderId::SSAO_LINEARIZE) = {"ssao_linearize", version};
  at(ShaderId::SSAO_MINIFY) = {"ssao_minify", version};
  at(ShaderId::SSAO_AO) = {"ssao_ao", version};
  at(ShaderId::SSAO_BLUR) = {"ssao_blur", version};
  at(ShaderId::SSAO_COMPOSITE) = {"ssao_composite", version};
  at(ShaderId::SSAO_VIEW) = {"ssao_view", version};
  at(ShaderId::TFRAG3_SUN) = {"tfrag3_sun", version, "tfrag3"};
  at(ShaderId::BLOOM_BRIGHT) = {"bloom", version, "bloom_bright"};
  at(ShaderId::BLOOM_BLUR) = {"bloom", version, "bloom_blur"};
  at(ShaderId::BLOOM_ADD) = {"bloom", version, "bloom_add"};
  at(ShaderId::MERC2_SHADOW) = {"merc2_shadow", version};
  at(ShaderId::GENERIC_SHADOW) = {"generic_shadow", version};
  at(ShaderId::EMISSIVE_TILES) = {"bloom", version, "emissive_tiles"};
  at(ShaderId::EMISSIVE_SELECT) = {"emissive_select", version};
  at(ShaderId::EMISSIVE_LIGHT) = {"bloom", version, "emissive_light"};
  at(ShaderId::OCEAN_WATER) = {"ocean_water", version, "ocean_common"};
  at(ShaderId::SWAMP_MIST) = {"bloom", version, "swamp_mist"};
  at(ShaderId::SMAA_EDGES) = {"smaa_edges", version};
  at(ShaderId::SMAA_WEIGHTS) = {"smaa_weights", version};
  at(ShaderId::SMAA_BLEND) = {"smaa_blend", version};
  at(ShaderId::SUN_VIS) = {"bloom", version, "sun_vis"};
  at(ShaderId::SUN_DISC) = {"sun_disc", version};
  at(ShaderId::SKY_BODY) = {"sun_disc", version, "sky_body"};
  at(ShaderId::SKY_STARS) = {"sky_stars", version};

  for (int i = 0; i < (int)ShaderId::MAX_SHADERS; i++) {
    if (i != (int)ShaderId::OCEAN_WATER && i != (int)ShaderId::FSR_EASU &&
        i != (int)ShaderId::FSR_RCAS) {
      ASSERT_MSG(m_shaders[i].okay(), "error compiling shader");
    }
  }
}

bool ShaderLibrary::fsr_ready() {
  if (m_fsr_state == 0) {
    at(ShaderId::FSR_EASU) = {"bloom", m_version, "fsr_easu"};
    at(ShaderId::FSR_RCAS) = {"bloom", m_version, "fsr_rcas"};
    m_fsr_state = at(ShaderId::FSR_EASU).okay() && at(ShaderId::FSR_RCAS).okay() ? 1 : -1;
    if (m_fsr_state < 0) {
      lg::warn("upscale: FSR 1 programs failed to build, bilinear instead");
    }
  }
  return m_fsr_state == 1;
}

bool ShaderLibrary::ocean_water_ready() {
  if (m_ocean_water_state == 0) {
    m_ocean_water_state = -1;
    GLint major = 0, minor = 0, max_level = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    if (GLAD_GL_VERSION_4_0 && glad_glPatchParameteri) {
      glGetIntegerv(GL_MAX_TESS_GEN_LEVEL, &max_level);
    }
    if (getenv("SANDOVER_NO_TESS")) {
      max_level = 0;  // forces the flat-mesh fallback
    }
    // the control stage asks for up to 64 segments per edge
    if (max_level < 64) {
      lg::warn("water: no tessellation stages (GL {}.{}, max level {}); flat ocean mesh", major,
               minor, max_level);
    } else {
      at(ShaderId::OCEAN_WATER) = {"ocean_water", m_version, "ocean_common"};
      if (at(ShaderId::OCEAN_WATER).okay()) {
        m_ocean_water_state = 1;
      } else {
        lg::warn("water: ocean_water failed to build; flat ocean mesh");
      }
    }
  }
  return m_ocean_water_state == 1;
}
