// Stylized material shading by class, shared fragment part (see Materials.h).
uniform int pbr_on;    // 0: vanilla path
uniform int pbr_mat;   // class (low 5 bits) + 32 swamp water + 128 `baked-tint`, per draw
uniform int pbr_view;  // PbrView
uniform float pbr_detail[13];
uniform float pbr_scale[13];  // 1 / tile metres (background), UV repeats (merc)
uniform float pbr_spec[13];
uniform float pbr_gloss[13];
uniform vec4 pbr_tint[13];  // rgb highlight colour, w: how much it follows the albedo hue
uniform vec3 pbr_ccol[13];  // debug class colours
uniform vec2 pbr_e_ramp;    // emissive E range over which highlights fade out
uniform int pbr_skip;       // perf opt: skip work whose result is multiplied by 0
uniform int pbr_src;        // PbrSource: 0 procedural array (unit 16), 1 per-texture maps (unit 17)
uniform vec2 pbr_draw;      // per draw: metallic, 1 if the texture has maps, 2 packed with a height
uniform sampler2DArray tex_T16;
// rgb tangent-space normal (+Y up the image), a roughness; packed: normal xy, roughness, height
uniform sampler2D tex_T17;
uniform vec4 pbr_px;       // parallax: depth (m, 0 = off), taps, grazing fade-in n.v range
uniform vec3 pbr_px_fade;  // parallax fade-out range (m) at the reference repeat, 1 / reference (m)
vec2 pbr_uv_shift = vec2(0.0);  // parallax UV offset of this fragment (pbr_parallax)
// last pbr_respond: roughness and F0 x lobe scale (0 where unshaded or emissive), for the
// local-light highlights
float pbr_hl_r = 1.0;
vec3 pbr_hl_f0 = vec3(0.0);

vec2 pbr_dn(vec2 uv, int c) {
  return texture(tex_T16, vec3(uv, float(c))).xy * 2.0 - 1.0;
}

/// Detail normal (xyz) and map roughness (w, < 0 without a map): Ng perturbed through the UV
/// cotangent frame of position p; `puv` is the procedural array coordinate.
vec4 pbr_uv_detail(vec3 Ng, vec3 p, vec2 uv, vec2 puv, int c) {
  vec3 dp1 = dFdx(p);
  vec3 dp2 = dFdy(p);
  vec2 du1 = dFdx(uv);
  vec2 du2 = dFdy(uv);
  vec3 r1 = cross(dp2, Ng);
  vec3 r2 = cross(Ng, dp1);
  vec3 T = r1 * du1.x + r2 * du2.x;
  vec3 B = r1 * du1.y + r2 * du2.y;
  float im = inversesqrt(max(max(dot(T, T), dot(B, B)), 1e-20));
  vec2 t = vec2(0.0);
  float rough = -1.0;
  if (pbr_src == 1) {
    if (pbr_draw.y > 1.5) {
      vec4 m = textureGrad(tex_T17, uv + pbr_uv_shift, du1, du2);
      vec2 xy = m.xy * 2.0 - 1.0;
      float z = sqrt(max(1.0 - dot(xy, xy), 0.0));
      t = vec2(xy.x, -xy.y) / max(z, 0.25) * pbr_detail[c];
      rough = m.z;
    } else if (pbr_draw.y > 0.5) {
      vec4 m = texture(tex_T17, uv);
      vec3 n = m.xyz * 2.0 - 1.0;
      // B follows +v, which runs down the image
      t = vec2(n.x, -n.y) / max(n.z, 0.25) * pbr_detail[c];
      rough = m.w;
    }
  } else {
    t = pbr_dn(puv, c) * pbr_detail[c];
  }
  return vec4(normalize(T * (im * t.x) + B * (im * t.y) + Ng), rough);
}

/// Blinn exponent: from the map roughness, else the class value.
float pbr_gloss_of(int c, float r) {
  return r < 0.0 ? pbr_gloss[c] : 2.0 / max(r * r, 0.004) + 2.0;
}

/// Highlight strength: the class value, a little stronger where the map is smooth.
float pbr_spec_of(int c, float r) {
  return pbr_spec[c] * (r < 0.0 ? 1.0 : 1.25 - 0.5 * r);
}

float pbr_lum(vec3 c) {
  return dot(c, vec3(0.299, 0.587, 0.114));
}

// soft two-step highlight: a faint broad band around a brighter core
float pbr_band(float b) {
  return 0.35 * smoothstep(0.08, 0.25, b) + 0.65 * smoothstep(0.45, 0.65, b);
}

