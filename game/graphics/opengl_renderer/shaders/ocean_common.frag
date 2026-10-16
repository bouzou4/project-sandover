#version 410 core

layout(location = 0) out vec4 color;
layout(location = 2) out vec4 emissive_out;  // only attached while the water glint writes it

centroid in vec4 fragment_color;
centroid in vec3 tex_coord;
uniform float color_mult;
uniform float alpha_mult;

uniform vec4 fog_color;
uniform int bucket;

centroid in float fog;

uniform sampler2D tex_T0;

// ocean shading (Materials.h water_setup_ocean); water_on 0 is the vanilla path
uniform int water_on;
uniform int water_sun_ok;
uniform vec3 water_sun_dir;  // world, toward the light
uniform vec3 water_sun_col;
uniform vec4 water_params;   // texture ripple, glint, glint exponent, fresnel
uniform float water_sparkle;
uniform int water_view;
uniform vec4 water_disp;  // amplitude sum (m), zero-amplitude range (m), subsurface, crest light
uniform vec4 water_foam;  // crest foam, crest Jacobian threshold, shore foam, shore band (m)
uniform vec4 water_tint;  // shallow colour strength, shallow depth (m), absorption (1/m),
                          // depth (m) by which the tint is gone and the water opaque
uniform float water_depth_blur;  // the shallow tint reads the depth as the mean over this radius (m)
uniform vec3 water_env_sky;     // mood environment (Materials.cpp compute_frame_lights), mean 1
uniform vec3 water_env_hor;
uniform vec3 water_env_sun;     // broad glow around the sun (or key) direction
uniform vec3 water_env_sun_dir;
uniform vec2 water_sky;         // reflection weight, haze distance (m)
centroid in vec2 water_gs;
centroid in vec3 water_rest;
centroid in float water_edge;
centroid in float water_band;
// window depth at the centroid: gl_FragCoord.z is at the pixel centre, which under MSAA can lie
// outside a thin or edge-on triangle, extrapolated far along it
noperspective centroid in float water_wz;
// merged envmap pass (perf bit 14): bucket 3 also adds what bucket 4 would blend onto it
centroid in vec3 env_tex_coord;
centroid in vec4 env_color;
uniform int water_env_merge;  // 1 fp16 target, 2 unorm target (clamped as stored)
uniform sampler2D tex_env;
uniform vec4 water_tiling;  // hex tiling 0..1, 1 / grid spacing (1/m), largest rotation (rad),
                            // 1 / the game's texture repeat (1/m)
uniform vec4 water_aa;  // detail fade start, end (log2 texels or noise cells per pixel), foam
                        // edge softness (pixels)
uniform vec4 water_rim;  // mid ocean's outer rim box (m): x0, x1, z0, z1; none when x1 < x0
float water_e = 0.;
uniform vec4 water_lod;  // mid ocean detail fade start, end (m); z 1 with perf-opt bit 27
float water_k = 1.;  // mid ocean detail kept at the pixel: 1 to the fade start, 0 past its end
// cost probe (water option 101): mid ocean terms off past (value >> 12) m, mask in the low bits;
// 2048 is the distance band view
uniform int water_lod_probe;
int water_lp = 0;

WATER_WAVES

WORLD_SHADOW_FRAG

WATER_SUN_GLINT

float water_lum(vec3 c) {
  return dot(c, vec3(0.299, 0.587, 0.114));
}

float water_hash(vec2 p) {
  uvec2 q = uvec2(ivec2(p)) * uvec2(1597334673u, 3812015801u);
  return float((q.x ^ q.y) * 1597334673u) * (1. / 4294967295.);
}

// ocean texture lookups of the shading: the game's uv, or hex tiling (Mikkelsen 2022) in world
// space, three rotated and offset samples blended on a triangle grid (water_hex_setup)
vec2 water_uv;    // the game's uv
float water_hk = 0.;  // 0 the game's uv only, 1 hex tiling only
vec2 water_huv;   // world uv, one unit per texture repeat
vec2 water_hdx;
vec2 water_hdy;
vec2 water_gdx;
vec2 water_gdy;
bool water_rest_ok = false;
mat2 water_hr[3];
vec2 water_ho[3];
float water_hw[3];

