// actor shadow maps (Jak 1), see ActorShadows.h; tex_T12 sun, tex_T13/14 lamps
uniform int shadow_on;
uniform int shadow_view;
uniform float shadow_strength;
uniform float shadow_sun_gain;  // the world map's day and elevation fade: no sun, no sun shadow
uniform sampler2DShadow tex_T12;
uniform sampler2DShadow tex_T13;
uniform sampler2DShadow tex_T14;
uniform mat4 shadow_mtx_sun;
uniform mat4 shadow_mtx_lamp[2];
uniform int shadow_nlamps;         // local lights with a map (the first ones)
uniform int shadow_nlights;        // local lights lighting merc
uniform vec4 shadow_lamp_pos[4];  // w: falloff radius
uniform vec4 shadow_lamp_col[4];  // rgb: power; w: cut-off radius
uniform float shadow_lamp_added[2];  // 1: a light the bake lacks; the tile pass shadows it
uniform float shadow_light_added[4];  // the same for each local light lighting merc
uniform vec2 shadow_texel;
uniform int shadow_taps;            // PCF taps per lookup: 1, 4 or 9 (shadow quality)
uniform float shadow_reach;         // sun map depth units: casters farther than this cast nothing
uniform vec4 shadow_marker_pos;     // landing marker under the player, w: radius (0 = none)
uniform float shadow_marker_strength;
uniform sampler2D shadow_caster_tex;  // unit 44, sun map colour: 1 - the nearest caster's cover
uniform int shadow_skip;       // perf opt: receivers skip taps that cannot see a caster
uniform sampler2D tex_T15;     // min sun-map depth per 32-texel cell, PCF footprint included
uniform float shadow_hue_t;    // sky-hued shadows strength, 0 = neutral (ActorShadows.cpp)
uniform vec3 shadow_hue_lgt;   // live fitted sun and ambient colours, luminance 1
uniform vec3 shadow_hue_amb;

// tap i in texels: a 3x3 grid, a 2x2 grid at half-texel offsets, or the centre
vec2 shadow_tap(int i) {
  if (shadow_taps >= 9) {
    return vec2(float(i % 3 - 1), float(i / 3 - 1));
  }
  return shadow_taps >= 4 ? vec2(float(i % 2), float(i / 2)) - 0.5 : vec2(0.0);
}

float shadow_pcf(sampler2DShadow m, vec3 t, float texel) {
  if (any(lessThan(t, vec3(0.0))) || any(greaterThan(t, vec3(1.0)))) {
    return 0.0;
  }
  int n = shadow_taps;
  float lit = 0.0;
  for (int i = 0; i < 9; i++) {
    if (i >= n) break;
    lit += texture(m, vec3(t.xy + shadow_tap(i) * texel, t.z));
  }
  return 1.0 - lit / float(max(n, 1));
}

// sun occlusion by casters within reach of the receiver
float shadow_sun(vec3 p, float bias) {
  if (shadow_sun_gain <= 0.0) {
    return 0.0;
  }
  vec3 t = (shadow_mtx_sun * vec4(p, 1.0)).xyz * 0.5 + 0.5;
  t.z -= bias;
  if (any(lessThan(t, vec3(0.0))) || any(greaterThan(t, vec3(1.0)))) {
    return 0.0;
  }
  // every texel under the taps is at or behind the receiver: all taps lit, occ exactly 0
  if (shadow_skip != 0 && texture(tex_T15, t.xy).r > t.z + 1e-6) {
    return 0.0;
  }
  int n = shadow_taps;
  float occ = 0.0;
  for (int i = 0; i < 9; i++) {
    if (i >= n) break;
    vec2 uv = t.xy + shadow_tap(i) * shadow_texel.x;
    float lit = texture(tex_T12, vec3(uv, t.z));
    float far_lit = texture(tex_T12, vec3(uv, t.z - shadow_reach));
    float o = max(far_lit - lit, 0.0);
    if (o > 0.0) {
      // a caster under a roof has no sun to block: the four texels this tap compared give
      // its sun share, so the penumbra fades with the core
      vec4 c = textureGather(shadow_caster_tex, uv);
      o *= min(min(c.x, c.y), min(c.z, c.w));
    }
    occ += o;
  }
  return shadow_sun_gain * occ / float(max(n, 1));
}

