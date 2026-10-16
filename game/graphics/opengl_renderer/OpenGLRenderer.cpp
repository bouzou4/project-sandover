#include "OpenGLRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <type_traits>

#include "common/goal_constants.h"
#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "common/util/Timer.h"
#include "common/util/exr.h"

#include "game/graphics/bug_report.h"
#include "game/graphics/hdr/HdrPresent.h"
#include "game/graphics/opengl_renderer/BlitDisplays.h"
#include "game/graphics/opengl_renderer/DepthCue.h"
#include "game/graphics/opengl_renderer/DirectRenderer.h"
#include "game/graphics/opengl_renderer/EyeRenderer.h"
#include "game/graphics/opengl_renderer/GlQueries.h"
#include "game/graphics/opengl_renderer/InlandWater.h"
#include "game/graphics/opengl_renderer/Materials.h"
#include "game/graphics/opengl_renderer/PerfTimers.h"
#include "game/graphics/opengl_renderer/ProgressRenderer.h"
#include "game/graphics/opengl_renderer/ShadowRenderer.h"
#include "game/graphics/opengl_renderer/SkyRenderer.h"
#include "game/graphics/opengl_renderer/SunDisc.h"
#include "game/graphics/opengl_renderer/TextureUploadHandler.h"
#include "game/graphics/opengl_renderer/VisDataHandler.h"
#include "game/graphics/opengl_renderer/Warp.h"
#include "game/graphics/opengl_renderer/background/Hfrag.h"
#include "game/graphics/opengl_renderer/background/Shrub.h"
#include "game/graphics/opengl_renderer/background/TFragment.h"
#include "game/graphics/opengl_renderer/LevelHeightfield.h"
#include "game/graphics/opengl_renderer/background/CanopyVol.h"
#include "game/graphics/opengl_renderer/background/Tie3.h"
#include "game/graphics/opengl_renderer/background/background_common.h"
#include "game/graphics/opengl_renderer/foreground/Generic2.h"
#include "game/graphics/opengl_renderer/foreground/Generic2BucketRenderer.h"
#include "game/graphics/opengl_renderer/foreground/Merc2BucketRenderer.h"
#include "game/graphics/opengl_renderer/foreground/Shadow2.h"
#include "game/graphics/opengl_renderer/ocean/OceanMidAndFar.h"
#include "game/graphics/opengl_renderer/ocean/OceanNear.h"
#include "game/graphics/opengl_renderer/sprite/Sprite3.h"
#include "game/graphics/pipelines/opengl.h"
#include "game/graphics/screenshot.h"
#include "game/graphics/xr/OpenXr.h"

#include "third-party/imgui/imgui.h"
#include "third-party/imgui/imgui_stdlib.h"

// for the vif callback
#include "game/kernel/common/kmachine.h"
#include "game/runtime.h"

#ifdef _WIN32
#include <Windows.h>
#endif
#include "common/util/string_util.h"

// perf-opt bit 25: GL queries off the per-frame path (GlQueries.h)
constexpr int kGlq = PERF_OPT_PRESENT2 + 1;

namespace {
std::string g_current_renderer;
}

/*!
 * OpenGL Error callback. If we do something invalid, this will be called.
 */
void GLAPIENTRY opengl_error_callback(GLenum source,
                                      GLenum type,
                                      GLuint id,
                                      GLenum severity,
                                      GLsizei /*length*/,
                                      const GLchar* message,
                                      const void* /*userParam*/) {
  if (severity == GL_DEBUG_SEVERITY_NOTIFICATION) {
    lg::debug("[{}] OpenGL notification 0x{:X} S{:X} T{:X}: {}", g_current_renderer, id, source,
              type, message);
  } else if (severity == GL_DEBUG_SEVERITY_LOW) {
    lg::info("[{}] OpenGL message 0x{:X} S{:X} T{:X}: {}", g_current_renderer, id, source, type,
             message);
  } else if (severity == GL_DEBUG_SEVERITY_MEDIUM) {
    lg::warn("[{}] OpenGL warn 0x{:X} S{:X} T{:X}: {}", g_current_renderer, id, source, type,
             message);
  } else if (severity == GL_DEBUG_SEVERITY_HIGH) {
    lg::error("[{}] OpenGL error 0x{:X} S{:X} T{:X}: {}", g_current_renderer, id, source, type,
              message);
    // ASSERT(false);
  }
}

OpenGLRenderer::OpenGLRenderer(std::shared_ptr<TexturePool> texture_pool,
                               std::shared_ptr<Loader> loader,
                               GameVersion version)
    : m_render_state(texture_pool, loader, version),
      m_collide_renderer(version),
      m_version(version) {
  // requires OpenGL 4.3
#ifndef __APPLE__
  // setup OpenGL errors
  glEnable(GL_DEBUG_OUTPUT);
  glDebugMessageCallback(opengl_error_callback, nullptr);
  // disable specific errors
  const GLuint gl_error_ignores_api_other[1] = {0x20071};  // some annoying nvidia message
  glDebugMessageControl(GL_DEBUG_SOURCE_API, GL_DEBUG_TYPE_OTHER, GL_DONT_CARE, 1,
                        &gl_error_ignores_api_other[0], GL_FALSE);
#endif

  lg::info("OpenGL context version: {}", (const char*)glGetString(GL_VERSION));
  lg::info("OpenGL context renderer: {}", (const char*)glGetString(GL_RENDERER));
  lg::info("OpenGL context vendor: {}", (const char*)glGetString(GL_VENDOR));
  lg::info("OpenGL context shading language version: {}",
           (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION));

  // set up screen draw

  glGenVertexArrays(1, &screen_vao);
  glGenBuffers(1, &screen_vbo);

  struct Vertex {
    float x, y;
  };
  constexpr std::array<Vertex, 4> vertices = {
      Vertex{-1, -1},
      Vertex{-1, 1},
      Vertex{1, -1},
      Vertex{1, 1},
  };

  glBindVertexArray(screen_vao);
  glBindBuffer(GL_ARRAY_BUFFER, screen_vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(Vertex) * 4, vertices.data(), GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0,               // location 0 in the shader
                        2,               // 2 floats per vert
                        GL_FLOAT,        // floats
                        GL_TRUE,         // normalized, ignored,
                        sizeof(Vertex),  //
                        nullptr          //
  );

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindVertexArray(0);

  // end set up screen draw

  const tfrag3::Level* common_level = nullptr;
  {
    auto p = scoped_prof("load-common");
    common_level = &m_render_state.loader->load_common(*m_render_state.texture_pool, "GAME");
    if (m_version == GameVersion::Jak1) {
      pbr_load_preset();
      m_render_state.loader->classify_common();
    }
  }

  // initialize all renderers
  switch (m_version) {
    case GameVersion::Jak1:
      break;
    case GameVersion::Jak2:
    case GameVersion::Jak3:
    case GameVersion::JakX:
      m_texture_animator =
          std::make_shared<TextureAnimator>(m_render_state.shaders, common_level, m_version);
      break;
    default:
      ASSERT(false);
  }

  m_merc2 = std::make_shared<Merc2>(m_render_state.shaders, anim_slot_array());
  if (m_version == GameVersion::Jak1) {
    m_actor_shadows = std::make_unique<ActorShadows>();
  }
  m_generic2 = std::make_shared<Generic2>(m_render_state.shaders);

  // initialize all renderers
  auto p = scoped_prof("init-bucket-renderers");
  switch (m_version) {
    case GameVersion::Jak1:
      init_bucket_renderers_jak1();
      break;
    case GameVersion::Jak2:
      init_bucket_renderers_jak2();
      break;
    case GameVersion::Jak3:
    case GameVersion::JakX:
      init_bucket_renderers_jak3();
      break;
    default:
      ASSERT(false);
  }
}

