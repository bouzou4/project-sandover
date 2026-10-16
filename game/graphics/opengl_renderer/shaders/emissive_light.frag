#version 410 core

// emissive tile lights, pass 3: added over opaque pixels with GL_ONE, GL_ONE (RGB only)
out vec4 color;

uniform sampler2D csz;  // camera-space z in metres, mip 0
uniform vec4 proj_info;
uniform float far_m;
uniform sampler2D color_tex;  // resolved frame before this pass
uniform sampler2D nrm_ss;
uniform sampler2DMS nrm_ms;
uniform int nrm_samples;  // 0: no target, 1: single-sample, else the sample count
uniform vec2 nrm_scale;
uniform sampler2D em_ss;
uniform sampler2DMS em_ms;
uniform int em_samples;
uniform vec2 em_ramp;
uniform sampler2D light_power;  // rgb: power, a: rank score (0 = no light)
uniform sampler2D light_pos;    // xyz: camera-space position
uniform int num_lights;
uniform sampler2D world_power;  // world lights: rgb power, a: 1 = light
uniform sampler2D world_pos;    // xyz: camera-space position, w: radius (m)
uniform int num_world;
uniform uint perf_opt;  // bit 1: the lists below replace the textures, unreached pixels skip
uniform vec4 l_power[32];
uniform vec4 l_pos[32];
uniform vec4 w_power[32];
uniform vec4 w_pos[32];
uniform usampler2D cull_tex;  // perf_opt bit 3 (with bit 1): per cell, x: tile lights, y: world lights
uniform int cull_px;          // cell size in pixels
uniform float intensity;
uniform float radius;
uniform float falloff;
uniform float albedo_mix;
uniform float max_add;  // soft limit of the added light (luma), so stacked lights never go white
uniform mat3 cam_to_world;  // world game units = cam_to_world * (view_sign * C * 4096) + cam_trans
uniform vec3 cam_trans;
uniform vec3 view_sign;
uniform int skip_added;  // merc level props: merc gives them the lights the bake lacks
// highlights: x strength (0 = off), y soft limit (brightest channel); material in the emissive mask's b, a
uniform vec2 llh;

ACTOR_SHADOW_FRAG

const float kWrap = 0.5;     // wrapped N.L: light reaches a little past the terminator
const float kMinDist = 0.2;  // metres; light fades to 0 at the light (no self-light, no ring)
// lights the bake lacks (glow props, dark eco crystals): a point light that dominates its first
// metres, 1 / (1 + d^2 / r0^2), windowed to 0 at the radius, with a soft limit of their own
const float kAddedR0 = 1.8;  // WorldLights.h kAddedR0M
const float kWideR0 = 3.0;   // WorldLights.h kWideR0M (dark eco crystal)
const float kAddedMax = 1.2;

vec3 decodeNormal(vec4 e) {
  return dot(e.rgb, e.rgb) < 0.01 ? vec3(0.0) : normalize(e.rgb * 2.0 - 1.0);
}

vec3 targetNormal(ivec2 p) {
  vec3 sum = vec3(0.0);
  if (nrm_samples == 1) {
    sum = decodeNormal(texelFetch(nrm_ss, p, 0));
  } else if (nrm_samples > 1) {
    for (int i = 0; i < nrm_samples; i++) {
      sum += decodeNormal(texelFetch(nrm_ms, p, i));
    }
  }
  return vec3(sum.xy * nrm_scale, sum.z);
}

vec3 position(ivec2 p) {
  p = clamp(p, ivec2(0), textureSize(csz, 0) - ivec2(1));
  float z = texelFetch(csz, p, 0).r;
  return vec3(((vec2(p) + 0.5) * proj_info.xy + proj_info.zw) * z, z);
}

vec3 depthNormal(ivec2 p, vec3 C) {
  vec3 L = position(p - ivec2(1, 0));
  vec3 R = position(p + ivec2(1, 0));
  vec3 D = position(p - ivec2(0, 1));
  vec3 U = position(p + ivec2(0, 1));
  vec3 dx = abs(R.z - C.z) < abs(C.z - L.z) ? R - C : C - L;
  vec3 dy = abs(U.z - C.z) < abs(C.z - D.z) ? U - C : C - D;
  return cross(dy, dx);
}

