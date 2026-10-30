PBR_COMMON_FRAG
uniform int pbr_sun_ok;    // the level has a blended day sun
uniform vec3 pbr_sun_dir;  // world, toward the light
uniform vec3 pbr_sun_col;  // normalized colour times intensity
in vec3 pbr_wpos;          // world, metres
in vec3 pbr_view_v;        // toward the camera, metres
in vec4 pbr_vn;            // sidecar smooth normal, w = 1 where defined
in vec4 pbr_pxn;           // parallax basis (pbr_bg_vert.glsl): normal, w = 1 where defined
in vec3 pbr_pxu;
in vec3 pbr_pxv;
in float pbr_sv;           // sun visibility x sun share, < 0: none (use the baked proxy)
in float pbr_e;            // emissive mask
in float pbr_kv;           // key-light weight, < 0: none (use the baked proxy)
in float pbr_ksun;         // the bake's sun (moon at night) at N.L = 1, 0: unknown or shadowed
uniform int pbr_key_ok;
uniform vec3 pbr_key_dir;  // world, toward the light
uniform vec3 pbr_key_col;
uniform float pbr_key_scale;
uniform int pbr_hl_on;   // local-light highlights read the material from the emissive mask
uniform float pbr_damp;  // highlights: roughness cut on porous classes in enclosed levels
vec2 pbr_hl = vec2(0.0, 1.0);  // highlight material for the emissive mask (pbr_hl_pack)

/// True if this draw takes the parallax offset (uniforms only, so derivatives stay defined).
bool pbr_px_on() {
  return pbr_on != 0 && pbr_src == 1 && pbr_draw.y > 1.5 && pbr_px.x > 0.0 && (pbr_probe & 1) == 0;
}

/*!
 * Parallax-offset UV for a draw with a packed height (call only when `pbr_px_on`): the view ray
 * is followed below the surface to the height field, `pbr_px.y` taps of a damped fixed-point
 * search. Depth is in metres, turned into UV through the world gradients of u and v; faded
 * out at grazing angles (silhouettes) and with distance, the fade range scaled by the texture's
 * metres per repeat (coarse texel density reads further). The normal and gradients come from
 * the interpolated per-vertex basis where the tree has one (continuous across triangle edges),
 * else from this triangle's screen derivatives. Sets `pbr_uv_shift`.
 */
/*!
 * Self-shadow of the parallax height field toward the sun at `uv` (the offset surface point):
 * the light ray is marched up through the field in the same basis as the offset, `pbr_pxs.w`
 * taps until it leaves the top; the largest height of the field above the ray, over the softness
 * share of the depth, is the occlusion. 1 lit.
 */
float pbr_parallax_shadow(vec2 uv, vec3 N, vec3 gu, vec3 gv, float depth, vec2 du1, vec2 du2) {
  vec3 L = pbr_sun_dir;
  float ndl = dot(N, L);
  if (ndl <= 0.02) {
    return 1.0;
  }
  float d0 = depth * (1.0 - textureGrad(tex_T17, uv, du1, du2).w);
  // to the top of the field, at most four depths along the surface
  float tmax = min(d0 / ndl, 4.0 * depth / max(sqrt(1.0 - ndl * ndl), 0.25));
  vec2 duv = vec2(dot(gu, L), dot(gv, L));
  float occ = 0.0;
  for (int i = 1; i <= 8; i++) {
    if (float(i) > pbr_pxs.w) {
      break;
    }
    float t = tmax * float(i) / pbr_pxs.w;
    float field = depth * (1.0 - textureGrad(tex_T17, uv + duv * t, du1, du2).w);
    // the field above the ray, fading over the march
    float above = (d0 - ndl * t) - field;
    occ = max(occ, clamp(above / (pbr_pxs.z * depth), 0.0, 1.0) * (1.0 - 0.5 * float(i - 1) / pbr_pxs.w));
  }
  // fades in past the early-out, so faces either side of it match
  return 1.0 - pbr_pxs.x * occ * smoothstep(0.02, 0.1, ndl);
}

vec2 pbr_parallax(vec2 uv) {
  vec2 du1 = dFdx(uv);
  vec2 du2 = dFdy(uv);
  vec3 V = normalize(pbr_view_v);
  vec3 N;
  vec3 gu;
  vec3 gv;
  float basis = 1.0;
  if (pbr_pxn.w > 0.999) {
    float nl = length(pbr_pxn.xyz);
    N = nl > 0.0 ? pbr_pxn.xyz / nl : V;
    // vertex normals flipped toward the camera can cancel near silhouettes
    basis = smoothstep(0.3, 0.6, nl);
    gu = pbr_pxu;
    gv = pbr_pxv;
  } else {
    vec3 dp1 = dFdx(pbr_wpos);
    vec3 dp2 = dFdy(pbr_wpos);
    N = cross(dp1, dp2);
    float nl = length(N);
    N = nl > 0.0 ? N / nl : V;
    N = dot(N, V) < 0.0 ? -N : N;
    vec3 r1 = cross(dp2, N);
    vec3 r2 = cross(N, dp1);
    float det = dot(dp1, r1);
    float inv = abs(det) > 1e-12 ? 1.0 / det : 0.0;
    gu = (r1 * du1.x + r2 * du2.x) * inv;
    gv = (r1 * du1.y + r2 * du2.y) * inv;
  }
  float ndv = dot(N, V);
  float g = max(length(gu), length(gv));
  // metres per texture repeat along the finer of u and v
  float rep = 1.0 / max(g, 1e-6);
  float k = clamp(rep * pbr_px_fade.z, 0.25, 4.0);
  float fade = basis * smoothstep(pbr_px.z, pbr_px.w, ndv) *
               (1.0 - smoothstep(pbr_px_fade.x * k, pbr_px_fade.y * k, length(pbr_view_v)));
  float depth = g > 0.0 ? pbr_px.x * fade : 0.0;
  // UV per metre of depth along the ray
  vec3 dir = (N * ndv - V) / max(ndv, 0.2);
  vec2 duv = depth > 0.0 ? vec2(dot(gu, dir), dot(gv, dir)) : vec2(0.0);
  float s = 0.5 * depth;
  for (int i = 0; i < 3; i++) {
    if (float(i) < pbr_px.y && depth > 0.0) {
      float h = textureGrad(tex_T17, uv + duv * s, du1, du2).w;
      s = 0.5 * (s + depth * (1.0 - h));
    }
  }
  pbr_uv_shift = duv * s;
  if (pbr_pxs.x > 0.0 && pbr_sun_ok != 0 && depth > 0.0 && (pbr_probe & 2) == 0) {
    pbr_px_sh = pbr_parallax_shadow(uv + pbr_uv_shift, N, gu, gv, depth * pbr_pxs.y, du1, du2);
  }
  return uv + pbr_uv_shift;
}