void OpenGLRenderer::init_bucket_renderers_jak3() {
  using namespace jak3;
  m_bucket_renderers.resize((int)BucketId::MAX_BUCKETS);
  m_bucket_categories.resize((int)BucketId::MAX_BUCKETS, BucketCategory::OTHER);
  {
    auto p = scoped_prof("render-inits");

    m_blit_displays =
        init_bucket_renderer<BlitDisplays>("blit", BucketCategory::OTHER, BucketId::BLIT_START);
    init_bucket_renderer<VisDataHandler>("vis", BucketCategory::OTHER, BucketId::BUCKET_2);

    // 4
    init_bucket_renderer<TextureUploadHandler>("tex-lcom-sky-pre", BucketCategory::TEX,
                                               BucketId::TEX_LCOM_SKY_PRE, m_texture_animator);
    init_bucket_renderer<DirectRenderer>("sky", BucketCategory::OTHER, BucketId::SKY, 1024 * 8);

    init_bucket_renderer<OceanMidAndFar>("ocean-mid-far", BucketCategory::OCEAN,
                                         BucketId::OCEAN_MID_FAR);
    // 7 (hack for progress menu box)
    init_bucket_renderer<DirectRenderer>("progress-hack", BucketCategory::OTHER,
                                         BucketId::PROGRESS_HACK, 0x8000);

    // 8 (in tfrag category for now, just for stat reporting.)
    init_bucket_renderer<Hfrag>("hfrag", BucketCategory::TFRAG, BucketId::HFRAG);

    // 10
    for (int i = 0; i < LEVEL_MAX; i++) {
#define GET_BUCKET_ID_FOR_LIST(bkt1, bkt2, idx) ((int)(bkt1) + ((int)(bkt2) - (int)(bkt1)) * (idx))
      // 10
      init_bucket_renderer<TextureUploadHandler>(
          fmt::format("tex-l{}-tfrag", i), BucketCategory::TEX,
          GET_BUCKET_ID_FOR_LIST(BucketId::TEX_L0_TFRAG, BucketId::TEX_L1_TFRAG, i),
          m_texture_animator);
      // 11
      init_bucket_renderer<TFragment>(
          fmt::format("tfrag-l{}-tfrag", i), BucketCategory::TFRAG,
          GET_BUCKET_ID_FOR_LIST(BucketId::TFRAG_L0_TFRAG, BucketId::TFRAG_L1_TFRAG, i),
          std::vector{tfrag3::TFragmentTreeKind::NORMAL}, false, i, anim_slot_array());
      Tie3* tie = init_bucket_renderer<Tie3>(
          fmt::format("tie-l{}-tfrag", i), BucketCategory::TIE,
          GET_BUCKET_ID_FOR_LIST(BucketId::TIE_L0_TFRAG, BucketId::TIE_L1_TFRAG, i), i,
          anim_slot_array());
      init_bucket_renderer<Tie3AnotherCategory>(
          fmt::format("etie-l{}-tfrag", i), BucketCategory::TIE,
          GET_BUCKET_ID_FOR_LIST(BucketId::ETIE_L0_TFRAG, BucketId::ETIE_L1_TFRAG, i), tie,
          tfrag3::TieCategory::NORMAL_ENVMAP);

      // 17
      init_bucket_renderer<Merc2BucketRenderer>(
          fmt::format("merc-l{}-tfrag", i), BucketCategory::MERC,
          GET_BUCKET_ID_FOR_LIST(BucketId::MERC_L0_TFRAG, BucketId::MERC_L1_TFRAG, i), m_merc2);
      init_bucket_renderer<Generic2BucketRenderer>(
          fmt::format("gmerc-l{}-tfrag", i), BucketCategory::MERC,
          GET_BUCKET_ID_FOR_LIST(BucketId::GMERC_L0_TFRAG, BucketId::GMERC_L1_TFRAG, i), m_generic2,
          Generic2::Mode::NORMAL);

      init_bucket_renderer<TextureUploadHandler>(
          fmt::format("tex-l{}-shrub", i), BucketCategory::TEX,
          GET_BUCKET_ID_FOR_LIST(BucketId::TEX_L0_SHRUB, BucketId::TEX_L1_SHRUB, i),
          m_texture_animator);
      init_bucket_renderer<Shrub>(
          fmt::format("shrub-l{}-shrub", i), BucketCategory::SHRUB,
          GET_BUCKET_ID_FOR_LIST(BucketId::SHRUB_L0_SHRUB, BucketId::SHRUB_L1_SHRUB, i));
      init_bucket_renderer<Merc2BucketRenderer>(
          fmt::format("merc-l{}-shrub", i), BucketCategory::MERC,
          GET_BUCKET_ID_FOR_LIST(BucketId::MERC_L0_SHRUB, BucketId::MERC_L1_SHRUB, i), m_merc2);
      init_bucket_renderer<Generic2BucketRenderer>(
          fmt::format("gmerc-l{}-shrub", i), BucketCategory::MERC,
          GET_BUCKET_ID_FOR_LIST(BucketId::GMERC_L0_SHRUB, BucketId::GMERC_L1_SHRUB, i), m_generic2,
          Generic2::Mode::NORMAL);

      // 230
      init_bucket_renderer<TextureUploadHandler>(
          fmt::format("tex-l{}-alpha", i), BucketCategory::TEX,
          GET_BUCKET_ID_FOR_LIST(BucketId::TEX_L0_ALPHA, BucketId::TEX_L1_ALPHA, i),
          m_texture_animator);
      init_bucket_renderer<TFragment>(
          fmt::format("tfrag-t-l{}-alpha", i), BucketCategory::TFRAG,
          GET_BUCKET_ID_FOR_LIST(BucketId::TFRAG_L0_ALPHA, BucketId::TFRAG_L1_ALPHA, i),
          std::vector{tfrag3::TFragmentTreeKind::TRANS}, false, i, anim_slot_array());
      init_bucket_renderer<Tie3AnotherCategory>(
          fmt::format("tie-t-l{}-alpha", i), BucketCategory::TIE,
          GET_BUCKET_ID_FOR_LIST(BucketId::TIE_L0_ALPHA, BucketId::TIE_L1_ALPHA, i), tie,
          tfrag3::TieCategory::TRANS);
      init_bucket_renderer<Tie3AnotherCategory>(
          fmt::format("etie-l{}-alpha", i), BucketCategory::TIE,
          GET_BUCKET_ID_FOR_LIST(BucketId::ETIE_L0_ALPHA, BucketId::ETIE_L1_ALPHA, i), tie,
          tfrag3::TieCategory::TRANS_ENVMAP);
      init_bucket_renderer<Merc2BucketRenderer>(
          fmt::format("merc-l{}-alpha", i), BucketCategory::MERC,
          GET_BUCKET_ID_FOR_LIST(BucketId::MERC_L0_ALPHA, BucketId::MERC_L1_ALPHA, i), m_merc2);
      init_bucket_renderer<Generic2BucketRenderer>(
          fmt::format("gmerc-l{}-alpha", i), BucketCategory::GENERIC,
          GET_BUCKET_ID_FOR_LIST(BucketId::GMERC_L0_ALPHA, BucketId::GMERC_L1_ALPHA, i), m_generic2,
          Generic2::Mode::NORMAL);
      init_bucket_renderer<Tie3AnotherCategory>(
          fmt::format("tie-w-l{}-water", i), BucketCategory::TIE,
          GET_BUCKET_ID_FOR_LIST(BucketId::TIE_L0_WATER, BucketId::TIE_L1_WATER, i), tie,
          tfrag3::TieCategory::WATER);
      init_bucket_renderer<Tie3AnotherCategory>(
          fmt::format("etie-l{}-water", i), BucketCategory::TIE,
          GET_BUCKET_ID_FOR_LIST(BucketId::ETIE_L0_WATER, BucketId::ETIE_L1_WATER, i), tie,
          tfrag3::TieCategory::WATER_ENVMAP);
    }

    // 340
    init_bucket_renderer<TextureUploadHandler>("tex-lcom-tfrag", BucketCategory::TEX,
                                               BucketId::TEX_LCOM_TFRAG, m_texture_animator);
    init_bucket_renderer<Merc2BucketRenderer>("merc-lcom-tfrag", BucketCategory::MERC,
                                              BucketId::MERC_LCOM_TFRAG, m_merc2);
    init_bucket_renderer<Generic2BucketRenderer>("gmerc-lcom-tfrag", BucketCategory::GENERIC,
                                                 BucketId::GMERC_LCOM_TFRAG, m_generic2,
                                                 Generic2::Mode::NORMAL);
    // 345
    init_bucket_renderer<TextureUploadHandler>("tex-lcom-shrub", BucketCategory::TEX,
                                               BucketId::TEX_LCOM_SHRUB, m_texture_animator);
    init_bucket_renderer<Merc2BucketRenderer>("merc-lcom-shrub", BucketCategory::MERC,
                                              BucketId::MERC_LCOM_SHRUB, m_merc2);

    // 350
    init_bucket_renderer<Shadow2>("shadow", BucketCategory::OTHER, BucketId::SHADOW);

    // 351
    for (int i = 0; i < LEVEL_MAX; i++) {
#define GET_BUCKET_ID_FOR_LIST(bkt1, bkt2, idx) ((int)(bkt1) + ((int)(bkt2) - (int)(bkt1)) * (idx))
      init_bucket_renderer<TextureUploadHandler>(
          fmt::format("tex-l{}-pris", i), BucketCategory::TEX,
          GET_BUCKET_ID_FOR_LIST(BucketId::TEX_L0_PRIS, BucketId::TEX_L1_PRIS, i),
          m_texture_animator);
      init_bucket_renderer<TextureUploadHandler>(
          fmt::format("tex-l{}-pris2", i), BucketCategory::TEX,
          GET_BUCKET_ID_FOR_LIST(BucketId::TEX_L0_PRIS2, BucketId::TEX_L1_PRIS2, i),
          m_texture_animator);

      init_bucket_renderer<Merc2BucketRenderer>(
          fmt::format("merc-l{}-pris", i), BucketCategory::MERC,
          GET_BUCKET_ID_FOR_LIST(BucketId::MERC_L0_PRIS, BucketId::MERC_L1_PRIS, i), m_merc2);
      init_bucket_renderer<Generic2BucketRenderer>(
          fmt::format("gmerc-l{}-pris", i), BucketCategory::GENERIC,
          GET_BUCKET_ID_FOR_LIST(BucketId::GMERC_L0_PRIS, BucketId::GMERC_L1_PRIS, i), m_generic2,
          Generic2::Mode::NORMAL);

      init_bucket_renderer<Generic2BucketRenderer>(
          fmt::format("gmerc2-l{}-pris", i), BucketCategory::GENERIC,
          GET_BUCKET_ID_FOR_LIST(BucketId::GMERC2_L0_PRIS, BucketId::GMERC2_L1_PRIS, i), m_generic2,
          Generic2::Mode::PRIM);

      init_bucket_renderer<Merc2BucketRenderer>(
          fmt::format("merc-l{}-pris2", i), BucketCategory::MERC,
          GET_BUCKET_ID_FOR_LIST(BucketId::MERC_L0_PRIS2, BucketId::MERC_L1_PRIS2, i), m_merc2);
      init_bucket_renderer<Generic2BucketRenderer>(
          fmt::format("gmerc-l{}-pris2", i), BucketCategory::GENERIC,
          GET_BUCKET_ID_FOR_LIST(BucketId::GMERC_L0_PRIS2, BucketId::GMERC_L1_PRIS2, i), m_generic2,
          Generic2::Mode::NORMAL);
      init_bucket_renderer<Generic2BucketRenderer>(
          fmt::format("gmerc2-l{}-pris2", i), BucketCategory::GENERIC,
          GET_BUCKET_ID_FOR_LIST(BucketId::GMERC2_L0_PRIS2, BucketId::GMERC2_L1_PRIS2, i),
          m_generic2, Generic2::Mode::PRIM);
    }

    // 401
    init_bucket_renderer<TextureUploadHandler>("tex-lcom-pris", BucketCategory::TEX,
                                               BucketId::TEX_LCOM_PRIS, m_texture_animator);
    init_bucket_renderer<Merc2BucketRenderer>("merc-lcom-pris", BucketCategory::MERC,
                                              BucketId::MERC_LCOM_PRIS, m_merc2);
    init_bucket_renderer<Generic2BucketRenderer>("gmerc-lcom-pris", BucketCategory::GENERIC,
                                                 BucketId::GMERC_LCOM_PRIS, m_generic2,
                                                 Generic2::Mode::NORMAL);

    init_bucket_renderer<Generic2BucketRenderer>("gmerc2-lcom-pris", BucketCategory::GENERIC,
                                                 BucketId::GMERC2_LCOM_PRIS, m_generic2,
                                                 Generic2::Mode::PRIM);

    // 461
    init_bucket_renderer<TextureUploadHandler>("tex-lcom-sky-post", BucketCategory::TEX,
                                               BucketId::TEX_LCOM_SKY_POST, m_texture_animator);
    init_bucket_renderer<OceanNear>("ocean-near", BucketCategory::OCEAN, BucketId::OCEAN_NEAR);

    // 463
    for (int i = 0; i < LEVEL_MAX; i++) {
#define GET_BUCKET_ID_FOR_LIST(bkt1, bkt2, idx) ((int)(bkt1) + ((int)(bkt2) - (int)(bkt1)) * (idx))
      // 463
      init_bucket_renderer<TextureUploadHandler>(
          fmt::format("tex-l{}-water", i), BucketCategory::TEX,
          GET_BUCKET_ID_FOR_LIST(BucketId::TEX_L0_WATER, BucketId::TEX_L1_WATER, i),
          m_texture_animator);
      init_bucket_renderer<Merc2BucketRenderer>(
          fmt::format("merc-l{}-water", i), BucketCategory::MERC,
          GET_BUCKET_ID_FOR_LIST(BucketId::MERC_L0_WATER, BucketId::MERC_L1_WATER, i), m_merc2);
      init_bucket_renderer<Generic2BucketRenderer>(
          fmt::format("gmerc-l{}-water", i), BucketCategory::GENERIC,
          GET_BUCKET_ID_FOR_LIST(BucketId::GMERC_L0_WATER, BucketId::GMERC_L1_WATER, i), m_generic2,
          Generic2::Mode::NORMAL);
      // 466
      init_bucket_renderer<TFragment>(
          fmt::format("tfrag-l{}-water", i), BucketCategory::TFRAG,
          GET_BUCKET_ID_FOR_LIST(BucketId::TFRAG_L0_WATER, BucketId::TFRAG_L1_WATER, i),
          std::vector{tfrag3::TFragmentTreeKind::WATER}, false, i, anim_slot_array());
      init_bucket_renderer<Generic2BucketRenderer>(
          fmt::format("gmerc2-l{}-water", i), BucketCategory::GENERIC,
          GET_BUCKET_ID_FOR_LIST(BucketId::GMERC2_L0_WATER, BucketId::GMERC2_L1_WATER, i),
          m_generic2, Generic2::Mode::PRIM);
    }

    // 563
    init_bucket_renderer<TextureUploadHandler>("tex-lcom-water", BucketCategory::TEX,
                                               BucketId::TEX_LCOM_WATER, m_texture_animator);
    init_bucket_renderer<Merc2BucketRenderer>("merc-lcom-water", BucketCategory::MERC,
                                              BucketId::MERC_LCOM_WATER, m_merc2);
    init_bucket_renderer<Generic2BucketRenderer>("generic2-lcom-water", BucketCategory::GENERIC,
                                                 BucketId::GMERC2_LCOM_WATER, m_generic2,
                                                 Generic2::Mode::PRIM);

    // 568
    init_bucket_renderer<TextureUploadHandler>("tex-sprite", BucketCategory::TEX,
                                               BucketId::TEX_SPRITE, m_texture_animator);
    init_bucket_renderer<Generic2BucketRenderer>("generic-sprite-1", BucketCategory::GENERIC,
                                                 BucketId::GENERIC_SPRITE_1, m_generic2,
                                                 Generic2::Mode::PRIM);
    init_bucket_renderer<Sprite3>("particles", BucketCategory::SPRITE, BucketId::PARTICLES);
    init_bucket_renderer<Generic2BucketRenderer>("generic-sprite-2", BucketCategory::GENERIC,
                                                 BucketId::GENERIC_SPRITE_2, m_generic2,
                                                 Generic2::Mode::PRIM);
    init_bucket_renderer<Generic2BucketRenderer>("generic-sprite-3", BucketCategory::OTHER,
                                                 BucketId::GENERIC_SPRITE_3, m_generic2,
                                                 Generic2::Mode::LIGHTNING);

    init_bucket_renderer<Shadow2>("shadow2", BucketCategory::OTHER, BucketId::SHADOW2);
    init_bucket_renderer<Shadow2>("shadow3", BucketCategory::OTHER, BucketId::SHADOW3);
    // 575
    init_bucket_renderer<TextureUploadHandler>("tex-warp", BucketCategory::TEX, BucketId::TEX_WARP,
                                               m_texture_animator);
    init_bucket_renderer<Warp>("generic-warp", BucketCategory::GENERIC, BucketId::GENERIC_WARP,
                               m_generic2);

    init_bucket_renderer<TextureUploadHandler>("debug-no-zbuf1", BucketCategory::OTHER,
                                               BucketId::DEBUG_NO_ZBUF1, m_texture_animator, true);
    // 578
    init_bucket_renderer<TextureUploadHandler>("tex-hud-hud-alpha", BucketCategory::TEX,
                                               BucketId::TEX_HUD_HUD_ALPHA, m_texture_animator,
                                               true);
    init_bucket_renderer<ProgressRenderer>("hud-draw-hud-alpha", BucketCategory::OTHER,
                                           BucketId::HUD_DRAW_HUD_ALPHA, 0x8000);
    init_bucket_renderer<TextureUploadHandler>("tex-hud-pris2", BucketCategory::TEX,
                                               BucketId::TEX_HUD_PRIS2, m_texture_animator, true);
    init_bucket_renderer<ProgressRenderer>("hud-draw-pris2", BucketCategory::OTHER,
                                           BucketId::HUD_DRAW_PRIS2, 0x8000);
    init_bucket_renderer<ProgressRenderer>("progress", BucketCategory::OTHER, BucketId::BUCKET582,
                                           0x8000);

    // 583
    init_bucket_renderer<DirectRenderer>("debug", BucketCategory::OTHER, BucketId::DEBUG, 0x8000);
    // 584
    init_bucket_renderer<DirectRenderer>("debug-no-zbuf2", BucketCategory::OTHER,
                                         BucketId::DEBUG_NO_ZBUF2, 0x8000);
    init_bucket_renderer<DirectRenderer>("debug-menu", BucketCategory::OTHER, BucketId::DEBUG_MENU,
                                         0x8000);

    auto eye_renderer = std::make_unique<EyeRenderer>("eyes", 0);
    m_render_state.eye_renderer = eye_renderer.get();
    m_jak3_eye_renderer = std::move(eye_renderer);

    // for any unset renderers, just set them to an EmptyBucketRenderer.
    for (size_t i = 0; i < m_bucket_renderers.size(); i++) {
      if (!m_bucket_renderers[i]) {
        init_bucket_renderer<EmptyBucketRenderer>(fmt::format("bucket-{}", i),
                                                  BucketCategory::OTHER, i);
      }

      m_bucket_renderers[i]->init_shaders(m_render_state.shaders);
      m_bucket_renderers[i]->init_textures(*m_render_state.texture_pool, GameVersion::Jak3);
    }
    m_jak3_eye_renderer->init_shaders(m_render_state.shaders);
    m_jak3_eye_renderer->init_textures(*m_render_state.texture_pool, GameVersion::Jak3);
  }
}

