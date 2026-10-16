// World sun map (WorldShadows.h): cascades of the static world drawn from the sun. Spliced in by
// Shader.cpp at every WORLD_SHADOW_FRAG token; uniforms set by world_shadows_set_uniforms.
#ifndef SO_WORLD_SHADOW
#define SO_WORLD_SHADOW
uniform int ws_on;                    // 1 world receivers, 2 actors, 4 water; 0 no map this frame
uniform int ws_n;                     // cascades drawn
uniform sampler2DArrayShadow ws_map;  // unit 49
uniform sampler2DArray ws_raw;        // unit 50: the same depths, no compare
uniform mat4 ws_mtx[3];               // receiver space to cascade clip
uniform vec4 ws_par[3];               // x: game units per texel, y: game units per unit of depth,
                                      // z: share of the layer the cascade fills (uv scale)
uniform vec4 ws_sun;                  // toward the sun in receiver space; w: day fade
uniform float ws_pen;                 // penumbra width per unit of blocker distance
uniform float ws_size;                // texels per side
uniform int ws_gate;                  // perf-opt bit 31: receivers skip lookups that cannot matter
uniform int ws_kernel;                // perf-opt bit 28: outer cascades take the cheaper filter

vec3 ws_dpdx = vec3(0.0);
vec3 ws_dpdy = vec3(0.0);
float ws_flat = 1.0;  // sine of the angle between the derivatives: near 0 on a sliver
float ws_pen_mul = 1.0;  // penumbra scale of the current lookup (water: kWsWaterPen)
const float kWsWaterPen = 6.0;

// screen derivatives of the receiver position: call at the top of main, before any discard
void ws_begin(vec3 p) {
  ws_dpdx = dFdx(p);
  ws_dpdy = dFdy(p);
}

const vec2 kWsDisk[16] = vec2[16](
    vec2(-0.942, -0.399), vec2(0.946, -0.769), vec2(-0.094, -0.929), vec2(0.345, 0.294),
    vec2(-0.916, 0.458), vec2(-0.815, -0.879), vec2(-0.383, 0.277), vec2(0.975, 0.756),
    vec2(0.443, -0.975), vec2(0.537, -0.474), vec2(-0.265, -0.419), vec2(0.792, 0.191),
    vec2(-0.242, 0.997), vec2(-0.814, 0.914), vec2(0.200, 0.786), vec2(0.144, -0.141));

const vec2 kWsGather[5] =
    vec2[5](vec2(0.0), vec2(0.98, 0.35), vec2(-0.35, 0.98), vec2(-0.98, -0.35), vec2(0.35, -0.98));
// zero-mean 8-tap penumbra kernel: 4 antipodal pairs
const vec2 kWsDisk8[8] = vec2[8](vec2(0.95, 0.20), vec2(-0.95, -0.20), vec2(-0.25, 0.90),
                                 vec2(0.25, -0.90), vec2(0.55, -0.45), vec2(-0.55, 0.45),
                                 vec2(0.40, 0.55), vec2(-0.40, -0.55));

// outer cascades: 5 depth gathers (20 texels) search the blockers, then 8 compare taps; none
// blocked is lit
float ws_vis_outer(vec3 t, float layer, vec2 dz, float zb, float texel, float depth_units,
                   float uv_texel, float pen, float r_search) {
  float nb = 0.0;
  float zsum = 0.0;
  for (int i = 0; i < 5; i++) {
    vec2 o = kWsGather[i] * r_search;
    vec4 d = textureGather(ws_raw, vec3(t.xy + o, layer));
    // each gathered texel against the receiver plane at its own centre (x, y, z, w order)
    vec2 f = fract((t.xy + o) * ws_size - 0.5);
    vec4 du = vec4(-f.x, 1.0 - f.x, 1.0 - f.x, -f.x) * uv_texel;
    vec4 dv = vec4(1.0 - f.y, 1.0 - f.y, -f.y, -f.y) * uv_texel;
    vec4 b = vec4(lessThan(d, t.z - zb + dot(o, dz) + du * dz.x + dv * dz.y));
    nb += dot(b, vec4(1.0));
    zsum += dot(b, d);
  }
  if (nb == 0.0) {
    return 1.0;
  }
  float w = max(1.5 * texel, pen * (t.z - zsum / nb) * depth_units);
  vec2 kr = vec2(0.5 * min(w / texel * uv_texel, 2.0 * r_search));
  float lit = 0.0;
  for (int i = 0; i < 8; i++) {
    vec2 o = kWsDisk8[i] * kr;
    lit += textureGrad(ws_map, vec4(t.xy + o, layer, t.z + dot(o, dz) - zb), vec2(0.0), vec2(0.0));
  }
  return lit * 0.125;
}

