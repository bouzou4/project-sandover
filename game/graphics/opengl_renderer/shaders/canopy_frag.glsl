// Canopy shade (background/Canopy.h): light under overhead foliage. Spliced in by Shader.cpp at
// its include token (actor shadow receivers, wind TIE, generic); uniforms set by
// canopy_set_uniforms.
#ifndef SO_CANOPY
#define SO_CANOPY
uniform int shadow_canopy_n;           // map slots bound, 0-2
uniform sampler2DArray canopy_tex;     // r: cover (1 - transmittance), g: canopy height (0..1 of span)
uniform mat4 shadow_canopy_mtx;        // receiver space to world
uniform vec4 shadow_canopy_rect[2];    // x0, z0, uv per game unit in x, z
uniform vec2 shadow_canopy_h[2];       // height y0, span (game units)
uniform vec4 shadow_canopy_sun;        // toward the sun (world); w: strength x low-sun fade x day
uniform vec4 shadow_canopy_par;        // patch threshold, patch ramp width, penumbra per metre, patch boost
uniform vec4 shadow_canopy_amb;        // rgb: light kept in the shade under full cover (cool)

// Under the canopy at p: `sun` = 1 in a sun patch (leaf gap at the projection toward the sun,
// moving with the sun), 0 in the shade; `sky` = the shade's light per channel; `u` = how much
// canopy covers p (0: none). False where no map covers p.
bool shadow_canopy_light(vec3 p_in, out float sun, out vec3 sky, out float u) {
  sun = 1.0;
  sky = vec3(1.0);
  u = 0.0;
  if (shadow_canopy_n == 0) {
    return false;
  }
  vec3 p = (shadow_canopy_mtx * vec4(p_in, 1.0)).xyz;
  vec3 L = shadow_canopy_sun.xyz / max(shadow_canopy_sun.y, 0.1);
  float texel_lod0 = float(textureSize(canopy_tex, 0).x);
  float best = 0.0, c = 0.0, cm = 0.0;
  for (int i = 0; i < 2; i++) {
    if (i >= shadow_canopy_n) {
      break;
    }
    vec4 r = shadow_canopy_rect[i];
    vec2 h = shadow_canopy_h[i];
    if (r.z == 0.0) {
      continue;  // empty slot
    }
    float texel_gu = 1.0 / (r.z * texel_lod0);  // game units per texel
    vec2 uv0 = (p.xz - r.xy) * r.zw;
    if (any(lessThan(uv0, vec2(0.0))) || any(greaterThan(uv0, vec2(1.0)))) {
      continue;
    }
    // mean cover over 6 m above p first: no canopy, no further taps
    float mi = textureLod(canopy_tex, vec3(uv0, float(i)), log2(6.0 * 4096.0 / texel_gu)).r;
    if (mi < 1e-3) {
      continue;
    }
    float lod_h = log2(2.0 * 4096.0 / texel_gu);
    // toward the sun up to the (smoothed) canopy height under the point, then under that hit
    vec2 q = p.xz;
    float y = 0.0;
    for (int k = 0; k < 2; k++) {
      y = h.x + h.y * textureLod(canopy_tex, vec3((q - r.xy) * r.zw, float(i)), lod_h).g;
      q = p.xz + L.xz * max(y - p.y, 0.0);
    }
    vec2 uv = (q - r.xy) * r.zw;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
      continue;
    }
    // patches: cover over about a metre (more with distance to the leaves)
    float pen = max(1.0 * 4096.0, shadow_canopy_par.z * max(y - p.y, 0.0) * length(L));
    float ci = textureLod(canopy_tex, vec3(uv, float(i)), log2(pen / texel_gu)).r;
    float above = smoothstep(0.5 * 4096.0, 2.0 * 4096.0, y - p.y);
    if (mi * above > best) {
      best = mi * above;
      c = ci * above;
      cm = best;
    }
  }
  if (best <= 0.0) {
    return false;
  }
  u = smoothstep(0.05, 0.35, cm) * shadow_canopy_sun.w;
  sun = smoothstep(shadow_canopy_par.x, shadow_canopy_par.x + shadow_canopy_par.y, 1.0 - c);
  sky = shadow_canopy_amb.rgb;
  return true;
}

// Light multiplier under the canopy at p, the same for every surface whatever its texture or
// bake: shade (cooler, darker by option 12) with sun patches (brighter by option 16) through the
// leaf gaps, faded in by the cover above p; 1 outside the canopy. `sun_occ` (an actor sun
// shadow) is kept only inside the sun patches: in the canopy's shade there is no sun to block.
float shadow_canopy_sun_vis = 1.0;  // share of the sun through the leaves, from the last call

vec3 shadow_canopy_factor(vec3 p, inout float sun_occ) {
  float sun, u;
  vec3 sky;
  shadow_canopy_sun_vis = 1.0;
  if (!shadow_canopy_light(p, sun, sky, u)) {
    return vec3(1.0);
  }
  shadow_canopy_sun_vis = mix(1.0, sun, u);
  sun_occ *= shadow_canopy_sun_vis;
  // patches: today's light plus a warm boost (more red, less blue)
  return mix(vec3(1.0), mix(sky, vec3(1.0) + shadow_canopy_par.w * vec3(1.3, 1.0, 0.5), sun), u);
}
#endif