void OpenGLRenderer::init_bucket_renderers_jak2() {
  using namespace jak2;
  m_bucket_renderers.resize((int)BucketId::MAX_BUCKETS);
  m_bucket_categories.resize((int)BucketId::MAX_BUCKETS, BucketCategory::OTHER);

  // 0
  init_bucket_renderer<VisDataHandler>("vis", BucketCategory::OTHER, BucketId::BUCKET_2);
  m_blit_displays =
      init_bucket_renderer<BlitDisplays>("blit", BucketCategory::OTHER, BucketId::BUCKET_3);
  init_bucket_renderer<TextureUploadHandler>("tex-lcom-sky-pre", BucketCategory::TEX,
                                             BucketId::TEX_LCOM_SKY_PRE, m_texture_animator);
  init_bucket_renderer<DirectRenderer>("sky-draw", BucketCategory::OTHER, BucketId::SKY_DRAW, 1024);
  init_bucket_renderer<OceanMidAndFar>("ocean-mid-far", BucketCategory::OCEAN,
                                       BucketId::OCEAN_MID_FAR);

  for (int i = 0; i < LEVEL_MAX; ++i) {
#define GET_BUCKET_ID_FOR_LIST(bkt1, bkt2, idx) ((int)(bkt1) + ((int)(bkt2) - (int)(bkt1)) * (idx))
    init_bucket_renderer<TextureUploadHandler>(
        fmt::format("tex-l{}-tfrag", i), BucketCategory::TEX,
        GET_BUCKET_ID_FOR_LIST(BucketId::TEX_L0_TFRAG, BucketId::TEX_L1_TFRAG, i),
        m_texture_animator);
    init_bucket_renderer<TFragment>(
        fmt::format("tfrag-l{}-tfrag", i), BucketCategory::TFRAG,
        GET_BUCKET_ID_FOR_LIST(BucketId::TFRAG_L0_TFRAG, BucketId::TFRAG_L1_TFRAG, i),
        std::vector{tfrag3::TFragmentTreeKind::NORMAL}, false, i, anim_slot_array());
    Tie3* tie = init_bucket_renderer<Tie3>(
        fmt::format("tie-l{}-tfrag", i), BucketCategory::TIE,
        GET_BUCKET_ID_FOR_LIST(BucketId::TIE_L0_TFRAG, BucketId::TIE_L1_TFRAG, i), i,
        anim_slot_array());
    init_bucket_renderer<Tie3AnotherCategory>(
        fmt::format("etie-l{}-tfrag", i), BucketCategory::TIE,
        GET_BUCKET_ID_FOR_LIST(BucketId::ETIE_L0_TFRAG, BucketId::ETIE_L1_TFRAG, i), tie,
        tfrag3::TieCategory::NORMAL_ENVMAP);
    init_bucket_renderer<Merc2BucketRenderer>(
        fmt::format("merc-l{}-tfrag", i), BucketCategory::MERC,
        GET_BUCKET_ID_FOR_LIST(BucketId::MERC_L0_TFRAG, BucketId::MERC_L1_TFRAG, i), m_merc2);
    init_bucket_renderer<Generic2BucketRenderer>(
        fmt::format("gmerc-l{}-tfrag", i), BucketCategory::MERC,
        GET_BUCKET_ID_FOR_LIST(BucketId::GMERC_L0_TFRAG, BucketId::GMERC_L1_TFRAG, i), m_generic2,
        Generic2::Mode::NORMAL);

    init_bucket_renderer<TextureUploadHandler>(
        fmt::format("tex-l{}-shrub", i), BucketCategory::TEX,
        GET_BUCKET_ID_FOR_LIST(BucketId::TEX_L0_SHRUB, BucketId::TEX_L1_SHRUB, i),
        m_texture_animator);
    init_bucket_renderer<Shrub>(
        fmt::format("shrub-l{}-shrub", i), BucketCategory::SHRUB,
        GET_BUCKET_ID_FOR_LIST(BucketId::SHRUB_L0_SHRUB, BucketId::SHRUB_L1_SHRUB, i));
    init_bucket_renderer<Merc2BucketRenderer>(
        fmt::format("merc-l{}-shrub", i), BucketCategory::MERC,
        GET_BUCKET_ID_FOR_LIST(BucketId::MERC_L0_SHRUB, BucketId::MERC_L1_SHRUB, i), m_merc2);
    init_bucket_renderer<Generic2BucketRenderer>(
        fmt::format("gmerc-l{}-shrub", i), BucketCategory::MERC,
        GET_BUCKET_ID_FOR_LIST(BucketId::GMERC_L0_SHRUB, BucketId::GMERC_L1_SHRUB, i), m_generic2,
        Generic2::Mode::NORMAL);

    init_bucket_renderer<TextureUploadHandler>(
        fmt::format("tex-l{}-alpha", i), BucketCategory::TEX,
        GET_BUCKET_ID_FOR_LIST(BucketId::TEX_L0_ALPHA, BucketId::TEX_L1_ALPHA, i),
        m_texture_animator);
    init_bucket_renderer<TFragment>(
        fmt::format("tfrag-t-l{}-alpha", i), BucketCategory::TFRAG,
        GET_BUCKET_ID_FOR_LIST(BucketId::TFRAG_T_L0_ALPHA, BucketId::TFRAG_T_L1_ALPHA, i),
        std::vector{tfrag3::TFragmentTreeKind::TRANS}, false, i, anim_slot_array());
    init_bucket_renderer<Tie3AnotherCategory>(
        fmt::format("tie-t-l{}-alpha", i), BucketCategory::TIE,
        GET_BUCKET_ID_FOR_LIST(BucketId::TIE_T_L0_ALPHA, BucketId::TIE_T_L1_ALPHA, i), tie,
        tfrag3::TieCategory::TRANS);
    init_bucket_renderer<Tie3AnotherCategory>(
        fmt::format("etie-t-l{}-alpha", i), BucketCategory::TIE,
        GET_BUCKET_ID_FOR_LIST(BucketId::ETIE_T_L0_ALPHA, BucketId::ETIE_T_L1_ALPHA, i), tie,
        tfrag3::TieCategory::TRANS_ENVMAP);
    init_bucket_renderer<Merc2BucketRenderer>(
        fmt::format("merc-l{}-alpha", i), BucketCategory::MERC,
        GET_BUCKET_ID_FOR_LIST(BucketId::MERC_L0_ALPHA, BucketId::MERC_L1_ALPHA, i), m_merc2);
    init_bucket_renderer<Generic2BucketRenderer>(
        fmt::format("gmerc-l{}-alpha", i), BucketCategory::GENERIC,
        GET_BUCKET_ID_FOR_LIST(BucketId::GMERC_L0_ALPHA, BucketId::GMERC_L1_ALPHA, i), m_generic2,
        Generic2::Mode::NORMAL);

    init_bucket_renderer<TextureUploadHandler>(
        fmt::format("tex-l{}-pris", i), BucketCategory::TEX,
        GET_BUCKET_ID_FOR_LIST(BucketId::TEX_L0_PRIS, BucketId::TEX_L1_PRIS, i),
        m_texture_animator);
    init_bucket_renderer<Merc2BucketRenderer>(
        fmt::format("merc-l{}-pris", i), BucketCategory::MERC,
        GET_BUCKET_ID_FOR_LIST(BucketId::MERC_L0_PRIS, BucketId::MERC_L1_PRIS, i), m_merc2);
    init_bucket_renderer<Generic2BucketRenderer>(
        fmt::format("gmerc-l{}-pris", i), BucketCategory::GENERIC,
        GET_BUCKET_ID_FOR_LIST(BucketId::GMERC_L0_PRIS, BucketId::GMERC_L1_PRIS, i), m_generic2,
        Generic2::Mode::NORMAL);

    init_bucket_renderer<TextureUploadHandler>(
        fmt::format("tex-l{}-pris2", i), BucketCategory::TEX,
        GET_BUCKET_ID_FOR_LIST(BucketId::TEX_L0_PRIS2, BucketId::TEX_L1_PRIS2, i),
        m_texture_animator);
    init_bucket_renderer<Merc2BucketRenderer>(
        fmt::format("merc-l{}-pris2", i), BucketCategory::MERC,
        GET_BUCKET_ID_FOR_LIST(BucketId::MERC_L0_PRIS2, BucketId::MERC_L1_PRIS2, i), m_merc2);
    init_bucket_renderer<Generic2BucketRenderer>(
        fmt::format("gmerc-l{}-pris2", i), BucketCategory::GENERIC,
        GET_BUCKET_ID_FOR_LIST(BucketId::GMERC_L0_PRIS2, BucketId::GMERC_L1_PRIS2, i), m_generic2,
        Generic2::Mode::NORMAL);

    init_bucket_renderer<TextureUploadHandler>(
        fmt::format("tex-l{}-water", i), BucketCategory::TEX,
        GET_BUCKET_ID_FOR_LIST(BucketId::TEX_L0_WATER, BucketId::TEX_L1_WATER, i),
        m_texture_animator);
    init_bucket_renderer<Merc2BucketRenderer>(
        fmt::format("merc-l{}-water", i), BucketCategory::MERC,
        GET_BUCKET_ID_FOR_LIST(BucketId::MERC_L0_WATER, BucketId::MERC_L1_WATER, i), m_merc2);
    init_bucket_renderer<Generic2BucketRenderer>(
        fmt::format("gmerc-l{}-water", i), BucketCategory::GENERIC,
        GET_BUCKET_ID_FOR_LIST(BucketId::GMERC_L0_WATER, BucketId::GMERC_L1_WATER, i), m_generic2,
        Generic2::Mode::NORMAL);
    init_bucket_renderer<TFragment>(
        fmt::format("tfrag-w-l{}-alpha", i), BucketCategory::TFRAG,
        GET_BUCKET_ID_FOR_LIST(BucketId::TFRAG_W_L0_WATER, BucketId::TFRAG_W_L1_WATER, i),
        std::vector{tfrag3::TFragmentTreeKind::WATER}, false, i, anim_slot_array());
    init_bucket_renderer<Tie3AnotherCategory>(
        fmt::format("tie-w-l{}-water", i), BucketCategory::TIE,
        GET_BUCKET_ID_FOR_LIST(BucketId::TIE_W_L0_WATER, BucketId::TIE_W_L1_WATER, i), tie,
        tfrag3::TieCategory::WATER);
    init_bucket_renderer<Tie3AnotherCategory>(
        fmt::format("etie-w-l{}-water", i), BucketCategory::TIE,
        GET_BUCKET_ID_FOR_LIST(BucketId::ETIE_W_L0_WATER, BucketId::ETIE_W_L1_WATER, i), tie,
        tfrag3::TieCategory::WATER_ENVMAP);
#undef GET_BUCKET_ID_FOR_LIST
  }
  // 180
  init_bucket_renderer<TextureUploadHandler>("tex-lcom-tfrag", BucketCategory::TEX,
                                             BucketId::TEX_LCOM_TFRAG, m_texture_animator);
  init_bucket_renderer<Merc2BucketRenderer>("merc-lcom-tfrag", BucketCategory::MERC,
                                            BucketId::MERC_LCOM_TFRAG, m_merc2);
  // 190
  init_bucket_renderer<TextureUploadHandler>("tex-lcom-shrub", BucketCategory::TEX,
                                             BucketId::TEX_LCOM_SHRUB, m_texture_animator);
  init_bucket_renderer<Merc2BucketRenderer>("merc-lcom-shrub", BucketCategory::MERC,
                                            BucketId::MERC_LCOM_SHRUB, m_merc2);
  init_bucket_renderer<Generic2BucketRenderer>("gmerc-lcom-tfrag", BucketCategory::GENERIC,
                                               BucketId::GMERC_LCOM_TFRAG, m_generic2,
                                               Generic2::Mode::NORMAL);
  init_bucket_renderer<Shadow2>("shadow", BucketCategory::OTHER, BucketId::SHADOW);
  // 220
  init_bucket_renderer<TextureUploadHandler>("tex-lcom-pris", BucketCategory::TEX,
                                             BucketId::TEX_LCOM_PRIS, m_texture_animator);
  init_bucket_renderer<Merc2BucketRenderer>("merc-lcom-pris", BucketCategory::MERC,
                                            BucketId::MERC_LCOM_PRIS, m_merc2);
  init_bucket_renderer<Generic2BucketRenderer>("gmerc-lcom-pris", BucketCategory::GENERIC,
                                               BucketId::GMERC_LCOM_PRIS, m_generic2,
                                               Generic2::Mode::NORMAL);
  init_bucket_renderer<TextureUploadHandler>("tex-lcom-water", BucketCategory::TEX,
                                             BucketId::TEX_LCOM_WATER, m_texture_animator);
  init_bucket_renderer<Merc2BucketRenderer>("merc-lcom-water", BucketCategory::MERC,
                                            BucketId::MERC_LCOM_WATER, m_merc2);
  init_bucket_renderer<TextureUploadHandler>("tex-lcom-sky-post", BucketCategory::TEX,
                                             BucketId::TEX_LCOM_SKY_POST, m_texture_animator);
  // 310
  init_bucket_renderer<OceanNear>("ocean-near", BucketCategory::OCEAN, BucketId::OCEAN_NEAR);
  init_bucket_renderer<TextureUploadHandler>("tex-all-sprite", BucketCategory::TEX,
                                             BucketId::TEX_ALL_SPRITE, m_texture_animator);
  init_bucket_renderer<Sprite3>("particles", BucketCategory::SPRITE, BucketId::PARTICLES);
  init_bucket_renderer<Shadow2>("shadow2", BucketCategory::OTHER, BucketId::SHADOW2);
  init_bucket_renderer<Generic2BucketRenderer>("effects", BucketCategory::OTHER, BucketId::EFFECTS,
                                               m_generic2, Generic2::Mode::LIGHTNING);
  init_bucket_renderer<TextureUploadHandler>("tex-all-warp", BucketCategory::TEX,
                                             BucketId::TEX_ALL_WARP, m_texture_animator);
  init_bucket_renderer<Warp>("warp", BucketCategory::GENERIC, BucketId::GMERC_WARP, m_generic2);
  init_bucket_renderer<TextureUploadHandler>("debug-no-zbuf1", BucketCategory::OTHER,
                                             BucketId::DEBUG_NO_ZBUF1, m_texture_animator, true);
  init_bucket_renderer<TextureUploadHandler>("tex-all-map", BucketCategory::TEX,
                                             BucketId::TEX_ALL_MAP, m_texture_animator, true);
  // 320
  init_bucket_renderer<ProgressRenderer>("progress", BucketCategory::OTHER, BucketId::PROGRESS,
                                         0x1000);
  init_bucket_renderer<DirectRenderer>("screen-filter", BucketCategory::OTHER,
                                       BucketId::SCREEN_FILTER, 256);
  init_bucket_renderer<TextureUploadHandler>("subtitle", BucketCategory::OTHER, BucketId::SUBTITLE,
                                             m_texture_animator, true);
  init_bucket_renderer<DirectRenderer>("debug2", BucketCategory::OTHER, BucketId::DEBUG2, 0x8000);
  init_bucket_renderer<DirectRenderer>("debug-no-zbuf2", BucketCategory::OTHER,
                                       BucketId::DEBUG_NO_ZBUF2, 0x8000);
  init_bucket_renderer<DirectRenderer>("debug3", BucketCategory::OTHER, BucketId::DEBUG3, 0x2000);

  auto eye_renderer = std::make_unique<EyeRenderer>("eyes", 0);
  m_render_state.eye_renderer = eye_renderer.get();
  m_jak2_eye_renderer = std::move(eye_renderer);

  {
    auto p = scoped_prof("render-inits");
    // for now, for any unset renderers, just set them to an EmptyBucketRenderer.
    for (size_t i = 0; i < m_bucket_renderers.size(); i++) {
      if (!m_bucket_renderers[i]) {
        init_bucket_renderer<EmptyBucketRenderer>(fmt::format("bucket-{}", i),
                                                  BucketCategory::OTHER, i);
      }

      m_bucket_renderers[i]->init_shaders(m_render_state.shaders);
      m_bucket_renderers[i]->init_textures(*m_render_state.texture_pool, GameVersion::Jak2);
    }
    m_jak2_eye_renderer->init_shaders(m_render_state.shaders);
    m_jak2_eye_renderer->init_textures(*m_render_state.texture_pool, GameVersion::Jak2);
  }
}

/*!
 * Construct bucket renderers.  We can specify different renderers for different buckets
 */
namespace {
/// Profiler group per Jak 1 bucket. TIE and ocean mid/far buckets time their own parts inside.
std::vector<perf::Group> perf_groups_jak1(const std::vector<BucketCategory>& cats) {
  using jak1::BucketId;
  std::vector<perf::Group> out(cats.size(), perf::Group::NONE);
  for (size_t i = 0; i < cats.size(); i++) {
    switch (cats[i]) {
      case BucketCategory::TEX:
        out[i] = perf::Group::TEX;
        break;
      case BucketCategory::TFRAG:
        out[i] = perf::Group::TFRAG;
        break;
      case BucketCategory::SHRUB:
        out[i] = perf::Group::SHRUB;
        break;
      case BucketCategory::GENERIC:
        out[i] = perf::Group::GENERIC;
        break;
      case BucketCategory::MERC:
        out[i] = perf::Group::MERC;
        break;
      case BucketCategory::SPRITE:
        out[i] = perf::Group::SPRITE;
        break;
      default:
        break;
    }
  }
  auto set = [&](BucketId id, perf::Group g) { out[(int)id] = g; };
  set(BucketId::SKY_DRAW, perf::Group::SKY);
  set(BucketId::TFRAG_TRANS0_AND_SKY_BLEND_LEVEL0, perf::Group::SKY_BLEND);
  set(BucketId::TFRAG_TRANS1_AND_SKY_BLEND_LEVEL1, perf::Group::SKY_BLEND);
  set(BucketId::SHADOW, perf::Group::SHADOW);
  set(BucketId::MERC_EYES_AFTER_PRIS, perf::Group::EYES);
  set(BucketId::OCEAN_NEAR, perf::Group::OCEAN_NEAR);
  set(BucketId::DEPTH_CUE, perf::Group::DEPTH_CUE);
  set(BucketId::DEBUG, perf::Group::DIRECT);
  set(BucketId::DEBUG_NO_ZBUF, perf::Group::DIRECT);
  set(BucketId::SUBTITLE, perf::Group::DIRECT);
  return out;
}
}  // namespace

