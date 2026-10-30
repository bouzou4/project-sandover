#include "Shader.h"

#include "common/log/log.h"
#include "common/util/Assert.h"
#include "common/util/FileUtil.h"

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

#include "game/graphics/opengl_renderer/PerfTimers.h"
#include "game/graphics/pipelines/opengl.h"

namespace scene_jitter {
namespace {
std::vector<std::pair<GLuint, GLint>> g_programs;
float g_ndc[2] = {0.f, 0.f};

// programs whose last pre-raster stage draws the scene into the render target
bool jittered(const std::string& name) {
  static const char* kScene[] = {
      "tfrag3",       "tfrag3_no_tex", "tfrag3_sun",    "shrub",     "tie_wind",
      "etie_base",    "etie",          "hfrag",         "merc2",     "emerc",
      "generic",      "sprite3_3d",    "sprite_3d",     "glow_draw", "sprite_distort",
      "sprite_distort_instanced",      "direct_basic",  "direct_basic_textured",
      "direct_basic_textured_multi_unit",               "direct2",   "sky",
      "ocean_common", "ocean_water",   "shadow",        "shadow2",   "collision",
      "sun_disc",     "sky_stars"};
  for (auto* n : kScene) {
    if (name == n) {
      return true;
    }
  }
  return false;
}

// main() becomes sandover_scene_main(); the new main() adds the offset after it
bool inject(std::string& src) {
  static const std::regex main_re(R"(void\s+main\s*\(\s*(void)?\s*\))");
  if (!std::regex_search(src, main_re)) {
    return false;
  }
  src = std::regex_replace(src, main_re, "void sandover_scene_main()");
  src +=
      "\nuniform vec2 sandover_jitter;\n"
      "void main() {\n"
      "  sandover_scene_main();\n"
      // a select, not a product: zero jitter adds 0 even where w is not finite
      "  gl_Position.xy += mix(vec2(0.0), sandover_jitter * gl_Position.w,\n"
      "                        notEqual(sandover_jitter, vec2(0.0)));\n"
      "}\n";
  return true;
}

void add(GLuint program) {
  const GLint loc = glGetUniformLocation(program, "sandover_jitter");
  if (loc < 0) {
    return;
  }
  for (auto& p : g_programs) {
    if (p.first == program) {
      p.second = loc;
      glProgramUniform2f(program, loc, g_ndc[0], g_ndc[1]);
      return;
    }
  }
  g_programs.emplace_back(program, loc);
  glProgramUniform2f(program, loc, g_ndc[0], g_ndc[1]);
}
}  // namespace

void set(float ndc_x, float ndc_y) {
  if (ndc_x == g_ndc[0] && ndc_y == g_ndc[1]) {
    return;
  }
  g_ndc[0] = ndc_x;
  g_ndc[1] = ndc_y;
  for (const auto& [program, loc] : g_programs) {
    glProgramUniform2f(program, loc, ndc_x, ndc_y);
  }
}

void get(float* ndc_x, float* ndc_y) {
  *ndc_x = g_ndc[0];
  *ndc_y = g_ndc[1];
}

int phase_count(int render_w, int display_w) {
  const float ratio = (float)display_w / (float)std::max(1, render_w);
  return std::max(1, (int)(8.f * ratio * ratio));
}

void halton_offset(int i, float* px, float* py) {
  auto halton = [](int index, int base) {
    float f = 1.f, r = 0.f;
    while (index > 0) {
      f /= (float)base;
      r += f * (float)(index % base);
      index /= base;
    }
    return r;
  };
  *px = halton(i + 1, 2) - 0.5f;
  *py = halton(i + 1, 3) - 0.5f;
}

}  // namespace scene_jitter