// sun visibility from cascade c at p, n the receiver's normal turned toward the sun
float ws_vis_cascade(int c, vec3 p, vec3 n) {
  mat4 m = ws_mtx[0];
  vec4 par = ws_par[0];
  if (c == 1) {
    m = ws_mtx[1];
    par = ws_par[1];
  } else if (c == 2) {
    m = ws_mtx[2];
    par = ws_par[2];
  }
  float layer = float(c);
  float texel = par.x;
  float uv_texel = 1.0 / ws_size;
  // offset, constant and slope bias stay at the full-size texel when the cascade fills part of
  // its layer
  float bias_texel = texel * par.z;
  // normal offset of one texel toward the sun, then the receiver plane's depth slope per uv
  vec3 t = (m * vec4(p + n * bias_texel, 1.0)).xyz * 0.5 + 0.5;
  vec3 tx = (m * vec4(ws_dpdx, 0.0)).xyz * 0.5;
  vec3 ty = (m * vec4(ws_dpdy, 0.0)).xyz * 0.5;
  t.xy *= par.z;
  tx.xy *= par.z;
  ty.xy *= par.z;
  float det = tx.x * ty.y - tx.y * ty.x;
  vec2 dz = vec2(0.0);
  float zb = 0.5 * bias_texel / par.y;
  // sliver triangles (strip joins) give near-parallel derivatives: no plane, a 2-texel bias
  if (ws_flat > 0.05 && abs(det) > 1e-16) {
    dz = vec2(ty.y * tx.z - tx.y * ty.z, tx.x * ty.z - ty.x * tx.z) / det;
    float dmax = 6.0 * texel / par.y / uv_texel;  // slopes up to about 80 degrees
    dz = clamp(dz, vec2(-dmax), vec2(dmax));
    zb += (abs(dz.x) + abs(dz.y)) * uv_texel * 0.5 * par.z;
  } else {
    zb = 2.0 * texel / par.y;
  }

  // blockers within reach of a 16 m penumbra size the kernel (at least 1.5 texels)
  float pen = ws_pen * ws_pen_mul;
  float r_search = max(pen * 16.0 * 4096.0, 2.0 * texel) / texel * uv_texel;
  if (ws_kernel != 0 && c > 0) {
    return ws_vis_outer(t, layer, dz, zb, texel, par.y, uv_texel, pen, r_search);
  }
  float d0 = textureLod(ws_raw, vec3(t.xy, layer), 0.0).r;
  float nb = d0 < t.z - zb ? 1.0 : 0.0;
  float zsum = nb * d0;
  for (int i = 0; i < 8; i++) {
    vec2 o = kWsDisk[2 * i] * r_search;
    float d = textureLod(ws_raw, vec3(t.xy + o, layer), 0.0).r;
    if (d < t.z + dot(o, dz) - zb) {
      zsum += d;
      nb += 1.0;
    }
  }
  if (nb == 0.0) {
    float lit = 0.0;
    for (int i = 0; i < 4; i++) {
      vec2 o = vec2(i == 0 || i == 2 ? -0.5 : 0.5, i < 2 ? -0.5 : 0.5) * uv_texel;
      lit += textureGrad(ws_map, vec4(t.xy + o, layer, t.z + dot(o, dz) - zb), vec2(0.0), vec2(0.0));
    }
    return lit * 0.25;
  }
  float w = max(1.5 * texel, pen * (t.z - zsum / nb) * par.y);
  vec2 kr = vec2(0.5 * min(w / texel * uv_texel, 2.0 * r_search));
  float lit = 0.0;
  for (int i = 0; i < 16; i++) {
    vec2 o = kWsDisk[i] * kr;
    lit += textureGrad(ws_map, vec4(t.xy + o, layer, t.z + dot(o, dz) - zb), vec2(0.0), vec2(0.0));
  }
  return lit / 16.0;
}

// sun visibility at p (receiver space, game units): 1 lit, 0 in the world's shadow; `covered`
// false where no cascade holds p
float ws_sun_vis_c(vec3 p, out bool covered) {
  covered = false;
  if (ws_n <= 0 || ws_sun.w <= 0.0) {
    return 1.0;
  }
  vec3 n = cross(ws_dpdx, ws_dpdy);
  ws_flat = length(n) / max(length(ws_dpdx) * length(ws_dpdy), 1e-20);
  n = ws_flat > 0.05 ? normalize(n) : ws_sun.xyz;
  n = dot(n, ws_sun.xyz) < 0.0 ? -n : n;
  float e[3] = float[3](1.0, 1.0, 1.0);
  int c0 = -1;
  for (int c = 0; c < 3; c++) {
    if (c >= ws_n) {
      break;
    }
    mat4 m = c == 0 ? ws_mtx[0] : (c == 1 ? ws_mtx[1] : ws_mtx[2]);
    vec3 t = (m * vec4(p, 1.0)).xyz * 0.5 + 0.5;
    e[c] = t.z < 1.0 ? max(abs(t.x - 0.5), abs(t.y - 0.5)) : 1.0;
    if (c0 < 0 && e[c] < 0.49) {
      c0 = c;
    }
  }
  if (c0 < 0) {
    return 1.0;
  }
  covered = true;
  // each cascade keeps 1 - band of what is left; the rest passes on, past the last to the bake
  float acc = 0.0;
  float w = 1.0;
  for (int c = 0; c < 3; c++) {
    if (c >= ws_n) {
      break;
    }
    float f = c < c0 ? 0.0 : 1.0 - smoothstep(0.42, 0.49, e[c]);
    if (f > 0.0 && w > 0.0) {
      acc += w * f * ws_vis_cascade(c, p, n);
    }
    w *= 1.0 - f;
  }
  acc += w;
  return mix(1.0, acc, ws_sun.w);
}

float ws_sun_vis(vec3 p) {
  bool covered;
  return ws_sun_vis_c(p, covered);
}

// world receivers (tfrag, tie, shrub): the map's visibility, 1 with option 32 off
float ws_world_vis(vec3 p) {
  return (ws_on & 1) != 0 ? ws_sun_vis(p) : 1.0;
}

// share of the receiver's colour the map removes: the bake's direct share (share) minus what the
// map lets through of the share it would have fully lit (share_lit), never below 0
float ws_removed(float share, float share_lit, float occ) {
  return max(share - (1.0 - occ) * share_lit, 0.0);
}

// water (option 34): sun visibility at a water pixel, -1 where the map is off or does not reach
float ws_water_vis(vec3 p) {
  if ((ws_on & 4) == 0) {
    return -1.0;
  }
  bool covered;
  ws_pen_mul = kWsWaterPen;  // a wider shore blend than the ground's
  float v = ws_sun_vis_c(p, covered);
  ws_pen_mul = 1.0;
  return covered ? v : -1.0;
}
#endif
