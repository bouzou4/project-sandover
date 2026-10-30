#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"
#include "common/dma/gs.h"
#include "common/math/Vector.h"

#include "game/graphics/pipelines/opengl.h"

struct SharedRenderState;
struct LevelData;

/*!
 * Click-to-inspect (Jak 1, off by default; `pc-set-inspect` or the renderer window).
 *
 * While on, an RGBA32UI target sits at colour attachment 3 of the render framebuffer and the
 * background and foreground buckets (tfrag, tie, shrub, merc, generic, bucket 5-62 minus shadow)
 * write to it (shaders/inspect_frag.glsl): x = draw record (index + 1 into this frame's draw
 * table, 0 = nothing, 0xff000000 | shader id = a draw that did not register), y = the provoking
 * vertex id, z/w = the interpolated texture coordinate. Sprites, sky and ocean do not write it,
 * so a click on a particle reports the surface behind it (the emissive mask still shows the
 * particle's E). Off: nothing is attached and no GL call is made.
 *
 * Ctrl + left click on the game view reads the pixel back and resolves it on the CPU (draw,
 * texture, material class, hit triangle and UV, sampled texel, sidecar E and the palette-slot
 * contributions of the nearest vertex). The panel writes verdicts to
 * `$SANDOVER_DATA/materials/emissive-overrides.csv`, which `sandover-unbake export-sidecar` folds
 * into E. See sandover/kb/howto/inspect.md.
 */
namespace inspect {

enum Kind : u8 { NONE = 0, TFRAG = 1, TIE = 2, SHRUB = 3, MERC = 4, GENERIC = 5 };

/// One registered draw of this frame.
struct DrawRecord {
  Kind kind = NONE;
  std::string level;
  int geom = 0;
  int tree = -1;      // fr3 tree index (tfrag_trees[geom], tie_trees[geom], shrub_trees)
  int draw = -1;      // draw index in the tree (merc: effect draw index)
  s32 tex = -1;       // level texture index; negative: anim slot
  u32 tbp = 0;        // generic: texture pool address
  std::string model;  // merc model name
  const LevelData* data = nullptr;  // merc: the draw's level (the common level has no name)
  DrawMode mode;
  math::Vector<s32, 4> itimes[4];
  math::Vector4f cam_trans;
};

/// True while the ID target is live this dispatch.
bool active();
/// Register a draw; returns its record id (0 if inspect is off).
u32 add(DrawRecord&& record);
/// Set the record id of the following draws on the active program.
void set(GLuint program, u32 record);
/// Allow or block ID writes (envmap second passes keep the base draw's id).
void write_mask(bool write);

}  // namespace inspect

class InspectRenderer {
 public:
  InspectRenderer();
  ~InspectRenderer();

  /// Attach and clear the ID target when `on`, detach once when not. Call before bucket 0.
  void begin(SharedRenderState* render_state, bool on);
  /// Route output 3 of the following bucket's draws to the ID target (or stop).
  void set_bucket(SharedRenderState* render_state, int bucket);
  /// Stop writing after the last bucket.
  void end(SharedRenderState* render_state);
  /// Click handling and the inspect panel. Call after the dispatch, with the frame's ImGui open.
  void frame(SharedRenderState* render_state);
  void draw_debug_window();

 private:
  struct Hit;
  bool read_pixel(SharedRenderState* rs, int x, int y, Hit& hit);
  void resolve(SharedRenderState* rs, Hit& hit);
  bool pick(SharedRenderState* rs, float fx, float fy);
  std::string summary() const;
  void draw_panel();
  void write_override(const char* verdict);
  void load_tables();

  GLuint m_tex = 0;
  int m_w = 0, m_h = 0, m_samples = 0;
  GLuint m_attached_fb = 0;
  GLuint m_rb_fbo = 0;
  GLuint m_rb_tex[3] = {0, 0, 0};  // 1x1: RGBA32UI id, RGBA8 colour, R8 emissive

  struct Material {
    std::string cls, tags, note;
  };
  std::unordered_map<u32, Material> m_materials;  // combo_id -> class row of materials.csv
  std::unordered_map<std::string, std::string> m_overrides;  // key -> last verdict
  bool m_tables_loaded = false;
  std::string m_materials_path, m_overrides_path;

  std::unique_ptr<Hit> m_hit;
  char m_note[256] = {};
  std::string m_status;
  u32 m_pick_seq = 0;
};