vec3 pbr_highlight_tint(int c, vec3 albedo) {
  vec3 a = albedo / max(max(albedo.r, albedo.g), max(albedo.b, 1e-3));
  float w = pbr_src == 1 ? max(pbr_tint[c].w, 0.85 * pbr_draw.x) : pbr_tint[c].w;
  return pbr_tint[c].rgb * mix(vec3(1.0), a, w);
}

bool pbr_shaded(int c) {
  return c != 0 && c != 6 && c != 10;
}

// ---- material response: metallic workflow, mood environment, roughness lobe ----
uniform vec4 pbr_look;    // metallic workflow, environment, fresnel, ambient response
uniform vec4 pbr_look2;   // roughness contrast, band (0 GGX, 1 stylized), lobe scale, metal darkening
uniform float pbr_f0;      // dielectric reflectance at normal incidence
uniform vec3 pbr_cls3[13];  // per class: environment scale, roughness bias, lobe scale
uniform vec3 pbr_env_sky;  // mood environment, relative to the bake (mean luminance 1)
uniform vec3 pbr_env_hor;
uniform vec3 pbr_env_gnd;
uniform vec3 pbr_env_sun;  // broad glow around the sun direction
uniform float pbr_env_bake;  // 1 where no fitted light is live: metals take the bake's colour
// 1: metals reflect the bake's hue under a neutral mood with half the class tint; 0: as #284
uniform float pbr_metal_hue;
float pbr_env_sun_vis = 1.0;  // share of the sun glow pbr_env adds (water: the world map's light)
uniform vec3 pbr_up;       // world up in this path's frame
uniform float pbr_sheen[13];  // per class: grazing sheen (cloth, foliage)
const int PBR_ICE = 12;  // PbrClass (Materials.h)
uniform vec4 pbr_ice;      // class `ice`: strength (0: off), inner depth (m), absorption path (m), coat roughness
uniform float pbr_ice_tr;  // class `ice`: sun transmission gain
// per class: leaf transmission, its lobe power, relief wrap width, bake sun-term wrap width
uniform vec4 pbr_scatter[32];  // per class (PBR_CLASS_COUNT used): transmit, power, wrap, bake wrap
float pbr_sun_trans = 1.0;  // sun transmittance to the shaded point, for the backlight only

/// Wrapped N.L: light reaches `w` past the terminator (w = 0: max(x, 0)).
float pbr_wrap(float x, float w) {
  return max((x + w) / (1.0 + w), 0.0);
}

/// Roughness after the class bias and the look's contrast; class gloss where there is no map.
// set by a caller before pbr_respond: a water film's weight and roughness (pbr_bg_frag.glsl)
float pbr_wet_w = 0.0;
float pbr_wet_r = 0.15;
float pbr_wet_f0 = 0.08;

float pbr_rough_of(int c, float r) {
  float rr = r < 0.0 ? sqrt(2.0 / max(pbr_gloss[c] - 2.0, 0.5)) : r;
  return mix(clamp(0.5 + (rr - 0.5) * pbr_look2.x + pbr_cls3[c].y, 0.04, 1.0), pbr_wet_r, pbr_wet_w);
}

/// Hemisphere irradiance for normal N (ground below, sky above).
vec3 pbr_hemi(vec3 N) {
  float y = dot(N, pbr_up);
  return mix(mix(pbr_env_gnd, pbr_env_hor, clamp(y + 1.0, 0.0, 1.0)), pbr_env_sky, clamp(y, 0.0, 1.0));
}

/// Mood environment along R, blurred toward the hemisphere irradiance as roughness grows.
vec3 pbr_env(vec3 R, float r, vec3 L) {
  float y = dot(R, pbr_up);
  float w = 0.08 + 0.5 * r;
  vec3 e = y < 0.0 ? mix(pbr_env_hor, pbr_env_gnd, smoothstep(0.0, w + 0.15, -y))
                   : mix(pbr_env_hor, pbr_env_sky, smoothstep(0.0, w + 0.4, y));
  e = mix(e, pbr_hemi(R), r * r);
  float ex = mix(24.0, 3.0, r);
  return e + pbr_env_sun * (pbr_env_sun_vis * pow(max(dot(R, L), 0.0), ex));
}