void water_hex_setup(vec2 xz, bool ok) {
  water_huv = xz * water_tiling.w;
  water_hdx = dFdx(water_huv);
  water_hdy = dFdy(water_huv);
  water_gdx = dFdx(water_uv);
  water_gdy = dFdy(water_uv);
  water_rest_ok = ok;
  if (!ok) {
    return;
  }
  water_hk = water_tiling.x * smoothstep(0., 1., water_band);
  vec2 s = xz * water_tiling.y;
  vec2 k = vec2(s.x - 0.57735027 * s.y, 1.15470054 * s.y);
  vec2 b = floor(k);
  vec2 f = k - b;
  float z = 1. - f.x - f.y;
  float up = z < 0. ? 1. : 0.;
  float sg = 2. * up - 1.;
  vec3 wt = pow(max(vec3(-z * sg, up - f.y * sg, up - f.x * sg), 0.), vec3(5.));
  wt /= max(wt.x + wt.y + wt.z, 1e-6);
  vec2 v[3] = vec2[3](b + vec2(up), b + vec2(up, 1. - up), b + vec2(1. - up, up));
  for (int i = 0; i < 3; i++) {
    float r = water_tiling.z * (2. * water_hash(v[i]) - 1.);
    float cr = cos(r);
    float sr = sin(r);
    water_hr[i] = mat2(cr, sr, -sr, cr);
    water_ho[i] = vec2(water_hash(v[i] + vec2(17., 5.)), water_hash(v[i] + vec2(3., 29.)));
    water_hw[i] = wt[i];
  }
}

/// Ocean texture at M * uv + c + o.
vec4 water_tex(mat2 M, vec2 c, vec2 o) {
  if (water_tiling.x <= 0.) {
    return texture(tex_T0, M * water_uv + c + o);
  }
  vec4 t = vec4(0.);
  if (water_hk < 1.) {
    t = textureGrad(tex_T0, M * water_uv + c + o, M * water_gdx, M * water_gdy);
  }
  if (water_hk > 0.) {
    vec2 p = M * water_huv + c + o;
    vec2 dx = M * water_hdx;
    vec2 dy = M * water_hdy;
    vec4 h = vec4(0.);
    for (int i = 0; i < 3; i++) {
      h += water_hw[i] * textureGrad(tex_T0, water_hr[i] * p + water_ho[i], water_hr[i] * dx,
                                     water_hr[i] * dy);
    }
    t = mix(t, h, water_hk);
  }
  return t;
}

float water_h(mat2 M, vec2 c, vec2 o) {
  return water_lum(water_tex(M, c, o).rgb);
}

float water_crest(mat2 M, vec2 c, vec2 d) {
  vec2 e = d * 3.;
  float blur = 0.25 * (water_h(M, c, vec2(e.x, e.y)) + water_h(M, c, -vec2(e.x, e.y)) +
                       water_h(M, c, vec2(e.x, -e.y)) + water_h(M, c, -vec2(e.x, -e.y)));
  return smoothstep(0.03, 0.12, water_h(M, c, vec2(0.)) - blur);
}

/// 1 where detail at `lod` (log2 of texels or noise cells per pixel) is resolved, 0 once it
/// would alias.
float water_detail(float lod) {
  return 1. - smoothstep(water_aa.x, water_aa.y, lod);
}

/// Ocean texture lod of the pixel, from the widest axis of its footprint.
float water_tex_lod() {
  vec2 ts = vec2(textureSize(tex_T0, 0));
  vec2 dx = (water_rest_ok ? water_hdx : water_gdx) * ts;
  vec2 dy = (water_rest_ok ? water_hdy : water_gdy) * ts;
  return 0.5 * log2(max(max(dot(dx, dx), dot(dy, dy)), 1e-12));
}

