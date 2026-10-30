#pragma once

#include <map>
#include <string>
#include <vector>

#include "common/common_types.h"

/*!
 * @file exr.h
 * Minimal OpenEXR (version 2) writer and reader for RGB captures, display-encoded (not
 * linearised), 1.0 = SDR white: single-part scanline image, channels B, G, R as HALF, no
 * compression, increasing-Y line order, one scanline per chunk. The reader only accepts files with
 * exactly that layout, i.e. what the writer produces.
 */
namespace exr {

u16 float_to_half(float f);
float half_to_float(u16 h);

/// Extra header attributes of type "string" (e.g. the tonemap the matching PNG used).
using StringAttrs = std::map<std::string, std::string>;

/*!
 * Write a half-float RGB EXR. `rgb` holds w * h pixels of `stride` floats each, the first
 * three being R, G, B. Row 0 of `rgb` is the top of the image unless `bottom_up` is set.
 * Returns false if the file could not be written.
 */
bool write_rgb_half(const std::string& path,
                    const float* rgb,
                    int w,
                    int h,
                    int stride,
                    bool bottom_up,
                    const StringAttrs& attrs = {});

struct Image {
  int w = 0, h = 0;
  std::vector<float> rgb;  // w * h * 3, top row first
  StringAttrs attrs;
};

/// Read a file produced by write_rgb_half. Returns false on any other layout.
bool read_rgb_half(const std::string& path, Image* out);

}  // namespace exr