/// Roughness-shaped lobe (GGX, optionally banded) times the Smith visibility, for one light.
vec3 pbr_lobe(vec3 N, vec3 V, vec3 L, float a, vec3 F0, float fres) {
  vec3 H = normalize(L + V);
  float ndh = max(dot(N, H), 0.0);
  float ndl = max(dot(N, L), 0.0);
  float ndv = max(dot(N, V), 1e-3);
  float a2 = a * a;
  float d = ndh * ndh * (a2 - 1.0) + 1.0;
  float g = a2 * a2 / (d * d);  // GGX / its peak (1 / a2)
  g = mix(g, pbr_band(g), pbr_look2.y);
  float k = 0.5 * a;
  float vis = 0.25 / ((ndl * (1.0 - k) + k) * (ndv * (1.0 - k) + k));
  vec3 F = F0 + (1.0 - F0) * pow(1.0 - max(dot(V, H), 0.0), 5.0) * fres;
  return g * min(1.0 / a2, 60.0) * vis * ndl * F;
}

/*!
 * Material response on top of the vanilla colour `rgb` = albedo x baked light `D` (1 = neutral).
 * Ng: geometric normal, Nd: detail normal, V: toward the eye, L/K: sun and key directions with
 * weights sv/kv and colours; `rough` from the map (< 0: class gloss), `metal` per texture;
 * `ef`: 1 - emissive fade. `spec` returns the added reflection and lobes, for the debug view.
 */
vec3 pbr_respond(vec3 rgb, vec3 albedo, vec3 D, vec3 Ng, vec3 Nd, vec3 V, float rough,
                 float metal, int c, float sv, vec3 L, vec3 Lcol, float kv, vec3 K, vec3 Kcol,
                 float ef, out vec3 spec) {
  float r = pbr_rough_of(c, rough);
  float a = max(r * r, 0.002);
  float m = clamp(metal * pbr_look.x, 0.0, 1.0);
  float mh = pbr_metal_hue;
  // the texture already carries its metal's colour; the class tint only nudges it
  vec3 F0 = mix(vec3(mix(pbr_f0, pbr_wet_f0, pbr_wet_w)), albedo * mix(pbr_tint[c].rgb, vec3(1.0), 0.5 * mh), m);
  float ndv = max(dot(Nd, V), 1e-3);
  // metals keep their hue at grazing angles (stylized)
  float fres = pbr_look.z * (1.0 - 0.65 * m);
  vec3 Fv = clamp(F0 + (max(vec3((1.0 - r) * (1.0 - r)), F0) - F0) * pow(1.0 - ndv, 5.0) * fres,
                 0.0, 1.0);

  float ndl = max(dot(Ng, L), 0.0);
  float ndk = max(dot(Ng, K), 0.0);
  // relief: sun and key on the detail normal; the mood hemisphere where the bake is ambient
  float rg = 1.0 + 2.0 * pbr_look.w;
  float w = pbr_scatter[c].z;
  float relief = 1.0 + ((pbr_wrap(dot(Nd, L), w) - pbr_wrap(dot(Ng, L), w)) * sv +
                        (pbr_wrap(dot(Nd, K), w) - pbr_wrap(dot(Ng, K), w)) * kv) * rg;
  float hg = pbr_lum(pbr_hemi(Ng));
  float amb = 1.0 + pbr_look.w * 2.5 * (pbr_lum(pbr_hemi(Nd)) - hg) / max(hg, 1e-3);
  relief = max(relief, 0.0) * mix(max(amb, 0.0), 1.0, 0.5 * clamp(sv, 0.0, 1.0));
  float kd = (1.0 - m) * (1.0 - pbr_look2.w * metal);
  // dielectrics reflect the world only where they are smooth; rough ones already have it in the bake
  float es = pbr_look.y * pbr_cls3[c].x * mix((1.0 - r) * (1.0 - r), 1.0, m);
  vec3 diff = rgb * relief * kd * (1.0 - pbr_lum(Fv) * (1.0 - m) * min(es, 1.0));

  vec3 env = pbr_env(reflect(-V, Nd), r, L);
  // metals take the light's hue from the bake, so the mood's hue would count twice
  env = mix(env, vec3(pbr_lum(env)), m * mh);
  // `baked-tint` metals (eco vent dishes) always keep the bake's hue
  float kt = (pbr_mat & 128) != 0 ? m : m * mix(pbr_env_bake, 1.0, mh);
  vec3 refl = Fv * mix(vec3(pbr_lum(D)), D, kt) * mix(env * es, mix(vec3(1.0), env, min(es, 1.0)), m);

  vec3 lobe = vec3(0.0);
  if (pbr_skip == 0 || sv * smoothstep(0.0, 0.25, ndl) != 0.0) {
    lobe += pbr_lobe(Nd, V, L, a, F0, fres) * Lcol * sv * smoothstep(0.0, 0.25, ndl);
  }
  if (pbr_skip == 0 || kv * smoothstep(0.0, 0.25, ndk) != 0.0) {
    lobe += pbr_lobe(Nd, V, K, a, F0, fres) * Kcol * kv * smoothstep(0.0, 0.25, ndk);
  }
  // sheen: a soft grazing rim in the albedo hue, lit by the bake
  vec3 sheen = pbr_sheen[c] * pow(1.0 - ndv, 3.0) * albedo * pbr_lum(D) * pbr_look.z;
  spec = refl + lobe * pbr_look2.z * pbr_cls3[c].z + sheen;
  if (pbr_scatter[c].x > 0.0) {
    // sun behind the leaf (Ng faces the eye), in the leaf's own hue; none once the sun sets
    spec += pbr_scatter[c].x * pbr_sun_trans * albedo * albedo * Lcol * sv * max(-dot(Ng, L), 0.0) *
            pow(max(dot(-V, L), 0.0), pbr_scatter[c].y) * smoothstep(0.0, 0.25, dot(L, pbr_up));
  }
  pbr_hl_r = r;
  pbr_hl_f0 = F0 * (pbr_cls3[c].z * ef);
  return mix(rgb, diff + spec, ef);
}