void OpenGLRenderer::init_bucket_renderers_jak1() {
  using namespace jak1;
  m_bucket_renderers.resize((int)BucketId::MAX_BUCKETS);
  m_bucket_categories.resize((int)BucketId::MAX_BUCKETS, BucketCategory::OTHER);

  std::vector<tfrag3::TFragmentTreeKind> normal_tfrags = {tfrag3::TFragmentTreeKind::NORMAL,
                                                          tfrag3::TFragmentTreeKind::LOWRES};
  std::vector<tfrag3::TFragmentTreeKind> dirt_tfrags = {tfrag3::TFragmentTreeKind::DIRT};
  std::vector<tfrag3::TFragmentTreeKind> ice_tfrags = {tfrag3::TFragmentTreeKind::ICE};
  auto sky_gpu_blender = std::make_shared<SkyBlendGPU>();
  auto sky_cpu_blender = std::make_shared<SkyBlendCPU>();

  //-------------
  // PRE TEXTURE
  //-------------
  // 0 : ??
  // 1 : ??
  // 2 : ??
  // 3 : SKY_DRAW
  init_bucket_renderer<SkyRenderer>("sky", BucketCategory::OTHER, BucketId::SKY_DRAW);
  // 4 : OCEAN_MID_AND_FAR
  init_bucket_renderer<OceanMidAndFar>("ocean-mid-far", BucketCategory::OCEAN,
                                       BucketId::OCEAN_MID_AND_FAR);

  //-----------------------
  // LEVEL 0 tfrag texture
  //-----------------------
  // 5 : TFRAG_TEX_LEVEL0
  init_bucket_renderer<TextureUploadHandler>("l0-tfrag-tex", BucketCategory::TEX,
                                             BucketId::TFRAG_TEX_LEVEL0, m_texture_animator);
  // 6 : TFRAG_LEVEL0
  init_bucket_renderer<TFragment>("l0-tfrag-tfrag", BucketCategory::TFRAG, BucketId::TFRAG_LEVEL0,
                                  normal_tfrags, false, 0, anim_slot_array());
  // 7 : TFRAG_NEAR_LEVEL0
  // 8 : TIE_NEAR_LEVEL0
  // 9 : TIE_LEVEL0
  init_bucket_renderer<Tie3WithEnvmapJak1>("l0-tfrag-tie", BucketCategory::TIE,
                                           BucketId::TIE_LEVEL0, 0);
  // 10 : MERC_TFRAG_TEX_LEVEL0
  init_bucket_renderer<Merc2BucketRenderer>("l0-tfrag-merc", BucketCategory::MERC,
                                            BucketId::MERC_TFRAG_TEX_LEVEL0, m_merc2);
  // 11 : GMERC_TFRAG_TEX_LEVEL0
  init_bucket_renderer<Generic2BucketRenderer>("l0-tfrag-generic", BucketCategory::GENERIC,
                                               BucketId::GENERIC_TFRAG_TEX_LEVEL0, m_generic2,
                                               Generic2::Mode::NORMAL);

  //-----------------------
  // LEVEL 1 tfrag texture
  //-----------------------
  // 12 : TFRAG_TEX_LEVEL1
  init_bucket_renderer<TextureUploadHandler>("l1-tfrag-tex", BucketCategory::TEX,
                                             BucketId::TFRAG_TEX_LEVEL1, m_texture_animator);
  // 13 : TFRAG_LEVEL1
  init_bucket_renderer<TFragment>("l1-tfrag-tfrag", BucketCategory::TFRAG, BucketId::TFRAG_LEVEL1,
                                  normal_tfrags, false, 1, anim_slot_array());
  // 14 : TFRAG_NEAR_LEVEL1
  // 15 : TIE_NEAR_LEVEL1
  // 16 : TIE_LEVEL1
  init_bucket_renderer<Tie3WithEnvmapJak1>("l1-tfrag-tie", BucketCategory::TIE,
                                           BucketId::TIE_LEVEL1, 1);
  // 17 : MERC_TFRAG_TEX_LEVEL1
  init_bucket_renderer<Merc2BucketRenderer>("l1-tfrag-merc", BucketCategory::MERC,
                                            BucketId::MERC_TFRAG_TEX_LEVEL1, m_merc2);
  // 18 : GMERC_TFRAG_TEX_LEVEL1
  init_bucket_renderer<Generic2BucketRenderer>("l1-tfrag-generic", BucketCategory::GENERIC,
                                               BucketId::GENERIC_TFRAG_TEX_LEVEL1, m_generic2,
                                               Generic2::Mode::NORMAL);

  //-----------------------
  // LEVEL 0 shrub texture
  //-----------------------
  // 19 : SHRUB_TEX_LEVEL0
  init_bucket_renderer<TextureUploadHandler>("l0-shrub-tex", BucketCategory::TEX,
                                             BucketId::SHRUB_TEX_LEVEL0, m_texture_animator);
  // 20 : SHRUB_NORMAL_LEVEL0
  init_bucket_renderer<Shrub>("l0-shrub", BucketCategory::SHRUB, BucketId::SHRUB_NORMAL_LEVEL0);
  // 21 : ???
  // 22 : SHRUB_BILLBOARD_LEVEL0
  // 23 : SHRUB_TRANS_LEVEL0
  // 24 : SHRUB_GENERIC_LEVEL0
  init_bucket_renderer<Generic2BucketRenderer>("l0-shrub-generic", BucketCategory::GENERIC,
                                               BucketId::SHRUB_GENERIC_LEVEL0, m_generic2,
                                               Generic2::Mode::NORMAL);

  //-----------------------
  // LEVEL 1 shrub texture
  //-----------------------
  // 25 : SHRUB_TEX_LEVEL1
  init_bucket_renderer<TextureUploadHandler>("l1-shrub-tex", BucketCategory::TEX,
                                             BucketId::SHRUB_TEX_LEVEL1, m_texture_animator);
  // 26 : SHRUB_NORMAL_LEVEL1
  init_bucket_renderer<Shrub>("l1-shrub", BucketCategory::SHRUB, BucketId::SHRUB_NORMAL_LEVEL1);
  // 27 : ???
  // 28 : SHRUB_BILLBOARD_LEVEL1
  // 29 : SHRUB_TRANS_LEVEL1
  // 30 : SHRUB_GENERIC_LEVEL1
  init_bucket_renderer<Generic2BucketRenderer>("l1-shrub-generic", BucketCategory::GENERIC,
                                               BucketId::SHRUB_GENERIC_LEVEL1, m_generic2,
                                               Generic2::Mode::NORMAL);

  //-----------------------
  // LEVEL 0 alpha texture
  //-----------------------
  init_bucket_renderer<TextureUploadHandler>("l0-alpha-tex", BucketCategory::TEX,
                                             BucketId::ALPHA_TEX_LEVEL0, m_texture_animator);  // 31
  init_bucket_renderer<SkyBlendHandler>("l0-alpha-sky-blend-and-tfrag-trans", BucketCategory::OTHER,
                                        BucketId::TFRAG_TRANS0_AND_SKY_BLEND_LEVEL0, 0,
                                        sky_gpu_blender, sky_cpu_blender, anim_slot_array());  // 32
  // 33
  init_bucket_renderer<TFragment>("l0-alpha-tfrag", BucketCategory::TFRAG,
                                  BucketId::TFRAG_DIRT_LEVEL0, dirt_tfrags, false, 0,
                                  anim_slot_array());  // 34
  // 35
  init_bucket_renderer<TFragment>("l0-alpha-tfrag-ice", BucketCategory::TFRAG,
                                  BucketId::TFRAG_ICE_LEVEL0, ice_tfrags, false, 0,
                                  anim_slot_array());
  // 37

  //-----------------------
  // LEVEL 1 alpha texture
  //-----------------------
  init_bucket_renderer<TextureUploadHandler>("l1-alpha-tex", BucketCategory::TEX,
                                             BucketId::ALPHA_TEX_LEVEL1, m_texture_animator);  // 38
  init_bucket_renderer<SkyBlendHandler>("l1-alpha-sky-blend-and-tfrag-trans", BucketCategory::OTHER,
                                        BucketId::TFRAG_TRANS1_AND_SKY_BLEND_LEVEL1, 1,
                                        sky_gpu_blender, sky_cpu_blender, anim_slot_array());  // 39
  // 40
  init_bucket_renderer<TFragment>("l1-alpha-tfrag-dirt", BucketCategory::TFRAG,
                                  BucketId::TFRAG_DIRT_LEVEL1, dirt_tfrags, false, 1,
                                  anim_slot_array());  // 41
  // 42
  init_bucket_renderer<TFragment>("l1-alpha-tfrag-ice", BucketCategory::TFRAG,
                                  BucketId::TFRAG_ICE_LEVEL1, ice_tfrags, false, 1,
                                  anim_slot_array());
  // 44

  init_bucket_renderer<Merc2BucketRenderer>("common-alpha-merc", BucketCategory::MERC,
                                            BucketId::MERC_AFTER_ALPHA, m_merc2);

  init_bucket_renderer<Generic2BucketRenderer>("common-alpha-generic", BucketCategory::GENERIC,
                                               BucketId::GENERIC_ALPHA, m_generic2,
                                               Generic2::Mode::NORMAL);                     // 46
  init_bucket_renderer<ShadowRenderer>("shadow", BucketCategory::OTHER, BucketId::SHADOW);  // 47

  //-----------------------
  // LEVEL 0 pris texture
  //-----------------------
  init_bucket_renderer<TextureUploadHandler>("l0-pris-tex", BucketCategory::TEX,
                                             BucketId::PRIS_TEX_LEVEL0, m_texture_animator);  // 48
  init_bucket_renderer<Merc2BucketRenderer>("l0-pris-merc", BucketCategory::MERC,
                                            BucketId::MERC_PRIS_LEVEL0, m_merc2);  // 49
  init_bucket_renderer<Generic2BucketRenderer>("l0-pris-generic", BucketCategory::GENERIC,
                                               BucketId::GENERIC_PRIS_LEVEL0, m_generic2,
                                               Generic2::Mode::NORMAL);  // 50

  //-----------------------
  // LEVEL 1 pris texture
  //-----------------------
  init_bucket_renderer<TextureUploadHandler>("l1-pris-tex", BucketCategory::TEX,
                                             BucketId::PRIS_TEX_LEVEL1, m_texture_animator);  // 51
  init_bucket_renderer<Merc2BucketRenderer>("l1-pris-merc", BucketCategory::MERC,
                                            BucketId::MERC_PRIS_LEVEL1, m_merc2);  // 52
  init_bucket_renderer<Generic2BucketRenderer>("l1-pris-generic", BucketCategory::GENERIC,
                                               BucketId::GENERIC_PRIS_LEVEL1, m_generic2,
                                               Generic2::Mode::NORMAL);  // 53

  // other renderers may output to the eye renderer
  m_render_state.eye_renderer = init_bucket_renderer<EyeRenderer>(
      "common-pris-eyes", BucketCategory::OTHER, BucketId::MERC_EYES_AFTER_PRIS);  // 54

  // hack: set to merc2 for debugging
  init_bucket_renderer<Merc2BucketRenderer>("common-pris-merc", BucketCategory::MERC,
                                            BucketId::MERC_AFTER_PRIS, m_merc2);  // 55
  init_bucket_renderer<Generic2BucketRenderer>("common-pris-generic", BucketCategory::GENERIC,
                                               BucketId::GENERIC_PRIS, m_generic2,
                                               Generic2::Mode::NORMAL);  // 56

  //-----------------------
  // LEVEL 0 water texture
  //-----------------------
  init_bucket_renderer<TextureUploadHandler>("l0-water-tex", BucketCategory::TEX,
                                             BucketId::WATER_TEX_LEVEL0, m_texture_animator);  // 57
  init_bucket_renderer<Merc2BucketRenderer>("l0-water-merc", BucketCategory::MERC,
                                            BucketId::MERC_WATER_LEVEL0, m_merc2);  // 58
  init_bucket_renderer<Generic2BucketRenderer>("l0-water-generic", BucketCategory::GENERIC,
                                               BucketId::GENERIC_WATER_LEVEL0, m_generic2,
                                               Generic2::Mode::NORMAL);  // 59

  //-----------------------
  // LEVEL 1 water texture
  //-----------------------
  init_bucket_renderer<TextureUploadHandler>("l1-water-tex", BucketCategory::TEX,
                                             BucketId::WATER_TEX_LEVEL1, m_texture_animator);  // 60
  init_bucket_renderer<Merc2BucketRenderer>("l1-water-merc", BucketCategory::MERC,
                                            BucketId::MERC_WATER_LEVEL1, m_merc2);  // 61
  init_bucket_renderer<Generic2BucketRenderer>("l1-water-generic", BucketCategory::GENERIC,
                                               BucketId::GENERIC_WATER_LEVEL1, m_generic2,
                                               Generic2::Mode::NORMAL);  // 62

  init_bucket_renderer<OceanNear>("ocean-near", BucketCategory::OCEAN, BucketId::OCEAN_NEAR);  // 63

  //-----------------------
  // DEPTH CUE
  //-----------------------
  init_bucket_renderer<DepthCue>("depth-cue", BucketCategory::OTHER, BucketId::DEPTH_CUE);  // 64

  //-----------------------
  // COMMON texture
  //-----------------------
  init_bucket_renderer<TextureUploadHandler>("common-tex", BucketCategory::TEX,
                                             BucketId::PRE_SPRITE_TEX, m_texture_animator);  // 65

  init_bucket_renderer<Sprite3>("sprite", BucketCategory::SPRITE, BucketId::SPRITE);  // 66

  init_bucket_renderer<DirectRenderer>("debug", BucketCategory::OTHER, BucketId::DEBUG, 0x20000);
  init_bucket_renderer<DirectRenderer>("debug-no-zbuf", BucketCategory::OTHER,
                                       BucketId::DEBUG_NO_ZBUF, 0x8000);
  // an extra custom bucket!
  init_bucket_renderer<DirectRenderer>("subtitle", BucketCategory::OTHER, BucketId::SUBTITLE, 6000);

  // for now, for any unset renderers, just set them to an EmptyBucketRenderer.
  for (size_t i = 0; i < m_bucket_renderers.size(); i++) {
    if (!m_bucket_renderers[i]) {
      init_bucket_renderer<EmptyBucketRenderer>(fmt::format("bucket-{}", i), BucketCategory::OTHER,
                                                (BucketId)i);
    }

    m_bucket_renderers[i]->init_shaders(m_render_state.shaders);
    m_bucket_renderers[i]->init_textures(*m_render_state.texture_pool, GameVersion::Jak1);
  }
  sky_cpu_blender->init_textures(*m_render_state.texture_pool, m_version);
  m_perf_groups = perf_groups_jak1(m_bucket_categories);
  sky_gpu_blender->init_textures(*m_render_state.texture_pool, m_version);
  m_vista = std::make_unique<VistaRenderer>(anim_slot_array());
}

namespace {
Fbo make_fbo(int w, int h, int msaa, bool make_zbuf_and_stencil, bool fp16 = false) {
  Fbo result;
  bool use_multisample = msaa > 1;

  // make framebuffer object
  glGenFramebuffers(1, &result.fbo_id);
  glBindFramebuffer(GL_FRAMEBUFFER, result.fbo_id);
  result.valid = true;

  // make texture that will hold the colors of the framebuffer
  GLuint tex;
  glGenTextures(1, &tex);
  result.tex_id = tex;
  glActiveTexture(GL_TEXTURE0);
  if (use_multisample) {
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, tex);
    glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, msaa, fp16 ? GL_RGBA16F : GL_RGBA8, w, h,
                            GL_TRUE);
  } else {
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, fp16 ? GL_RGBA16F : GL_RGBA8, w, h, 0, GL_RGBA,
                 fp16 ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE, nullptr);
  }
  // make depth and stencil buffers that will hold the... depth and stencil buffers
  if (make_zbuf_and_stencil) {
    // a texture rather than a renderbuffer, so screen-space passes can sample it without a copy
    GLuint zbuf;
    glGenTextures(1, &zbuf);
    result.zbuf_stencil_id = zbuf;
    result.zbuf_is_texture = true;
    const GLenum ztarget = use_multisample ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D;
    glBindTexture(ztarget, zbuf);
    if (use_multisample) {
      glTexImage2DMultisample(ztarget, msaa, GL_DEPTH24_STENCIL8, w, h, GL_TRUE);
    } else {
      glTexImage2D(ztarget, 0, GL_DEPTH24_STENCIL8, w, h, 0, GL_DEPTH_STENCIL,
                   GL_UNSIGNED_INT_24_8, nullptr);
      glTexParameteri(ztarget, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(ztarget, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(ztarget, GL_TEXTURE_MAX_LEVEL, 0);
    }
    glBindTexture(ztarget, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, ztarget, zbuf, 0);
  }
  // attach texture to framebuffer as target for colors

  if (use_multisample) {
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D_MULTISAMPLE, tex, 0);
  } else {
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
  }

  GLenum render_targets[1] = {GL_COLOR_ATTACHMENT0};
  glDrawBuffers(1, render_targets);
  auto status = glCheckFramebufferStatus(GL_FRAMEBUFFER);

  if (status != GL_FRAMEBUFFER_COMPLETE) {
    lg::error("Failed to setup framebuffer: {} {} {} {} ", w, h, msaa, make_zbuf_and_stencil);
    switch (status) {
      case GL_FRAMEBUFFER_UNDEFINED:
        lg::print("GL_FRAMEBUFFER_UNDEFINED\n");
        break;
      case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT:
        lg::print("GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT\n");
        break;
      case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT:
        lg::print("GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT\n");
        break;
      case GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER:
        lg::print("GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER\n");
        break;
      case GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER:
        lg::print("GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER\n");
        break;
      case GL_FRAMEBUFFER_UNSUPPORTED:
        lg::print("GL_FRAMEBUFFER_UNSUPPORTED\n");
        break;
      case GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE:
        lg::print("GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE\n");
        break;
      case GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS:
        lg::print("GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS\n");
        break;
    }
    ASSERT(false);
  }

  result.multisample_count = msaa;
  result.multisampled = use_multisample;
  result.is_window = false;
  result.width = w;
  result.height = h;

  return result;
}

/// Sets the frame camera (has_pc_data) from the first tfrag bucket's port data before any
/// bucket runs, so the Jak 1 ocean mid and far (bucket 4) see this frame's camera.
void prime_camera_jak1(DmaFollower dma, SharedRenderState* rs) {
  const u32 base = rs->buckets_base;
  const int buckets[2] = {(int)jak1::BucketId::TFRAG_LEVEL0, (int)jak1::BucketId::TFRAG_LEVEL1};
  for (int b : buckets) {
    const u32 start = base + 16 * b;
    const u32 end = start + 16;
    while (dma.current_tag_offset() != start) {
      dma.read_and_advance();
    }
    while (dma.current_tag_offset() != end) {
      auto t = dma.read_and_advance();
      if (t.size_bytes == sizeof(TfragPcPortData) &&
          t.vifcode1().kind == VifCode::Kind::PC_PORT) {
        TfragPcPortData port;
        memcpy(&port, t.data, sizeof(TfragPcPortData));
        apply_stereo_eye(&port.camera, rs);
        update_render_state_from_pc_settings(rs, port);
        return;
      }
    }
  }
}
}  // namespace

void OpenGLRenderer::blit_display(ScopedProfilerNode& prof) {
  if (m_blit_displays) {
    m_blit_displays->do_copy_back(&m_render_state, prof);
  }
}

/*!
 * Main render function. This is called from the gfx loop with the chain passed from the game.
 */