namespace {
constexpr unsigned kStageVert = 1, kStageFrag = 2, kStageTesc = 4, kStageTese = 8;

// token -> file, in splice order: first use only, or every use (every_use), inserted text not
// rescanned. LEVEL_HEIGHT_GLSL stays last so earlier snippets may use it.
struct Snippet {
  const char* token;
  const char* file;
  unsigned stages;
  bool every_use;
};
constexpr Snippet kSnippets[] = {
    {"SSAO_NORMAL_VERT", "ssao_normal_vert.glsl", kStageVert, false},
    {"SSAO_NORMAL_FRAG", "ssao_normal_frag.glsl", kStageFrag, false},
    {"SSAO_FOG_T", "ssao_fog_t.glsl", kStageFrag, false},
    {"ACTOR_SHADOW_FRAG", "actor_shadow_frag.glsl", kStageFrag, false},
    {"CANOPY_FRAG", "canopy_frag.glsl", kStageFrag, true},
    {"WORLD_SHADOW_FRAG", "world_shadow_frag.glsl", kStageFrag, true},
    {"WORLD_SHADOW_VERT", "world_shadow_vert.glsl", kStageVert, true},
    {"CAVE_DARK_FRAG", "cave_dark_frag.glsl", kStageFrag, false},
    {"INSPECT_VERT", "inspect_vert.glsl", kStageVert, false},
    {"INSPECT_FRAG", "inspect_frag.glsl", kStageFrag, false},
    {"PBR_BG_VERT", "pbr_bg_vert.glsl", kStageVert, false},
    {"PBR_BG_FRAG", "pbr_bg_frag.glsl", kStageFrag, false},
    {"PBR_COMMON_FRAG", "pbr_common_frag.glsl", kStageFrag, false},
    {"SWAMP_MIST_GLSL", "swamp_mist.glsl", kStageFrag, false},
    {"SCREEN_HISTORY_GLSL", "screen_history.glsl", kStageFrag, false},
    {"WATER_WAVES", "water_waves.glsl", kStageVert | kStageFrag | kStageTesc | kStageTese, false},
    {"WATER_SUN_GLINT", "water_sun_glint.glsl", kStageFrag, false},
    {"XR_GS_VERT", "xr_gs.glsl", kStageVert | kStageTesc | kStageTese, false},
    {"TONEMAP_SDR_GLSL", "tonemap_sdr.glsl", kStageFrag, false},
    {"ATMO_UNIFORMS_VERT", "atmo_uniforms_vert.glsl", kStageVert, false},
    {"ATMO_FOG_VERT", "atmo_fog_vert.glsl", kStageVert, false},
    {"ATMO_FOG_MERC_VERT", "atmo_fog_merc_vert.glsl", kStageVert, false},
    {"ATMO_COMBINE_VERT", "atmo_combine_vert.glsl", kStageVert, false},
    {"EM_VALUE_VERT", "em_value_vert.glsl", kStageVert, false},
    {"MERC_CLIP_AXIS_VERT", "merc_clip_axis_vert.glsl", kStageVert, false},
    {"EMISSIVE_MASK_FRAG", "emissive_mask_frag.glsl", kStageFrag, false},
    {"NORMAL_DECODE_FRAG", "normal_decode_frag.glsl", kStageFrag, false},
    {"PBR_ALBEDO_GRAD_FRAG", "pbr_albedo_grad_frag.glsl", kStageFrag, false},
    {"FOG_TINT_FRAG", "fog_tint_frag.glsl", kStageFrag, false},
    {"IW_COLUMN_FRAG", "iw_column_frag.glsl", kStageFrag, false},
    {"BLUR_VEC_GLSL", "blur_vec.glsl", kStageFrag, false},
    {"LEVEL_HEIGHT_GLSL", "level_height.glsl", kStageVert | kStageFrag, true},
};

void splice_snippets(std::string& src, unsigned stage) {
  for (const auto& sn : kSnippets) {
    if (!(sn.stages & stage)) {
      continue;
    }
    size_t at = src.find(sn.token);
    if (at == std::string::npos) {
      continue;
    }
    const std::string body =
        file_util::read_text_file(file_util::get_file_path({Shader::shader_folder, sn.file}));
    const size_t len = strlen(sn.token);
    if (!sn.every_use) {
      src.replace(at, len, body);
      continue;
    }
    std::string out;
    size_t from = 0;
    do {
      out.append(src, from, at - from).append(body);
      from = at + len;
    } while ((at = src.find(sn.token, from)) != std::string::npos);
    src = out.append(src, from, std::string::npos);
  }
}
}  // namespace