bool lists() {
  return (perf_opt & 2u) != 0u;
}
bool culled() {
  return (perf_opt & 10u) == 10u;
}
vec4 lightPower(int k) {
  return lists() ? l_power[k] : texelFetch(light_power, ivec2(k, 0), 0);
}
vec4 lightPos(int k) {
  return lists() ? l_pos[k] : texelFetch(light_pos, ivec2(k, 0), 0);
}
vec4 worldPower(int k) {
  return lists() ? w_power[k] : texelFetch(world_power, ivec2(k, 0), 0);
}
vec4 worldPos(int k) {
  return lists() ? w_pos[k] : texelFetch(world_pos, ivec2(k, 0), 0);
}

bool reachesTile(int k, vec3 C, float r2) {
  vec3 v = l_pos[k].xyz - C;
  float d2 = dot(v, v);
  return !(d2 >= r2 || d2 < 1e-8);
}
bool reachesWorld(int k, vec3 C) {
  vec3 v = w_pos[k].xyz - C;
  float d2 = dot(v, v);
  return w_power[k].a > 0.0 && !(d2 >= w_pos[k].w * w_pos[k].w || d2 < 1e-8);
}

// true when a light passes the range tests of the loops in main (else they add exactly 0)
bool anyLightReaches(vec3 C, uvec2 mask) {
  float r2 = radius * radius;
  if (culled()) {
    while (mask.x != 0u) {
      int k = findLSB(mask.x);
      mask.x &= mask.x - 1u;
      if (reachesTile(k, C, r2)) {
        return true;
      }
    }
    while (mask.y != 0u) {
      int k = findLSB(mask.y);
      mask.y &= mask.y - 1u;
      if (reachesWorld(k, C)) {
        return true;
      }
    }
    return false;
  }
  for (int k = 0; k < num_lights; k++) {
    if (l_power[k].a <= 0.0) {
      break;
    }
    if (reachesTile(k, C, r2)) {
      return true;
    }
  }
  for (int k = 0; k < num_world; k++) {
    if (reachesWorld(k, C)) {
      return true;
    }
  }
  return false;
}

// highlight material of this pixel: x GGX alpha (0 = none), y F0 x lobe, z (1 - roughness)^2
vec3 hl_m = vec3(0.0);
vec3 hl_V = vec3(0.0, 0.0, 1.0);

/// Highlight of one light toward unit direction l: the material shading's GGX lobe (GGX over its
/// peak, capped at 60, Smith visibility, N.L) with a Schlick fresnel that roughness dulls; GGX
/// alpha `a` (hl_m.x, or wider for an area source).
float hlLobeA(vec3 n, vec3 l, float a) {
  float ndl = dot(n, l);
  if (ndl <= 0.0) {
    return 0.0;  // also keeps l = -V (normalize of 0) out
  }
  vec3 H = normalize(l + hl_V);
  float ndh = max(dot(n, H), 0.0);
  float ndv = max(dot(n, hl_V), 1e-3);
  float a2 = a * a;
  float d = ndh * ndh * (a2 - 1.0) + 1.0;
  float g = a2 * a2 / (d * d) * min(1.0 / a2, 60.0);
  float k = 0.5 * a;
  float vis = 0.25 / ((ndl * (1.0 - k) + k) * (ndv * (1.0 - k) + k));
  float F = hl_m.y + (max(hl_m.z, hl_m.y) - hl_m.y) * pow(1.0 - max(dot(hl_V, H), 0.0), 5.0);
  return g * vis * ndl * F;
}

float hlLobe(vec3 n, vec3 l) {
  return hlLobeA(n, l, hl_m.x);
}

