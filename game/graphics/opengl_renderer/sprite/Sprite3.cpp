#include "Sprite3.h"

#include <algorithm>
#include <cmath>

#include "common/log/log.h"

#include "game/graphics/gfx.h"
#include "game/graphics/opengl_renderer/EmissiveTable.h"
#include "game/graphics/opengl_renderer/GlQueries.h"
#include "game/graphics/opengl_renderer/SsaoRenderer.h"
#include "game/graphics/opengl_renderer/Swamp.h"
#include "game/graphics/opengl_renderer/background/background_common.h"
#include "game/graphics/opengl_renderer/dma_helpers.h"

#include "fmt/format.h"
#include "third-party/imgui/imgui.h"

namespace {

/*!
 * Does the next DMA transfer look like it could be the start of a 2D group?
 */
bool looks_like_2d_chunk_start(const DmaFollower& dma) {
  return dma.current_tag().qwc == 1 && dma.current_tag().kind == DmaTag::Kind::CNT;
}

/*!
 * Read the header. Asserts if it's bad.
 * Returns the number of sprites.
 * Advances 1 dma transfer
 */
u32 process_sprite_chunk_header(DmaFollower& dma) {
  auto transfer = dma.read_and_advance();
  // note that flg = true, this should use double buffering
  bool ok = verify_unpack_with_stcycl(transfer, VifCode::Kind::UNPACK_V4_32, 4, 4, 1,
                                      SpriteDataMem::Header, false, true);
  ASSERT(ok);
  u32 header[4];
  memcpy(header, transfer.data, 16);
  ASSERT(header[0] <= Sprite3::SPRITES_PER_CHUNK);
  return header[0];
}

constexpr int SPRITE_RENDERER_MAX_SPRITES = 1920 * 12;
}  // namespace

Sprite3::Sprite3(const std::string& name, int my_id)
    : BucketRenderer(name, my_id), m_direct(name, my_id, 1024) {
  opengl_setup();
}

void Sprite3::opengl_setup() {
  // Set up OpenGL for 'normal' sprites
  opengl_setup_normal();

  // Set up OpenGL for distort sprites
  opengl_setup_distort();
}

void Sprite3::opengl_setup_normal() {
  glGenBuffers(1, &m_ogl.vertex_buffer);
  glGenVertexArrays(1, &m_ogl.vao);
  glBindVertexArray(m_ogl.vao);
  glBindBuffer(GL_ARRAY_BUFFER, m_ogl.vertex_buffer);
  auto verts = SPRITE_RENDERER_MAX_SPRITES * 4;
  auto bytes = verts * sizeof(SpriteVertex3D);
  glBufferData(GL_ARRAY_BUFFER, bytes, nullptr, GL_STREAM_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(
      0,                                       // location 0 in the shader
      4,                                       // 4 floats per vert (w unused)
      GL_FLOAT,                                // floats
      GL_TRUE,                                 // normalized, ignored,
      sizeof(SpriteVertex3D),                  //
      (void*)offsetof(SpriteVertex3D, xyz_sx)  // offset in array (why is this a pointer...)
  );

  glEnableVertexAttribArray(1);
  glVertexAttribPointer(
      1,                                        // location 1 in the shader
      4,                                        // 4 color components
      GL_FLOAT,                                 // floats
      GL_TRUE,                                  // normalized, ignored,
      sizeof(SpriteVertex3D),                   //
      (void*)offsetof(SpriteVertex3D, quat_sy)  // offset in array (why is this a pointer...)
  );

  glEnableVertexAttribArray(2);
  glVertexAttribPointer(
      2,                                     // location 2 in the shader
      4,                                     // 4 color components
      GL_FLOAT,                              // floats
      GL_TRUE,                               // normalized, ignored,
      sizeof(SpriteVertex3D),                //
      (void*)offsetof(SpriteVertex3D, rgba)  // offset in array (why is this a pointer...)
  );

  glEnableVertexAttribArray(3);
  glVertexAttribIPointer(
      3,                                             // location 3 in the shader
      2,                                             // 4 color components
      GL_UNSIGNED_SHORT,                             // floats
      sizeof(SpriteVertex3D),                        //
      (void*)offsetof(SpriteVertex3D, flags_matrix)  // offset in array (why is this a pointer...)
  );

  glEnableVertexAttribArray(4);
  glVertexAttribIPointer(
      4,                                     // location 4 in the shader
      4,                                     // 3 floats per vert
      GL_UNSIGNED_SHORT,                     // floats
      sizeof(SpriteVertex3D),                //
      (void*)offsetof(SpriteVertex3D, info)  // offset in array (why is this a pointer...)
  );
  glBindBuffer(GL_ARRAY_BUFFER, 0);

  u32 idx_buffer_len = SPRITE_RENDERER_MAX_SPRITES * 5;
  glGenBuffers(1, &m_ogl.index_buffer);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ogl.index_buffer);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx_buffer_len * sizeof(u32), nullptr, GL_STREAM_DRAW);

  glBindVertexArray(0);

  m_vertices_3d.resize(verts);
  m_index_buffer_data.resize(idx_buffer_len);

  m_default_mode.disable_depth_write();
  m_default_mode.set_depth_test(GsTest::ZTest::GEQUAL);
  m_default_mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_DST_SRC_DST);
  m_default_mode.set_aref(38);
  m_default_mode.set_alpha_test(DrawMode::AlphaTest::GEQUAL);
  m_default_mode.set_alpha_fail(GsTest::AlphaFail::FB_ONLY);
  m_default_mode.set_at(true);
  m_default_mode.set_zt(true);
  m_default_mode.set_ab(true);

  m_current_mode = m_default_mode;
}

/*!
 * Handle DMA data that does the per-frame setup.
 * This should get the dma chain immediately after the call to sprite-draw-distorters.
 * It ends right before the sprite-add-matrix-data for the 3d's
 */