/*!
 * Albedo at `uv` with the parallax offset where the draw takes it. The alpha stays the unshifted
 * one (alpha test and silhouettes unchanged); a shifted sample landing on a cut-out texel keeps the
 * unshifted colour.
 */
vec4 pbr_parallax_albedo(sampler2D tex, vec2 uv) {
  vec2 du1 = dFdx(uv);
  vec2 du2 = dFdy(uv);
  vec4 base = texture(tex, uv);
  vec4 shifted = textureGrad(tex, pbr_parallax(uv), du1, du2);
  return vec4(shifted.a < 0.5 * base.a ? base.rgb : shifted.rgb, base.a);
}

// ---- sun glints (snow): crystals in world-anchored cells, averaged by pixel footprint ----
uniform float pbr_glint[32];   // per class (PBR_CLASS_COUNT used): one glint's peak x sun colour
uniform vec4 pbr_glint_shape;  // cell (m), share of cells holding a crystal, facet spread, lobe (rad)
float pbr_sun_occ = 0.0;       // actor shadow on the sun at this pixel, set by the caller
float pbr_fog_keep = 1.0;      // share of this pixel left after fog and haze, set by the caller
float pbr_glint_e = 0.0;       // glint bloom weight for the emissive mask (spots only)

uvec3 pbr_pcg3(uvec3 v) {
  v = v * 1664525u + 1013904223u;
  v.x += v.y * v.z;
  v.y += v.z * v.x;
  v.z += v.x * v.y;
  v ^= v >> 16u;
  v.x += v.y * v.z;
  v.y += v.z * v.x;
  v.z += v.x * v.y;
  return v;
}

/// One cell's glint at cell coordinates g (spot near the centre, radius r cells).
float pbr_glint_cell(vec2 g, vec2 id, uint ax, vec3 N, vec3 H, float dens, float s2, float w2,
                     float r, float peak) {
  uvec3 h = pbr_pcg3(uvec3(uvec2(ivec2(id)), ax));
  vec3 u = vec3(h >> 8u) * (1.0 / 16777216.0);
  if (u.x >= dens) {
    return 0.0;
  }
  vec3 u2 = vec3(pbr_pcg3(h) >> 8u) * (1.0 / 16777216.0);
  // facet tilt: Gaussian, mean square = spread^2
  vec2 d = sqrt(-log(max(u.y, 1e-6)) * s2) * vec2(cos(6.2831853 * u.z), sin(6.2831853 * u.z));
  vec3 T = vec3(ax == 0u ? 0.0 : 1.0, 0.0, ax == 0u ? 1.0 : 0.0);
  T = normalize(T - N * dot(T, N));
  vec3 n = normalize(N + d.x * T + d.y * cross(N, T));
  vec2 o = g - id - (0.4 + 0.2 * u2.xy);
  return peak * exp(-2.0 * (1.0 - dot(n, H)) / w2) * exp(-dot(o, o) / (r * r));
}

/*!
 * Sun glints of class `c` at this pixel, before the sun colour and share: one crystal facet per
 * occupied cell of a grid on the world plane facing Ng (cell `pbr_glint_shape.x` metres), a facet
 * normal spread around N, a lobe around the half vector H, an HDR spot of peak `pbr_glint[c]` near
 * the cell centre (0.06 cells, at least 2 px, at most 0.3 cells). The 2x2 cells nearest the pixel
 * are summed, so a spot is never cut at a cell edge. Where a cell is under ~6 px (`fw` = fwidth
 * of the world position) the field fades into its own mean, a sheen along the sun's mirror path.
 * `spot` returns the spot part alone (for the bloom mask).
 */
float pbr_glints(int c, vec3 Ng, vec3 N, vec3 V, vec3 L, vec3 fw, out float spot) {
  float cell = pbr_glint_shape.x;
  float dens = pbr_glint_shape.y;
  float s2 = pbr_glint_shape.z * pbr_glint_shape.z;
  float w2 = pbr_glint_shape.w * pbr_glint_shape.w;
  vec3 H = normalize(L + V);
  vec3 a = abs(Ng);
  uint ax = a.y >= a.x && a.y >= a.z ? 1u : (a.x >= a.z ? 0u : 2u);
  vec2 q = ax == 1u ? pbr_wpos.xz : (ax == 0u ? pbr_wpos.zy : pbr_wpos.xy);
  float mpp = ax == 1u ? max(fw.x, fw.z) : (ax == 0u ? max(fw.z, fw.y) : max(fw.x, fw.y));
  float cpx = cell / max(mpp, 1e-6);
  float r = clamp(2.0 / cpx, 0.06, 0.3);
  // the field's mean: occupied share x spot area x expected lobe over the facet spread
  float mean = pbr_glint[c] * dens * 3.14159 * r * r * w2 / (w2 + s2) *
               exp(-2.0 * (1.0 - dot(N, H)) / (w2 + s2));
  float f = smoothstep(3.0, 6.0, cpx);
  spot = 0.0;
  if (f <= 0.0) {
    return mean;
  }
  vec2 g = q / cell;
  vec2 base = floor(g - 0.5);
  float glint = 0.0;
  for (int j = 0; j < 2; j++) {
    for (int i = 0; i < 2; i++) {
      glint += pbr_glint_cell(g, base + vec2(i, j), ax, N, H, dens, s2, w2, r, pbr_glint[c]);
    }
  }
  spot = f * glint;
  return mix(mean, glint, f);
}

// ---- swamp look (Swamp.h): water surface, wet surfaces; swamp level only; 4: wet shoreline ----
uniform int sw_flags;   // 1 water surface, 2 wet surfaces, 4 wet shoreline (sea levels),
                        // 16 wetness through the PBR roughness instead of the film
uniform vec4 sw_gloss;  // flag 16, off sand: film weight, film roughness, wet F0, darkening
uniform vec2 sw_splash;  // flag 16 with 4, off sand: splash zone fully wet to, dry at (m above sea)
uniform vec4 sw_water;  // body multiplier, reflection, ripple slope, glint
uniform vec4 sw_wet;    // porous darkening, film strength, puddle coverage
uniform float sw_time;  // seconds
uniform vec4 sw_shore;  // sea height (m), band height (m), noise cell (m)
LEVEL_HEIGHT_GLSL

float sw_hash(vec2 p) {
  vec3 p3 = fract(vec3(p.xyx) * 0.1031);
  p3 += dot(p3, p3.yzx + 33.33);
  return fract((p3.x + p3.y) * p3.z);
}