vec3 tileLight(int k, vec3 C, vec3 n, float r2, inout vec3 spec) {
  vec3 v = lightPos(k).xyz - C;
  float d2 = dot(v, v);
  if (d2 >= r2 || d2 < 1e-8) {
    return vec3(0.0);
  }
  float d = sqrt(d2);
  float ndl = max((dot(n, v / d) + kWrap) / (1.0 + kWrap), 0.0);
  float att = pow(1.0 - d / radius, falloff) * smoothstep(0.0, kMinDist, d);
  if (hl_m.x > 0.0) {
    // a tile stands for a tile-wide patch of glow: widen the lobe by that width seen from here,
    // so neighbouring tiles' lobes overlap into one sheen instead of a dot each
    float src = float(textureSize(csz, 0).x) / 64.0 * lightPos(k).z * abs(proj_info.x);
    float a = min(hl_m.x + src / d, 1.0);
    spec += lightPower(k).rgb * (hlLobeA(n, v / d, a) * att);
  }
  return lightPower(k).rgb * (ndl * att);
}

// a: 1 a baked light; 1.5 a light the bake lacks, 2 + its shadow map if it has one; +0.25 wide
void worldLight(int k, vec3 C, vec3 n, inout vec3 sum, inout vec3 sum_added, inout vec3 spec) {
  vec4 pw = worldPower(k);
  if (pw.a <= 0.0) {
    return;
  }
  vec4 lp = worldPos(k);
  vec3 v = lp.xyz - C;
  float d2 = dot(v, v);
  if (d2 >= lp.w * lp.w || d2 < 1e-8) {
    return;
  }
  float d = sqrt(d2);
  float ndl = max((dot(n, v / d) + kWrap) / (1.0 + kWrap), 0.0);
  if (pw.a < 1.25) {
    sum += pw.rgb * (ndl * pow(1.0 - d / lp.w, falloff) * smoothstep(0.0, kMinDist, d));
    if (hl_m.x > 0.0) {
      spec += pw.rgb * (hlLobe(n, v / d) * pow(1.0 - d / lp.w, falloff) *
                        smoothstep(0.0, kMinDist, d));
    }
    return;
  }
  if (skip_added == 1) {
    return;
  }
  float w = 1.0 - d2 / (lp.w * lp.w);
  float r0 = abs(fract(pw.a * 2.0) - 0.5) < 0.1 ? kWideR0 : kAddedR0;
  float att = w * w / (1.0 + d2 / (r0 * r0));
  int m = int(pw.a + 0.2) - 2;
  if (m >= 0 && m < shadow_nlamps && shadow_on == 1 && (!lists() || ndl * att > 0.0)) {
    att *= 1.0 - shadow_lamp(m, cam_to_world * (view_sign * C * 4096.0) + cam_trans);
  }
  sum_added += pw.rgb * (ndl * att);
  if (hl_m.x > 0.0) {
    spec += pw.rgb * (hlLobe(n, v / d) * att);
  }
}

float emissive(ivec2 p) {
  if (em_samples <= 1) {
    return texelFetch(em_ss, p, 0).r;
  }
  float e = 0.0;
  for (int i = 0; i < em_samples; i++) {
    e += texelFetch(em_ms, p, i).r;
  }
  return e / float(em_samples);
}

// the emissive mask's b, a over the samples: roughness averaged over the samples that carry a
// material (a < 1; an edge with none would pull it toward 0, a sparkle), 1 - sqrt(F0 x lobe) over all
vec2 hlMaterial(ivec2 p) {
  if (em_samples <= 1) {
    return texelFetch(em_ss, p, 0).ba;
  }
  float r = 0.0;
  float a = 0.0;
  float n = 0.0;
  for (int i = 0; i < em_samples; i++) {
    vec2 m = texelFetch(em_ms, p, i).ba;
    a += m.y;
    if (m.y < 1.0) {
      r += m.x;
      n += 1.0;
    }
  }
  return vec2(n > 0.0 ? r / n : 1.0, a / float(em_samples));
}

