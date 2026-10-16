#include "exr.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

namespace exr {

u16 float_to_half(float f) {
  u32 x;
  memcpy(&x, &f, 4);
  const u32 sign = (x >> 16) & 0x8000;
  const u32 exp = (x >> 23) & 0xff;
  u32 man = x & 0x7fffff;
  if (exp == 0xff) {
    return (u16)(sign | 0x7c00 | (man ? 0x200 : 0));
  }
  const int e = (int)exp - 127 + 15;
  if (e >= 31) {
    return (u16)(sign | 0x7c00);
  }
  if (e <= 0) {
    if (e < -10) {
      return (u16)sign;
    }
    man |= 0x800000;
    const int shift = 14 - e;
    u32 h = man >> shift;
    const u32 rem = man & ((1u << shift) - 1);
    const u32 half = 1u << (shift - 1);
    if (rem > half || (rem == half && (h & 1))) {
      h++;
    }
    return (u16)(sign | h);
  }
  u32 h = ((u32)e << 10) | (man >> 13);
  const u32 rem = man & 0x1fff;
  if (rem > 0x1000 || (rem == 0x1000 && (h & 1))) {
    h++;  // a carry into the exponent is the correct rounding, up to infinity
  }
  return (u16)(sign | h);
}

float half_to_float(u16 h) {
  const u32 sign = (u32)(h & 0x8000) << 16;
  const u32 exp = (h >> 10) & 0x1f;
  const u32 man = h & 0x3ff;
  u32 x;
  if (exp == 0) {
    float f = (float)man / 16777216.f;  // man * 2^-24
    memcpy(&x, &f, 4);
    x |= sign;
  } else if (exp == 31) {
    x = sign | 0x7f800000 | (man << 13);
  } else {
    x = sign | ((exp - 15 + 127) << 23) | (man << 13);
  }
  float f;
  memcpy(&f, &x, 4);
  return f;
}

namespace {
constexpr u32 kMagic = 20000630;
constexpr s32 kHalf = 1;
const char* kChannels[3] = {"B", "G", "R"};  // EXR lists channels alphabetically

template <typename T>
void put(std::vector<u8>& b, T v) {
  const size_t at = b.size();
  b.resize(at + sizeof(T));
  memcpy(b.data() + at, &v, sizeof(T));
}

void put_str(std::vector<u8>& b, const std::string& s) {
  b.insert(b.end(), s.begin(), s.end());
  b.push_back(0);
}

void attr(std::vector<u8>& b, const std::string& name, const std::string& type, s32 size) {
  put_str(b, name);
  put_str(b, type);
  put(b, size);
}

void box(std::vector<u8>& b, const std::string& name, int w, int h) {
  attr(b, name, "box2i", 16);
  for (s32 v : {0, 0, w - 1, h - 1}) {
    put(b, v);
  }
}

template <typename T>
bool get(const std::vector<u8>& b, size_t& at, T* v) {
  if (at + sizeof(T) > b.size()) {
    return false;
  }
  memcpy(v, b.data() + at, sizeof(T));
  at += sizeof(T);
  return true;
}

bool get_str(const std::vector<u8>& b, size_t& at, std::string* s) {
  if (at >= b.size()) {
    return false;
  }
  const size_t end = std::find(b.begin() + at, b.end(), (u8)0) - b.begin();
  if (end >= b.size()) {
    return false;
  }
  s->assign((const char*)b.data() + at, end - at);
  at = end + 1;
  return true;
}
}  // namespace

bool write_rgb_half(const std::string& path,
                    const float* rgb,
                    int w,
                    int h,
                    int stride,
                    bool bottom_up,
                    const StringAttrs& attrs) {
  std::vector<u8> b;
  put<u32>(b, kMagic);
  put<u32>(b, 2);
  attr(b, "channels", "chlist", 3 * (2 + 16) + 1);
  for (const char* c : kChannels) {
    put_str(b, c);
    put<s32>(b, kHalf);
    put<u32>(b, 0);  // pLinear, reserved
    put<s32>(b, 1);
    put<s32>(b, 1);
  }
  b.push_back(0);
  attr(b, "compression", "compression", 1);
  b.push_back(0);
  box(b, "dataWindow", w, h);
  box(b, "displayWindow", w, h);
  attr(b, "lineOrder", "lineOrder", 1);
  b.push_back(0);
  attr(b, "pixelAspectRatio", "float", 4);
  put(b, 1.f);
  attr(b, "screenWindowCenter", "v2f", 8);
  put(b, 0.f);
  put(b, 0.f);
  attr(b, "screenWindowWidth", "float", 4);
  put(b, 1.f);
  for (const auto& [k, v] : attrs) {
    attr(b, k, "string", (s32)v.size());
    b.insert(b.end(), v.begin(), v.end());
  }
  b.push_back(0);

  const u32 line_bytes = (u32)w * 3 * 2;
  const u64 first = b.size() + (u64)h * 8;
  for (int y = 0; y < h; y++) {
    put<u64>(b, first + (u64)y * (8 + line_bytes));
  }
  b.reserve(b.size() + (size_t)h * (8 + line_bytes));
  for (int y = 0; y < h; y++) {
    put<s32>(b, y);
    put<u32>(b, line_bytes);
    const float* row = rgb + (size_t)(bottom_up ? h - 1 - y : y) * w * stride;
    for (int c = 2; c >= 0; c--) {
      for (int x = 0; x < w; x++) {
        put<u16>(b, float_to_half(row[(size_t)x * stride + c]));
      }
    }
  }
  std::ofstream f(path, std::ios::binary);
  f.write((const char*)b.data(), (std::streamsize)b.size());
  return (bool)f;
}

bool read_rgb_half(const std::string& path, Image* out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    return false;
  }
  std::vector<u8> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  size_t at = 0;
  u32 magic, version;
  if (!get(b, at, &magic) || !get(b, at, &version) || magic != kMagic || version != 2) {
    return false;
  }
  int w = 0, h = 0, channels = 0;
  out->attrs.clear();
  for (;;) {
    std::string name, type;
    s32 size;
    if (!get_str(b, at, &name)) {
      return false;
    }
    if (name.empty()) {
      break;
    }
    if (!get_str(b, at, &type) || !get(b, at, &size) || size < 0 || at + size > b.size()) {
      return false;
    }
    const u8* v = b.data() + at;
    if (name == "channels") {
      size_t c = at;
      std::string cn;
      while (get_str(b, c, &cn) && !cn.empty()) {
        s32 type_id = -1;
        if (!get(b, c, &type_id) || channels >= 3 || cn != kChannels[channels] ||
            type_id != kHalf) {
          return false;
        }
        channels++;
        c += 12;
      }
    } else if (name == "compression" || name == "lineOrder") {
      if (size < 1 || v[0] != 0) {
        return false;
      }
    } else if (name == "dataWindow") {
      s32 wd[4];
      memcpy(wd, v, 16);
      if (wd[0] != 0 || wd[1] != 0) {
        return false;
      }
      w = wd[2] + 1;
      h = wd[3] + 1;
    } else if (type == "string") {
      out->attrs[name] = std::string((const char*)v, size);
    }
    at += size;
  }
  if (channels != 3 || w <= 0 || h <= 0) {
    return false;
  }
  out->w = w;
  out->h = h;
  out->rgb.assign((size_t)w * h * 3, 0.f);
  for (int i = 0; i < h; i++) {
    u64 off;
    if (!get(b, at, &off)) {
      return false;
    }
    size_t c = off;
    s32 y;
    u32 n;
    if (!get(b, c, &y) || !get(b, c, &n) || y < 0 || y >= h || n != (u32)w * 6 ||
        c + n > b.size()) {
      return false;
    }
    for (int ch = 2; ch >= 0; ch--) {
      for (int x = 0; x < w; x++) {
        u16 hv;
        get(b, c, &hv);
        out->rgb[((size_t)y * w + x) * 3 + ch] = half_to_float(hv);
      }
    }
  }
  return true;
}

}  // namespace exr