void OpenGLRenderer::render(DmaFollower dma, const RenderOptions& settings) {
  m_profiler.clear();
  m_render_state.reset();
  m_render_state.stereo_active =
      m_version == GameVersion::Jak1 && (Gfx::g_global_settings.stereo_enable || settings.xr);
  m_xr_repeat = settings.xr && settings.xr_repeat;
  if (!m_render_state.stereo_active && m_stereo_fbo.valid) {
    m_stereo_fbo.clear();
  }
  m_render_state.ee_main_memory = g_ee_main_mem;
  m_render_state.offset_of_s7 = offset_of_s7();
  m_upscaled = false;
  const bool scaled = m_version == GameVersion::Jak1 && settings.display_w > 0 &&
                      settings.display_h > 0 &&
                      (settings.display_w != settings.game_res_w ||
                       settings.display_h != settings.game_res_h);
  m_display_w = scaled ? settings.display_w : 0;
  m_display_h = scaled ? settings.display_h : 0;
  if (!scaled || m_render_state.stereo_active) {
    m_upscaler.release_targets();
  }
  perf::frame_begin(settings.game_res_w, settings.game_res_h, settings.msaa_samples);

  {
    g_current_renderer = "frame-setup";
    auto prof = m_profiler.root()->make_scoped_child("frame-setup");
    perf::Scope ps(perf::Group::SETUP);
    setup_frame(settings);
    if (settings.gpu_sync) {
      glFinish();
    }
  }

  {
    g_current_renderer = "loader";
    auto prof = m_profiler.root()->make_scoped_child("loader");
    if (m_last_pmode_alp == 0 && settings.pmode_alp_register != 0 && m_enable_fast_blackout_loads) {
      // blackout, load everything and don't worry about frame rate
      m_render_state.loader->update_blocking(*m_render_state.texture_pool);

    } else {
      m_render_state.loader->update(*m_render_state.texture_pool);
    }
  }

  // render the buckets!
  {
    auto prof = m_profiler.root()->make_scoped_child("buckets");
    static GpuTimer frame_timer("frame");
    const bool frame_timing = Gfx::g_global_settings.pbr_timing;
    if (frame_timing) {
      frame_timer.begin();
    }
    perf::Scope ps(perf::Group::BUCKETS);
    if (m_render_state.stereo_active) {
      dispatch_buckets_stereo(dma, prof, settings.gpu_sync,
                              m_version == GameVersion::Jak1 ? settings.xr : nullptr);
    } else {
      dispatch_buckets(dma, prof, settings.gpu_sync);
    }
    if (frame_timing) {
      frame_timer.end();
    }
    if (m_texture_animator) {
      // if animation requests weren't made, assume the level is unloaded and the textures should
      // reset.
      m_texture_animator->clear_stale_textures(m_render_state.frame_idx);
    }
  }

  // blit framebuffer so that it can be used as a texture by the game later
  {
    g_current_renderer = "blit-display";
    auto prof = m_profiler.root()->make_scoped_child("blit-display");
    perf::Scope ps(perf::Group::BLIT_DISPLAY);
    blit_display(prof);
  }

  // apply effects done with PCRTC registers, as well as blit the framebuffer to the window and
  // apply brightness/contrast
  {
    g_current_renderer = "pcrtc";
    auto prof = m_profiler.root()->make_scoped_child("pcrtc");
    const bool pcrtc_timing =
        m_version == GameVersion::Jak1 && Gfx::g_global_settings.ssao_gpu_timing;
    if (pcrtc_timing) {
      m_pcrtc_timer.begin();
    }
    perf::Scope ps(perf::Group::PCRTC);
    do_pcrtc_effects(settings.pmode_alp_register, settings.brightness_contrast_color,
                     settings.brightness_contrast_alpha, &m_render_state, prof,
                     !settings.save_screenshot);
    if (pcrtc_timing) {
      m_pcrtc_timer.end();
    }
    if (settings.gpu_sync) {
      glFinish();
    }
  }

  m_last_pmode_alp = settings.pmode_alp_register;

  if (settings.save_screenshot) {
    g_current_renderer = "screenshot";
    auto prof = m_profiler.root()->make_scoped_child("screenshot");
    int read_buffer;
    int x, y, w, h, fbo_id;

    if (m_fbo_state.hdr && !bug_report::owns(settings.screenshot_path)) {
      save_hdr_scene(prof, settings.screenshot_path,
                     !settings.internal_res_screenshot &&
                         !(Gfx::g_hdr_present.active && Gfx::g_hdr_present.hdr));
    }
    if (settings.internal_res_screenshot) {
      Fbo* screenshot_src;
      // can't screenshot from a multisampled buffer directly -
      // a calibration pattern is drawn by the final pass, so it goes through that pass too
      if (m_fbo_state.hdr || Gfx::g_global_settings.hdr_cal_pattern != 0) {
        screenshot_src = hdr_screenshot_source(prof);
        read_buffer = GL_COLOR_ATTACHMENT0;
      } else if (m_render_state.stereo_active && m_stereo_fbo.valid) {
        screenshot_src = &m_stereo_fbo;
        read_buffer = GL_COLOR_ATTACHMENT0;
      } else if (m_upscaled) {
        // after the upscale and the text buckets, before the final pass and any HDR encode
        screenshot_src = m_upscaler.display();
        read_buffer = GL_COLOR_ATTACHMENT0;
      } else if (m_fbo_state.resources.resolve_buffer.valid) {
        screenshot_src = &m_fbo_state.resources.resolve_buffer;
        read_buffer = GL_COLOR_ATTACHMENT0;
      } else {
        screenshot_src = m_fbo_state.render_fbo;
        read_buffer = GL_COLOR_ATTACHMENT0;  // an FBO has no GL_FRONT (single-sample shots)
      }
      w = screenshot_src->width;
      h = screenshot_src->height;
      x = 0;
      y = 0;
      fbo_id = screenshot_src->fbo_id;
    } else if (Gfx::g_hdr_present.active && Gfx::g_hdr_present.hdr) {
      // the window holds scRGB; save the SDR tonemap of the game image instead
      Fbo* src = hdr_screenshot_source(prof);
      read_buffer = GL_COLOR_ATTACHMENT0;
      w = src->width;
      h = src->height;
      x = 0;
      y = 0;
      fbo_id = src->fbo_id;
    } else {
      // SDR through DXGI: the window's image is in the RGBA8 stand-in
      fbo_id = Gfx::window_fbo();
      read_buffer = fbo_id ? GL_COLOR_ATTACHMENT0 : GL_BACK;
      w = settings.draw_region_width;
      h = settings.draw_region_height;
      x = m_render_state.draw_offset_x;
      y = m_render_state.draw_offset_y;
    }
    finish_screenshot(settings.screenshot_path, w, h, x, y, fbo_id, read_buffer,
                      settings.quick_screenshot);
  }

  if (settings.draw_render_debug_window) {
    g_current_renderer = "render-window";
    auto prof = m_profiler.root()->make_scoped_child("render-window");
    draw_renderer_selection_window();
    // add a profile bar for the imgui stuff
    // vif_interrupt_callback(0);
    if (settings.gpu_sync) {
      glFinish();
    }
  }

  if (m_version == GameVersion::Jak1) {
    m_inspect.frame(&m_render_state);
  }

  if (settings.draw_loader_window) {
    g_current_renderer = "loader-window";
    m_render_state.loader->draw_debug_window();
  }

  m_profiler.finish();
  //  if (m_profiler.root_time() > 0.018) {
  //    fmt::print("Slow frame: {:.2f} ms\n", m_profiler.root_time() * 1000);
  //    fmt::print("{}\n", m_profiler.to_string());
  //  }

  if (settings.draw_profiler_window) {
    g_current_renderer = "profiler-window";
    m_profiler.draw();
  }

  if (settings.draw_small_profiler_window) {
    g_current_renderer = "small-profiler-window";
    SmallProfilerStats stats;
    stats.draw_calls = m_profiler.root()->stats().draw_calls;
    stats.triangles = m_profiler.root()->stats().triangles;
    for (int i = 0; i < (int)BucketCategory::MAX_CATEGORIES; i++) {
      stats.time_per_category[i] = m_category_times[i];
    }
    m_small_profiler.draw(m_render_state.load_status_debug, stats);
  }

  if (settings.draw_subtitle_editor_window) {
    g_current_renderer = "subtitle-editor-window";
    if (m_subtitle_editor == nullptr) {
      m_subtitle_editor = new SubtitleEditor();
    }
    m_subtitle_editor->draw_window();
  }

  if (settings.draw_filters_window) {
    g_current_renderer = "filters-window";
    m_filters_menu.draw_window();
  }

  if (settings.gpu_sync) {
    g_current_renderer = "gpu-sync";
    glFinish();
  }

  g_current_renderer = "end";
}

/*!
 * Draw the per-renderer debug window
 */
void OpenGLRenderer::draw_renderer_selection_window() {
  ImGui::Begin("Renderer Debug");

  ImGui::Checkbox("Use old single-draw", &m_render_state.no_multidraw);
  ImGui::SliderFloat("Fog Adjust", &m_render_state.fog_intensity, 0, 10);
  ImGui::Checkbox("Sky CPU", &m_render_state.use_sky_cpu);
  ImGui::Checkbox("Occlusion Cull", &m_render_state.use_occlusion_culling);
  ImGui::Checkbox("Blackout Loads", &m_enable_fast_blackout_loads);
  if (m_version == GameVersion::Jak1 && ImGui::TreeNode("Stereo")) {
    auto& gs = Gfx::g_global_settings;
    ImGui::Checkbox("Enable (side by side)", &gs.stereo_enable);
    ImGui::SliderFloat("IPD (m)", &gs.stereo_ipd_m, 0.f, 0.1f);
    ImGui::SliderFloat("World scale", &gs.stereo_world_scale, 0.01f, 1.f, "%.3f",
                       ImGuiSliderFlags_Logarithmic);
    ImGui::Checkbox("Swap eyes (cross-eyed)", &gs.stereo_swap_eyes);
    ImGui::Checkbox("OpenXR", &gs.xr_enable);
    ImGui::SliderFloat("Head translation", &gs.xr_pos_scale, 0.f, 1.f);
    if (ImGui::Button("Recentre (F6)")) {
      gs.xr_recentre_requests++;
    }
    ImGui::TextUnformatted(xr::status_line().c_str());
    ImGui::TreePop();
  }
  if (m_version == GameVersion::Jak1 && ImGui::TreeNode("SSAO")) {
    m_ssao.draw_debug_window();
    ImGui::TreePop();
  }
  if (m_version == GameVersion::Jak1 && ImGui::TreeNode("Inspect")) {
    m_inspect.draw_debug_window();
    ImGui::TreePop();
  }
  if (m_version == GameVersion::Jak1 && ImGui::TreeNode("Bloom")) {
    m_bloom.draw_debug_window();
    ImGui::TreePop();
  }
  if (m_version == GameVersion::Jak1 && ImGui::TreeNode("Emissive lights")) {
    m_emissive_lights.draw_debug_window();
    ImGui::TreePop();
  }
  if (ImGui::TreeNode("Level heightfield")) {
    heightfield_draw_debug_window();
    ImGui::TreePop();
  }
  if (m_actor_shadows && ImGui::TreeNode("Actor shadows")) {
    m_actor_shadows->draw_debug_window();
    ImGui::TreePop();
  }
  if (m_version == GameVersion::Jak1 && ImGui::TreeNode("Sun")) {
    unbake_draw_debug_window();
    ImGui::TreePop();
  }
  if (m_version == GameVersion::Jak1 && ImGui::TreeNode("Materials")) {
    pbr_draw_debug_window();
    ImGui::TreePop();
  }
  if (m_version == GameVersion::Jak1 && ImGui::TreeNode("SSAO Depth")) {
    m_ssao_depth.draw_debug_window();
    ImGui::TreePop();
  }
  if (m_vista && ImGui::TreeNode("Vista")) {
    m_vista->draw_debug_window();
    ImGui::TreePop();
  }

  if (m_texture_animator && ImGui::TreeNode("Texture Animator")) {
    m_texture_animator->draw_debug_window();
    ImGui::TreePop();
  }

  ImGui::InputText("Renderer Filter", &m_renderer_filter);

  for (size_t i = 0; i < m_bucket_renderers.size(); i++) {
    auto renderer = m_bucket_renderers[i].get();
    if (!m_renderer_filter.empty() && !str_util::contains(renderer->name(), m_renderer_filter)) {
      continue;
    }
    if (renderer && !renderer->empty()) {
      ImGui::PushID(i);
      if (ImGui::TreeNode(renderer->name_and_id().c_str())) {
        ImGui::Checkbox("Enable", &renderer->enabled());
        renderer->draw_debug_window();
        ImGui::TreePop();
      }
      ImGui::PopID();
    }
  }
  if (ImGui::TreeNode("Texture Pool")) {
    m_render_state.texture_pool->draw_debug_window();
    ImGui::TreePop();
  }
  if (m_jak2_eye_renderer) {
    if (ImGui::TreeNode("Eyes")) {
      m_jak2_eye_renderer->draw_debug_window();
      ImGui::TreePop();
    }
  }
  if (m_jak3_eye_renderer) {
    if (ImGui::TreeNode("Eyes")) {
      m_jak3_eye_renderer->draw_debug_window();
      ImGui::TreePop();
    }
  }
  ImGui::End();
}

/*!
 * Pre-render frame setup.
 */
void OpenGLRenderer::setup_frame(const RenderOptions& settings) {
  // SDL controls the window framebuffer, so we just update the size:
  auto& window_fb = m_fbo_state.resources.window;

  bool window_resized = window_fb.width != settings.window_framebuffer_width ||
                        window_fb.height != settings.window_framebuffer_height;
  window_fb.valid = true;
  window_fb.is_window = true;
  window_fb.fbo_id = 0;
  window_fb.width = settings.window_framebuffer_width;
  window_fb.height = settings.window_framebuffer_height;
  window_fb.multisample_count = 1;
  window_fb.multisampled = false;

  const bool hdr =
      m_version == GameVersion::Jak1 &&
      (Gfx::g_global_settings.hdr_target || (Gfx::g_hdr_present.active && Gfx::g_hdr_present.hdr));
  // see if the render FBO is still applicable
  bool rebuilt = false;
  if (settings.save_screenshot || window_resized || !m_fbo_state.render_fbo ||
      hdr != m_fbo_state.hdr ||
      !m_fbo_state.render_fbo->matches(settings.game_res_w, settings.game_res_h,
                                       settings.msaa_samples)) {
    rebuilt = true;
    // doesn't match, set up a new one for these settings
    lg::info("FBO Setup: requested {}x{}, msaa {}", settings.game_res_w, settings.game_res_h,
             settings.msaa_samples);

    // clear old framebuffers
    m_fbo_state.resources.render_buffer.clear();
    m_fbo_state.resources.resolve_buffer.clear();

    // NOTE: we will ALWAYS render the game to a separate framebuffer instead of directly to the
    // window framebuffer.

    // create a fbo to render to, with the desired settings
    m_fbo_state.resources.render_buffer =
        make_fbo(settings.game_res_w, settings.game_res_h, settings.msaa_samples, true, hdr);
    m_fbo_state.hdr = hdr;
    lg::info("FBO Setup: colour {}", hdr ? "RGBA16F" : "RGBA8");
    m_fbo_state.render_fbo = &m_fbo_state.resources.render_buffer;

    if (settings.msaa_samples != 1) {
      lg::info("FBO Setup: using second temporary buffer: res: {}x{}", settings.game_res_w,
               settings.game_res_h);

      // we'll need a temporary fbo to do the msaa resolve step
      // non-multisampled, and doesn't need z/stencil
      m_fbo_state.resources.resolve_buffer =
          make_fbo(settings.game_res_w, settings.game_res_h, 1, false, hdr);
    } else {
      lg::info("FBO Setup: not using second temporary buffer");
    }
  }

  ASSERT_MSG(settings.game_res_w > 0 && settings.game_res_h > 0,
             fmt::format("Bad viewport size from game_res: {}x{}\n", settings.game_res_w,
                         settings.game_res_h));

  ASSERT_MSG(!m_fbo_state.render_fbo->is_window, "window fbo");

  if (m_version == GameVersion::Jak1) {
    glBindFramebuffer(GL_FRAMEBUFFER, Gfx::window_fbo());
    glViewport(0, 0, m_fbo_state.resources.window.width, m_fbo_state.resources.window.height);
    glClearColor(0.0, 0.0, 0.0, 0.0);
    glClearDepth(0.0);
    glDepthMask(GL_TRUE);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glDisable(GL_BLEND);

    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo_state.render_fbo->fbo_id);
    glClearColor(0.0, 0.0, 0.0, 0.0);
    glClearDepth(0.0);
    glClearStencil(0);
    glDepthMask(GL_TRUE);
    // Note: could rely on sky renderer to clear depth and color, but this causes problems with
    // letterboxing
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glDisable(GL_BLEND);
    m_render_state.stencil_dirty = false;
  }
  // jak 2 does the clear in BlitDisplays.cpp

  // setup the draw region to letterbox later
  m_render_state.draw_region_w = settings.draw_region_width;
  m_render_state.draw_region_h = settings.draw_region_height;

  // center the letterbox
  m_render_state.draw_offset_x =
      (settings.window_framebuffer_width - m_render_state.draw_region_w) / 2;
  m_render_state.draw_offset_y =
      (settings.window_framebuffer_height - m_render_state.draw_region_h) / 2;

  m_render_state.render_fb = m_fbo_state.render_fbo->fbo_id;
  m_render_state.render_fb_depth_tex =
      m_fbo_state.render_fbo->zbuf_is_texture ? *m_fbo_state.render_fbo->zbuf_stencil_id : 0;
  m_render_state.hdr_target = m_fbo_state.hdr;
  m_render_state.render_scale =
      m_display_h > 0 ? (float)settings.game_res_h / (float)m_display_h : 1.f;

  if (m_render_state.draw_region_w <= 0 || m_render_state.draw_region_h <= 0) {
    // trying to draw to 0 size region... opengl doesn't like this.
    m_render_state.draw_region_w = 320;
    m_render_state.draw_region_h = 240;
  }

  m_render_state.render_fb_x = 0;
  m_render_state.render_fb_y = 0;
  m_render_state.render_fb_w = settings.game_res_w;
  m_render_state.render_fb_h = settings.game_res_h;
  glViewport(0, 0, settings.game_res_w, settings.game_res_h);

  const std::array<s64, 14> key = {settings.window_framebuffer_width,
                                   settings.window_framebuffer_height,
                                   settings.game_res_w,
                                   settings.game_res_h,
                                   settings.msaa_samples,
                                   hdr,
                                   m_render_state.render_fb,
                                   m_render_state.render_fb_depth_tex,
                                   m_render_state.stereo_active,
                                   settings.xr != nullptr,
                                   m_render_state.draw_region_w,
                                   m_render_state.draw_region_h,
                                   m_render_state.draw_offset_x,
                                   m_render_state.draw_offset_y};
  static_assert(std::is_trivially_copyable_v<GfxGlobalSettings>);
  const auto* gs = reinterpret_cast<const u8*>(&Gfx::g_global_settings);
  const bool gs_changed = std::memcmp(gs, m_glq_settings.data(), m_glq_settings.size()) != 0;
  if (rebuilt || key != m_glq_key || gs_changed) {
    if (gs_changed && !rebuilt && key == m_glq_key && m_glq_settings_logs < 16) {
      size_t at = 0;
      while (at < m_glq_settings.size() && gs[at] == m_glq_settings[at]) {
        at++;
      }
      m_glq_settings_logs++;
      lg::info("glq: settings changed at byte {}, cached GL state dropped", at);
    }
    glq::invalidate_state();
    m_glq_key = key;
    std::memcpy(m_glq_settings.data(), gs, m_glq_settings.size());
  }
}