void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  vec3 C = position(p);
  if (C.z >= far_m * 0.999 || (lightPower(0).a <= 0.0 && num_world == 0)) {
    discard;
  }
  uvec2 mask = uvec2(0u);
  if (culled()) {
    // lights listed for this cell (lights that cannot reach it add exactly 0)
    mask = texelFetch(cull_tex, p / cull_px, 0).xy;
    mask.y &= num_world >= 32 ? 0xffffffffu : (1u << uint(num_world)) - 1u;
  }
  if (lists() && !anyLightReaches(C, mask)) {
    discard;
  }
  vec3 n = targetNormal(p);
  if (dot(n, n) <= 0.0) {
    n = depthNormal(p, C);
  }
  if (dot(n, n) <= 0.0) {
    discard;
  }
  n = normalize(dot(n, C) > 0.0 ? -n : n);
  float hl_s = 0.0;
  if (llh.x > 0.0) {
    vec2 m = hlMaterial(p);
    float s = 1.0 - m.y;
    hl_s = s * s;
    float r = clamp(m.x, 0.04, 1.0);
    hl_m = hl_s > 0.0 ? vec3(max(r * r, 0.002), hl_s, (1.0 - r) * (1.0 - r)) : vec3(0.0);
    hl_V = normalize(-C);
  }
  float r2 = radius * radius;
  vec3 sum = vec3(0.0);
  vec3 sum_added = vec3(0.0);
  vec3 spec = vec3(0.0);
  if (culled()) {
    while (mask.x != 0u) {
      int k = findLSB(mask.x);
      mask.x &= mask.x - 1u;
      sum += tileLight(k, C, n, r2, spec);
    }
    while (mask.y != 0u) {
      int k = findLSB(mask.y);
      mask.y &= mask.y - 1u;
      worldLight(k, C, n, sum, sum_added, spec);
    }
  } else {
    for (int k = 0; k < num_lights; k++) {
      if (lightPower(k).a <= 0.0) {
        break;
      }
      sum += tileLight(k, C, n, r2, spec);
    }
    // fixed bound: a uniform-bounded loop around shadow_lamp broke the NVIDIA compiler elsewhere
    for (int k = 0; k < 32; k++) {
      if (k >= num_world) {
        break;
      }
      worldLight(k, C, n, sum, sum_added, spec);
    }
  }
  if (lists() && all(equal(sum, vec3(0.0))) && all(equal(sum_added, vec3(0.0)))) {
    discard;  // adds exactly 0
  }
  // emissive surfaces keep their own colour
  float keep = 1.0 - smoothstep(em_ramp.x, em_ramp.y, emissive(p));
  sum = max(sum * intensity * keep, vec3(0.0));
  sum_added = max(sum_added * intensity * keep, vec3(0.0));
  if (lists() && all(equal(sum, vec3(0.0))) && all(equal(sum_added, vec3(0.0)))) {
    discard;
  }
  sum *= max_add / max(max_add + dot(sum, vec3(0.299, 0.587, 0.114)), 1e-4);
  sum += sum_added * (kAddedMax / max(kAddedMax + dot(sum_added, vec3(0.299, 0.587, 0.114)), 1e-4));
  // PS2 colour 0.5 is the unlit texture; part of the glow takes the surface's tint
  vec3 c = texelFetch(color_tex, p, 0).rgb;
  vec3 tint = mix(vec3(1.0), clamp(c * 2.0, 0.0, 1.5), albedo_mix);
  vec3 add = sum * tint;
  if (hl_m.x > 0.0) {
    // metals (high F0) take the surface's hue, dielectrics the light's; soft limit on its own,
    // on the brightest channel so a saturated light never overshoots in one channel
    spec = max(spec * (intensity * keep * llh.x), vec3(0.0));
    spec *= llh.y / (llh.y + max(spec.r, max(spec.g, spec.b)));
    vec3 hue = c / max(max(c.r, max(c.g, c.b)), 1e-3);
    add += spec * mix(vec3(1.0), hue, smoothstep(0.1, 0.35, hl_s));
  }
  // an FP16 target keeps NaN/inf (RGBA8 blending clamps them away); never add either
  color = vec4(any(isnan(add)) || any(isinf(add)) ? vec3(0.0) : add, 0.0);
}