float water_noise(vec2 p) {
  vec2 i = floor(p);
  vec2 f = fract(p);
  vec2 u = f * f * (3. - 2. * f);
  return mix(mix(water_hash(i), water_hash(i + vec2(1., 0.)), u.x),
             mix(water_hash(i + vec2(0., 1.)), water_hash(i + vec2(1., 1.)), u.x), u.y);
}

struct Water {
  vec3 ray;    // world direction of the pixel, per unit of view depth
  vec3 N;      // Gerstner normal
  vec2 xz;     // rest position (m)
  float J;     // Jacobian of the horizontal displacement (< 1 where the surface bunches up)
  float h;     // height over the amplitude sum, -1..1
  float dist;  // horizontal distance to the camera (m)
  float depth; // water depth under the surface (m, vertical)
  float path;  // length of the pixel ray under the surface (m)
  float deep;  // 0 at the waterline, 1 from the opaque depth on (and with nothing behind)
  float near;  // 1 close to the camera, 0 past the shore fade
  float tint;  // depth for the shallow tint: `depth` averaged over the blur ring (m)
};

/// Water depth (m) over a ring of `r` pixels: an underwater silhouette (a rock face close
/// behind one pixel, the far seabed behind the next) no longer cuts the tint in a hard line.
float water_depth_mean(float vz, float d0, float r, float ray_y) {
  ivec2 sz = textureSize(water_depth, 0) - 1;
  float sum = d0, n = 1.;
  for (int i = 0; i < 8; i++) {
    float a = 0.78539816 * float(i);
    ivec2 p = clamp(ivec2(gl_FragCoord.xy + r * vec2(cos(a), sin(a))), ivec2(0), sz);
    float ds = texelFetch(water_depth, p, 0).r;
    float dz = water_viewz(ds) - vz;
    if (ds <= 1e-7 || isinf(dz) || isnan(dz)) {
      sum += water_tint.w;  // nothing behind: as deep as the tint goes
      n += 1.;
    } else if (dz >= 0.) {  // skips posts and floats in front of the water
      sum += min(dz / 4096. * max(-ray_y, 0.), water_tint.w);
      n += 1.;
    }
  }
  return sum / n;
}

Water water_geom() {
  // derivatives before any branch; a rest point behind the camera gives 0 (no fade)
  vec2 rxz = water_rest.z > 0. ? water_rest.xy / water_rest.z : vec2(0.);
  water_xz_dx = dFdx(rxz);
  water_xz_dy = dFdy(rxz);
  Water w;
  w.ray = water_rot * vec3((water_gs - water_proj.xy) * water_proj.zw, 1.);
  w.N = vec3(0., 1., 0.);
  w.xz = vec2(0.);
  w.J = 1.;
  w.h = 0.;
  w.dist = 1e4;
  if (water_disp_on != 0 && water_rest.z > 0.) {
    w.xz = water_rest.xy / water_rest.z;
    w.dist = length(w.xz - water_eye.xz / 4096.);
    if (bucket == 3 && water_lod.z > 0.) {
      water_k = 1. - smoothstep(water_lod.x, water_lod.y, w.dist);
      water_chop_k = water_k;
    }
    if (w.dist < float(water_lod_probe >> 12)) {
      water_lp = 0;
    }
  }
  if (water_disp_on != 0 && water_rest.z > 0. && (water_lp & 128) == 0) {
    vec3 D, ddx, ddz;
    water_gerstner(w.xz, w.dist, water_edge, D, ddx, ddz);
    w.N = normalize(cross(vec3(0., 0., 1.) + ddz, vec3(1., 0., 0.) + ddx));
    w.J = (1. + ddx.x) * (1. + ddz.z) - ddz.x * ddx.z;
    w.h = D.y / max(water_disp.x, 1e-4);
  }
  w.depth = 1e4;
  w.path = 1e4;
  w.tint = 1e4;
  w.near = 0.;
  if (water_depth_ok != 0) {
    float ds = texelFetch(water_depth, ivec2(gl_FragCoord.xy), 0).r;
    float vz = water_viewz(water_wz);
    float dz = water_viewz(ds) - vz;
    // nothing behind the surface (cleared depth, sky, far) or a non-finite delta: max depth
    if (ds > 1e-7 && !isinf(dz) && !isnan(dz)) {
      dz = clamp(dz, 0., 4096. * 1e4);
      w.depth = dz / 4096. * max(-w.ray.y, 0.);
      w.path = dz / 4096. * length(w.ray);
    }
    w.near = 1. - smoothstep(water_shore_fade.x, water_shore_fade.y,
                             vz / 4096. * length(w.ray.xz));
    float px = water_depth_blur / max(length(water_xz_dx), 1e-4);
    w.tint = w.depth;
    // the tint is only read by water_shade, weighted by near
    bool tint_used = (bucket == 0 || bucket == 3) && w.near > 0.;
    if (water_depth_blur > 0. && px > 1. && (tint_used || (perf_opt & (1u << 12)) == 0u)) {
      w.tint = water_depth_mean(vz, min(w.depth, water_tint.w), min(px, 32.), w.ray.y);
    }
  }
  w.deep = smoothstep(0., water_tint.w, w.depth);
  return w;
}