float sw_noise(vec2 p) {
  vec2 i = floor(p);
  vec2 f = fract(p);
  f = f * f * (3.0 - 2.0 * f);
  return mix(mix(sw_hash(i), sw_hash(i + vec2(1.0, 0.0)), f.x),
             mix(sw_hash(i + vec2(0.0, 1.0)), sw_hash(i + vec2(1.0, 1.0)), f.x), f.y);
}

/// Shoreline wetness: 1 up to the lower third of the band above the sea, 0 at its top; the top
/// edge follows world noise (faded where a cell is under a few pixels), steep faces get half.
float sw_shore_band(vec3 Ng, float mpp) {
  float cell = sw_shore.z;
  vec2 xz = pbr_wpos.xz;
  float n = 0.65 * sw_noise(xz / cell) + 0.35 * sw_noise(xz / (0.37 * cell) + 5.1) - 0.5;
  n *= 1.0 - smoothstep(0.05 * cell, 0.15 * cell, mpp);
  float band = sw_shore.y * mix(0.5, 1.0, abs(Ng.y));
  float h = pbr_wpos.y - sw_shore.x + 0.6 * band * n;
  return 1.0 - smoothstep(0.35 * band, band, h);
}

/// Cover at world point q (game units): a solid surface 0.6-1.5 m or more above it, read from
/// the four texels around q of each bound heightfield slot on that slot's own grid and blended
/// bilinearly (the covered values, not the heights), so a cover edge fades over a texel instead
/// of stepping on the grid; the max over slots (an empty slot or one that misses q reads open).
float sw_cover_at(vec3 q) {
  float best = 0.0;
  for (int i = 0; i < 2; i++) {
    if (i >= lh_n) {
      break;
    }
    vec4 rc = lh_rect[i];
    vec2 r = rc.zw;
    if (r.x == 0.0 || r.y == 0.0) {
      continue;
    }
    vec2 t = (q.xz - rc.xy) * r - 0.5;
    vec2 g = floor(t);
    vec2 f = t - g;
    float c[4];
    for (int k = 0; k < 4; k++) {
      vec2 cell = g + vec2(float(k & 1), float(k >> 1)) + 0.5;
      float top = lh_top_slot(rc.xy + cell / r, i, 0);
      c[k] = smoothstep(0.6, 1.5, (top - q.y) * (1.0 / 4096.0));
    }
    // each loaded map on its own grid; a point covered in either is covered
    best = max(best, mix(mix(c[0], c[1], f.x), mix(c[2], c[3], f.x), f.y));
  }
  return best;
}

/// Share of the rain that reaches world point `p` (metres) on a surface facing `n`: 0 under a
/// roof, cave or overhang (the level heightfield has a solid surface over the point stepped off
/// its surface: 1 m above flat ground, 1.5 m in front of a wall), 1 under open sky. Five taps
/// 1.5 m apart along the surface (its two tangents: x and z on the ground, along the wall and up
/// on a wall), each blended over four texels, fade a cover edge over about 3 m. 1 with no map.
float sw_rain(vec3 p, vec3 n) {
  if (lh_n == 0) {
    return 1.0;
  }
  vec3 q = p + n * mix(1.5, 1.0, abs(n.y));
  vec3 t1 = abs(n.y) > 0.99 ? vec3(1.0, 0.0, 0.0) : normalize(cross(vec3(0.0, 1.0, 0.0), n));
  vec3 t2 = cross(n, t1);
  float covered = 0.0;
  for (int k = 0; k < 5; k++) {
    vec3 o = k == 0 ? vec3(0.0) : 1.5 * (k == 1 ? t1 : k == 2 ? -t1 : k == 3 ? t2 : -t2);
    covered += sw_cover_at((q + o) * 4096.0);
  }
  return 1.0 - covered * 0.2;
}

/// Splash zone on non-porous surfaces by the sea: fully wet to sw_splash.x above it, dry at .y,
/// with a world-noise edge and wet streaks running up steep faces.
float sw_splash_zone(vec3 Ng, float mpp) {
  vec2 xz = pbr_wpos.xz;
  float cell = sw_shore.z;
  float n = 0.65 * sw_noise(xz / cell) + 0.35 * sw_noise(xz / (0.37 * cell) + 5.1) - 0.5;
  n *= 1.0 - smoothstep(0.05 * cell, 0.15 * cell, mpp);
  vec2 tn = vec2(-Ng.z, Ng.x);
  float tl = length(tn);
  float u = tl > 1e-3 ? dot(xz, tn / tl) : xz.x;
  float streak = smoothstep(0.55, 0.85, sw_noise(vec2(u * 2.5, 7.3))) * (1.0 - abs(Ng.y)) *
                 (1.0 - smoothstep(0.03, 0.1, mpp));
  float fade = sw_splash.y - sw_splash.x;
  float h = pbr_wpos.y - sw_shore.x + fade * (n - streak);
  return 1.0 - smoothstep(sw_splash.x, sw_splash.y, h);
}

/// Height gradient of one layer of rain rings: one drop per cell per cycle, each ring growing to
/// 0.9 cells and dying out, a 7 cm ripple under a Gaussian band.
vec2 sw_ring_layer(vec2 p, float t, float cell, float seed) {
  vec2 c = floor(p / cell);
  vec2 g = vec2(0.0);
  for (int j = -1; j <= 1; j++) {
    for (int i = -1; i <= 1; i++) {
      vec2 id = c + vec2(i, j);
      vec2 ctr = (id + 0.2 + 0.6 * vec2(sw_hash(id + seed), sw_hash(id + seed + 17.3))) * cell;
      float life = fract(t * 0.9 + sw_hash(id + seed + 41.9));
      vec2 d = p - ctr;
      float r = length(d);
      float x = r - life * cell * 0.9;
      float w = 0.04 + 0.05 * life;
      float band = (1.0 - life) * (1.0 - life) * exp(-x * x / (w * w));
      g += band * cos(x * 90.0) * d / max(r, 1e-3);
    }
  }
  return g;
}

/// Rain-ripple slope at world xz `p` (metres), faded out where a ripple is under a few pixels
/// (`mpp`: metres per pixel, length(fwidth(p)) taken in uniform control flow).
vec2 sw_ripples(vec2 p, float mpp) {
  float fade = 1.0 - smoothstep(0.012, 0.05, mpp);
  if (fade <= 0.0) {
    return vec2(0.0);
  }
  return fade * 0.35 * (sw_ring_layer(p, sw_time, 0.55, 0.0) +
                        sw_ring_layer(p + 0.27, sw_time + 0.5, 0.4, 7.0));
}