void OpenGLRenderer::dispatch_buckets_jak1(DmaFollower dma,
                                           ScopedProfilerNode& prof,
                                           bool sync_after_buckets) {
  // The first thing the DMA chain should be a call to a common default-registers chain.
  // this chain resets the state of the GS. After this is buckets
  m_category_times.fill(0);

  m_render_state.buckets_base =
      dma.current_tag_offset() + 16;  // offset by 1 qw for the initial call
  m_render_state.next_bucket = m_render_state.buckets_base;
  m_render_state.bucket_for_vis_copy = (int)jak1::BucketId::TFRAG_LEVEL0;
  m_render_state.num_vis_to_copy = jak1::LEVEL_MAX;

  // Find the default regs buffer
  auto initial_call_tag = dma.current_tag();
  ASSERT(initial_call_tag.kind == DmaTag::Kind::CALL);
  auto initial_call_default_regs = dma.read_and_advance();
  ASSERT(initial_call_default_regs.transferred_tag == 0);  // should be a nop.
  m_render_state.default_regs_buffer = dma.current_tag_offset();
  auto default_regs_tag = dma.current_tag();
  ASSERT(default_regs_tag.kind == DmaTag::Kind::CNT);
  ASSERT(default_regs_tag.qwc == 10);
  // TODO verify data in here.
  auto default_data = dma.read_and_advance();
  ASSERT(default_data.size_bytes > 148);
  memcpy(m_render_state.fog_color.data(), default_data.data + 144, 4);
  auto default_ret_tag = dma.current_tag();
  ASSERT(default_ret_tag.qwc == 0);
  ASSERT(default_ret_tag.kind == DmaTag::Kind::RET);
  dma.read_and_advance();

  // now we should point to the first bucket!
  ASSERT(dma.current_tag_offset() == m_render_state.next_bucket);
  m_render_state.next_bucket += 16;

  const auto& gs = Gfx::g_global_settings;
  const bool ssao_capture =
      gs.ssao_depth_capture || gs.ssao_depth_debug || gs.ssao_msaa_probe_request;
  const int ssao_bucket = ssao_capture ? SsaoDepth::capture_bucket_jak1() : -1;
  const bool gpu_timing = gs.ssao_gpu_timing;
  const bool vista = gs.vista_enable && m_vista;
  const bool ssao = gs.ssao_enable;
  m_render_state.ssao_stencil = ssao;
  if (ssao) {
    perf::Scope ps(perf::Group::SSAO_TARGETS);
    m_ssao.begin_normals(&m_render_state);
  } else if (m_ssao.normals_attached()) {
    m_ssao.end_normals(&m_render_state);
  }
  const bool bloom = gs.bloom_enable && m_render_state.version == GameVersion::Jak1;
  const bool smaa = Gfx::smaa_on() && !m_render_state.stereo_active;
  if (!smaa) {
    m_smaa.release_targets();
  }
  const bool em_lights =
      ssao && gs.emissive_lights_enable && m_render_state.version == GameVersion::Jak1;
  m_render_state.emissive_mask = m_render_state.version == GameVersion::Jak1 &&
                                 ((ssao && gs.ssao_emissive) || bloom || em_lights);
  m_render_state.light_material = em_lights && gs.light_highlights &&
                                  gs.light_highlights_strength > 0.f &&
                                  pbr_active(&m_render_state);
  {
    perf::Scope ps(perf::Group::SSAO_TARGETS);
    m_ssao.begin_emissive(&m_render_state, m_render_state.emissive_mask);
  }
  m_inspect.begin(&m_render_state, gs.inspect_enable && !m_render_state.stereo_active);
  if (!vista) {
    m_render_state.vista_drawn = false;
  }
  m_render_state.vista_frame_fog = vista ? gs.vista_fog_mode : VISTA_FOG_MAIN;
  if (gpu_timing) {
    m_ssao_depth.frame_timer().begin();
  }
  prime_camera_jak1(dma, &m_render_state);
  heightfield_prepare(&m_render_state);
  canopy_vol_prepare(&m_render_state);
  if (m_actor_shadows) {
    auto p = prof.make_scoped_child("actor-shadows");
    perf::Scope ps(perf::Group::ACTOR_SHADOWS);
    m_actor_shadows->prepass(dma, &m_render_state, *m_merc2, *m_generic2, p);
  }
  const bool actor_shadows = m_render_state.shadow.active;
  const bool water = water_active(&m_render_state);
  m_render_state.water_depth_tex = 0;
  inland_water::set_depth(0, nullptr);

  // loop over the buckets!
  for (size_t bucket_id = 0; bucket_id < m_bucket_renderers.size(); bucket_id++) {
    auto& renderer = m_bucket_renderers[bucket_id];
    auto bucket_prof = prof.make_scoped_child(renderer->name_and_id());
    g_current_renderer = renderer->name_and_id();
    if (ssao) {
      SsaoRenderer::set_bucket_stencil((int)bucket_id);
    }
    // lg::info("Render: {} start", g_current_renderer);
    if (actor_shadows && bucket_id == (int)jak1::BucketId::SHADOW) {
      // the shadow maps and the landing disc replace the stencil shadow
      while (dma.current_tag_offset() != m_render_state.next_bucket) {
        dma.read_and_advance();
      }
    } else {
      perf::Scope ps(bucket_id < m_perf_groups.size() ? m_perf_groups[bucket_id]
                                                       : perf::Group::NONE);
      renderer->render(dma, &m_render_state, bucket_prof);
    }
    if (ssao) {
      SsaoRenderer::set_bucket_normals(&m_render_state, (int)bucket_id + 1);
    }
    m_inspect.set_bucket(&m_render_state, (int)bucket_id + 1);
    if (sync_after_buckets) {
      auto pp = scoped_prof("finish");
      glFinish();
    }

    // lg::info("Render: {} end", g_current_renderer);
    //  should have ended at the start of the next chain
    ASSERT(dma.current_tag_offset() == m_render_state.next_bucket);
    m_render_state.next_bucket += 16;
    if (!m_render_state.stereo_repeat_pass) {
      vif_interrupt_callback(bucket_id);
    }
    m_category_times[(int)m_bucket_categories[bucket_id]] += bucket_prof.get_elapsed_time();

    // hack to draw the collision mesh in the middle the drawing
    if (bucket_id == 31 - 1 && Gfx::g_global_settings.collision_enable) {
      auto p = prof.make_scoped_child("collision-draw");
      m_collide_renderer.render(&m_render_state, p);
    }

    if (vista && bucket_id == 31 - 1) {
      auto p = prof.make_scoped_child("vista");
      perf::Scope ps(perf::Group::VISTA);
      auto cam = [&](jak1::BucketId id) {
        auto* tf = dynamic_cast<TFragment*>(m_bucket_renderers[(int)id].get());
        return tf ? tf->camera_this_frame(m_render_state.frame_idx) : nullptr;
      };
      m_vista->render(&m_render_state, p, cam(jak1::BucketId::TFRAG_LEVEL0),
                      cam(jak1::BucketId::TFRAG_LEVEL1));
    }

    // the sun: after the opaque background (and vista), before the transparent buckets
    if (gs.sun_disc && bucket_id == 31 - 1) {
      // a child of the live bucket node: a new child of `prof` can move bucket_prof's node
      auto p = bucket_prof.make_scoped_child("sun-disc");
      const TfragPcPortData* ports[2];
      for (int i = 0; i < 2; i++) {
        auto* tf = dynamic_cast<TFragment*>(
            m_bucket_renderers[(int)(i ? jak1::BucketId::TFRAG_LEVEL1 : jak1::BucketId::TFRAG_LEVEL0)]
                .get());
        ports[i] = tf ? tf->camera_this_frame(m_render_state.frame_idx) : nullptr;
      }
      m_sun_disc.render(&m_render_state, p, ports);
    }

    if (vista && (int)bucket_id == (int)jak1::BucketId::TFRAG_ICE_LEVEL1) {
      auto p = prof.make_scoped_child("vista-alpha");
      perf::Scope ps(perf::Group::VISTA_ALPHA);
      m_vista->render_alpha(&m_render_state, p);
    }

    // water shading: the mid ocean draws after the opaque tfrag and tie of both levels, so it
    // can read the depth under its surface
    auto* ocean = bucket_id == (int)jak1::BucketId::GENERIC_TFRAG_TEX_LEVEL1
                      ? dynamic_cast<OceanMidAndFar*>(
                            m_bucket_renderers[(int)jak1::BucketId::OCEAN_MID_AND_FAR].get())
                      : nullptr;
    // also when the toggle flipped on after the frame started and the mid was kept back; no
    // copy in a frame without ocean (perf bit 13)
    const bool ocean_drawn = !Gfx::perf_opt(PERF_OPT_WATER + 1) ||
                             (ocean && ocean->drawn_in_frame(m_render_state.frame_idx));
    if (ocean && ((water && ocean_drawn) || ocean->mid_pending())) {
      auto p = prof.make_scoped_child("water-depth");
      {
        perf::Scope ps(perf::Group::WATER_DEPTH);
        m_water_depth.capture(&m_render_state, p, true);
      }
      m_render_state.water_depth_tex = m_water_depth.depth_tex();
      const int ocean_bucket = (int)jak1::BucketId::OCEAN_MID_AND_FAR;
      if (ssao) {
        SsaoRenderer::set_bucket_stencil(ocean_bucket);
        SsaoRenderer::set_bucket_normals(&m_render_state, ocean_bucket);
      }
      m_inspect.set_bucket(&m_render_state, ocean_bucket);
      auto pm = prof.make_scoped_child("ocean-mid-deferred");
      perf::Scope ps(perf::Group::OCEAN_MID);
      ocean->flush_mid_deferred(&m_render_state, pm);
      if (ssao) {
        SsaoRenderer::set_bucket_stencil((int)bucket_id);
        SsaoRenderer::set_bucket_normals(&m_render_state, (int)bucket_id + 1);
      }
      m_inspect.set_bucket(&m_render_state, (int)bucket_id + 1);
    }

    if ((int)bucket_id == ssao_bucket) {
      auto p = prof.make_scoped_child("ssao-depth-capture");
      perf::Scope ps(perf::Group::SSAO_DEPTH);
      m_ssao_depth.capture(&m_render_state, p);
    }

    if (ssao && bucket_id == (int)jak1::BucketId::GENERIC_PRIS) {
      auto p = prof.make_scoped_child("ssao");
      {
        perf::Scope ps(perf::Group::SSAO);
        m_ssao.render(&m_render_state, p);
      }
      if (m_ssao.have_frame()) {
        inland_water::set_depth(m_ssao.csz_texture(), m_ssao.proj_info());
      }
      if (swamp_mist_wanted(&m_render_state)) {
        auto pm = prof.make_scoped_child("swamp-mist");
        perf::Scope ps(perf::Group::SWAMP_MIST);
        m_swamp_mist.render(&m_render_state, pm, m_ssao);
      }
    }
    // a sea mist (Misty Island) draws over the near ocean
    if (ssao && bucket_id == (int)jak1::BucketId::OCEAN_NEAR &&
        swamp_mist_wanted(&m_render_state, true)) {
      auto pm = prof.make_scoped_child("swamp-mist");
      perf::Scope ps(perf::Group::SWAMP_MIST);
      m_swamp_mist.render(&m_render_state, pm, m_ssao, true);
    }

    if (!em_lights && g_burst_frame >= 0 && bucket_id == (int)jak1::BucketId::SPRITE) {
      lg::info("EMLIGHTS f={} off ssao={} lights={}", g_burst_frame, gs.ssao_enable,
               gs.emissive_lights_enable);
    }
    if (em_lights && bucket_id == (int)jak1::BucketId::SPRITE) {
      auto p = prof.make_scoped_child("emissive-lights");
      perf::Scope ps(perf::Group::EMISSIVE_LIGHTS);
      m_emissive_lights.render(&m_render_state, p, m_ssao, m_ssao.emissive_texture(),
                               m_ssao.emissive_samples());
    }
    if (bloom && bucket_id == (int)jak1::BucketId::SPRITE) {
      auto p = prof.make_scoped_child("bloom");
      perf::Scope ps(perf::Group::BLOOM);
      m_bloom.render(&m_render_state, p, m_ssao.emissive_texture(), m_ssao.emissive_samples());
    }
    // the sun's starburst and wash: a camera effect over the scene and its bloom, under the HUD
    if (gs.sun_disc && bucket_id == (int)jak1::BucketId::SPRITE) {
      auto p = prof.make_scoped_child("sky-flare");
      m_sun_disc.render_flare(&m_render_state, p);
    }
    // the scene is complete; the debug, HUD text and subtitle buckets follow
    if (smaa && bucket_id == (int)jak1::BucketId::SPRITE) {
      auto p = prof.make_scoped_child("smaa");
      perf::Scope ps(perf::Group::SMAA);
      m_smaa.render(&m_render_state, p);
    }
    // render scale: the text buckets that follow draw at display size
    if (m_display_w > 0 && !m_render_state.stereo_active &&
        bucket_id == (int)jak1::BucketId::SPRITE) {
      auto p = prof.make_scoped_child("upscale");
      perf::Scope ps(perf::Group::UPSCALE);
      m_upscaler.run(&m_render_state, p, m_display_w, m_display_h);
      m_upscaled = true;
    }
  }

  m_inspect.end(&m_render_state);

  if (ssao) {
    SsaoRenderer::reset_stencil();
    auto p = prof.make_scoped_child("ssao-view");
    perf::Scope ps(perf::Group::SSAO_VIEW);
    m_ssao.draw_view(&m_render_state, p);
  }

  if (ssao_capture && gs.ssao_depth_debug) {
    auto p = prof.make_scoped_child("ssao-depth-debug");
    m_ssao_depth.draw_debug(&m_render_state, p);
  }
  if (gpu_timing) {
    m_ssao_depth.frame_timer().end();
  }

  // TODO ending data.
}

void OpenGLRenderer::dispatch_buckets_jak2(DmaFollower dma,
                                           ScopedProfilerNode& prof,
                                           bool sync_after_buckets) {
  // The first thing the DMA chain should be a call to a common default-registers chain.
  // this chain resets the state of the GS. After this is buckets
  m_category_times.fill(0);

  m_render_state.buckets_base = dma.current_tag_offset();  // starts at 0 in jak 2
  m_render_state.next_bucket = m_render_state.buckets_base + 16;
  m_render_state.bucket_for_vis_copy = (int)jak2::BucketId::BUCKET_2;
  m_render_state.num_vis_to_copy = jak2::LEVEL_MAX;

  for (size_t bucket_id = 0; bucket_id < m_bucket_renderers.size(); bucket_id++) {
    auto& renderer = m_bucket_renderers[bucket_id];
    auto bucket_prof = prof.make_scoped_child(renderer->name_and_id());
    g_current_renderer = renderer->name_and_id();
    // lg::info("Render: {} start", g_current_renderer);
    renderer->render(dma, &m_render_state, bucket_prof);
    if (sync_after_buckets) {
      auto pp = scoped_prof("finish");
      glFinish();
    }

    // lg::info("Render: {} end", g_current_renderer);
    //  should have ended at the start of the next chain
    ASSERT(dma.current_tag_offset() == m_render_state.next_bucket);
    m_render_state.next_bucket += 16;
    vif_interrupt_callback(bucket_id + 1);
    m_category_times[(int)m_bucket_categories[bucket_id]] += bucket_prof.get_elapsed_time();

    // hack to draw the collision mesh in the middle the drawing
    if (bucket_id + 1 == (int)jak2::BucketId::TEX_L0_ALPHA &&
        Gfx::g_global_settings.collision_enable) {
      auto p = prof.make_scoped_child("collision-draw");
      m_collide_renderer.render(&m_render_state, p);
    }
  }
  vif_interrupt_callback(m_bucket_renderers.size());

  // TODO ending data.
}