void Sprite3::handle_sprite_frame_setup(DmaFollower& dma,
                                        GameVersion version,
                                        SharedRenderState* render_state,
                                        ScopedProfilerNode& /*prof*/) {
  // first is some direct data
  auto direct_data = dma.read_and_advance();
  ASSERT(direct_data.size_bytes == 3 * 16);
  memcpy(m_sprite_direct_setup, direct_data.data, 3 * 16);
  ASSERT(m_sprite_direct_setup[0] == 0x2000000000008001);
  ASSERT(m_sprite_direct_setup[1] == 0xEEEEEEEEEEEEEEEE);
  ASSERT(m_sprite_direct_setup[2] == 0x000000000005126B);
  ASSERT(m_sprite_direct_setup[3] == 0x0000000000000047);
  ASSERT(m_sprite_direct_setup[4] == 0x0000000000000005);
  ASSERT(m_sprite_direct_setup[5] == 0x0000000000000008);

  // next would be the program, but it's 0 size on the PC and isn't sent.

  // next is the "frame data"
  switch (version) {
    case GameVersion::Jak1: {
      render_state->shaders[ShaderId::SPRITE3].activate();
      auto frame_data = dma.read_and_advance();
      ASSERT(frame_data.size_bytes == (int)sizeof(SpriteFrameDataJak1));  // very cool
      ASSERT(frame_data.vifcode0().kind == VifCode::Kind::STCYCL);
      VifCodeStcycl frame_data_stcycl(frame_data.vifcode0());
      ASSERT(frame_data_stcycl.cl == 4);
      ASSERT(frame_data_stcycl.wl == 4);
      ASSERT(frame_data.vifcode1().kind == VifCode::Kind::UNPACK_V4_32);
      VifCodeUnpack frame_data_unpack(frame_data.vifcode1());
      ASSERT(frame_data_unpack.addr_qw == SpriteDataMem::FrameData);
      ASSERT(frame_data_unpack.use_tops_flag == false);
      SpriteFrameDataJak1 jak1_data;
      memcpy(&jak1_data, frame_data.data, sizeof(SpriteFrameDataJak1));
      m_frame_data.from_jak1(jak1_data);
    } break;
    case GameVersion::Jak2:
    case GameVersion::Jak3:
    case GameVersion::JakX: {
      render_state->shaders[ShaderId::SPRITE3].activate();
      auto frame_data = dma.read_and_advance();
      ASSERT(frame_data.size_bytes == (int)sizeof(SpriteFrameData));  // very cool
      ASSERT(frame_data.vifcode0().kind == VifCode::Kind::STCYCL);
      VifCodeStcycl frame_data_stcycl(frame_data.vifcode0());
      ASSERT(frame_data_stcycl.cl == 4);
      ASSERT(frame_data_stcycl.wl == 4);
      ASSERT(frame_data.vifcode1().kind == VifCode::Kind::UNPACK_V4_32);
      VifCodeUnpack frame_data_unpack(frame_data.vifcode1());
      ASSERT(frame_data_unpack.addr_qw == SpriteDataMem::FrameData);
      ASSERT(frame_data_unpack.use_tops_flag == false);
      memcpy(&m_frame_data, frame_data.data, sizeof(SpriteFrameData));
    } break;
    default:
      ASSERT_NOT_REACHED();
  }

  // next, a MSCALF.
  auto mscalf = dma.read_and_advance();
  ASSERT(mscalf.size_bytes == 0);
  ASSERT(mscalf.vifcode0().kind == VifCode::Kind::MSCALF);
  ASSERT(mscalf.vifcode0().immediate == SpriteProgMem::Init);
  ASSERT(mscalf.vifcode1().kind == VifCode::Kind::FLUSHE);

  // next base and offset
  auto base_offset = dma.read_and_advance();
  ASSERT(base_offset.size_bytes == 0);
  ASSERT(base_offset.vifcode0().kind == VifCode::Kind::BASE);
  ASSERT(base_offset.vifcode0().immediate == SpriteDataMem::Buffer0);
  ASSERT(base_offset.vifcode1().kind == VifCode::Kind::OFFSET);
  ASSERT(base_offset.vifcode1().immediate == SpriteDataMem::Buffer1);
}

void Sprite3::render_3d(DmaFollower& dma, SharedRenderState* render_state) {
  // one time matrix data
  auto matrix_data = dma.read_and_advance();
  ASSERT(matrix_data.size_bytes == sizeof(Sprite3DMatrixData));

  bool unpack_ok = verify_unpack_with_stcycl(matrix_data, VifCode::Kind::UNPACK_V4_32, 4, 4, 5,
                                             SpriteDataMem::Matrix, false, false);
  ASSERT(unpack_ok);
  static_assert(sizeof(m_3d_matrix_data) == 5 * 16);
  memcpy(&m_3d_matrix_data, matrix_data.data, sizeof(m_3d_matrix_data));
  if (render_state->eye.active && render_state->game_camera.valid) {
    math::Vector4f cam[4];
    for (int i = 0; i < 4; i++) {
      for (int j = 0; j < 4; j++) {
        cam[i][j] = m_3d_matrix_data.camera(j, i);
      }
    }
    eye_world_to_clip(cam, render_state->game_camera.persp, render_state->eye);
    for (int i = 0; i < 4; i++) {
      for (int j = 0; j < 4; j++) {
        m_3d_matrix_data.camera(j, i) = cam[i][j];
      }
    }
  } else if (render_state->stereo_eye_x != 0.f && render_state->has_pc_data) {
    // world-to-clip matrix: translation row -= e * perspective row 0
    float* cam = m_3d_matrix_data.camera.data();
    for (int i = 0; i < 4; i++) {
      cam[12 + i] -= render_state->camera_persp[0][i] * render_state->stereo_eye_x;
    }
  }
  // TODO
}