/// Share of open sky above a swamp surface: the sidecar's sun visibility, else the bake's level.
float sw_sky(vec3 baked) {
  return pbr_sv >= 0.0 ? clamp(pbr_sv * 1.5, 0.0, 1.0) : smoothstep(0.4, 0.9, pbr_lum(baked));
}

/// What a swamp surface reflects: the canopy and banks around it (the bake's own light, dim), with
/// the mood sky only where the sky is open.
vec3 sw_surround(vec3 R, float r, vec3 L, vec3 baked) {
  vec3 sky = pbr_env(R, r, L) * pbr_lum(baked);
  return mix(baked * 0.35, sky, 0.5 * sw_sky(baked));
}

/// The swamp water: dark murky body, a reflection only at grazing angles, rain rings, glint.
vec3 sw_water_shade(vec3 rgb, vec3 baked, vec3 V, vec3 L) {
  vec2 g = sw_ripples(pbr_wpos.xz, length(fwidth(pbr_wpos.xz))) * sw_water.z *
           sw_rain(pbr_wpos, vec3(0.0, 1.0, 0.0));
  // slow, broad swell so the reflection is not a flat mirror
  vec2 q = pbr_wpos.xz * 0.35 + vec2(sw_time * 0.05, -sw_time * 0.04);
  float n0 = sw_noise(q);
  g += 0.15 * vec2(n0 - sw_noise(q + vec2(0.37, 0.0)), n0 - sw_noise(q + vec2(0.0, 0.37)));
  vec3 N = normalize(pbr_up - g.x * vec3(1.0, 0.0, 0.0) - g.y * vec3(0.0, 0.0, 1.0));
  float ndv = max(dot(N, V), 1e-3);
  float F = clamp((0.02 + 0.98 * pow(1.0 - ndv, 5.0)) * sw_water.y, 0.0, 0.6);
  vec3 col = mix(rgb * sw_water.x, sw_surround(reflect(-V, N), 0.1, L, baked), F);
  float sv = pbr_sv >= 0.0 ? pbr_sv : 0.6 * smoothstep(0.35, 0.9, pbr_lum(baked));
  if (pbr_sun_ok != 0) {
    col += pbr_lobe(N, V, L, 0.06, vec3(0.02), 1.0) * pbr_sun_col * sv * sw_water.w;
  }
  return col;
}

// ---- ice (class 12): glossy coat over a darker, bluer interior, sun through thin ice ----
/// The draw's albedo texture with explicit gradients (defined by the including shader).
vec4 pbr_albedo_grad(vec2 uv, vec2 dx, vec2 dy);

/*!
 * Ice over the shaded colour `lit`. The texture is sampled again below the surface along the
 * refracted view ray (blurred) and tinted by its own colour over the path length (Beer-Lambert),
 * under a smooth fresnel coat with a sharp sun lobe; faces turned from the sun, and edges seen
 * toward it, glow in the ice colour. Blue-shifted texels take the full response, white ones less.
 */
vec3 pbr_ice_shade(vec3 lit, vec3 rgb, vec3 albedo, vec3 baked, vec2 uv, vec3 Ng, vec3 Nd, vec3 V,
                   vec3 L, float sv, float ef) {
  vec3 dp1 = dFdx(pbr_wpos);
  vec3 dp2 = dFdy(pbr_wpos);
  vec2 du1 = dFdx(uv);
  vec2 du2 = dFdy(uv);
  float k = pbr_ice.x;
  float w = mix(0.35, 1.0, smoothstep(0.02, 0.12, albedo.b - 0.5 * (albedo.r + albedo.g)));
  vec3 fN = cross(dp1, dp2);
  fN = length(fN) > 0.0 ? normalize(fN) : Ng;
  fN = dot(fN, V) < 0.0 ? -fN : fN;
  vec3 r1 = cross(dp2, fN);
  vec3 r2 = cross(fN, dp1);
  float det = dot(dp1, r1);
  float inv = abs(det) > 1e-12 ? 1.0 / det : 0.0;
  vec3 gu = (r1 * du1.x + r2 * du2.x) * inv;
  vec3 gv = (r1 * du1.y + r2 * du2.y) * inv;
  float ndv = max(dot(Ng, V), 0.0);
  // refracted ray (n = 1.31): tangential part shrinks, cosine of the inner angle
  vec3 t = (Ng * ndv - V) / 1.31;
  float cosr = sqrt(max(1.0 - dot(t, t), 0.09));
  vec2 duv = vec2(dot(gu, t), dot(gv, t)) * (pbr_ice.y / cosr);
  duv *= min(1.0, 0.5 / max(length(duv), 1e-6));
  vec4 inner = pbr_albedo_grad(uv + pbr_uv_shift + duv, du1 * 4.0, du2 * 4.0);
  vec3 ia = inner.a < 0.25 ? albedo : inner.rgb;
  // down to the layer and back, in absorption paths; the ice colour is its own transmittance
  float path = 2.0 * pbr_ice.y / cosr / pbr_ice.z * k;
  vec3 tc = albedo / max(max(albedo.r, albedo.g), max(albedo.b, 1e-3));
  vec3 light = baked * clamp(pbr_lum(rgb) / max(pbr_lum(albedo * baked), 1e-4), 0.0, 2.0);
  vec3 interior = ia * light * pow(clamp(tc, 0.05, 1.0), vec3(path)) * 0.7;
  vec3 col = mix(lit, interior, w * min(0.7 * k, 0.95));

  vec3 Nc = normalize(mix(Ng, Nd, 0.3));
  float ndc = max(dot(Nc, V), 1e-3);
  float rc = pbr_ice.w;
  float F = 0.04 + 0.96 * pow(1.0 - ndc, 5.0);
  // sky only (the sun is the lobe below), half its chroma so a warm sky cannot turn the body green
  vec3 R = reflect(-V, Nc);
  vec3 sky = sw_surround(R, rc, -R, baked);
  sky = mix(vec3(pbr_lum(sky)), sky, 0.5);
  col = mix(col, sky, clamp(F * (1.0 + k), 0.0, 0.6));
  if (pbr_sun_ok != 0) {
    // sun highlight in the sun's colour, added on top: GGX over its peak, no stylized band
    vec3 Nh = normalize(mix(Ng, Nd, 0.5));
    vec3 hv = L + V;
    vec3 H = dot(hv, hv) > 1e-6 ? normalize(hv) : Nh;
    float ndh = max(dot(Nh, H), 0.0);
    float a2 = rc * rc * rc * rc;
    float dd = ndh * ndh * (a2 - 1.0) + 1.0;
    float Fh = 0.04 + 0.96 * pow(1.0 - max(dot(V, H), 0.0), 5.0);
    float facing = smoothstep(0.0, 0.2, dot(Nh, L));
    float hl = (a2 * a2 / (dd * dd)) * Fh * facing * sv * 8.0 * k * w;
    col += pbr_sun_col * 3.0 * (1.0 - exp(-hl / 3.0));
    // through thin ice: only light entering from behind the face, in the ice's own blue
    float back = clamp(-dot(Ng, L), 0.0, 1.0);
    float toward = pow(clamp(dot(-V, L), 0.0, 1.0), 6.0);
    float thin = mix(0.4, 1.0, (1.0 - ndv) * (1.0 - ndv));
    // sun share (open to the sun at this hour; caves and overhangs ~0), the bake without a sidecar
    float lit_bake = smoothstep(0.25, 0.6, pbr_lum(baked)) *
                     (pbr_sv >= 0.0 ? clamp(pbr_sv * 1.5, 0.0, 1.0) : 1.0);
    vec3 tcol = tc * tc / max(max(tc.r * tc.r, tc.g * tc.g), max(tc.b * tc.b, 1e-3));
    vec3 tr = pbr_lum(pbr_sun_col) * tcol * back * (0.35 + 1.5 * toward) * thin * lit_bake *
              pbr_ice_tr * k * w;
    float tm = max(max(tr.r, tr.g), max(tr.b, 1e-4));
    col += tr * (1.5 * (1.0 - exp(-tm / 1.5)) / tm);
  }
  return mix(lit, col, ef);
}