void OpenGLRenderer::dispatch_buckets_jak3(DmaFollower dma,
                                           ScopedProfilerNode& prof,
                                           bool sync_after_buckets) {
  // The first thing the DMA chain should be a call to a common default-registers chain.
  // this chain resets the state of the GS. After this is buckets
  m_category_times.fill(0);

  m_render_state.buckets_base = dma.current_tag_offset();  // starts at 0 in jak 2
  m_render_state.next_bucket = m_render_state.buckets_base + 16;
  m_render_state.bucket_for_vis_copy = (int)jak3::BucketId::BUCKET_2;
  m_render_state.num_vis_to_copy = jak3::LEVEL_MAX;

  for (size_t bucket_id = 0; bucket_id < m_bucket_renderers.size(); bucket_id++) {
    auto& renderer = m_bucket_renderers[bucket_id];
    auto bucket_prof = prof.make_scoped_child(renderer->name_and_id());
    g_current_renderer = renderer->name_and_id();
    // lg::info("Render: {} start", g_current_renderer);
    renderer->render(dma, &m_render_state, bucket_prof);
    if (sync_after_buckets) {
      auto pp = scoped_prof("finish");
      glFinish();
    }

    // lg::info("Render: {} end", g_current_renderer);
    //  should have ended at the start of the next chain
    ASSERT(dma.current_tag_offset() == m_render_state.next_bucket);
    m_render_state.next_bucket += 16;
    vif_interrupt_callback(bucket_id + 1);
    m_category_times[(int)m_bucket_categories[bucket_id]] += bucket_prof.get_elapsed_time();

    // hack to draw the collision mesh in the middle the drawing
    if (bucket_id + 1 == (int)jak3::BucketId::TEX_L0_ALPHA &&
        Gfx::g_global_settings.collision_enable) {
      auto p = prof.make_scoped_child("collision-draw");
      m_collide_renderer.render(&m_render_state, p);
    }

    if (bucket_id == (int)jak3::BucketId::TEX_HUD_HUD_ALPHA) {
      auto p = prof.make_scoped_child("color-filter");
      m_blit_displays->apply_color_filter(&m_render_state, p);
    }
  }
  vif_interrupt_callback(m_bucket_renderers.size());

  // TODO ending data.
}

/*!
 * Stereo debug path (Jak 1): replays the frame's DMA chain once per eye with the camera moved by
 * +/- IPD/2 along its x axis, and copies each eye into one half of a double-width buffer. The
 * second pass skips VIF interrupts and per-frame simulation so the game sees a single frame.
 */
void OpenGLRenderer::dispatch_buckets_stereo(DmaFollower dma,
                                             ScopedProfilerNode& prof,
                                             bool sync_after_buckets,
                                             xr::FrameTargets* xr) {
  const auto& gs = Gfx::g_global_settings;
  const float units_per_m = 4096.f / std::max(gs.stereo_world_scale, 0.001f);
  const float half_ipd = 0.5f * gs.stereo_ipd_m * units_per_m;
  // camera-space +x is screen left here (measured), so the left eye sits at +x
  float eye_x[2] = {half_ipd, -half_ipd};
  if (xr) {
    // OpenXR +x is right: negate the runtime's eye positions
    eye_x[0] = -xr->eye[0].head_x_m * units_per_m;
    eye_x[1] = -xr->eye[1].head_x_m * units_per_m;
  }
  // head tracking: the game's frustum edges (tangents) against the runtime's, per screen axis
  bool tracked = false;
  float fov_x = 1.f, fov_y = 1.f;
  // also without tracking: the eyes then only take the IPD offset, but keep the runtime's fov
  if (xr && m_render_state.game_camera.valid) {
    const auto& gc = m_render_state.game_camera;
    const float fog = gc.fog.x();
    const auto& p = gc.persp;
    if (fog != 0.f && p[0][0] != 0.f && p[1][1] != 0.f && xr->render_tan_x > 0.f &&
        xr->render_tan_y > 0.f) {
      // the renderers map GS x +-256 and y +-112 around the centre to the full target
      const float game_tan_x = std::abs(256.f * p[2][3] / (fog * p[0][0]));
      const float game_tan_y = std::abs(112.f * p[2][3] / (fog * p[1][1]));
      fov_x = game_tan_x / xr->render_tan_x;
      fov_y = game_tan_y / xr->render_tan_y;
      tracked = true;
      if (std::abs(game_tan_x - m_xr_logged_tan[0]) > 1e-3f ||
          std::abs(game_tan_y - m_xr_logged_tan[1]) > 1e-3f) {
        m_xr_logged_tan[0] = game_tan_x;
        m_xr_logged_tan[1] = game_tan_y;
        lg::info("[xr] game frustum half-tangents x {:.3f} y {:.3f} -> eye scale x {:.3f} y {:.3f}",
                 game_tan_x, game_tan_y, fov_x, fov_y);
      }
    }
  }
  const Fbo* src = m_fbo_state.render_fbo;
  const int w = src->width;
  const int h = src->height;
  if (!m_stereo_fbo.matches(2 * w, h, 1) || m_stereo_hdr != m_fbo_state.hdr) {
    m_stereo_fbo.clear();
    m_stereo_fbo = make_fbo(2 * w, h, 1, false, m_fbo_state.hdr);
    m_stereo_hdr = m_fbo_state.hdr;
    glq::invalidate_state();
    glBindFramebuffer(GL_FRAMEBUFFER, src->fbo_id);
  }

  for (int eye = 0; eye < 2; eye++) {
    m_render_state.stereo_eye_x = eye_x[eye];
    m_render_state.eye.active = false;
    if (tracked) {
      const auto& t = xr->eye[eye];
      auto& ev = m_render_state.eye;
      ev.active = true;
      std::memcpy(ev.rot, t.rot, sizeof(ev.rot));
      for (int k = 0; k < 3; k++) {
        ev.t[k] = t.pos_m[k] * units_per_m;
      }
      ev.fov_x = fov_x;
      ev.fov_y = fov_y;
      m_render_state.stereo_eye_x = 0.f;
    }
    // a re-drawn chain repeats no per-frame work in either eye
    m_render_state.stereo_repeat_pass = eye == 1 || m_xr_repeat;
    if (eye == 1) {
      m_render_state.reset();
      glBindFramebuffer(GL_FRAMEBUFFER, src->fbo_id);
      glViewport(0, 0, w, h);
      glDisable(GL_SCISSOR_TEST);
      glClearColor(0.0, 0.0, 0.0, 0.0);
      glClearDepth(0.0);
      glClearStencil(0);
      glDepthMask(GL_TRUE);
      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
      glDisable(GL_BLEND);
      m_render_state.stencil_dirty = false;
    }
    auto p = prof.make_scoped_child(eye == 0 ? "eye-left" : "eye-right");
    dispatch_buckets(dma, p, sync_after_buckets);

    const int dst_x = ((eye == 1) != gs.stereo_swap_eyes) ? w : 0;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, src->fbo_id);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_stereo_fbo.fbo_id);
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0, 0, w, h, dst_x, 0, dst_x + w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    if (xr) {
      if (tracked) {
        copy_eye_to_xr_cropped(*xr, eye, dst_x, w, h);
      } else {
        copy_eye_to_xr(xr->eye[eye], dst_x, w, h);
      }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, src->fbo_id);
  }
  m_render_state.stereo_eye_x = 0.f;
  m_render_state.eye.active = false;
  m_render_state.stereo_repeat_pass = false;
  if (xr) {
    xr->rendered = true;
  }
}

/*!
 * Copies one eye from the side-by-side buffer into its OpenXR swapchain image. The game draws a
 * centred frustum, so the image is shifted to put its centre on the runtime's forward direction
 * for that eye (the runtime's field of view is usually asymmetric). The bytes are copied as they
 * are: with an sRGB swapchain format the display-encoded output is read back unchanged.
 */
void OpenGLRenderer::copy_eye_to_xr(const xr::EyeTarget& t, int src_x, int w, int h) {
  if (!m_xr_fbo) {
    glGenFramebuffers(1, &m_xr_fbo);
  }
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_xr_fbo);
  glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.gl_texture, 0);
  glDisable(GL_FRAMEBUFFER_SRGB);
  glDisable(GL_SCISSOR_TEST);
  // NDC position of the straight-ahead direction in the runtime's frustum
  const float fx = -(t.tan_right + t.tan_left) / (t.tan_right - t.tan_left);
  const float fy = -(t.tan_up + t.tan_down) / (t.tan_up - t.tan_down);
  const int dx = (int)std::lround(0.5f * fx * t.width);
  const int dy = (int)std::lround(0.5f * fy * t.height);
  if (dx != 0 || dy != 0) {
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
  }
  glBindFramebuffer(GL_READ_FRAMEBUFFER, m_stereo_fbo.fbo_id);
  const bool same_size = w == t.width && h == t.height;
  glBlitFramebuffer(src_x, 0, src_x + w, h, dx, dy, dx + t.width, dy + t.height,
                    GL_COLOR_BUFFER_BIT, same_size ? GL_NEAREST : GL_LINEAR);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_xr_fbo);
  glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
}

/*!
 * Head-tracked copy: the frame was drawn with a symmetric frustum of half-tangents
 * render_tan_x/y; the eye's own (asymmetric) frustum is cut out of it and scaled to its image.
 */
void OpenGLRenderer::copy_eye_to_xr_cropped(const xr::FrameTargets& frame,
                                            int eye,
                                            int src_x,
                                            int w,
                                            int h) {
  const auto& t = frame.eye[eye];
  if (!m_xr_fbo) {
    glGenFramebuffers(1, &m_xr_fbo);
  }
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_xr_fbo);
  glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.gl_texture, 0);
  glDisable(GL_FRAMEBUFFER_SRGB);
  glDisable(GL_SCISSOR_TEST);
  const float tx = frame.render_tan_x, ty = frame.render_tan_y;
  auto px = [&](float tan) { return (int)std::lround((tan + tx) / (2.f * tx) * w); };
  auto py = [&](float tan) { return (int)std::lround((tan + ty) / (2.f * ty) * h); };
  const int x0 = src_x + px(t.tan_left), x1 = src_x + px(t.tan_right);
  const int y0 = py(t.tan_down), y1 = py(t.tan_up);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, m_stereo_fbo.fbo_id);
  const bool same_size = x1 - x0 == t.width && y1 - y0 == t.height;
  glBlitFramebuffer(x0, y0, x1, y1, 0, 0, t.width, t.height, GL_COLOR_BUFFER_BIT,
                    same_size ? GL_NEAREST : GL_LINEAR);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_xr_fbo);
  glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
}

/*!
 * This function finds buckets and dispatches them to the appropriate part.
 */
void OpenGLRenderer::dispatch_buckets(DmaFollower dma,
                                      ScopedProfilerNode& prof,
                                      bool sync_after_buckets) {
  g_current_renderer = "dispatch-buckets pre";

  m_render_state.version = m_version;
  if (!m_render_state.stereo_repeat_pass) {
    m_render_state.frame_idx++;
  }
  switch (m_version) {
    case GameVersion::Jak1:
      dispatch_buckets_jak1(dma, prof, sync_after_buckets);
      break;
    case GameVersion::Jak2:
      dispatch_buckets_jak2(dma, prof, sync_after_buckets);
      break;
    case GameVersion::Jak3:
    case GameVersion::JakX:
      dispatch_buckets_jak3(dma, prof, sync_after_buckets);
      break;
    default:
      ASSERT(false);
  }

  g_current_renderer = "dispatch-buckets post";
}

#ifdef _WIN32
void win_print_last_error(const std::string& msg) {
  LPSTR lpMsgBuf;

  FormatMessage(
      FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_IGNORE_INSERTS,
      NULL, GetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&lpMsgBuf, 0, NULL);

  lg::error("[OpenGLRenderer] {} Win Err: {}", msg, lpMsgBuf);
}

void copy_texture_to_clipboard(int width, int height, const std::vector<u32>& texture_data) {
  std::vector<u32> data(texture_data);

  // BGR -> RGB
  for (auto& px : data) {
    u8 r = px >> 0;
    u8 g = px >> 8;
    u8 b = px >> 16;
    u8 a = px >> 24;
    px = (a << 24) | (r << 16) | (g << 8) | (b << 0);
  }

  // Calculate the total size of the image data
  size_t image_size = data.size() * sizeof(u32);

  // BMP/DIB file header
  BITMAPINFOHEADER header;
  header.biSize = sizeof(header);
  header.biWidth = width;
  header.biHeight = height;
  header.biPlanes = 1;
  header.biBitCount = 32;
  header.biCompression = BI_RGB;
  header.biSizeImage = 0;
  header.biXPelsPerMeter = 0;
  header.biYPelsPerMeter = 0;
  header.biClrUsed = 0;
  header.biClrImportant = 0;

  // Open the clipboard
  if (!OpenClipboard(NULL)) {
    win_print_last_error("Failed to open the clipboard.");
    return;
  }

  // Empty the clipboard
  if (!EmptyClipboard()) {
    win_print_last_error("Failed to empty the clipboard.");
    CloseClipboard();
    return;
  }

  // Create a global memory object to hold the image data
  HGLOBAL hClipboardData = GlobalAlloc(GMEM_MOVEABLE, sizeof(header) + image_size);
  if (hClipboardData == NULL) {
    win_print_last_error("Failed to allocate memory for clipboard data.");
    CloseClipboard();
    return;
  }

  // Get a pointer to the global memory object
  void* pData = GlobalLock(hClipboardData);
  if (pData == NULL) {
    win_print_last_error("Failed to lock clipboard memory.");
    CloseClipboard();
    GlobalFree(hClipboardData);
    return;
  }

  // Copy the image data into the global memory object
  memcpy(pData, &header, sizeof(header));
  memcpy((char*)pData + sizeof(header), data.data(), image_size);

  // Unlock the global memory object
  if (!GlobalUnlock(hClipboardData) && GetLastError() != NO_ERROR) {
    win_print_last_error("Failed to unlock memory.");
    CloseClipboard();
    GlobalFree(hClipboardData);
    return;
  }

  // Set the image data to clipboard
  if (!SetClipboardData(CF_DIB, hClipboardData)) {
    win_print_last_error("Failed to set clipboard data.");
    CloseClipboard();
    GlobalFree(hClipboardData);
    return;
  }

  // Close the clipboard
  CloseClipboard();
  GlobalFree(hClipboardData);

  lg::info("Image data copied to clipboard successfully!");
}
#endif

/*!
 * Take a screenshot!
 */
void OpenGLRenderer::finish_screenshot(const std::string& output_name,
                                       int width,
                                       int height,
                                       int x,
                                       int y,
                                       GLuint fbo,
                                       int read_buffer,
                                       bool quick_screenshot) {
  Timer frame_timer;
  std::vector<u32> buffer(width * height);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  GLint oldbuf, oldreadbuf;
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &oldbuf);
  glGetIntegerv(GL_READ_BUFFER, &oldreadbuf);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
  glReadBuffer(read_buffer);
  glReadPixels(x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, buffer.data());

  // set alpha. our renderers mess this up in a way that isn't relevant to the final framebuffer.
  for (auto& px : buffer) {
    px |= 0xff000000;
  }

#ifdef _WIN32
  if (quick_screenshot) {
    // copy to clipboard (windows only)
    copy_texture_to_clipboard(width, height, buffer);
  }
#else
  (void)quick_screenshot;
  lg::error("screenshot to clipboard NYI non-Windows");
#endif

  // flip upside down in place
  for (int h = 0; h < height / 2; h++) {
    for (int w = 0; w < width; w++) {
      std::swap(buffer[h * width + w], buffer[(height - h - 1) * width + w]);
    }
  }

  if (bug_report::owns(output_name)) {
    auto gl_str = [](GLenum e) {
      auto s = glGetString(e);
      return s ? std::string((const char*)s) : std::string();
    };
    Timer report_timer;
    bug_report::deliver(std::move(buffer), width, height,
                        {gl_str(GL_VENDOR), gl_str(GL_RENDERER), gl_str(GL_VERSION)});
    lg::info("bug report frame: {:.1f} ms on the render thread, {:.1f} ms of it the report",
             frame_timer.getMs(), report_timer.getMs());
  } else {
    file_util::write_rgba_png(output_name, buffer.data(), width, height);
  }
  glReadBuffer(oldreadbuf);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, oldbuf);
}

