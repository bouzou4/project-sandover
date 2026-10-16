#pragma once

#include <string>

#include "common/common_types.h"

/*!
 * Per-texture emissive values for renderers without sidecar E (Jak 1): sprites and particles
 * (Sprite3) and merc models. Read from `$SANDOVER_DATA/materials/emissive-actors.csv` (or
 * `$SANDOVER_EMISSIVE_ACTORS`), written by `sandover-materials actor-emissive` from the emissive
 * overrides and ladder strengths. Columns: `combo_id,name,e,min_chroma[,lights]`.
 *
 * Consulted only while the emissive mask is live (bloom or the SSAO mask on); an empty or missing
 * file leaves every renderer as before. See sandover/kb/howto/materials.md "Actor emissive table".
 */
namespace emissive_table {

struct Entry {
  float e = 0.f;           // mask value written by the texture's draws (the sidecar's E units)
  float min_chroma = 0.f;  // sprites: vertex colours less saturated than this write no E
  float lights = 1.f;      // share of E that feeds the tile lights (0: glows and blooms only)
};

/// Load on first use; true when at least one row is present.
bool active();
/// Entry for a texture `combo_id` (tpage << 16 | index), or nullptr.
const Entry* find(u32 combo_id);
/// Re-read the file (debug window button).
void reload();
/// Rows loaded and the file they came from.
int size();
const std::string& path();

}  // namespace emissive_table