/// Lava light added to `lit`: diffuse on top of the bake (less the share the level's bake already
/// holds) and the downward reflections of smooth and metal surfaces, fitted under the white cap.
vec3 lava_light_add(vec3 lit, vec3 albedo, vec3 Ng, vec3 Nd, vec3 V, float rough, int c,
                    float ef) {
  vec4 lu = lava_under(pbr_wpos, Ng);
  vec3 add = (lava_on & 4) != 0 ? albedo * lu.rgb * (1.0 - lava_under_k.x) : vec3(0.0);
  float r = pbr_rough_of(c, rough);
  float m = clamp(pbr_draw.x * pbr_look.x, 0.0, 1.0);
  vec3 F0 = mix(vec3(pbr_f0), albedo, m);
  float fv = pow(1.0 - max(dot(Nd, V), 1e-3), 5.0) * pbr_look.z;
  vec3 F = F0 + (max(vec3((1.0 - r) * (1.0 - r)), F0) - F0) * fv;
  float es = min(pbr_look.y * pbr_cls3[c].x * mix((1.0 - r) * (1.0 - r), 1.0, m), 1.0);
  float down = smoothstep(0.0, 0.5, -dot(reflect(-V, Nd), pbr_up));
  add = lava_fit(lit, (add + F * es * lava_light.w * lu.a * down * lava_light.rgb) * ef);
  if (lava_view == 2) {
    return add;
  } else if (lava_view == 1) {
    return mix(vec3(pbr_lum(lit) * 0.3), vec3(1.0, 0.4, 0.0), lu.a);
  }
  return lit + add;
}

/// Wetness weight of class c (PbrClass order): how much it darkens and takes a water film when
/// wet; `ice` takes none while the ice rule is on (stone's when it is off). Selects on the uniform
/// class, not a const array: a dynamically indexed const array lands in scratch memory.
float sw_por(int c) {
  if (c == PBR_ICE && pbr_ice.x > 0.0) {
    return 0.0;
  }
  return c == 1 || c == 2 || c == 8 || c == 12 ? 1.0
         : c == 4 || c == 5               ? 0.7
         : c == 3 || c == 11              ? 0.25
                                          : 0.0;
}

// backlight sun transmittance (background/CanopyVol.h): leaf volume, then the level heightfield
uniform int pbr_bl_n;               // leaf volume maps bound, 0 = off
uniform int pbr_bl_solid;           // 1: test the level heightfield too
uniform vec4 pbr_bl_par;            // leaf optical depth scale, step (m), solid margin (m), -
uniform sampler2DArray canopy_vol;  // per 2x2-texel cell: lowest leaf, highest leaf, volume cover
uniform vec4 pbr_bl_rect[2];        // x0, z0 (game units), cells per game unit, -
uniform vec2 pbr_bl_h[2];           // leaf height y0, span (game units)
uniform ivec2 pbr_bl_size[2];       // cells of each map

/// Share of the sun reaching p (game units) along L: through the crowns' leaf volume, then
/// blocked where the solid world rises above the ray.
float pbr_sun_transmittance(vec3 p, vec3 L) {
  float tau = 0.0;
  float ls = pbr_bl_par.y * 4096.0;
  for (int i = 0; i < 2; i++) {
    if (i >= pbr_bl_n) {
      break;
    }
    vec4 r = pbr_bl_rect[i];
    vec2 h = pbr_bl_h[i];
    if (r.z == 0.0) {
      continue;  // empty slot
    }
    for (int k = 0; k < 6; k++) {
      vec3 q = p + L * ((float(k) + 0.5) * ls);
      ivec2 t = ivec2(floor((q.xz - r.xy) * r.z));
      if (any(lessThan(t, ivec2(0))) || any(greaterThanEqual(t, pbr_bl_size[i]))) {
        continue;
      }
      vec3 m = texelFetch(canopy_vol, ivec3(t, i), 0).rgb;
      float lo = h.x + h.y * m.r;
      float hi = h.x + h.y * m.g;
      float in_crown = smoothstep(lo - 2048.0, lo + 2048.0, q.y) * (1.0 - smoothstep(hi - 2048.0, hi + 2048.0, q.y));
      tau += -log(max(1.0 - m.b, 0.02)) / max(hi - lo, 4096.0) * ls * in_crown;
    }
  }
  float T = exp(-pbr_bl_par.x * tau);
  // start 2 m out sideways, past the leaf's own trunk
  float blocked = pbr_bl_solid != 0 ? lh_sun_blocked(p, L, 2.0 / max(length(L.xz), 0.3), pbr_bl_par.z) : 0.0;
  return T * (1.0 - blocked);
}