void Sprite3::render_2d_group0(DmaFollower& dma,
                               SharedRenderState* render_state,
                               ScopedProfilerNode& prof) {
  // opengl sprite frame setup
  auto shid = render_state->shaders[ShaderId::SPRITE3].id();
  glUniform4fv(glGetUniformLocation(shid, "hvdf_offset"), 1, m_3d_matrix_data.hvdf_offset.data());
  glUniform1f(glGetUniformLocation(shid, "pfog0"), m_frame_data.pfog0);
  glUniform1f(glGetUniformLocation(shid, "min_scale"), m_frame_data.min_scale);
  glUniform1f(glGetUniformLocation(shid, "max_scale"), m_frame_data.max_scale);
  glUniform1f(glGetUniformLocation(shid, "fog_min"), m_frame_data.fog_min);
  glUniform1f(glGetUniformLocation(shid, "fog_max"), m_frame_data.fog_max);
  vista_atmo_foreground_uniforms(render_state, shid);
  // the game's depth fog as the background computes it (fog value hvdf.w + persp[2][3] * depth),
  // for the fog floor of blended world sprites; and the swamp mist
  glUniform4f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, shid, "van_fog"), render_state->camera_hvdf_off.w(),
              render_state->camera_persp[2][3], render_state->camera_fog.y(),
              render_state->camera_fog.z());
  const auto& rot = render_state->camera_rot;
  glUniform3f(GLQ_LOC(PERF_OPT_GEOMETRY + 2, shid, "van_fwd"), rot[0][2], rot[1][2], rot[2][2]);
  swamp_mist_sprite_uniforms(render_state, shid);
  // glUniform1f(glGetUniformLocation(shid, "bonus"), m_frame_data.bonus);
  // glUniform4fv(glGetUniformLocation(shid, "hmge_scale"), 1, m_frame_data.hmge_scale.data());
  glUniform1f(glGetUniformLocation(shid, "deg_to_rad"), m_frame_data.deg_to_rad);
  glUniform1f(glGetUniformLocation(shid, "inv_area"), m_frame_data.inv_area);
  glUniformMatrix4fv(glGetUniformLocation(shid, "camera"), 1, GL_FALSE,
                     m_3d_matrix_data.camera.data());
  glUniform4fv(glGetUniformLocation(shid, "xy_array"), 8, m_frame_data.xy_array[0].data());
  glUniform4fv(glGetUniformLocation(shid, "xyz_array"), 4, m_frame_data.xyz_array[0].data());
  glUniform4fv(glGetUniformLocation(shid, "st_array"), 4, m_frame_data.st_array[0].data());
  if (render_state->eye.active) {
    // billboard size in screen units follows the eye's projection scale
    auto bx = m_frame_data.basis_x, by = m_frame_data.basis_y;
    bx.x() *= render_state->eye.fov_x;
    by.y() *= render_state->eye.fov_y;
    glUniform4fv(glGetUniformLocation(shid, "basis_x"), 1, bx.data());
    glUniform4fv(glGetUniformLocation(shid, "basis_y"), 1, by.data());
  } else {
    glUniform4fv(glGetUniformLocation(shid, "basis_x"), 1, m_frame_data.basis_x.data());
    glUniform4fv(glGetUniformLocation(shid, "basis_y"), 1, m_frame_data.basis_y.data());
  }

  u16 last_prog = -1;

  while (looks_like_2d_chunk_start(dma)) {
    m_debug_stats.blocks_2d_grp0++;
    // 4 packets per chunk

    // first is the header
    u32 sprite_count = process_sprite_chunk_header(dma);
    m_debug_stats.count_2d_grp0 += sprite_count;

    // second is the vector data
    u32 expected_vec_size = sizeof(SpriteVecData2d) * sprite_count;
    auto vec_data = dma.read_and_advance();
    ASSERT(expected_vec_size <= sizeof(m_vec_data_2d));
    unpack_to_no_stcycl(&m_vec_data_2d, vec_data, VifCode::Kind::UNPACK_V4_32, expected_vec_size,
                        SpriteDataMem::Vector, false, true);

    // third is the adgif data
    u32 expected_adgif_size = sizeof(AdGifData) * sprite_count;
    auto adgif_data = dma.read_and_advance();
    ASSERT(expected_adgif_size <= sizeof(m_adgif));
    unpack_to_no_stcycl(&m_adgif, adgif_data, VifCode::Kind::UNPACK_V4_32, expected_adgif_size,
                        SpriteDataMem::Adgif, false, true);

    // fourth is the actual run!!!!!
    auto run = dma.read_and_advance();
    ASSERT(run.vifcode0().kind == VifCode::Kind::NOP);
    ASSERT(run.vifcode1().kind == VifCode::Kind::MSCAL);

    if (m_enabled) {
      if (run.vifcode1().immediate != last_prog) {
        // one-time setups and flushing
        flush_sprites(render_state, prof, false);
      }

      if (run.vifcode1().immediate == SpriteProgMem::Sprites2dGrp0) {
        if (m_2d_enable) {
          do_block_common(SpriteMode::Mode2D, sprite_count, render_state, prof);
        }
      } else {
        if (m_3d_enable) {
          do_block_common(SpriteMode::Mode3D, sprite_count, render_state, prof);
        }
      }
      last_prog = run.vifcode1().immediate;
    }
  }
}

/*!
 * Run the pre-sprite directrenderer.
 */
bool Sprite3::render_direct(DmaFollower& dma,
                            SharedRenderState* render_state,
                            ScopedProfilerNode& prof) {
  m_direct.reset_state();
  while (dma.current_tag().qwc != 7 && dma.current_tag_offset() != render_state->next_bucket) {
    auto direct_data = dma.read_and_advance();
    m_direct.render_vif(direct_data.vif0(), direct_data.vif1(), direct_data.data,
                        direct_data.size_bytes, render_state, prof);
  }
  m_direct.flush_pending(render_state, prof);

  // if sprites are off, after all the directrenderer dma, there is nothing left and we must exit
  if (dma.current_tag_offset() == render_state->next_bucket) {
    return true;
  }
  return false;
}

void Sprite3::render_fake_shadow(DmaFollower& dma) {
  // TODO
  // nop + flushe
  auto nop_flushe = dma.read_and_advance();
  ASSERT(nop_flushe.vifcode0().kind == VifCode::Kind::NOP);
  ASSERT(nop_flushe.vifcode1().kind == VifCode::Kind::FLUSHE);
}

/*!
 * Handle DMA data for group1 2d's (HUD)
 */