// soft disc straight below the player, fading with the drop height (about 20 m)
float shadow_marker(vec3 p) {
  if (shadow_marker_pos.w <= 0.0) {
    return 0.0;
  }
  vec3 d = p - shadow_marker_pos.xyz;
  float drop = -d.y;
  if (drop < -0.3 * 4096.0 || drop > 20.0 * 4096.0) {
    return 0.0;
  }
  float r = length(d.xz) / shadow_marker_pos.w;
  return shadow_marker_strength * (1.0 - smoothstep(0.4, 1.0, r)) * (1.0 - drop / (20.0 * 4096.0));
}

float shadow_lamp(int i, vec3 p) {
  vec4 c = shadow_mtx_lamp[i] * vec4(p, 1.0);
  if (c.w <= 0.0) {
    return 0.0;
  }
  vec3 t = c.xyz / c.w * 0.5 + 0.5;
  t.z -= 0.0005;
  return i == 0 ? shadow_pcf(tex_T13, t, shadow_texel.y) : shadow_pcf(tex_T14, t, shadow_texel.y);
}

// estimated lamp light luminance at p (inverse square with a smooth cut-off)
float shadow_lamp_light(int i, vec3 p) {
  vec3 d = p - shadow_lamp_pos[i].xyz;
  float d2 = dot(d, d);
  float r0 = shadow_lamp_pos[i].w;
  float rc = shadow_lamp_col[i].w;
  float win = clamp(1.0 - d2 / (rc * rc), 0.0, 1.0);
  return dot(shadow_lamp_col[i].rgb, vec3(0.299, 0.587, 0.114)) * win * win / (1.0 + d2 / (r0 * r0));
}

// fraction of the lamps' share of the receiver's light that actors block; the estimate is
// doubled and capped at half the light, so the shadow reads soft but visible
float shadow_lamps_removed(vec3 p, float total_lum) {
  float r = 0.0;
  for (int i = 0; i < 2; i++) {
    if (i < shadow_nlamps && shadow_lamp_added[i] < 0.5) {
      float share = clamp(2.0 * shadow_lamp_light(i, p) / max(total_lum, 0.05), 0.0, 0.5);
      if (shadow_skip == 0 || share > 0.0) {
        r += share * shadow_lamp(i, p);
      }
    }
  }
  return r;
}

const vec3 kLocalTint[4] = vec3[4](vec3(1.0, 0.2, 0.2), vec3(0.2, 1.0, 0.2), vec3(0.3, 0.4, 1.0),
                                   vec3(1.0, 1.0, 0.2));

// debug view 4 on the background: grey scene, tinted where a local-light map covers it
// (map 0 red, map 1 green), black where an actor occludes that light
vec3 shadow_local_debug_bg(vec3 p, vec3 rgb) {
  vec3 c = vec3(dot(rgb, vec3(0.299, 0.587, 0.114))) * 0.6;
  for (int i = 0; i < 2; i++) {
    if (i < shadow_nlamps) {
      vec4 h = shadow_mtx_lamp[i] * vec4(p, 1.0);
      vec3 t = h.xyz / max(h.w, 1e-6) * 0.5 + 0.5;
      if (h.w > 0.0 && all(greaterThanEqual(t, vec3(0.0))) && all(lessThanEqual(t, vec3(1.0)))) {
        c = mix(c, kLocalTint[i], 0.3) * (1.0 - shadow_lamp(i, p));
      }
    }
  }
  return c;
}

