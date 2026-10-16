#version 410 core

// emissive tile lights, pass 1 (every second pixel). stage 0: one fragment per tile;
// 1: one per split x split block of taps, partial sums; 2: one per tile, combining stage 1
layout(location = 0) out vec4 flux;  // rgb: mean ramp(E) x colour over the taps, a: fill
layout(location = 1) out vec4 pos;   // xyz: camera-space emission position, w: 1 if valid
layout(location = 2) out vec4 part2;
layout(location = 3) out vec4 part3;

uniform sampler2D color_tex;  // resolved frame, linear filtering
uniform sampler2D csz;        // camera-space z in metres, mip 1 read
uniform vec4 proj_info;
uniform float far_m;
uniform sampler2D em_ss;
uniform sampler2DMS em_ms;
uniform int em_samples;
uniform vec2 em_ramp;
uniform ivec2 tile_px;
uniform float near_slack;
uniform int stage;
uniform int split;
uniform sampler2D part_tex[4];  // stage 2: stage 1's four outputs
// nearest emissive sprite, 1 / z in metres (0: none): additive particles write no depth
uniform sampler2D sz_ss;
uniform sampler2DMS sz_ms;
uniform int sz_on;

// FP16 target: keep the 8-bit range the lights were tuned on; NaN and negatives read as 0
vec3 frameColor(vec3 c) {
  return any(isnan(c)) ? vec3(0.0) : clamp(c, 0.0, 1.0);
}

// mask red: E; green: the part of E whose texture gives no light (emissive table `lights`)
float emissive(ivec2 p) {
  vec2 m = em_samples <= 1 ? texelFetch(em_ss, p, 0).rg : texelFetch(em_ms, p, 0).rg;
  return max(m.r - m.g, 0.0);
}

float spriteZ(ivec2 p) {
  if (sz_on == 0) {
    return 0.0;
  }
  float v = em_samples <= 1 ? texelFetch(sz_ss, p, 0).r : texelFetch(sz_ms, p, 0).r;
  return v > 0.0 ? 1.0 / v : 0.0;
}

struct Acc {
  vec3 sum;
  vec2 ssum;
  float wsum, zsum, zw, zmin, taps, covered;
  vec2 lo, hi;  // covered taps' box, tile pixels
};

Acc accEmpty() {
  return Acc(vec3(0.0), vec2(0.0), 0.0, 0.0, 0.0, 1e30, 0.0, 0.0, vec2(float(1 << 20)), vec2(-1.0));
}

// taps [t0, t1) of the tile at pixel `base` (tap t is tile pixel 2t)
void accumulate(inout Acc a, ivec2 base, ivec2 t0, ivec2 t1) {
  ivec2 size = textureSize(color_tex, 0);
  ivec2 zhi = textureSize(csz, 1) - ivec2(1);
  for (int j = 2 * t0.y; j < 2 * t1.y; j += 2) {
    for (int i = 2 * t0.x; i < 2 * t1.x; i += 2) {
      ivec2 p = base + ivec2(i, j);
      if (p.x + 1 >= size.x || p.y + 1 >= size.y) {
        continue;
      }
      a.taps += 1.0;
      float z = texelFetch(csz, min(p >> 1, zhi), 1).r;
      bool solid = z < far_m * 0.999;
      float sz = spriteZ(p);
      if (solid && sz > 0.0) {
        z = min(z, sz);
      }
      if (solid) {
        a.zmin = min(a.zmin, z);
      }
      float e = smoothstep(em_ramp.x, em_ramp.y, emissive(p));
      if (e <= 0.0) {
        continue;
      }
      if (e > 0.5) {
        a.covered += 1.0;
        a.lo = min(a.lo, vec2(i, j));
        a.hi = max(a.hi, vec2(i, j));
      }
      vec3 c = frameColor(texture(color_tex, (vec2(p) + 1.0) / vec2(size)).rgb);
      a.sum += e * c;
      float wl = e * max(dot(c, vec3(0.299, 0.587, 0.114)), 1e-3);
      a.ssum += wl * (vec2(p) + 1.0);
      a.wsum += wl;
      if (solid) {
        a.zsum += wl * z;
        a.zw += wl;
      }
    }
  }
}

void finish(Acc a) {
  // share of the covered taps' bounding box that glows: low for thin arcs, high for compact lamps
  ivec2 box = (ivec2(a.hi) - ivec2(a.lo)) / 2 + ivec2(1);
  float fill = a.covered > 0.0 ? a.covered / float(box.x * box.y) : 0.0;
  flux = vec4(a.sum / max(a.taps, 1.0), fill);
  if (a.wsum <= 0.0 || a.zmin >= 1e29) {
    // emission over sky only has nothing to stand on
    flux = vec4(0.0);
    pos = vec4(0.0);
    return;
  }
  // emission without depth of its own (not a sprite): pull to the tile's nearest surface
  float z = min(a.zw > 0.0 ? a.zsum / a.zw : a.zmin, a.zmin + near_slack);
  vec2 s = a.ssum / a.wsum;
  pos = vec4((s * proj_info.xy + proj_info.zw) * z, z, 1.0);
}

void main() {
  ivec2 frag = ivec2(gl_FragCoord.xy);
  Acc a = accEmpty();
  part2 = vec4(0.0);
  part3 = vec4(0.0);
  if (stage == 0) {
    accumulate(a, frag * tile_px, ivec2(0), (tile_px + 1) / 2);
    finish(a);
  } else if (stage == 1) {
    ivec2 taps = (tile_px + 1) / 2;
    ivec2 chunk = (taps + split - 1) / split;
    ivec2 t0 = (frag % split) * chunk;
    accumulate(a, (frag / split) * tile_px, t0, min(t0 + chunk, taps));
    flux = vec4(a.sum, a.taps);
    pos = vec4(a.ssum, a.wsum, a.zsum);
    part2 = vec4(a.zw, a.zmin, a.covered, 0.0);
    part3 = vec4(a.lo, a.hi);
  } else {
    for (int y = 0; y < split; y++) {
      for (int x = 0; x < split; x++) {
        ivec2 q = frag * split + ivec2(x, y);
        vec4 p0 = texelFetch(part_tex[0], q, 0);
        vec4 p1 = texelFetch(part_tex[1], q, 0);
        vec4 p2 = texelFetch(part_tex[2], q, 0);
        vec4 p3 = texelFetch(part_tex[3], q, 0);
        a.sum += p0.rgb;
        a.taps += p0.a;
        a.ssum += p1.xy;
        a.wsum += p1.z;
        a.zsum += p1.w;
        a.zw += p2.x;
        a.zmin = min(a.zmin, p2.y);
        a.covered += p2.z;
        a.lo = min(a.lo, p3.xy);
        a.hi = max(a.hi, p3.zw);
      }
    }
    finish(a);
  }
}