void Sprite3::render_2d_group1(DmaFollower& dma,
                               SharedRenderState* render_state,
                               ScopedProfilerNode& prof) {
  // one time matrix data upload
  auto mat_upload = dma.read_and_advance();
  bool mat_ok = verify_unpack_with_stcycl(mat_upload, VifCode::Kind::UNPACK_V4_32, 4, 4, 80,
                                          SpriteDataMem::Matrix, false, false);
  ASSERT(mat_ok);
  ASSERT(mat_upload.size_bytes == sizeof(m_hud_matrix_data));
  memcpy(&m_hud_matrix_data, mat_upload.data, sizeof(m_hud_matrix_data));

  // opengl sprite frame setup
  glUniform4fv(
      glGetUniformLocation(render_state->shaders[ShaderId::SPRITE3].id(), "hud_hvdf_offset"), 1,
      m_hud_matrix_data.hvdf_offset.data());
  glUniform4fv(glGetUniformLocation(render_state->shaders[ShaderId::SPRITE3].id(), "hud_hvdf_user"),
               75, m_hud_matrix_data.user_hvdf[0].data());
  glUniformMatrix4fv(
      glGetUniformLocation(render_state->shaders[ShaderId::SPRITE3].id(), "hud_matrix"), 1,
      GL_FALSE, m_hud_matrix_data.matrix.data());
  if (render_state->eye.active) {
    // HUD sprites keep the game's screen scale
    auto shid = render_state->shaders[ShaderId::SPRITE3].id();
    glUniform4fv(glGetUniformLocation(shid, "basis_x"), 1, m_frame_data.basis_x.data());
    glUniform4fv(glGetUniformLocation(shid, "basis_y"), 1, m_frame_data.basis_y.data());
  }

  // loop through chunks.
  while (looks_like_2d_chunk_start(dma)) {
    m_debug_stats.blocks_2d_grp1++;
    // 4 packets per chunk

    // first is the header
    u32 sprite_count = process_sprite_chunk_header(dma);
    m_debug_stats.count_2d_grp1 += sprite_count;

    // second is the vector data
    u32 expected_vec_size = sizeof(SpriteVecData2d) * sprite_count;
    auto vec_data = dma.read_and_advance();
    ASSERT(expected_vec_size <= sizeof(m_vec_data_2d));
    unpack_to_no_stcycl(&m_vec_data_2d, vec_data, VifCode::Kind::UNPACK_V4_32, expected_vec_size,
                        SpriteDataMem::Vector, false, true);

    // third is the adgif data
    u32 expected_adgif_size = sizeof(AdGifData) * sprite_count;
    auto adgif_data = dma.read_and_advance();
    ASSERT(expected_adgif_size <= sizeof(m_adgif));
    unpack_to_no_stcycl(&m_adgif, adgif_data, VifCode::Kind::UNPACK_V4_32, expected_adgif_size,
                        SpriteDataMem::Adgif, false, true);

    // fourth is the actual run!!!!!
    auto run = dma.read_and_advance();
    ASSERT(run.vifcode0().kind == VifCode::Kind::NOP);
    ASSERT(run.vifcode1().kind == VifCode::Kind::MSCAL);

    switch (render_state->version) {
      case GameVersion::Jak1:
        ASSERT(run.vifcode1().immediate == SpriteProgMem::Sprites2dHud_Jak1);
        break;
      case GameVersion::Jak2:
        ASSERT(run.vifcode1().immediate == SpriteProgMem::Sprites2dHud_Jak2);
        break;
      case GameVersion::Jak3:
      case GameVersion::JakX:
        ASSERT_EQ_IMM(run.vifcode1().immediate, (int)SpriteProgMem::Sprites2dHud_Jak3);
        break;
      default:
        ASSERT_NOT_REACHED();
    }
    if (m_enabled && m_2d_enable) {
      do_block_common(SpriteMode::ModeHUD, sprite_count, render_state, prof);
    }
  }
}

void Sprite3::render(DmaFollower& dma, SharedRenderState* render_state, ScopedProfilerNode& prof) {
  switch (render_state->version) {
    case GameVersion::Jak1:
      render_jak1(dma, render_state, prof);
      break;
    case GameVersion::Jak2:
    case GameVersion::Jak3:
    case GameVersion::JakX:
      render_jak2(dma, render_state, prof);
      break;
    default:
      ASSERT_NOT_REACHED();
  }
}

void Sprite3::render_jak2(DmaFollower& dma,
                          SharedRenderState* render_state,
                          ScopedProfilerNode& prof) {
  m_debug_stats = {};
  auto data0 = dma.read_and_advance();
  ASSERT(data0.vif0() == 0 || data0.vifcode0().kind == VifCode::Kind::MARK);
  ASSERT(data0.vif1() == 0 || data0.vifcode1().kind == VifCode::Kind::NOP);
  ASSERT(data0.size_bytes == 0);

  if (dma.current_tag_offset() == render_state->next_bucket) {
    return;
  }

  // Before anything else, some directrenderer DMA might have been sent
  {
    auto child = prof.make_scoped_child("direct");
    if (render_direct(dma, render_state, child)) {
      return;
    }
  }

  // First is the distorter
  {
    auto child = prof.make_scoped_child("distorter");
    render_distorter(dma, render_state, child);
  }

  // next, the normal sprite stuff
  handle_sprite_frame_setup(dma, render_state->version, render_state, prof);

  // 3d sprites
  render_3d(dma, render_state);

  // 2d draw
  // m_sprite_renderer.reset_state();
  {
    auto child = prof.make_scoped_child("2d-group0");
    render_2d_group0(dma, render_state, child);
    flush_sprites(render_state, prof, false);
  }

  // shadow draw
  render_fake_shadow(dma);

  // 2d draw (HUD)
  {
    auto child = prof.make_scoped_child("2d-group1");
    render_2d_group1(dma, render_state, child);
    flush_sprites(render_state, prof, true);
    auto nop_flushe = dma.read_and_advance();
    ASSERT(nop_flushe.vifcode0().kind == VifCode::Kind::NOP);
    ASSERT(nop_flushe.vifcode1().kind == VifCode::Kind::FLUSHE);
  }

  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glBlendEquation(GL_FUNC_ADD);

  {
    auto p = prof.make_scoped_child("glow");
    glow_dma_and_draw(dma, render_state, p);
  }

  // fmt::print("next bucket is 0x{}\n", render_state->next_bucket);
  while (dma.current_tag_offset() != render_state->next_bucket) {
    // auto tag = dma.current_tag();
    auto data = dma.read_and_advance();
    (void)data;
    // VifCode code(data.vif0());
    // fmt::print("@ 0x{:x} tag: {}", dma.current_tag_offset(), tag.print());
    // fmt::print(" vif0: {}\n", code.print());
    // fmt::print(" vif1: {}\n", VifCode(data.vif1()).print());
  }
}