/// Highlight material of the emissive mask's b and a: roughness, 1 - sqrt(lum(F0) x lobe).
/// (0, 1) is "none", what every writer outside the material shading leaves.
vec2 pbr_hl_pack(float r, vec3 F0) {
  return vec2(r, 1.0 - sqrt(clamp(pbr_lum(F0), 0.0, 1.0)));
}

/// Debug views (PbrView) over the shaded colour `lit`.
vec3 pbr_debug_view(int c, vec3 rgb, vec3 Nd, vec3 hl, vec3 vis, float rough, vec3 lit) {
  if (pbr_view == 1) {
    return pbr_ccol[c] * (0.35 + 0.65 * pbr_lum(rgb));
  } else if (pbr_view == 2) {
    return Nd * 0.5 + 0.5;
  } else if (pbr_view == 3) {
    return hl;
  } else if (pbr_view == 4) {
    return vis;
  } else if (pbr_view == 5) {
    return vec3(pbr_rough_of(c, rough), pbr_draw.x, 0.0);
  } else if (pbr_view == 6) {
    return pbr_hemi(Nd) * 0.5;
  } else if (pbr_view == 7) {
    // parallax: r packed map, g offset (1 = 1/256 UV), b depth uniform (1 = 8 cm)
    return vec3(pbr_draw.y > 1.5 ? 1.0 : 0.0, length(pbr_uv_shift) * 256.0, pbr_px.x / 0.08);
  }
  return lit;
}

// ---- lava rules (LavaField.h): glow of lava-class texels, the lava field's light from below ----
uniform int lava_on;     // 1 glow; 2 lava field, 4 the field gives diffuse light
uniform vec4 lava_glow;  // chroma key from / full, HDR gain, emissive mask of a molten texel
uniform vec3 lava_glow2; // white-hot core strength, crust darkening, white cap (max channel)
uniform int lava_view;   // debug: 1 field view factor, 2 the added lava light alone
float lava_e = 0.0;      // emissive mask of this fragment from the glow

/// Molten weight of a texel: blackbody chroma key R - max(0.6 G, B) between the two thresholds.
float lava_molten(vec3 albedo) {
  return smoothstep(lava_glow.x, lava_glow.y, albedo.r - max(0.6 * albedo.g, albedo.b));
}

/// Soft limit of c's max channel toward `cap` from 0.7 cap, hue kept: the toon tonemap clips
/// per channel above its halo start, which turns orange yellow-white.
vec3 lava_soft_cap(vec3 c, float cap) {
  float m = max(c.r, max(c.g, c.b));
  float k = 0.7 * cap;
  if (m <= k) {
    return c;
  }
  float w = cap - k;
  return c * ((k + w * (1.0 - exp(-(m - k) / w))) / m);
}