/// Crest foam (Jacobian) and shoreline foam (scene depth), cut from an animated noise.
float water_foam_mask(Water w) {
  vec2 fx = dFdx(w.xz);
  vec2 fy = dFdy(w.xz);
  float fp = sqrt(max(dot(fx, fx), dot(fy, fy)));  // metres per pixel
  // octaves finer than the pixel fade out; the rest are rescaled to keep the foam's coverage,
  // by no more than the coarse octave alone needs
  vec3 a = vec3(0.55, 0.3, 0.15) * vec3(water_detail(log2(max(2.3 * fp, 1e-6))),
                                        water_detail(log2(max(5.9 * fp, 1e-6))),
                                        water_detail(log2(max(13.1 * fp, 1e-6))));
  vec3 o = vec3(water_noise(w.xz * 2.3 + vec2(0.13, 0.07) * water_time),
                water_noise(w.xz * 5.9 - vec2(0.05, 0.11) * water_time),
                water_noise(w.xz * 13.1 + vec2(0.09, -0.04) * water_time)) - 0.5;
  float n = 0.5 + dot(a, o) * min(0.6442049 / max(length(a), 1e-4), 1.1712817);
  float c = (1. - smoothstep(water_foam.y - 0.25, water_foam.y, w.J)) * smoothstep(0.2, 0.7, w.h);
  // none in the first metres, where the noise cells would fill the screen
  c *= smoothstep(1.5, 6., w.dist) * (1. - smoothstep(0.4 * water_disp.y, water_disp.y, w.dist));
  float cc = 1. - 0.6 * c;
  // edges widened by the pixel's change in the noise and in the threshold itself
  float aa = 0.5 * water_aa.z * fwidth(n - cc);
  float crest = water_foam.x * smoothstep(cc - aa, cc + 0.06 + aa, n);
  float r = w.depth / max(min(water_foam.w, water_tint.w), 1e-3);
  float band = 0.5 + 0.5 * sin(r * 12.566 + water_time * 1.8);
  float cut = (1. - clamp(r, 0., 1.)) * (0.55 + 0.45 * band);
  float sa = 0.5 * water_aa.z * fwidth(n + cut);
  float shore = max(smoothstep(1. - cut - sa, 1.08 - cut + sa, n), 1. - smoothstep(0.05, 0.2, r));
  shore *= water_foam.z * w.near;
  return clamp(max(crest, shore), 0., 1.);
}

/// The sun glow the reflection adds along R (water_reflection's `glow`).
float water_sun_glow(vec3 N, vec3 V) {
  return pow(max(dot(reflect(-V, N), normalize(water_env_sun_dir)), 0.), 24.);
}