void Sprite3::render_jak1(DmaFollower& dma,
                          SharedRenderState* render_state,
                          ScopedProfilerNode& prof) {
  m_debug_stats = {};
  // First thing should be a NEXT with two nops. this is a jump from buckets to sprite data
  auto data0 = dma.read_and_advance();
  ASSERT(data0.vif1() == 0);
  ASSERT(data0.vif0() == 0);
  ASSERT(data0.size_bytes == 0);

  if (dma.current_tag().kind == DmaTag::Kind::CALL) {
    // sprite renderer didn't run, let's just get out of here.
    for (int i = 0; i < 4; i++) {
      dma.read_and_advance();
    }
    ASSERT(dma.current_tag_offset() == render_state->next_bucket);
    return;
  }

  // Before anything else, some directrenderer DMA might have been sent
  {
    auto child = prof.make_scoped_child("direct");
    if (render_direct(dma, render_state, child)) {
      return;
    }
  }

  // First is the distorter
  {
    auto child = prof.make_scoped_child("distorter");
    render_distorter(dma, render_state, child);
  }

  // next, sprite frame setup.
  handle_sprite_frame_setup(dma, render_state->version, render_state, prof);

  // 3d sprites
  render_3d(dma, render_state);

  // 2d draw
  // m_sprite_renderer.reset_state();
  {
    auto child = prof.make_scoped_child("2d-group0");
    render_2d_group0(dma, render_state, child);
    flush_sprites(render_state, prof, false);
  }

  // shadow draw
  render_fake_shadow(dma);

  // 2d draw (HUD)
  {
    auto child = prof.make_scoped_child("2d-group1");
    render_2d_group1(dma, render_state, child);
    if (render_state->defer_hud_sprites) {
      m_hud_deferred = true;
    } else {
      flush_sprites(render_state, prof, true);
    }
  }

  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glBlendEquation(GL_FUNC_ADD);

  // TODO finish this up.
  // fmt::print("next bucket is 0x{}\n", render_state->next_bucket);
  while (dma.current_tag_offset() != render_state->next_bucket) {
    //    auto tag = dma.current_tag();
    // fmt::print("@ 0x{:x} tag: {}", dma.current_tag_offset(), tag.print());
    auto data = dma.read_and_advance();
    VifCode code(data.vif0());
    // fmt::print(" vif0: {}\n", code.print());
    if (code.kind == VifCode::Kind::NOP) {
      // fmt::print(" vif1: {}\n", VifCode(data.vif1()).print());
    }
  }
}

namespace {
bool additive_key(u64 key) {
  DrawMode mode;
  mode.as_int() = key & 0xffffffff;
  const auto ab = mode.get_alpha_blend();
  return mode.get_ab_enable() &&
         (ab == DrawMode::AlphaBlend::SRC_0_SRC_DST || ab == DrawMode::AlphaBlend::SRC_0_FIX_DST);
}
}  // namespace

void Sprite3::sort_world_sprites(SharedRenderState* render_state, bool em_table) {
  // additive sprites commute only if they also add on the emissive buffer; listed light
  // textures MAX there (flush_sprites), so they keep their depth order
  for (auto* bucket : m_bucket_list) {
    bool commutes = additive_key(bucket->key);
    if (commutes && em_table) {
      if (const auto* gt =
              render_state->texture_pool->lookup_gpu_texture((u32)(bucket->key >> 32))) {
        commutes = !emissive_table::find(((u32)gt->tex_id.page << 16) | gt->tex_id.tex);
      }
    }
    bucket->commutes = commutes;
  }

  // view w of the sprite centre, as matrix_transform in sprite3_3d.vert
  const float* c = m_3d_matrix_data.camera.data();
  m_sort_keys.clear();
  for (const auto* bucket : m_bucket_list) {
    for (size_t i = 0; i < bucket->ids.size(); i += 5) {
      const u32 sprite = bucket->ids[i] / 4;
      const auto& p = m_vertices_3d[sprite * 4].xyz_sx;
      const float w = c[15] + c[3] * p.x() + c[7] * p.y() + c[11] * p.z();
      m_sort_keys.push_back({std::abs(w), sprite, bucket});
    }
  }
  std::sort(m_sort_keys.begin(), m_sort_keys.end(), [](const SortKey& a, const SortKey& b) {
    return a.w != b.w ? a.w > b.w : a.sprite < b.sprite;
  });

  // runs are reused across flushes so their id vectors keep their capacity
  size_t used = 0;
  auto new_run = [&](u64 key) {
    if (used == m_sorted_runs.size()) {
      m_sorted_runs.emplace_back();
    }
    Bucket& run = m_sorted_runs[used++];
    run.key = key;
    run.ids.clear();
    return used - 1;
  };
  size_t stretch = 0;  // first run of the current commuting stretch
  bool in_stretch = false;
  for (const auto& k : m_sort_keys) {
    const u64 key = k.bucket->key;
    size_t run = SIZE_MAX;
    if (k.bucket->commutes) {
      if (!in_stretch) {
        stretch = used;
        in_stretch = true;
      }
      for (size_t r = stretch; r < used; r++) {
        if (m_sorted_runs[r].key == key) {
          run = r;
          break;
        }
      }
    } else {
      in_stretch = false;
      if (used && m_sorted_runs[used - 1].key == key) {
        run = used - 1;
      }
    }
    if (run == SIZE_MAX) {
      run = new_run(key);
    }
    const u32 v = k.sprite * 4;
    m_sorted_runs[run].ids.insert(m_sorted_runs[run].ids.end(),
                                  {v, v + 1, v + 2, v + 3, UINT32_MAX});
  }

  m_sorted_list.clear();
  for (size_t r = 0; r < used; r++) {
    m_sorted_list.push_back(&m_sorted_runs[r]);
  }
}