Shader::Shader(const std::string& shader_name, GameVersion version, const std::string& frag_name)
    : m_name(shader_name) {
  perf::AddMs hitch_t(perf::Counter::HITCH_SHADER_MS);
  perf::add(perf::Counter::HITCH_SHADERS, 1);
  if (perf::on()) {
    lg::info("shader: {} built inside a frame", shader_name);
  }
  const std::string height_scale = version == GameVersion::Jak1 ? "1.0" : "0.5";
  const std::string scissor_height = version == GameVersion::Jak1 ? "448.0" : "416.0";
  const std::string scissor_adjust = "512.0 / " + scissor_height;

  // read the shader source
  auto vert_src =
      file_util::read_text_file(file_util::get_file_path({shader_folder, shader_name + ".vert"}));
  auto frag_src = file_util::read_text_file(file_util::get_file_path(
      {shader_folder, (frag_name.empty() ? shader_name : frag_name) + ".frag"}));

  splice_snippets(vert_src, kStageVert);
  splice_snippets(frag_src, kStageFrag);

  // optional tessellation stages: <name>.tesc and <name>.tese next to the vertex shader
  std::string tesc_src, tese_src;
  const auto tesc_path = file_util::get_file_path({shader_folder, shader_name + ".tesc"});
  const auto tese_path = file_util::get_file_path({shader_folder, shader_name + ".tese"});
  if (file_util::file_exists(tesc_path) && file_util::file_exists(tese_path)) {
    tesc_src = file_util::read_text_file(tesc_path);
    tese_src = file_util::read_text_file(tese_path);
    splice_snippets(tesc_src, kStageTesc);
    splice_snippets(tese_src, kStageTese);
    for (auto* s : {&tesc_src, &tese_src}) {
      *s = std::regex_replace(*s, std::regex("HEIGHT_SCALE"), height_scale);
      *s = std::regex_replace(*s, std::regex("SCISSOR_ADJUST"), "(" + scissor_adjust + ")");
    }
  }

  const bool jitter = version == GameVersion::Jak1 && scene_jitter::jittered(shader_name) &&
                      scene_jitter::inject(tese_src.empty() ? vert_src : tese_src);
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
  // samplers on units 30+ (kb/facts/renderer.md texture units), named rather than tex_T<i>
  for (const auto& [name, unit] :
       {std::pair<const char*, int>{"canopy_tex", 30}, std::pair<const char*, int>{"lh_tex", 32},
        std::pair<const char*, int>{"canopy_vol", 43}, std::pair<const char*, int>{"ws_map", 49},
        std::pair<const char*, int>{"ws_raw", 50}, std::pair<const char*, int>{"ws_mm", 57},
        std::pair<const char*, int>{"ws_selflit_vtx", 58},
        std::pair<const char*, int>{"ws_glow", 59}}) {
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
  // the previous frame's bones (motion vectors, Merc2): binding point 2
  GLint prevBonesLoc = glGetUniformBlockIndex(m_program, "ub_prev_bones");
  if (prevBonesLoc != -1) {
    glUniformBlockBinding(m_program, prevBonesLoc, 2);
  }

  if (jitter) {
    scene_jitter::add(m_program);
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
  at(ShaderId::SSAO_BLUR) = {"ssao_ao", version, "ssao_denoise"};
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
  at(ShaderId::SUN_VIS) = {"bloom", version, "sun_vis"};
  at(ShaderId::SUN_DISC) = {"sun_disc", version};
  at(ShaderId::SKY_BODY) = {"sun_disc", version, "sky_body"};
  at(ShaderId::SKY_STARS) = {"sky_stars", version};
  at(ShaderId::MOTION_VECTORS) = {"bloom", version, "motion_vectors"};
  at(ShaderId::MOTION_VECTORS_VIEW) = {"bloom", version, "motion_vectors_view"};
  at(ShaderId::CONTACT_SHADOW) = {"bloom", version, "contact_shadow"};
  at(ShaderId::CONTACT_SHADOW_TEMPORAL) = {"bloom", version, "contact_shadow_temporal"};
  at(ShaderId::CONTACT_SHADOW_COMPOSITE) = {"bloom", version, "contact_shadow_composite"};
  at(ShaderId::MOTION_BLUR_TILES) = {"bloom", version, "motion_blur_tiles"};
  at(ShaderId::MOTION_BLUR) = {"bloom", version, "motion_blur"};
  at(ShaderId::TEMPORAL_ACCUM) = {"ssao_ao", version, "temporal_accum"};
  at(ShaderId::SSAO_MARKS) = {"bloom", version, "ssao_marks"};
  at(ShaderId::VISTA_HIZ) = {"bloom", version, "vista_hiz"};
  at(ShaderId::XR_UI) = {"xr_ui", version};

  for (int i = 0; i < (int)ShaderId::MAX_SHADERS; i++) {
    if (i != (int)ShaderId::OCEAN_WATER) {
      ASSERT_MSG(m_shaders[i].okay(), "error compiling shader");
    }
  }
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