// the sun terms that read pbr_sun_occ can run here: glints, or a wet film (rain on porous
// classes, the shore band and splash zone near sea level)
bool pbr_sun_occ_read() {
  if (pbr_on == 0) {
    return false;
  }
  int c = pbr_mat & 31;
  int c1 = c == 10 ? 1 : c;  // lava crust shades as class 1
  if (pbr_sv > 0.0 && max(pbr_glint[c], pbr_glint[c1]) > 0.0) {
    return true;
  }
  if ((sw_flags & 6) == 0 || max(sw_por(c), sw_por(c1)) <= 0.0) {
    return false;
  }
  if ((sw_flags & 2) != 0) {
    return true;
  }
  float reach = max(1.3 * abs(sw_shore.y), (sw_flags & 16) != 0
                    ? sw_splash.y + 1.5 * abs(sw_splash.y - sw_splash.x) : 0.0);
  return pbr_wpos.y - sw_shore.x < reach + 0.01;
}

/// Background shading on top of the vanilla colour `rgb` (texture `albedo` at `uv`, baked light
/// `baked`).
vec3 pbr_background(vec3 rgb, vec3 albedo, vec3 baked, vec2 uv) {
  int c = pbr_mat & 31;
  // lava class: the crust shades as stone, molten texels glow over it (lava_glow_mix)
  float lava_m = -1.0;
  vec3 lava_off = rgb;  // this texel with lava off (unshaded, dry)
  if (c == 10 && (lava_on & 1) != 0) {
    lava_m = lava_molten(albedo);
    c = 1;
  }
  if ((sw_flags & 1) != 0 && (pbr_mat & 32) != 0) {
    return sw_water_shade(rgb, baked, normalize(pbr_view_v),
                          pbr_sun_ok != 0 ? pbr_sun_dir : vec3(0.0, 1.0, 0.0));
  }
  if ((pbr_probe & 64) != 0) {
    return rgb;
  }
  // unshaded classes (none, special, lava): pbr_respond mixes back to rgb with weight 0
  if (pbr_skip != 0 && !pbr_shaded(c) && pbr_view == 0) {
    return rgb;
  }
  vec3 V = normalize(pbr_view_v);
  vec3 glint_fw = fwidth(pbr_wpos);
  vec3 f = cross(dFdx(pbr_wpos), dFdy(pbr_wpos));
  f = length(f) > 0.0 ? normalize(f) : V;
  if (dot(f, V) < 0.0) {
    f = -f;
  }
  vec3 Ng = f;
  if (pbr_vn.w > 0.999) {
    Ng = normalize(pbr_vn.xyz);
    Ng = dot(Ng, f) < 0.0 ? -Ng : Ng;
  }
  if ((pbr_probe & 512) != 0) {
    return rgb + 1e-20 * Ng;
  }
  vec3 Nd;
  float rough = -1.0;
  if (pbr_src == 1) {
    vec4 d = pbr_uv_detail(Ng, pbr_wpos, uv, uv, c);
    Nd = d.xyz;
    rough = d.w;
  } else {
    vec3 p = pbr_wpos * pbr_scale[c];
    vec3 b = pow(abs(Ng), vec3(4.0));
    b /= (b.x + b.y + b.z);
    float s = pbr_detail[c];
    vec2 tx = pbr_dn(p.zy, c) * s;
    vec2 ty = pbr_dn(p.xz, c) * s;
    vec2 tz = pbr_dn(p.xy, c) * s;
    Nd = normalize(vec3(tx + Ng.zy, Ng.x).zyx * b.x + vec3(ty + Ng.xz, Ng.y).xzy * b.y +
                   vec3(tz + Ng.xy, Ng.z) * b.z);
  }
  if ((pbr_probe & 1024) != 0) {
    return rgb + 1e-20 * (Nd + rough);
  }
  // rain only where the sky is open: under cover the ground stays damp, without puddles
  float rain = (sw_flags & 2) != 0 && sw_por(c) > 0.0 ? sw_rain(pbr_wpos, Ng) : 1.0;
  float wet = (sw_flags & 2) != 0 ? sw_por(c) * mix(0.6, 1.0, rain) : 0.0;
  float film = wet;
  float puddle = 0.0;
  float sw_mpp = 0.0;
  if ((sw_flags & 6) != 0 && sw_por(c) > 0.0) {
    // per draw (flags, class), so derivatives here are defined; not in the branches below
    sw_mpp = length(fwidth(pbr_wpos.xz));
  }
  if ((sw_flags & 4) != 0 && sw_por(c) > 0.0) {
    // opt 32: both bands are exactly 0 outside these heights (noise amplitudes 0.5, streak 1)
    float dy = pbr_wpos.y - sw_shore.x;
    bool splash = (sw_flags & 16) != 0 && (pbr_mat & 32) == 0;
    bool dry = dy <= -0.3 ||
               dy >= 1.3 * sw_shore.y * mix(0.5, 1.0, abs(Ng.y)) &&
                   (!splash || dy - 1.5 * abs(sw_splash.y - sw_splash.x) >= sw_splash.y);
    // off below the sea: the shallows show the seabed through the water
    float b = 0.0;
    if (!(dry && pbr_win(32))) {
      b = sw_shore_band(Ng, sw_mpp);
      if (splash) {
        b = max(b, sw_splash_zone(Ng, sw_mpp));
      }
    }
    b *= smoothstep(-0.3, 0.05, dy);
    wet = sw_por(c) * b;
    film = wet * b;
  }
  if (wet > 0.0) {
    vec2 xz = pbr_wpos.xz;
    float n = 0.65 * sw_noise(xz / 1.8) + 0.35 * sw_noise(xz / 0.6 + 5.1);
    float cover = sw_wet.z * rain;
    puddle = smoothstep(0.9, 0.98, dot(Ng, pbr_up)) * smoothstep(0.4, 0.6, wet) *
             smoothstep(1.02 - cover, 1.1 - cover, n);
    // under the water film (16) the shore's non-sand surfaces take its own darkening
    bool film_rock = (sw_flags & 20) == 20 && (pbr_mat & 32) == 0;
    float k = mix(1.0, film_rock ? sw_gloss.w : sw_wet.x, wet) * mix(1.0, 0.7, puddle);
    rgb *= k;
    albedo *= k;
    if ((sw_flags & 4) != 0) {
      // wet ground keeps its hue and gains a little chroma
      float s = 1.0 + (film_rock ? 0.5 : 0.3) * wet;
      rgb = max(mix(vec3(pbr_lum(rgb)), rgb, s), 0.0);
      albedo = max(mix(vec3(pbr_lum(albedo)), albedo, s), 0.0);
    }
    Nd = normalize(mix(Nd, Ng, puddle));
  }
  float bw = pbr_scatter[c].w;
  if (bw > 0.0 && pbr_sun_ok != 0 && pbr_vn.w > 0.999 && pbr_ksun > 0.0) {
    // bake's sun term: N.L softened over +-w (quadratic knee), plus cool light scattered into the shade
    float x = dot(normalize(pbr_vn.xyz), pbr_sun_dir);
    float soft = x > bw ? x : (x < -bw ? 0.0 : (x + bw) * (x + bw) / (4.0 * bw));
    float bleed = 0.3 * bw * exp(-((x + 0.2) / 0.35) * ((x + 0.2) / 0.35));
    vec3 hue = pbr_sun_col / max(pbr_lum(pbr_sun_col), 1e-3);
    vec3 dl = pbr_ksun * (hue * (soft - max(x, 0.0)) + vec3(0.84, 1.01, 1.4) * bleed);
    rgb = max(rgb + albedo * dl, vec3(0.0));
    baked = max(baked + dl, vec3(0.0));
  }
  if ((pbr_probe & 2048) != 0) {
    return rgb + 1e-20 * (Nd + rough + albedo + baked);
  }
  float ef = 1.0 - smoothstep(pbr_e_ramp.x, pbr_e_ramp.y, pbr_e);
  float sv = pbr_sv >= 0.0 ? pbr_sv : 0.6 * smoothstep(0.35, 0.9, pbr_lum(baked));
  sv *= ef;
  if (pbr_sun_ok == 0 || !pbr_shaded(c)) {
    sv = 0.0;
  }
  float kv = pbr_kv >= 0.0 ? pbr_kv : pbr_key_scale * 0.5 * smoothstep(0.1, 0.6, pbr_lum(baked));
  kv *= ef;
  if (pbr_key_ok == 0 || !pbr_shaded(c) || sv > 0.0 && pbr_sv < 0.0) {
    kv = 0.0;
  }
  vec3 L = pbr_sun_ok != 0 ? pbr_sun_dir : vec3(0.0, 1.0, 0.0);
  vec3 K = pbr_key_ok != 0 ? pbr_key_dir : vec3(0.0, 1.0, 0.0);
  if ((sw_flags & 16) != 0 && wet > 0.0) {
    // a water film on non-porous ground: film roughness, relief filled in; sand (bit 32) drinks it
    float w = (pbr_mat & 32) != 0 ? 0.0 : clamp(sw_gloss.x * wet, 0.0, 1.0);
    pbr_wet_w = w;
    pbr_wet_r = sw_gloss.y;
    pbr_wet_f0 = sw_gloss.z;
    Nd = normalize(mix(Nd, Ng, 0.6 * w));
    film = w > 0.0 ? 0.0 : film;
  }
  if ((pbr_probe & (1 << 26)) == 0) {
    ssao_detail_set(Ng, Nd, V);
  }
  vec3 hl;
  // backlit only (Ng faces the eye, the sun behind the leaf): elsewhere the term is 0 anyway
  if (pbr_bl_n > 0 && pbr_scatter[c].x > 0.0 && dot(Ng, L) < 0.0 && dot(-V, L) > 0.0 && sv > 0.0 &&
      (pbr_probe & (1 << 25)) == 0) {
    pbr_sun_trans = pbr_sun_transmittance(pbr_wpos * 4096.0, L);
  }
  // a cast shadow already took the direct share
  pbr_px_sh = mix(pbr_px_sh, 1.0, clamp(pbr_sun_occ, 0.0, 1.0));
  if (pbr_px_sh < 1.0 && pbr_ksun > 0.0) {
    vec3 nb = pbr_vn.w > 0.999 ? normalize(pbr_vn.xyz) : Ng;
    pbr_px_sun = clamp(pbr_ksun * max(dot(nb, L), 0.0) / max(pbr_lum(baked), 1e-3), 0.0, 1.0);
  }
  if ((pbr_probe & 4096) != 0) {
    return rgb + 1e-20 * (Nd + rough + albedo + baked + sv + kv + pbr_px_sun + pbr_sun_trans);
  }
  vec3 lit = pbr_respond(rgb, albedo, baked, Ng, Nd, V, rough, pbr_draw.x, c, sv, L, pbr_sun_col,
                         kv, K, pbr_key_col, pbr_shaded(c) ? ef : 0.0, hl);
  if ((pbr_probe & 8192) != 0) {
    return lit + 1e-20 * hl;
  }
  if (c == PBR_ICE && pbr_ice.x > 0.0) {
    lit = pbr_ice_shade(lit, rgb, albedo, baked, uv, Ng, Nd, V, L, sv, ef);
  }
  float hr = pbr_hl_r * (1.0 - pbr_damp * sw_por(c));
  vec3 hf0 = pbr_hl_f0;
  if (pbr_glint[c] > 0.0 && pbr_sv >= 0.0 && sv > 0.0 && (pbr_probe & 16) == 0) {
    // direct sun only: the sidecar share, a strong sun above the horizon (snow's night slot is a
    // weak "day" light), the face toward it, not under an actor shadow; fog keeps its density
    float sun_i = max(pbr_sun_col.r, max(pbr_sun_col.g, pbr_sun_col.b));
    float gate = sv * smoothstep(0.02, 0.15, dot(L, pbr_up)) * smoothstep(0.0, 0.1, dot(Ng, L)) *
                 smoothstep(0.4, 0.7, sun_i) * (1.0 - clamp(pbr_sun_occ, 0.0, 1.0)) * pbr_fog_keep;
    float spot;
    vec3 gl = pbr_glints(c, Ng, Ng, V, L, glint_fw, spot) * gate * pbr_sun_col;
    // HDR spots bloom through the emissive mask; the far sheen does not touch it
    pbr_glint_e = smoothstep(0.1, 1.0, spot * gate * pbr_lum(pbr_sun_col));
    lit += gl;
    hl += gl;
  }
  if (wet > 0.0) {
    // water film: sharp reflection on the smoothed normal; puddles flat, with rain rings
    vec2 g = puddle > 0.0 ? sw_ripples(pbr_wpos.xz, sw_mpp) * puddle : vec2(0.0);
    vec3 Nc = normalize(mix(normalize(Ng + (Nd - Ng) * 0.35), pbr_up, puddle) -
                        g.x * vec3(1.0, 0.0, 0.0) - g.y * vec3(0.0, 0.0, 1.0));
    float ndv = max(dot(Nc, V), 1e-3);
    float rc = mix(0.12, 0.05, puddle);
    float amt = sw_wet.y * max(film, puddle) * ef;
    // a thin film: little reflection except at grazing angles, of the local light, not the sky
    float F = min((0.02 + 0.98 * pow(1.0 - ndv, 5.0)) * mix(1.0, 2.0, puddle),
                  mix(0.15, 0.35, puddle)) * amt;
    if ((sw_flags & 4) == 0) {
      lit = mix(lit, sw_surround(reflect(-V, Nc), rc, L, baked), F);
    }
    // the film's sun lobe takes the actor and world map shadow
    float lamt = amt * (1.0 - clamp(pbr_sun_occ, 0.0, 1.0));
    lit += 0.5 * pbr_lobe(Nc, V, L, rc * rc, vec3(0.02), 1.0) * pbr_sun_col * sv * lamt;
    // a water film: its own roughness and water's F0
    hr = mix(hr, rc, min(2.0 * amt, 1.0));
    hf0 = mix(hf0, vec3(0.02), min(2.0 * amt, 1.0));
  }
  if (pbr_hl_on != 0) {
    // molten lava texels are no stone: no highlight
    pbr_hl = lava_m > 0.0 ? vec2(0.0, 1.0) : pbr_hl_pack(hr, hf0);
  }
  if ((pbr_probe & 32768) != 0) {
  } else if (lava_m >= 0.0) {
    // the crust of lava-class rock takes the lava's light too
    if ((lava_on & 2) != 0) {
      lit = lava_light_add(lit, albedo, Ng, Nd, V, rough, c, ef);
    }
    // floors keep the darkened stone crust; walls are never darker than vanilla
    float lava_floor = smoothstep(0.6, 0.9, dot(Ng, pbr_up));
    lit = lava_view == 2 ? lit * (1.0 - lava_m)
                         : lava_glow_mix(lit, rgb, lava_m, albedo, lava_floor);
    lit = mix(max(lit, lava_off), lit, lava_view == 2 ? 1.0 : lava_floor);
  } else if ((lava_on & 2) != 0 && pbr_shaded(c)) {
    lit = lava_light_add(lit, albedo, Ng, Nd, V, rough, c, ef);
  }
  if (pbr_view >= 8) {
    // wet views: 8 wetness, 9 r sand bit / g class porosity / b shaded class, 10 film weight,
    // 11 roughness after the film, 12 specular and environment term x4
    return pbr_view == 8    ? vec3(wet)
           : pbr_view == 9  ? vec3((pbr_mat & 32) != 0 ? 1.0 : 0.0, sw_por(c), pbr_shaded(c) ? 1.0 : 0.0)
           : pbr_view == 10 ? vec3(pbr_wet_w)
           : pbr_view == 11 ? vec3(pbr_rough_of(c, rough))
                            : hl * 4.0;
  }
  if ((pbr_probe & 16384) != 0) {
    return lit;
  }
  return pbr_debug_view(c, rgb, Nd, hl, vec3(sv, kv, 0.0), rough, lit);
}