/// Molten texels: the vanilla colour `rgb` with HDR headroom, the body held orange under the
/// white cap (the toon tonemap clips per channel above 1.2, which turns orange yellow-white); the
/// brightest (R + G) add a yellow-hot core whose centre may pass the cap a little (to 1.65 at
/// most: yellow, never white). The mask blooms the cores fully and the body a quarter, so a
/// large lava field does not smear the frame. The crust's response is darkened for contrast by
/// `dark` x the crust setting: 1 on lava floors and lakes, 0 on walls (the wall-base lava
/// textures meet plain rock textures at a seam, where darker crust read as a band with an edge).
vec3 lava_glow_mix(vec3 crust, vec3 rgb, float m, vec3 albedo, float dark) {
  float heat = m * smoothstep(0.95, 1.3, albedo.r + albedo.g);
  lava_e = m * lava_glow.w * (0.25 + 0.75 * heat);
  vec3 body = lava_soft_cap(rgb * (1.0 + lava_glow.z * m), lava_glow2.z);
  vec3 hot = lava_soft_cap(body + lava_glow2.x * heat * heat * vec3(1.0, 0.72, 0.32),
                           lava_glow2.z + 0.5 * heat);
  return mix(crust * (1.0 - lava_glow2.y * dark), hot, m);
}

uniform sampler2D lava_tex;   // r lava coverage (mips)
uniform sampler2D lava_near;  // r slab top, g nearest lava height (both - lava_fld.y), b distance
                              // to it (m); linear, no mips
uniform vec4 lava_box;        // field origin x, z (m), 1 / extent x, z (m)
uniform vec4 lava_fld;        // texel (m), height origin (m), reach above the lava (m), last mip
uniform vec4 lava_light;      // the lava's light (the game's dir2 x gain), reflection share
uniform vec3 lava_under_k;    // bake share cut, field margin (m), white cap of a lit surface

/// Lava light `add` on a surface whose colour is `lit`: what fits under the white cap, soft
/// (tanh of the headroom), so a lit surface stays orange-lit, never clipped white.
vec3 lava_fit(vec3 lit, vec3 add) {
  float head = max(lava_under_k.z - max(lit.r, max(lit.g, lit.b)), 0.0);
  float a = max(add.r, max(add.g, add.b));
  if (a <= 1e-5 || head <= 1e-5) {
    return vec3(0.0);
  }
  float x = min(a / head, 8.0);
  float t = (1.0 - exp(-2.0 * x)) / (1.0 + exp(-2.0 * x));
  return add * (head * t / a);
}

float lava_cov(vec2 p, float r) {
  float lod = clamp(log2(max(2.0 * r, lava_fld.x) / lava_fld.x), 0.0, lava_fld.w);
  return textureLod(lava_tex, (p - lava_box.xy) * lava_box.zw, lod).r;
}

/*!
 * Lava light on a surface at world `p` (m) with normal N: the lava as an area light below. The
 * view factor of lava under a point h above it is taken from the coverage of discs of radius h/2,
 * h and 2h centred toward where N faces (r^2 / (r^2 + h^2) bands); facing (1 - N.y) / 2, so
 * floors take none. Inside the column's slab (under its first ceiling) and within the reach.
 * rgb: the light, a: the view factor alone.
 */
vec4 lava_under(vec3 p, vec3 N) {
  vec2 uv = (p.xz - lava_box.xy) * lava_box.zw;
  if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
    return vec4(0.0);  // outside the field's box (the edge texels would clamp)
  }
  vec3 s = textureLod(lava_near, uv, 0.0).rgb;
  float Z = lava_fld.z;
  float dh = p.y - (lava_fld.y + s.y);
  if (dh < -0.5 || dh > Z || s.z > lava_under_k.y) {
    return vec4(0.0);
  }
  float h = max(dh, 1.0);
  vec2 c = p.xz + N.xz * h;
  float vf = 0.2 * lava_cov(c, 0.5 * h) + 0.3 * lava_cov(c, h) + 0.3 * lava_cov(c, 2.0 * h) +
             0.2 * lava_cov(c, 4.0 * h);
  // full near the lava, an S-shaped fade to nothing at the reach: no edge where the light ends;
  // the same fade away from the lava sideways (gone at 60% of the field's margin)
  float up = 1.0 - smoothstep(0.1 * Z, Z, dh);
  vf *= smoothstep(-0.5, 0.0, dh) * up * up *
        (1.0 - smoothstep(0.2 * lava_under_k.y, 0.6 * lava_under_k.y, s.z)) *
        (1.0 - smoothstep(lava_fld.y + s.x + 0.3, lava_fld.y + s.x + 0.8, p.y));
  return vec4(lava_light.rgb * (vf * 0.5 * (1.0 - N.y)), vf);
}