void Sprite3::draw_debug_window() {
  ImGui::Checkbox("Glow", &m_enable_glow);
  ImGui::Checkbox("new glow", &m_glow_renderer.new_mode);
  ImGui::Separator();
  ImGui::Text("Distort sprites: %d", m_distort_stats.total_sprites);
  ImGui::Text("2D Group 0 (World) blocks: %d sprites: %d", m_debug_stats.blocks_2d_grp0,
              m_debug_stats.count_2d_grp0);
  ImGui::Text("2D Group 1 (HUD) blocks: %d sprites: %d", m_debug_stats.blocks_2d_grp1,
              m_debug_stats.count_2d_grp1);
  ImGui::Checkbox("World back to front (Sandover path)", &m_sort_world);
  ImGui::SameLine();
  ImGui::Text("draws: %d", m_sorted_draws);
  ImGui::Checkbox("Culling", &m_enable_culling);
  ImGui::Checkbox("2d", &m_2d_enable);
  ImGui::SameLine();
  ImGui::Checkbox("3d", &m_3d_enable);
  ImGui::Checkbox("Distort", &m_distort_enable);
  ImGui::Checkbox("Distort instancing", &m_enable_distort_instancing);
  ImGui::Separator();
  m_glow_renderer.draw_debug_window();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Render (for real)

void Sprite3::flush_sprites(SharedRenderState* render_state,
                            ScopedProfilerNode& prof,
                            bool double_draw,
                            bool keep) {
  glBindVertexArray(m_ogl.vao);

  glEnable(GL_PRIMITIVE_RESTART);
  glPrimitiveRestartIndex(UINT32_MAX);

  // upload vertex buffer
  glBindBuffer(GL_ARRAY_BUFFER, m_ogl.vertex_buffer);
  glBufferData(GL_ARRAY_BUFFER, m_sprite_idx * sizeof(SpriteVertex3D) * 4, m_vertices_3d.data(),
               GL_STREAM_DRAW);

  // bloom and tile lights: additive world sprites add to the emissive mask, HUD sprites clear it
  const bool emissive =
      render_state->emissive_mask &&
      (Gfx::g_global_settings.bloom_enable || Gfx::g_global_settings.emissive_lights_enable);
  // the Sandover path draws world sprites back to front; the vanilla path keeps upstream's order
  const bool sorted = Gfx::g_global_settings.sandover_path && !double_draw && m_sort_world &&
                      render_state->version == GameVersion::Jak1;
  if (sorted) {
    sort_world_sprites(render_state, emissive && emissive_table::active());
  }
  if (!double_draw) {
    m_sorted_draws = sorted ? (int)m_sorted_list.size() : 0;
  }
  const auto& draw_list = sorted ? m_sorted_list : m_bucket_list;

  // two passes through the buckets. first to build the index buffer
  u32 idx_offset = 0;
  for (const auto bucket : draw_list) {
    memcpy(&m_index_buffer_data[idx_offset], bucket->ids.data(), bucket->ids.size() * sizeof(u32));
    bucket->offset_in_idx_buffer = idx_offset;
    idx_offset += bucket->ids.size();
  }

  // now upload it
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ogl.index_buffer);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx_offset * sizeof(u32), m_index_buffer_data.data(),
               GL_STREAM_DRAW);

  const GLint em_loc =
      emissive ? glGetUniformLocation(render_state->shaders[ShaderId::SPRITE3].id(), "em_out")
               : -1;
  // listed light textures (emissive_table) write their own E with a max blend instead
  const bool em_table = emissive && emissive_table::active();
  const GLint em_table_loc =
      emissive ? glGetUniformLocation(render_state->shaders[ShaderId::SPRITE3].id(), "em_table")
               : -1;
  const GLint em_chroma_loc =
      emissive ? glGetUniformLocation(render_state->shaders[ShaderId::SPRITE3].id(), "em_chroma")
               : -1;
  const GLint em_unlit_loc =
      emissive ? glGetUniformLocation(render_state->shaders[ShaderId::SPRITE3].id(), "em_unlit")
               : -1;
  // tile lights: emissive sprites keep their nearest depth on draw buffer 4 (max of 1 / z)
  const bool sz = emissive && SsaoRenderer::sprite_depth_drawing(true);
  if (sz) {
    const GLuint prog = render_state->shaders[ShaderId::SPRITE3].id();
    const float* lin = SsaoRenderer::depth_lin();
    glUniform3f(glGetUniformLocation(prog, "sz_lin"), lin[0], lin[1], lin[2]);
    glUniform1f(glGetUniformLocation(prog, "sz_min_e"),
                0.5f * Gfx::g_global_settings.ssao_emissive_lo);
  }

  // now do draws!
  for (const auto bucket : draw_list) {
    u32 tbp = bucket->key >> 32;
    DrawMode mode;
    mode.as_int() = bucket->key & 0xffffffff;

    std::optional<u64> tex;
    tex = render_state->texture_pool->lookup(tbp);

    if (!tex) {
      lg::warn("Failed to find texture at {}, using random (sprite)", tbp);
      tex = render_state->texture_pool->get_placeholder_texture();
    }
    ASSERT(tex);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, *tex);

    auto settings = setup_opengl_from_draw_mode(mode, GL_TEXTURE0, false);

    glUniform1f(glGetUniformLocation(render_state->shaders[ShaderId::SPRITE3].id(), "alpha_min"),
                double_draw ? settings.aref_first : 0.016);
    glUniform1f(glGetUniformLocation(render_state->shaders[ShaderId::SPRITE3].id(), "alpha_max"),
                10.f);
    glUniform1i(glGetUniformLocation(render_state->shaders[ShaderId::SPRITE3].id(), "tex_T0"), 0);
    {
      const auto ab = mode.get_alpha_blend();
      const bool additive = mode.get_ab_enable() && (ab == DrawMode::AlphaBlend::SRC_0_SRC_DST ||
                                                     ab == DrawMode::AlphaBlend::SRC_0_FIX_DST);
      glUniform4f(glGetUniformLocation(render_state->shaders[ShaderId::SPRITE3].id(), "atmo_color"),
                  render_state->fog_color[0] / 255.f, render_state->fog_color[1] / 255.f,
                  render_state->fog_color[2] / 255.f, additive ? 1.f : 0.f);
    }
    if (emissive) {
      const auto ab = mode.get_alpha_blend();
      const bool additive = mode.get_ab_enable() && (ab == DrawMode::AlphaBlend::SRC_0_SRC_DST ||
                                                     ab == DrawMode::AlphaBlend::SRC_0_FIX_DST);
      const emissive_table::Entry* light = nullptr;
      if (em_table && !double_draw) {
        if (const auto* gt = render_state->texture_pool->lookup_gpu_texture(tbp)) {
          light = emissive_table::find(((u32)gt->tex_id.page << 16) | gt->tex_id.tex);
        }
      }
      // sprites keep the highlight material of the surface behind them
      SsaoRenderer::emissive_draw_buffers(double_draw || additive || light, true,
                                          !render_state->light_material);
      if (double_draw) {
        glDisablei(GL_BLEND, 2);
      } else if (light) {
        glEnablei(GL_BLEND, 2);
        glBlendEquationi(2, GL_MAX);
        glUniform1f(em_chroma_loc, light->min_chroma);
        glUniform1f(em_unlit_loc, 1.f - light->lights);
      }
      glUniform1f(em_loc, double_draw ? 0.f : 1.f);
      glUniform1f(em_table_loc, light ? light->e : 0.f);
      if (sz) {
        glEnablei(GL_BLEND, 4);
        glBlendEquationi(4, GL_MAX);
        glBlendFunci(4, GL_ONE, GL_ONE);
      }
    }

    prof.add_draw_call();
    prof.add_tri(2 * (bucket->ids.size() / 5));

    glDrawElements(GL_TRIANGLE_STRIP, bucket->ids.size(), GL_UNSIGNED_INT,
                   (void*)(bucket->offset_in_idx_buffer * sizeof(u32)));

    if (double_draw) {
      switch (settings.kind) {
        case DoubleDrawKind::NONE:
          break;
        case DoubleDrawKind::AFAIL_NO_DEPTH_WRITE:
          prof.add_draw_call();
          prof.add_tri(2 * (bucket->ids.size() / 5));
          glUniform1f(
              glGetUniformLocation(render_state->shaders[ShaderId::SPRITE3].id(), "alpha_min"),
              -10.f);
          glUniform1f(
              glGetUniformLocation(render_state->shaders[ShaderId::SPRITE3].id(), "alpha_max"),
              settings.aref_second);
          glDepthMask(GL_FALSE);
          glDrawElements(GL_TRIANGLE_STRIP, bucket->ids.size(), GL_UNSIGNED_INT,
                         (void*)(bucket->offset_in_idx_buffer * sizeof(u32)));
          break;
        default:
          ASSERT(false);
      }
    }
  }

  if (emissive) {
    SsaoRenderer::emissive_draw_buffers(false);
    if (em_table) {
      glBlendEquationi(2, GL_FUNC_ADD);
    }
    if (sz) {
      SsaoRenderer::sprite_depth_drawing(false);
      glDisablei(GL_BLEND, 4);
      glBlendEquationi(4, GL_FUNC_ADD);
    }
  }
  if (!keep) {
    clear_sprites();
  }
  glBindVertexArray(0);
}

