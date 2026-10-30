// Level heightfield (LevelHeightfield.h): highest and lowest solid surface per texel, built at
// load from the collision mesh. Spliced in by Shader.cpp at its include token; uniforms set by
// heightfield_set_uniforms. Positions in game units (metres x 4096).
#ifndef SO_LEVEL_HEIGHT
#define SO_LEVEL_HEIGHT
uniform int lh_n;              // map slots bound, 0-2 (0: every query reads as open)
uniform sampler2DArray lh_tex; // r: top, g: bottom; 1..65534 = a height, r 0 / g 65535 = none
uniform vec4 lh_rect[2];       // x0, z0 (game units), texels per game unit in x, z
uniform vec2 lh_h[2];          // y0, span (game units)
uniform int lh_levels;         // mip levels of lh_tex

// texel at xz in slot i at mip `lod` (r, g 0..65535), false outside the map; the mip k cell is
// texel >> k, as the loader's reduction (the allocation holds whole cells at every level)
bool lh_fetch(vec2 xz, int i, int lod, out vec2 v) {
  vec4 r = lh_rect[i];
  v = vec2(0.0, 65535.0);
  ivec2 t = ivec2(floor((xz - r.xy) * r.zw));
  ivec2 size = textureSize(lh_tex, 0).xy;
  if (r.z == 0.0 || any(lessThan(t, ivec2(0))) || any(greaterThanEqual(t, size))) {
    return false;
  }
  t = min(t >> lod, textureSize(lh_tex, lod).xy - 1);
  v = texelFetch(lh_tex, ivec3(t, i), lod).rg * 65535.0;
  return true;
}

int lh_lod(float lod) {
  return clamp(int(floor(lod + 0.5)), 0, lh_levels - 1);
}

float lh_top_slot(vec2 xz, int i, int l) {
  vec2 v;
  if (lh_fetch(xz, i, l, v) && v.r > 0.5) {
    return lh_h[i].x + lh_h[i].y * (v.r - 1.0) / 65533.0;
  }
  return -1e30;
}

// highest solid surface over the texel footprint 2^lod at xz (the mips hold the max); -1e30
// where none
float lh_top(vec2 xz, float lod) {
  float best = -1e30;
  int l = lh_lod(lod);
  for (int i = 0; i < 2; i++) {
    if (i >= lh_n) {
      break;
    }
    best = max(best, lh_top_slot(xz, i, l));
  }
  return best;
}

// lowest solid surface over the footprint (the mips hold the min); 1e30 where none
float lh_bottom(vec2 xz, float lod) {
  float best = 1e30;
  int l = lh_lod(lod);
  for (int i = 0; i < 2; i++) {
    if (i >= lh_n) {
      break;
    }
    vec2 h = lh_h[i];
    vec2 v;
    if (lh_fetch(xz, i, l, v) && v.g < 65534.5) {
      best = min(best, h.x + h.y * (v.g - 1.0) / 65533.0);
    }
  }
  return best;
}

// Share of the sun blocked at p by the solid world along L (toward the sun, unit): ten taps
// out to about 70 m past `start_m`, each over a footprint that grows with the distance (max mips,
// chosen per slot from its own texel, never miss a roof), soft over +-margin_m. 0 = open.
float lh_sun_blocked(vec3 p, vec3 L, float start_m, float margin_m) {
  if (lh_n == 0) {
    return 0.0;
  }
  float m = margin_m * 4096.0;
  float blocked = 0.0;
  for (int k = 0; k < 10; k++) {
    float t = 4096.0 * (start_m + pow(1.6, float(k)) - 1.0);
    vec3 q = p + L * t;
    float hs = -1e30;
    for (int i = 0; i < 2; i++) {
      if (i >= lh_n) {
        break;
      }
      hs = max(hs, lh_top_slot(q.xz, i, lh_lod(log2(max(0.5 * t * lh_rect[i].z, 1.0)))));
    }
    blocked = max(blocked, smoothstep(-m, m, hs - q.y));
  }
  return blocked;
}
#endif