/// What the fresnel term reflects: the mood environment along R, at the fog colour's brightness
/// on the horizon, hazing into the fog colour with distance; the fog colour alone at weight 0.
vec3 water_reflection(vec3 N, vec3 V, float dist, float open, float glow) {
  vec3 fogc = fog_color.rgb;
  vec3 R = reflect(-V, N);
  // below the horizon a wave reflects other water, which reads as horizon
  vec3 e = mix(water_env_hor, water_env_sky, smoothstep(0., 0.48, max(R.y, 0.)));
  e += water_env_sun * (open * glow);
  e *= water_lum(fogc) / max(water_lum(water_env_hor), 1e-3);
  e = mix(min(e, vec3(0.97)), fogc, 1. - exp(-dist / water_sky.y));
  return mix(fogc, e, water_sky.x);
}

/// Gerstner normal with the texture ripples, shallow colour, crest light, sun light, glint,
/// crest sparkle, subsurface tint, fresnel toward the sky reflection, then foam.
vec3 water_shade(vec3 rgb, Water w, float foam) {
  // the pixel ray on the water plane (world) for the sun map, derivatives before any branch
  vec3 wsp = water_eye.xyz + w.ray * ((water_eye.w - water_eye.y) / min(w.ray.y, -1e-4));
  ws_begin(wsp);
  vec2 d = 1.5 / vec2(textureSize(tex_T0, 0));
  const mat2 I = mat2(1.);
  // the texture ripples and sparkle fade where the texture is finer than the pixel, and with
  // the mid ocean's distance
  float detail = water_detail(water_tex_lod()) * water_k;
  // taps skipped at weight 0; the game-uv path keeps them (implicit derivatives)
  bool taps = detail > 0. || water_tiling.x <= 0.;
  vec2 g = vec2(0.);
  if (taps && (water_lp & 32) == 0) {
    g = vec2(water_h(I, vec2(0.), vec2(d.x, 0.)) - water_h(I, vec2(0.), -vec2(d.x, 0.)),
             water_h(I, vec2(0.), vec2(0., d.y)) - water_h(I, vec2(0.), -vec2(0., d.y)));
  }
  vec3 N = normalize(w.N + vec3(-g.x, 0., -g.y) * 8. * water_params.x * detail);
  float glint_var =
      sun_glint.w > 0.5 && (water_lp & 2) == 0 ? wsg_pixel_var(N) + water_slope_lost : 0.;
  vec3 V = -normalize(w.ray);
  if (V.y < 0.) {
    V = -V;  // camera below the surface or a flipped projection sign
  }
  float ndv = max(dot(N, V), 0.);
  float F = 0.02 + 0.98 * pow(1. - ndv, 5.);
  float f = water_params.w * F;

  float lum = water_lum(rgb);
  vec3 shallow_col = min(mix(vec3(lum), rgb, 1.25) * 1.9 + 0.02, vec3(0.95));
  // ends at the opaque depth, above where the terrain meshes stop, so their edge never shows
  float sh = water_tint.x * exp(-w.tint / water_tint.y) *
             (1. - smoothstep(0., water_tint.w, w.tint)) * w.near;
  vec3 col = mix(rgb, shallow_col, sh);
  col *= 1. + water_disp.w * clamp(w.h, -1., 1.) * 0.5;

  vec3 glint = vec3(0.);
  vec3 sun_g = vec3(0.);  // analytic glint, linear
  vec3 lit = col;
  float sss = water_disp.z * max(w.h, 0.) * 0.35;
  vec3 sun = vec3(1.);
  float fl = 0.85;
  float open = 1.;  // the world sun map's light here: it takes every sun term below
  float glow = (water_lp & 4) == 0 ? water_sun_glow(N, V) : 0.;
  if (water_sun_ok != 0) {
    vec3 L = normalize(water_sun_dir);
    vec3 H = normalize(L + V);
    float up = smoothstep(0., 0.12, L.y);
    float ndh = max(dot(N, H), 0.);
    vec3 sun_g0 = vec3(0.);
    if (sun_glint.w > 0.5 && (water_lp & 2) == 0) {
      vec3 Lg = dot(sun_glint_dir, sun_glint_dir) > 0.25 ? normalize(sun_glint_dir) : L;
      sun_g0 = wsg_radiance(N, V, Lg, sun_glint_col, vec3(0., 1., 0.), glint_var);
    }
    // every term `open` scales is 0 otherwise, so the lookup cannot change the colour
    bool map_counts = up > 0. || glow > 0. || any(notEqual(sun_g0, vec3(0.)));
    if ((water_lp & 1) == 0 && map_counts && w.ray.y < 0. && water_eye.y > water_eye.w) {
      open = wsg_sun_vis(wsp);
    }
    lit = col * max(1. + 1.2 * up * open * (dot(N, L) - L.y), 0.3);
    float crest = 0.;
    // the sparkle is scaled by up * open: no crest taps where the sun is down or shadowed;
    // the game-uv path keeps them (implicit derivatives)
    bool lit_sun = up * open > 0. || (perf_opt & (1u << 27)) == 0u || water_tiling.x <= 0.;
    if (taps && lit_sun && (water_lp & 64) == 0) {
      crest = water_crest(I, vec2(0.), d);
      crest = max(crest, 0.6 * water_crest(mat2(1.6, 1.2, -1.2, 1.6), vec2(0.37), d));
    }
    float sparkle = water_sparkle * detail * crest * (0.35 + pow(ndh, 8.));
    // the analytic glint replaces the Blinn one; the crest sparkle stays
    float blinn = sun_glint.w > 0.5 ? 0. : water_params.y * pow(ndh, water_params.z);
    glint = water_sun_col * (up * open * (blinn + sparkle));
    sun_g = sun_g0 * open;
    // light through the wave tops, strongest looking toward the sun
    float back = pow(max(dot(-V, L), 0.), 3.) * up * open;
    sss = water_disp.z * max(w.h, 0.) * (0.35 + 1.4 * back);
    sun = min(water_sun_col, vec3(1.5));
    fl = 0.75 + 0.25 * max(dot(N, L), 0.) * up * open;
  }
  lit += shallow_col * sun * sss * 0.6;
  lit = min(lit, vec3(0.97));
  vec3 refl = (water_lp & 8) == 0 ? water_reflection(N, V, w.dist, open, glow) : fog_color.rgb;
  lit = mix(lit, refl, f);
  // foam is bright but never emissive
  vec3 foam_col = vec3(0.93, 0.96, 0.97) * clamp(lum * 3.5, 0.4, 1.) * fl;
  lit = mix(lit, foam_col, foam);
  glint *= 1. - foam;
  sun_g *= 1. - foam;
  water_e = clamp(max(glint.r, max(glint.g, glint.b)), 0., 1.);
  // a little bloom from the sun glint, never a glare sheet
  water_e = max(water_e, 0.25 * clamp(water_lum(sun_g), 0., 1.));

  if (water_view == 1) {
    return N * 0.5 + 0.5;
  } else if (water_view == 2) {
    return glint + sun_g;
  } else if (water_view == 3) {
    return vec3(f);
  } else if (water_view == 4) {
    return V * 0.5 + 0.5;
  } else if (water_view == 5) {
    return normalize(water_sun_dir) * 0.5 + 0.5;
  } else if (water_view == 6) {
    return vec3(foam);
  } else if (water_view == 7) {
    return vec3(w.deep, w.near, 0.);
  } else if (water_view == 8) {
    return vec3(w.h * 0.5 + 0.5, clamp(w.J, 0., 1.), 0.);
  } else if (water_view == 9) {
    return vec3(detail, clamp(water_tex_lod() * 0.25 + 0.5, 0., 1.), 0.);
  } else if (water_view == 19) {
    float wv = ws_water_vis(wsp);  // the sun the world map leaves; blue: no cascade
    return wv < 0. ? vec3(0., 0., 1.) : vec3(wv);
  }
  return sun_glint.w > 0.5 ? wsg_add(lit + glint, sun_g) : lit + glint;
}