void Sprite3::clear_sprites() {
  m_sprite_buckets.clear();
  m_bucket_list.clear();
  m_sorted_list.clear();
  m_last_bucket_key = UINT64_MAX;
  m_last_bucket = nullptr;
  m_sprite_idx = 0;
}

void Sprite3::flush_deferred_hud(SharedRenderState* render_state,
                                 ScopedProfilerNode& prof,
                                 bool keep) {
  if (!m_hud_deferred) {
    return;
  }
  m_hud_deferred = keep;
  render_state->shaders[ShaderId::SPRITE3].activate();
  flush_sprites(render_state, prof, true, keep);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glBlendEquation(GL_FUNC_ADD);
}

void Sprite3::drop_deferred_hud() {
  if (m_hud_deferred) {
    m_hud_deferred = false;
    clear_sprites();
  }
}

void Sprite3::handle_tex0(u64 val,
                          SharedRenderState* /*render_state*/,
                          ScopedProfilerNode& /*prof*/) {
  GsTex0 reg(val);

  // update tbp
  m_current_tbp = reg.tbp0();
  m_current_mode.set_tcc(reg.tcc());

  // tbw: assume they got it right
  // psm: assume they got it right
  // tw: assume they got it right
  // th: assume they got it right

  ASSERT(reg.tfx() == GsTex0::TextureFunction::MODULATE);
  ASSERT(reg.psm() != GsTex0::PSM::PSMT4HH);

  // cbp: assume they got it right
  // cpsm: assume they got it right
  // csm: assume they got it right
}

void Sprite3::handle_tex1(u64 val,
                          SharedRenderState* /*render_state*/,
                          ScopedProfilerNode& /*prof*/) {
  GsTex1 reg(val);
  m_current_mode.set_filt_enable(reg.mmag());
}

void Sprite3::handle_zbuf(u64 val,
                          SharedRenderState* /*render_state*/,
                          ScopedProfilerNode& /*prof*/) {
  // note: we can basically ignore this. There's a single z buffer that's always configured the same
  // way - 24-bit, at offset 448.
  GsZbuf x(val);
  ASSERT(x.psm() == TextureFormat::PSMZ24);
  ASSERT(x.zbp() == 448 || x.zbp() == 304);  // 304 for jak 2.

  m_current_mode.set_depth_write_enable(!x.zmsk());
}

void Sprite3::handle_clamp(u64 val,
                           SharedRenderState* /*render_state*/,
                           ScopedProfilerNode& /*prof*/) {
  if (!(val == 0b101 || val == 0 || val == 1 || val == 0b100)) {
    ASSERT_MSG(false, fmt::format("clamp: 0x{:x}", val));
  }

  m_current_mode.set_clamp_s_enable(val & 0b001);
  m_current_mode.set_clamp_t_enable(val & 0b100);
}

void Sprite3::update_mode_from_alpha1(u64 val, DrawMode& mode) {
  GsAlpha reg(val);
  if (reg.a_mode() == GsAlpha::BlendMode::SOURCE && reg.b_mode() == GsAlpha::BlendMode::DEST &&
      reg.c_mode() == GsAlpha::BlendMode::SOURCE && reg.d_mode() == GsAlpha::BlendMode::DEST) {
    // (Cs - Cd) * As + Cd
    // Cs * As  + (1 - As) * Cd
    mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_DST_SRC_DST);

  } else if (reg.a_mode() == GsAlpha::BlendMode::SOURCE &&
             reg.b_mode() == GsAlpha::BlendMode::ZERO_OR_FIXED &&
             reg.c_mode() == GsAlpha::BlendMode::SOURCE &&
             reg.d_mode() == GsAlpha::BlendMode::DEST) {
    // (Cs - 0) * As + Cd
    // Cs * As + (1) * CD
    mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_0_SRC_DST);
  } else if (reg.a_mode() == GsAlpha::BlendMode::SOURCE &&
             reg.b_mode() == GsAlpha::BlendMode::ZERO_OR_FIXED &&
             reg.c_mode() == GsAlpha::BlendMode::ZERO_OR_FIXED &&
             reg.d_mode() == GsAlpha::BlendMode::DEST) {
    ASSERT(reg.fix() == 128);
    // Cv = (Cs - 0) * FIX + Cd
    // if fix = 128, it works out to 1.0
    mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_0_FIX_DST);
    // src plus dest
  } else if (reg.a_mode() == GsAlpha::BlendMode::SOURCE &&
             reg.b_mode() == GsAlpha::BlendMode::DEST &&
             reg.c_mode() == GsAlpha::BlendMode::ZERO_OR_FIXED &&
             reg.d_mode() == GsAlpha::BlendMode::DEST) {
    // Cv = (Cs - Cd) * FIX + Cd
    ASSERT(reg.fix() == 64);
    mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_DST_FIX_DST);
  } else if (reg.a_mode() == GsAlpha::BlendMode::ZERO_OR_FIXED &&
             reg.b_mode() == GsAlpha::BlendMode::SOURCE &&
             reg.c_mode() == GsAlpha::BlendMode::SOURCE &&
             reg.d_mode() == GsAlpha::BlendMode::DEST) {
    // (0 - Cs) * As + Cd
    // Cd - Cs * As
    // s, d
    mode.set_alpha_blend(DrawMode::AlphaBlend::ZERO_SRC_SRC_DST);
  }

  else {
    lg::error("unsupported blend: a {} b {} c {} d {}", (int)reg.a_mode(), (int)reg.b_mode(),
              (int)reg.c_mode(), (int)reg.d_mode());
    mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_DST_SRC_DST);
    ASSERT(false);
  }
}