// light the local lights add to a merc vertex at p (merc camera space, where shadow_cnrm already
// points outward), each through its map; dbg: per-light luminance in the view-4 tints
vec3 shadow_actor_lights(vec3 p, vec3 nrm, out vec3 dbg) {
  vec3 n = normalize(nrm);
  vec3 sum = vec3(0.0);
  vec3 sum_added = vec3(0.0);
  dbg = vec3(0.0);
  for (int i = 0; i < 4; i++) {
    if (i < shadow_nlights) {
      vec3 d = shadow_lamp_pos[i].xyz - p;
      float d2 = dot(d, d);
      float r0 = shadow_lamp_pos[i].w;
      float rc = shadow_lamp_col[i].w;
      float win = clamp(1.0 - d2 / (rc * rc), 0.0, 1.0);
      // wrapped N.L: light reaches a little past the terminator
      float ndl = max((dot(n, d * inversesqrt(max(d2, 1.0))) + 0.5) / 1.5, 0.0);
      // normal offset against self-shadow acne (about 6 cm)
      float vis = i < shadow_nlamps ? 1.0 - shadow_lamp(i, p + n * 256.0) : 1.0;
      vec3 add = shadow_lamp_col[i].rgb * (ndl * vis * win * win / (1.0 + d2 / (r0 * r0)));
      if (shadow_light_added[i] > 0.5) {
        sum_added += add;
      } else {
        sum += add;
      }
      dbg += kLocalTint[i] * dot(add, vec3(0.299, 0.587, 0.114));
    }
  }
  // soft limit, so stacked lights never wash a character out; lights the bake lacks: x2.5 for
  // merc's vertex colour (0.5 = as drawn, Jak's run lower), soft limit 1.5
  sum_added *= 2.5;
  return sum * (0.6 / (0.6 + dot(sum, vec3(0.299, 0.587, 0.114)))) +
         sum_added * (1.5 / (1.5 + dot(sum_added, vec3(0.299, 0.587, 0.114))));
}

// per-channel weight (luminance 1) of a removed sun share s: what stays has the ambient's hue;
// shadow_hue_t blends it from grey (0) to the fitted sun/ambient split (1)
vec3 shadow_hue(float s) {
  const vec3 kY = vec3(0.299, 0.587, 0.114);
  vec3 w = s * shadow_hue_lgt / max((1.0 - s) * shadow_hue_amb + s * shadow_hue_lgt, vec3(1e-4));
  w = max(mix(vec3(1.0), w / max(dot(w, kY), 1e-4), shadow_hue_t), vec3(0.0));
  return w / max(dot(w, kY), 1e-4);
}

// removal weights u rescaled so the pixel c loses exactly k of its luminance, then pulled toward
// grey just enough that no channel loses more than all of it (k * u <= 1)
vec3 shadow_fit(vec3 u, float k, vec3 c) {
  const vec3 kY = vec3(0.299, 0.587, 0.114);
  float lc = dot(c, kY);
  if (lc > 1e-4) {
    u *= lc / max(dot(u * c, kY), 1e-4);
  }
  float mx = max(u.r, max(u.g, u.b));
  float a = mx > 1.0 ? clamp((1.0 / max(k, 1e-4) - 1.0) / (mx - 1.0), 0.0, 1.0) : 1.0;
  return mix(vec3(1.0), u, a);
}

// background receiver multiplier: k removed share, ks its sun part, m the landing disc,
// s the sun share, c the receiver's colour
vec3 shadow_keep(float k, float ks, float m, float s, vec3 c) {
  if (shadow_hue_t <= 0.0) {
    return vec3(1.0 - clamp(k, 0.0, 1.0)) * (1.0 - m);
  }
  if (k <= 0.0 && m <= 0.0) {
    return vec3(1.0);
  }
  float kc = clamp(k, 0.0, 1.0);
  vec3 h = shadow_hue(max(s, 1e-3));
  // lamps and the disc follow the sky only where the sun reaches (outdoors)
  vec3 ho = mix(vec3(1.0), h, smoothstep(0.02, 0.2, s));
  vec3 u = k > 0.0 ? (min(ks, k) * h + max(k - ks, 0.0) * ho) / k : vec3(1.0);
  return (1.0 - kc * shadow_fit(u, kc, c)) * (1.0 - m * shadow_fit(ho, m, c));
}

WORLD_SHADOW_FRAG
CANOPY_FRAG