void OpenGLRenderer::do_pcrtc_effects(float alp,
                                      int brightness_contrast_color,
                                      int brightness_contrast_alpha,
                                      SharedRenderState* render_state,
                                      ScopedProfilerNode& prof,
                                      bool direct_ok) {
  // direct: write the swapchain image here, skipping the stand-in and the separate encode
  const auto& win = m_fbo_state.resources.window;
  // SDR: the blackout fade blends in 8 bits, which a scale before the write would not match
  const bool hdr_out = Gfx::g_hdr_present.active && Gfx::g_hdr_present.hdr;
  const hdr::DirectTarget direct =
      hdr::begin_direct(win.width, win.height, direct_ok && (hdr_out || alp >= 1));
  Fbo* window_blit_src = nullptr;
  // perf-opt bit 19: at 1:1 the final pass averages the MSAA samples itself, no resolve
  const Fbo* ms_src = nullptr;
  if (!(render_state->stereo_active && m_stereo_fbo.valid) && !m_upscaled &&
      m_fbo_state.resources.resolve_buffer.valid && direct_ok && Gfx::perf_opt(PERF_OPT_POST + 3) &&
      m_fbo_state.render_fbo->multisampled &&
      m_fbo_state.render_fbo->width == render_state->draw_region_w &&
      m_fbo_state.render_fbo->height == render_state->draw_region_h) {
    ms_src = m_fbo_state.render_fbo;
  }
  if (render_state->stereo_active && m_stereo_fbo.valid) {
    window_blit_src = &m_stereo_fbo;
  } else if (m_upscaled) {
    window_blit_src = m_upscaler.display();  // the upscaled scene with the text buckets
  } else if (ms_src && !direct.check) {
    window_blit_src = &m_fbo_state.resources.resolve_buffer;  // not read
  } else if (m_fbo_state.resources.resolve_buffer.valid) {
    perf::Scope ps(perf::Group::PCRTC_RESOLVE);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_fbo_state.render_fbo->fbo_id);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_fbo_state.resources.resolve_buffer.fbo_id);
    glBlitFramebuffer(0,                                            // srcX0
                      0,                                            // srcY0
                      m_fbo_state.render_fbo->width,                // srcX1
                      m_fbo_state.render_fbo->height,               // srcY1
                      0,                                            // dstX0
                      0,                                            // dstY0
                      m_fbo_state.resources.resolve_buffer.width,   // dstX1
                      m_fbo_state.resources.resolve_buffer.height,  // dstY1
                      GL_COLOR_BUFFER_BIT,                          // mask
                      GL_LINEAR                                     // filter
    );
    window_blit_src = &m_fbo_state.resources.resolve_buffer;
  } else {
    window_blit_src = &m_fbo_state.resources.render_buffer;
  }

  perf::Scope post_scope(perf::Group::PCRTC_POST);
  static glq::SavedState saved("pcrtc", kGlq);
  saved.capture();
  const bool cull = saved.cull(), scissor = saved.scissor();
  float color = (float)brightness_contrast_color / 128.0f;
  float alpha = (float)brightness_contrast_alpha / 128.0f;
  auto& shader = render_state->shaders[ShaderId::POST_PROCESSING];
  // a check frame also draws the stand-in, for the presenter to compare the two paths
  const int passes = direct.fbo && direct.check ? 2 : 1;
  for (int pass = 0; pass < passes; pass++) {
    const bool to_direct = direct.fbo && pass == 0;
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    if (to_direct) {
      glBindFramebuffer(GL_FRAMEBUFFER, direct.fbo);
      glDisable(GL_CULL_FACE);
      glDisable(GL_SCISSOR_TEST);
      if (render_state->draw_region_w != win.width || render_state->draw_region_h != win.height ||
          render_state->draw_offset_x != 0 || render_state->draw_offset_y != 0) {
        glViewport(0, 0, win.width, win.height);
        glClearColor(0.f, 0.f, 0.f, 0.f);
        glClear(GL_COLOR_BUFFER_BIT);
      }
      glViewport(render_state->draw_offset_x,
                 direct.h - render_state->draw_offset_y - render_state->draw_region_h,
                 render_state->draw_region_w, render_state->draw_region_h);
    } else if (direct.fbo && !hdr_out) {
      // SDR check reference: the GL window itself, as the GL swap presented it
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      glDisable(GL_SCISSOR_TEST);
      glViewport(0, 0, win.width, win.height);
      glClearColor(0.f, 0.f, 0.f, 0.f);
      glClear(GL_COLOR_BUFFER_BIT);
      scissor ? glEnable(GL_SCISSOR_TEST) : glDisable(GL_SCISSOR_TEST);
      glViewport(render_state->draw_offset_x, render_state->draw_offset_y,
                 render_state->draw_region_w, render_state->draw_region_h);
    } else {
      glViewport(render_state->draw_offset_x, render_state->draw_offset_y,
                 render_state->draw_region_w, render_state->draw_region_h);
      glBindFramebuffer(GL_FRAMEBUFFER, Gfx::window_fbo());
    }
    glBindTexture(GL_TEXTURE_2D, *window_blit_src->tex_id);

    glBindVertexArray(screen_vao);
    glBindBuffer(GL_ARRAY_BUFFER, screen_vbo);

    shader.activate();
    glUniform1i(GLQ_LOC(kGlq, shader.id(), "tex_T0"), 0);
    set_tonemap_uniforms(shader, Gfx::g_hdr_present.active && Gfx::g_hdr_present.hdr);
    // a check frame's second pass is the reference: resolve, stand-in, separate encode
    const bool use_ms = ms_src && pass == 0;
    if (use_ms) {
      glActiveTexture(GL_TEXTURE1);
      glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, *ms_src->tex_id);
      glActiveTexture(GL_TEXTURE0);
      glUniform1i(GLQ_LOC(kGlq, shader.id(), "ms_samples"), ms_src->multisample_count);
    }
    if (to_direct) {
      glUniform1i(GLQ_LOC(kGlq, shader.id(), "direct_flip"), 1);
      glUniform1i(GLQ_LOC(kGlq, shader.id(), "hdr_pq"), direct.pq ? 1 : 0);
      glUniform1f(GLQ_LOC(kGlq, shader.id(), "hdr_scale"), alp < 1 ? alp : 1.f);
    }
    if (brightness_contrast_color < 0) {
      // subtractive blend - note that color is already negative
      float color_neg = color * alpha;
      glUniform4f(GLQ_LOC(kGlq, shader.id(), "color_mult"), 1.0f, 1.0f, 1.0f, alpha);
      glUniform4f(GLQ_LOC(kGlq, shader.id(), "color_add"), color_neg, color_neg, color_neg, 0.0f);
    } else {
      // additive blend
      glUniform4f(GLQ_LOC(kGlq, shader.id(), "color_mult"), 1.0f, 1.0f, 1.0f, alpha);
      glUniform4f(GLQ_LOC(kGlq, shader.id(), "color_add"), color, color, color, 0.0f);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glActiveTexture(GL_TEXTURE0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    if (use_ms) {
      glActiveTexture(GL_TEXTURE1);
      glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);
      glActiveTexture(GL_TEXTURE0);
      glUniform1i(GLQ_LOC(kGlq, shader.id(), "ms_samples"), 0);
    }

    if (to_direct) {
      glUniform1i(GLQ_LOC(kGlq, shader.id(), "direct_flip"), 0);
      cull ? glEnable(GL_CULL_FACE) : glDisable(GL_CULL_FACE);
      scissor ? glEnable(GL_SCISSOR_TEST) : glDisable(GL_SCISSOR_TEST);
    }

    glEnable(GL_BLEND);
    if (alp < 1 && !to_direct) {
      glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
      glBlendEquation(GL_FUNC_ADD);

      m_blackout_renderer.draw(Vector4f(0, 0, 0, 1.f - alp), render_state, prof);
    }
    glEnable(GL_DEPTH_TEST);
  }
}

void OpenGLRenderer::set_tonemap_uniforms(const Shader& shader, bool hdr_out) {
  const auto& gs = Gfx::g_global_settings;
  // mode -1 passes the RGBA8 source through untouched
  const int mode = m_fbo_state.hdr ? gs.tonemap_mode : -1;
  glUniform1i(GLQ_LOC(kGlq, shader.id(), "tm_mode"), mode);
  glUniform1f(GLQ_LOC(kGlq, shader.id(), "tm_exposure"), gs.tonemap_exposure);
  glUniform1f(GLQ_LOC(kGlq, shader.id(), "tm_knee"), gs.tonemap_knee);
  glUniform1f(GLQ_LOC(kGlq, shader.id(), "tm_toon_knee"), gs.tonemap_toon_knee);
  glUniform1f(GLQ_LOC(kGlq, shader.id(), "tm_toon_range"), gs.tonemap_toon_range);
  glUniform2f(GLQ_LOC(kGlq, shader.id(), "tm_white"), gs.tonemap_white_start,
              gs.tonemap_white_full);
  glUniform2f(GLQ_LOC(kGlq, shader.id(), "tm_halo"), gs.tonemap_halo_start, gs.tonemap_halo_full);
  const auto& h = Gfx::g_hdr_present;
  glUniform1i(GLQ_LOC(kGlq, shader.id(), "hdr_out"), hdr_out ? 1 : 0);
  glUniform4f(GLQ_LOC(kGlq, shader.id(), "hdr_nits"), h.paper_white_nits, h.peak_nits, h.black_nits,
              h.knee);
  glUniform1f(GLQ_LOC(kGlq, shader.id(), "hdr_saturation"), h.saturation);
  glUniform1f(GLQ_LOC(kGlq, shader.id(), "hdr_ref_nits"),
              h.desktop_white_nits > 0.f ? h.desktop_white_nits : 200.f);
  glUniform1i(GLQ_LOC(kGlq, shader.id(), "hdr_cal"), gs.hdr_cal_pattern);
  glUniform1i(GLQ_LOC(kGlq, shader.id(), "hdr_pq"), 0);
  glUniform1f(GLQ_LOC(kGlq, shader.id(), "hdr_scale"), 1.f);
  glUniform1i(GLQ_LOC(kGlq, shader.id(), "direct_flip"), 0);
  glUniform1i(GLQ_LOC(kGlq, shader.id(), "tex_ms"), 1);
  glUniform1i(GLQ_LOC(kGlq, shader.id(), "ms_samples"), 0);
}

/// The FP16 buffer the final pass tonemaps: bloom and emissive composited, before the tonemap.
Fbo* OpenGLRenderer::hdr_scene_fbo() {
  if (m_render_state.stereo_active && m_stereo_fbo.valid) {
    return &m_stereo_fbo;
  }
  if (m_upscaled) {
    return m_upscaler.display();
  }
  if (m_fbo_state.resources.resolve_buffer.valid) {
    return &m_fbo_state.resources.resolve_buffer;
  }
  return &m_fbo_state.resources.render_buffer;
}

/*!
 * With the FP16 target on, read the untonemapped scene (hdr_scene_fbo) and write it next to the
 * screenshot as <path minus .png>.exr: half-float RGB, display-encoded (not linearised), 1.0 = SDR
 * white. The EXR header records the tonemap and which image the PNG is (window_png: the presented
 * back buffer with brightness/contrast; otherwise the internal-resolution tonemap without them).
 * Also logs the range of the image and, with SANDOVER_HDR_MASK set, writes <path>.hdr.png:
 * R = max channel above 1 (0..2 -> 0..255), G = max channel / 2, B = negative.
 */
void OpenGLRenderer::save_hdr_scene(ScopedProfilerNode& prof,
                                    const std::string& path,
                                    bool window_png) {
  auto p = prof.make_scoped_child("hdr-exr");
  Fbo* src = hdr_scene_fbo();
  const int w = src->width, h = src->height;
  std::vector<float> px((size_t)w * h * 4);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, src->fbo_id);
  glReadBuffer(GL_COLOR_ATTACHMENT0);
  glReadPixels(0, 0, w, h, GL_RGBA, GL_FLOAT, px.data());
  float max_c = 0.f;
  size_t over = 0, under = 0, bad = 0;
  for (size_t i = 0; i < px.size(); i += 4) {
    bool o = false, u = false;
    for (int c = 0; c < 3; c++) {
      float v = px[i + c];
      if (!std::isfinite(v)) {
        bad++;
        continue;
      }
      max_c = std::max(max_c, v);
      o |= v > 1.f;
      u |= v < 0.f;
    }
    over += o;
    under += u;
  }
  const bool has_png_ext = path.size() > 4 && path.substr(path.size() - 4) == ".png";
  const std::string stem = has_png_ext ? path.substr(0, path.size() - 4) : path;
  if (std::getenv("SANDOVER_HDR_MASK")) {
    std::vector<u32> mask((size_t)w * h);
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        const float* q = &px[((size_t)(h - 1 - y) * w + x) * 4];
        float m = std::max({q[0], q[1], q[2]});
        float lo = std::min({q[0], q[1], q[2]});
        u32 r = (u32)(std::clamp(m - 1.f, 0.f, 1.f) * 255.f);
        u32 g = (u32)(std::clamp(m * 0.5f, 0.f, 1.f) * 255.f);
        u32 b = lo < 0.f ? 255u : 0u;
        mask[(size_t)y * w + x] = 0xff000000 | (b << 16) | (g << 8) | r;
      }
    }
    file_util::write_rgba_png(stem + ".hdr.png", mask.data(), w, h);
  }
  const double n = (double)w * h;
  lg::info("hdr-stats: max {:.4f} over1 {:.5f} neg {:.5f} nonfinite {}", max_c, over / n, under / n,
           bad);

  const auto& gs = Gfx::g_global_settings;
  const exr::StringAttrs attrs = {
      {"sandoverTonemapMode", gs.tonemap_mode == (int)TonemapMode::FILMIC ? "filmic"
                              : gs.tonemap_mode == (int)TonemapMode::TOON ? "toon"
                                                                           : "clamp"},
      {"sandoverTonemapExposure", fmt::format("{}", gs.tonemap_exposure)},
      {"sandoverTonemapKnee", fmt::format("{}", gs.tonemap_knee)},
      {"sandoverTonemapToonKnee", fmt::format("{}", gs.tonemap_toon_knee)},
      {"sandoverTonemapToonRange", fmt::format("{}", gs.tonemap_toon_range)},
      {"sandoverTonemapWhiteStart", fmt::format("{}", gs.tonemap_white_start)},
      {"sandoverTonemapWhiteFull", fmt::format("{}", gs.tonemap_white_full)},
      {"sandoverTonemapHaloStart", fmt::format("{}", gs.tonemap_halo_start)},
      {"sandoverTonemapHaloFull", fmt::format("{}", gs.tonemap_halo_full)},
      {"sandoverPng", window_png ? "window" : "internal"},
  };
  if (!exr::write_rgb_half(stem + ".exr", px.data(), w, h, 4, true, attrs)) {
    lg::error("failed to write {}.exr", stem);
  }
}

/*!
 * With the FP16 target on, tonemap the (resolved) game-resolution image into an RGBA8 buffer for
 * internal-resolution screenshots, without the brightness/contrast terms, like the RGBA8 path.
 */
Fbo* OpenGLRenderer::hdr_screenshot_source(ScopedProfilerNode& prof) {
  auto p = prof.make_scoped_child("hdr-screenshot");
  Fbo* src = hdr_scene_fbo();
  const int w = src->width, h = src->height;
  if (!m_hdr_ldr_fbo.matches(w, h, 1)) {
    m_hdr_ldr_fbo.clear();
    m_hdr_ldr_fbo = make_fbo(w, h, 1, false);
  }

  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glBindFramebuffer(GL_FRAMEBUFFER, m_hdr_ldr_fbo.fbo_id);
  glViewport(0, 0, w, h);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, *src->tex_id);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  auto& shader = m_render_state.shaders[ShaderId::POST_PROCESSING];
  shader.activate();
  glUniform1i(glGetUniformLocation(shader.id(), "tex_T0"), 0);
  set_tonemap_uniforms(shader, false);
  glUniform4f(glGetUniformLocation(shader.id(), "color_mult"), 1.f, 1.f, 1.f, 1.f);
  glUniform4f(glGetUniformLocation(shader.id(), "color_add"), 0.f, 0.f, 0.f, 0.f);
  glBindVertexArray(screen_vao);
  glBindBuffer(GL_ARRAY_BUFFER, screen_vbo);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindVertexArray(0);
  glEnable(GL_DEPTH_TEST);
  return &m_hdr_ldr_fbo;
}