void Sprite3::handle_alpha(u64 val,
                           SharedRenderState* /*render_state*/,
                           ScopedProfilerNode& /*prof*/) {
  update_mode_from_alpha1(val, m_current_mode);
}

void Sprite3::do_block_common(SpriteMode mode,
                              u32 count,
                              SharedRenderState* render_state,
                              ScopedProfilerNode& prof) {
  m_current_mode = m_default_mode;
  for (u32 sprite_idx = 0; sprite_idx < count; sprite_idx++) {
    if (m_sprite_idx == SPRITE_RENDERER_MAX_SPRITES) {
      flush_sprites(render_state, prof, mode == ModeHUD);
    }

    if (mode == Mode2D && render_state->has_pc_data && m_enable_culling) {
      // we can skip sprites that are out of view
      // it's probably possible to do this for 3D as well.
      auto bsphere = m_vec_data_2d[sprite_idx].xyz_sx;
      bsphere.w() = std::max(bsphere.w(), m_vec_data_2d[sprite_idx].sy());
      if (bsphere.w() == 0 || !sphere_in_view_ref(bsphere, render_state->camera_planes)) {
        continue;
      }
    }

    if (render_state->version > GameVersion::Jak1) {
      // glow code sets the matrix to -1,
      // jak 2 adds:
      // ibltz vi08, L4
      // which is set from ilw.y vi08, 1(vi02)
      if (m_vec_data_2d[sprite_idx].matrix() == -1) {
        continue;
      }
    }

    auto& adgif = m_adgif[sprite_idx];
    handle_tex0(adgif.tex0_data, render_state, prof);
    handle_tex1(adgif.tex1_data, render_state, prof);
    if (GsRegisterAddress(adgif.clamp_addr) == GsRegisterAddress::ZBUF_1) {
      handle_zbuf(adgif.clamp_data, render_state, prof);
    } else {
      handle_clamp(adgif.clamp_data, render_state, prof);
    }
    handle_alpha(adgif.alpha_data, render_state, prof);

    u64 key = (((u64)m_current_tbp) << 32) | m_current_mode.as_int();
    Bucket* bucket;
    if (key == m_last_bucket_key) {
      bucket = m_last_bucket;
    } else {
      auto it = m_sprite_buckets.find(key);
      if (it == m_sprite_buckets.end()) {
        bucket = &m_sprite_buckets[key];
        bucket->key = key;
        m_bucket_list.push_back(bucket);
      } else {
        bucket = &it->second;
      }
    }
    u32 start_vtx_id = m_sprite_idx * 4;
    bucket->ids.push_back(start_vtx_id);
    bucket->ids.push_back(start_vtx_id + 1);
    bucket->ids.push_back(start_vtx_id + 2);
    bucket->ids.push_back(start_vtx_id + 3);
    bucket->ids.push_back(UINT32_MAX);

    auto& vert1 = m_vertices_3d.at(start_vtx_id + 0);

    if (render_state->version == GameVersion::Jak3 || render_state->version == GameVersion::JakX) {
      auto flag = m_vec_data_2d[sprite_idx].flag();

      if ((flag & 0x10) || (flag & 0x20)) {
        // these flags mean we need to swap vertex order around - not yet implemented since it's too
        // hard to get right without this code running.
        // ASSERT_NOT_REACHED();
      }
    }

    vert1.xyz_sx = m_vec_data_2d[sprite_idx].xyz_sx;
    vert1.quat_sy = m_vec_data_2d[sprite_idx].flag_rot_sy;
    // ftoi'd in the original game, and I believe the VIF would discard the upper bits on pack
    vert1.rgba = m_vec_data_2d[sprite_idx].rgba;
    vert1.rgba.x() = (int)vert1.rgba.x() & 0xff;
    vert1.rgba.y() = (int)vert1.rgba.y() & 0xff;
    vert1.rgba.z() = (int)vert1.rgba.z() & 0xff;
    vert1.rgba.w() = (int)vert1.rgba.w() & 0xff;
    vert1.rgba /= 255;
    vert1.flags_matrix[0] = m_vec_data_2d[sprite_idx].flag();
    vert1.flags_matrix[1] = m_vec_data_2d[sprite_idx].matrix();
    vert1.info[0] = 0;  // hack
    vert1.info[1] = m_current_mode.get_tcc_enable();
    vert1.info[2] = 0;
    vert1.info[3] = mode;

    m_vertices_3d.at(start_vtx_id + 1) = vert1;
    m_vertices_3d.at(start_vtx_id + 2) = vert1;
    m_vertices_3d.at(start_vtx_id + 3) = vert1;

    m_vertices_3d.at(start_vtx_id + 1).info[2] = 1;
    m_vertices_3d.at(start_vtx_id + 2).info[2] = 3;
    m_vertices_3d.at(start_vtx_id + 3).info[2] = 2;

    // note that PC swaps the last two vertices
    if (render_state->version == GameVersion::Jak3) {
      auto flag = m_vec_data_2d[sprite_idx].flag();
      switch (flag & 0x30) {
        case 0x10:
          // FLAG 16: 1, 0, 3, 2
          m_vertices_3d.at(start_vtx_id + 0).info[2] = 0;
          m_vertices_3d.at(start_vtx_id + 1).info[2] = 1;
          m_vertices_3d.at(start_vtx_id + 2).info[2] = 3;
          m_vertices_3d.at(start_vtx_id + 3).info[2] = 2;
          break;
        case 0x20:
          // FLAG 32: 3, 2, 1, 0
          m_vertices_3d.at(start_vtx_id + 0).info[2] = 3;
          m_vertices_3d.at(start_vtx_id + 1).info[2] = 2;
          m_vertices_3d.at(start_vtx_id + 2).info[2] = 0;
          m_vertices_3d.at(start_vtx_id + 3).info[2] = 1;
          break;
        case 0x30:
          // 2, 3, 0, 1
          m_vertices_3d.at(start_vtx_id + 0).info[2] = 2;
          m_vertices_3d.at(start_vtx_id + 1).info[2] = 3;
          m_vertices_3d.at(start_vtx_id + 2).info[2] = 1;
          m_vertices_3d.at(start_vtx_id + 3).info[2] = 0;
          break;
      }
    }

    ++m_sprite_idx;
  }
}