// ---- sunken caustics (Caustics.h); mirror: sandover/tools/sandover/caustics.py ----
uniform vec3 cau_w;  // live weight sum of the caustic slots, 0 = off
uniform vec4 cau_a;  // 1 / cell (1/m), loop phase (rad), amplitude, web half-width (0: soft)
uniform vec4 cau_b;  // drift (cells), web mean
uniform int cau_ldr;  // 1: 8-bit colour target, the web's peaks are soft-limited below white
in vec3 pbr_cau;

uvec3 cau_pcg(uvec3 v) {
  v = v * 1664525u + 1013904223u;
  v.x += v.y * v.z;
  v.y += v.z * v.x;
  v.z += v.x * v.y;
  v ^= v >> 16u;
  v.x += v.y * v.z;
  v.y += v.z * v.x;
  v.z += v.x * v.y;
  return v;
}

float cau_corner(ivec3 c, vec3 d, int seed) {
  uvec3 h = cau_pcg(uvec3(c + ivec3(seed, seed * 7, seed * 13)));
  return dot(vec3(h & 0xffffu) / 32767.5 - 1.0, d);
}

/// Quintic gradient noise, standard deviation 0.191.
float cau_noise(vec3 p, int seed) {
  ivec3 i = ivec3(floor(p));
  vec3 f = p - vec3(i);
  vec3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
  return mix(mix(mix(cau_corner(i, f, seed), cau_corner(i + ivec3(1, 0, 0), f - vec3(1, 0, 0), seed), u.x),
                 mix(cau_corner(i + ivec3(0, 1, 0), f - vec3(0, 1, 0), seed),
                     cau_corner(i + ivec3(1, 1, 0), f - vec3(1, 1, 0), seed), u.x), u.y),
             mix(mix(cau_corner(i + ivec3(0, 0, 1), f - vec3(0, 0, 1), seed),
                     cau_corner(i + ivec3(1, 0, 1), f - vec3(1, 0, 1), seed), u.x),
                 mix(cau_corner(i + ivec3(0, 1, 1), f - vec3(0, 1, 1), seed),
                     cau_corner(i + ivec3(1, 1, 1), f - vec3(1, 1, 1), seed), u.x), u.y), u.z);
}