/// 1 where the mid ocean keeps its shading, falling to 0 at its outer rim: from 80% of the way
/// to the rim along the pixel's bearing, so the step to the vanilla ocean-far is a gradient.
float water_rim_keep(vec3 ray) {
  if (water_rim.y < water_rim.x || ray.y >= 0.) {
    return 1.;
  }
  vec2 e = water_eye.xz / 4096.;
  vec2 p = e + ray.xz * ((water_eye.w - water_eye.y) / 4096. / ray.y);
  vec2 d = p - e;
  float dist = length(d);
  if (dist < 1e-3) {
    return 1.;
  }
  d /= dist;
  vec2 lim = vec2(d.x > 0. ? water_rim.y : water_rim.x, d.y > 0. ? water_rim.w : water_rim.z);
  vec2 t = (lim - e) / max(abs(d), vec2(1e-6)) * sign(d);
  float exit = min(abs(d.x) > 1e-6 ? t.x : 1e9, abs(d.y) > 1e-6 ? t.y : 1e9);
  return exit > 0. ? 1. - smoothstep(0.8 * exit, exit, dist) : 1.;
}

void main() {
  water_uv = tex_coord.xy / tex_coord.z;
  vec4 T0 = texture(tex_T0, water_uv);
  vec4 T0v = T0;
  Water w;
  float foam = 0.;
  // the envmap buckets read none of the shading
  bool env_only = (bucket == 2 || bucket == 4) && (perf_opt & (1u << 12)) != 0u;
  water_lp = bucket == 3 ? water_lod_probe : 0;
  if ((water_lp & 1024) != 0) {
    env_only = true;
  }
  if (water_on != 0 && !env_only) {
    w = water_geom();
    foam = (water_lp & 16) == 0 ? water_foam_mask(w) : 0.;
    // the texture lookups of the shading; the alpha bucket reads only the foam
    if (bucket != 1 || (perf_opt & (1u << 12)) == 0u) {
      water_hex_setup(w.xz, water_disp_on != 0 && water_rest.z > 0.);
      if ((water_lp & 256) != 0) {
        water_hk = 0.;
      }
    }
    if (bucket == 0 || bucket == 3) {
      T0 = water_tex(mat2(1.), vec2(0.), vec2(0.));
    }
  }
  if (bucket == 0) {
    color.rgb = fragment_color.rgb * T0.rgb;
    if (water_on != 0) {
      color.rgb = water_shade(color.rgb, w, foam);
    }
    color.a = fragment_color.a;
    if (water_on != 0) {
      color.a = mix(color.a, 1., foam);
      if (water_depth_ok != 0) {
        // absorption: the seabed fades into the level's ocean colour with depth
        float op = water_tint.z > 0. ? w.deep : 0.;  // absorb 0 keeps the vanilla alpha
        color.a = mix(color.a, 1., max(1. - exp(-water_tint.z * w.path), op));
      }
    }
    color.rgb = mix(color.rgb, fog_color.rgb, clamp(fog_color.a * fog, 0, 1));
  } else if (bucket == 1 || bucket == 2 || bucket == 4) {
    color = fragment_color * T0;
    if (water_on != 0 && bucket == 1) {
      color.a *= 1. - foam;  // less envmap on the foam
    }
  } else if (bucket == 3) {
    color = fragment_color * T0;
    if (water_on != 0 && (water_lp & 1024) == 0) {
      color.rgb = water_shade(color.rgb, w, foam);
      color.a *= 1. - foam;
      float keep = water_rim_keep(w.ray);
      color = mix(fragment_color * T0v, color, keep);
      water_e *= keep;
    }
    color.rgb = mix(color.rgb, fog_color.rgb, clamp(fog_color.a * fog, 0, 1));
  }
  if (bucket == 3 && water_env_merge != 0 && env_color.a > 0.5) {
    // the patch's envmap twin (alpha 1 marks one): bucket 4 blends (DST_ALPHA, ONE), alpha 0
    vec4 dst = water_env_merge == 2 ? clamp(color, 0., 1.) : color;
    vec3 env = env_color.rgb * texture(tex_env, env_tex_coord.xy / env_tex_coord.z).rgb;
    color = vec4(dst.rgb + env * dst.a, 0.);
  }
  if (water_on != 0 && water_view == 18) {
    color = vec4(bucket == 0 ? 1. : 0., bucket == 3 ? 1. : 0., water_edge, 1.);
  }
  if (water_on != 0 && water_view == 20) {
    // seam coverage: near red, mid green, opaque; the envmap passes add nothing
    color = bucket == 0 ? vec4(1., 0., 0., 1.) : bucket == 3 ? vec4(0., 1., 0., 1.)
                                                             : vec4(0., 0., 0., color.a);
  }
  if (water_on != 0 && water_view >= 10 && water_view <= 19) {
    // 10 envmap mask (the texture alpha buckets 1 and 3 write), 11 no envmap, 12 near ocean
    // opaque, 13 envmap only
    bool env = bucket == 1 || bucket == 2 || bucket == 4;
    if (water_view == 10 && (bucket == 0 || bucket == 3)) {
      color = vec4(vec3(bucket == 0 ? texture(tex_T0, water_uv).a : T0.a), 1.);
    } else if ((water_view == 10 || water_view == 11 || water_view == 19) && env) {
      color = bucket == 1 ? vec4(color.rgb, 0.) : vec4(0., 0., 0., color.a);
    } else if ((water_view == 12 && bucket == 0) || (water_view == 19 && bucket == 0)) {
      color.a = 1.;
    } else if (water_view == 13 && (bucket == 0 || bucket == 3)) {
      color.rgb = vec3(0.);
    } else if (water_view >= 14 && water_view <= 17) {
      // envmap factors, each alone (near: buckets 0-2, mid: 3-4): 14 envmap pass coverage,
      // 15 envmap texture, 16 destination alpha the envmap is scaled by, 17 envmap vertex colour
      vec3 f = water_view == 15 ? T0.rgb : water_view == 17 ? fragment_color.rgb : vec3(0.5);
      if (water_view == 16) {
        f = vec3(1.);
      }
      if (bucket == 0 || bucket == 3) {
        color = vec4(0., 0., 0., water_view == 16 ? color.a : 1.);
      } else if (bucket == 1 && water_view != 16) {
        color.a = 1.;
      } else if (bucket == 2) {
        color = vec4(f, 1.);
      } else if (bucket == 4) {
        color = vec4(f, color.a);  // keeps the mid's alpha-0 trick against the second mesh
      }
    }
  }
  if (water_on != 0 && (water_lod_probe & 2048) != 0 && (bucket == 0 || bucket == 3)) {
    // distance bands (m): 140 red, 280 yellow, 500 green, 900 cyan, 2000 blue, white; near magenta
    float dd = w.dist;
    vec3 bc = dd < 140. ? vec3(1., 0., 0.) : dd < 280. ? vec3(1., 1., 0.)
            : dd < 500. ? vec3(0., 1., 0.) : dd < 900. ? vec3(0., 1., 1.)
            : dd < 2000. ? vec3(0., 0., 1.) : vec3(1.);
    color = bucket == 0 ? vec4(1., 0., 1., 1.) : vec4(bc, 1.);
  }
  // reflected sun blooms but is no tile light (R = G): the sun already lights the shore
  float em = water_e * (1. - clamp(fog_color.a * fog, 0., 1.));
  emissive_out = vec4(em, em, 0., 1.);
}
