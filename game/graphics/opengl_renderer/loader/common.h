#pragma once

#include "common/common_types.h"
#include "common/custom_data/Tfrag3Data.h"
#include "common/util/Timer.h"

#include "game/graphics/opengl_renderer/background/Canopy.h"
#include "game/graphics/opengl_renderer/background/CanopyVol.h"
#include "game/graphics/opengl_renderer/LevelHeightfield.h"
#include "game/graphics/opengl_renderer/background/Unbake.h"
#include "game/graphics/texture/TexturePool.h"

#include "third-party/glad/include/glad/glad.h"

namespace lava_field {
struct Prepared;
}

struct PbrLevelMaps;

struct LevelData {
  std::unique_ptr<tfrag3::Level> level;
  std::vector<GLuint> textures;
  u64 load_id = UINT64_MAX;

  struct TieOpenGL {
    GLuint vertex_buffer;
    GLuint index_buffer;
    bool has_wind = false;
    GLuint wind_indices;
  };
  std::array<std::vector<TieOpenGL>, tfrag3::TIE_GEOS> tie_data;
  std::array<std::vector<GLuint>, tfrag3::TIE_GEOS> tfrag_vertex_data;
  // geo 0 tree indices for the world sun map; empty unless PERF_OPT_WORLD_PREPASS was on at load
  std::vector<GLuint> tfrag_index_data;
  std::vector<GLuint> shrub_vertex_data;
  GLuint collide_vertices;

  GLuint merc_vertices;
  GLuint merc_indices;
  std::unordered_map<std::string, const tfrag3::MercModel*> merc_model_lookup;

  GLuint hfrag_vertices;
  GLuint hfrag_indices;

  std::unique_ptr<UnbakeSidecar> unbake;  // Jak 1 moving sun, null if absent or mismatched
  std::vector<u8> tex_class;              // Jak 1 material class per texture (Materials.h)
  std::vector<u8> tex_selflit;  // Jak 1: 1 per glow prop texture (world_lights::self_lit_textures)
  std::shared_ptr<PbrLevelMaps> pbr_maps;  // Jak 1 per-texture normal/roughness maps
  std::unique_ptr<CanopyMap> canopy;       // Jak 1 canopy shade map, null if none
  std::unique_ptr<CanopyVolMap> canopy_vol;  // Jak 1 leaf volume (CanopyVol.h), null if none
  std::shared_ptr<lava_field::Prepared> lava;  // Jak 1 lava field (LavaField.h), null without lava
  std::unique_ptr<LevelHeightfield> heightfield;  // Jak 1 sky levels: solid top/bottom (load)

  int frames_since_last_used = 0;
};

struct MercRef {
  const tfrag3::MercModel* model = nullptr;
  u64 load_id = 0;
  const LevelData* level = nullptr;
  bool operator==(const MercRef& other) const {
    return model == other.model && load_id == other.load_id;
  }
};

struct LoaderInput {
  LevelData* lev_data;
  TexturePool* tex_pool;
  std::unordered_map<std::string, std::vector<MercRef>>* mercs;
};

class LoaderStage {
 public:
  LoaderStage(const std::string& name) : m_name(name) {}
  virtual bool run(Timer& timer, LoaderInput& data) = 0;
  virtual void reset() = 0;
  virtual ~LoaderStage() = default;
  const std::string& name() const { return m_name; }

 protected:
  std::string m_name;
};