/*!
 * Caustic light to add to the vertex colour: the envelope times a zero-mean pattern of world
 * position (3D, so walls and ceilings get it too) that loops at the original's tempo. Faded to
 * the mean where a cell shrinks under a few pixels. Call in uniform control flow.
 */
vec3 pbr_caustic() {
  if (cau_w == vec3(0.0)) {
    return vec3(0.0);
  }
  vec3 q = pbr_wpos * cau_a.x;
  float fade = 1.0 - smoothstep(0.025, 0.08, length(fwidth(q)));
  if (fade <= 0.0) {
    return vec3(0.0);
  }
  float n = (cau_noise(q + cau_b.xyz, 0) * cos(cau_a.y) +
             cau_noise(q - cau_b.xyz + 17.0, 1) * sin(cau_a.y)) * 5.236;
  float c = cau_a.w > 0.0 ? exp(-(n / cau_a.w) * (n / cau_a.w)) - cau_b.w : n;
  return pbr_cau * max(cau_a.z * c * fade, -1.0);
}

/// pbr_caustic() for a fragment of colour `base` x texture `t0`. On an 8-bit target the added
/// light is scaled down (hue kept) so the peak channel eases into 1 from 0.8 instead of clipping.
vec3 pbr_caustic_lit(vec3 base, vec3 t0) {
  vec3 c = pbr_caustic();
  if (cau_ldr == 0) {
    return c;
  }
  vec3 b0 = base * t0, b1 = (base + c) * t0;
  float p0 = max(b0.r, max(b0.g, b0.b));
  float p1 = max(b1.r, max(b1.g, b1.b));
  if (p1 <= 0.8 || p1 <= p0) {
    return c;
  }
  float p = 0.8 + 0.2 * (1.0 - exp(-(p1 - 0.8) / 0.2));
  return c * clamp((p - p0) / (p1 - p0), 0.0, 1.0);
}
