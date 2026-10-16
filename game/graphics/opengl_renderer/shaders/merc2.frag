#version 410 core

layout(location = 0) out vec4 color;
// emissive mask, draw buffer 2: every depth-writing draw writes its own E (Merc2::do_draws)
layout(location = 2) out vec4 emissive_out;
uniform float em_value;
uniform float em_unlit;  // share of em_value that gives no tile light (mask green)
uniform int em_mat;      // 1: b, a carry the highlight material, 2: write none (a = 1)
uniform vec2 llh_merc;   // actor-light highlights: strength (0 = off), soft limit (brightest channel)
uniform int llh_prop;    // merc level prop: baked lights' highlight comes from the tile pass
SSAO_NORMAL_FRAG
INSPECT_FRAG
in vec4 vtx_color;
in vec2 vtx_st;
in float fog;

uniform sampler2D tex_T0;

uniform vec4 fog_color;
in float haze_f;
uniform vec3 haze_color;
uniform int ignore_alpha;
uniform vec4 light_dir0_fade;
uniform vec4 light_dir1_fade_en;

uniform int decal_enable;

uniform int gfx_hack_no_tex;

uniform int fur_on;
uniform vec4 fur_tex;  // density (strands per UV unit), root darkening, view, 1 if MSAA target
uniform vec2 fur_body;  // this draw's root darkening; strand coverage kept where the surface faces the camera
in float fur_h;

float fur_hash(vec2 c) {
  return fract(sin(dot(c, vec2(127.1, 311.7))) * 43758.5453);
}

in vec3 shadow_cpos;
in vec3 shadow_cnrm;
in vec4 vtx_sun;
in float vtx_light_lum;
in vec3 vtx_base;  // the vertex's own colour, before the level light
uniform int merc_emitter;  // orbs, cells, lamps, crystals: level light only
uniform float shadow_cover;  // share of the sun a roof overhead takes from this actor
uniform float shadow_rig_sun;  // share of this actor's rig that is the sun (kits keep theirs)

ACTOR_SHADOW_FRAG
CAVE_DARK_FRAG
PBR_COMMON_FRAG
uniform float pbr_vsign;  // sign of the camera-space view vector
in vec3 pbr_mpos;
uniform mat4 lava_c2w;    // merc camera space to world (game units)
uniform float lava_char;  // lava field light on characters, times the background's
uniform float lava_vanilla;  // 1 where the mood's dir2 already lights characters from below
uniform float lava_gain;     // the field's gain (lava_light = the game's light x this)
in vec3 pbr_mnrm;
in vec4 pbr_key;
in vec3 pbr_kcol;

/// Highlight of the actor lights (shadow_actor_lights' lights, falloff and shadows) on the last
/// pbr_respond material, at camera-space position p (normal n, toward the eye V).
vec3 merc_light_spec(vec3 p, vec3 n, vec3 V) {
  float a = max(pbr_hl_r * pbr_hl_r, 0.002);
  vec3 s = vec3(0.0);
  for (int i = 0; i < 4; i++) {
    if (i >= shadow_nlights) {
      break;
    }
    vec4 lp = shadow_lamp_pos[i];
    vec4 lc = shadow_lamp_col[i];
    vec3 d = lp.xyz - p;
    float d2 = dot(d, d);
    float win = clamp(1.0 - d2 / (lc.w * lc.w), 0.0, 1.0);
    float att = win * win / (1.0 + d2 / (lp.w * lp.w));
    vec3 L = d * inversesqrt(max(d2, 1.0));
    bool added = shadow_light_added[i] > 0.5;
    if (att > 0.0 && dot(n, L) > 0.0 && (added || llh_prop == 0)) {
      float vis = i < shadow_nlamps ? 1.0 - shadow_lamp(i, p + n * 256.0) : 1.0;
      float k = added ? 2.5 : 1.0;
      s += pbr_lobe(n, V, L, a, pbr_hl_f0, 1.0) * lc.rgb * (att * vis * k);
    }
  }
  // vertex-colour units (0.5 = as drawn) to colour, then the soft limit on the brightest channel
  s *= 2.0 * llh_merc.x;
  return s * (llh_merc.y / (llh_merc.y + max(s.r, max(s.g, s.b))));
}

float merc_lava_m = -1.0;  // last pbr_merc: molten share of a lava-class texel (< 0: not lava)

/// Character shading on top of the vanilla colour: UV detail normal, key-light highlight.
/*!
 * Ice on characters (class `ice`, the background rule in camera space): inner layer sampled along
 * the refracted view ray and tinted by the texture's own colour, a sky coat, the key light's lobe
 * with a soft limit, and the key light through faces turned from it, in the ice's blue.
 */
vec3 pbr_ice_merc(vec3 lit, vec3 rgb, vec3 albedo, vec3 light, vec3 Ng, vec3 Nd, vec3 V, vec3 L,
                  vec3 Lcol, float sv) {
  vec3 p = pbr_mpos * (pbr_vsign / 4096.0);
  vec3 dp1 = dFdx(p);
  vec3 dp2 = dFdy(p);
  vec2 du1 = dFdx(vtx_st);
  vec2 du2 = dFdy(vtx_st);
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
  vec3 t = (Ng * ndv - V) / 1.31;
  float cosr = sqrt(max(1.0 - dot(t, t), 0.09));
  vec2 duv = vec2(dot(gu, t), dot(gv, t)) * (pbr_ice.y / cosr);
  duv *= min(1.0, 0.5 / max(length(duv), 1e-6));
  vec4 inner = textureGrad(tex_T0, vtx_st + duv, du1 * 4.0, du2 * 4.0);
  vec3 ia = inner.a < 0.25 ? albedo : inner.rgb;
  float path = 2.0 * pbr_ice.y / cosr / pbr_ice.z * k;
  vec3 tc = albedo / max(max(albedo.r, albedo.g), max(albedo.b, 1e-3));
  vec3 lt = light * clamp(pbr_lum(rgb) / max(pbr_lum(albedo * light), 1e-4), 0.0, 2.0);
  vec3 interior = ia * lt * pow(clamp(tc, 0.05, 1.0), vec3(path)) * 0.7;
  vec3 col = mix(lit, interior, w * min(0.7 * k, 0.95));
  vec3 Nc = normalize(mix(Ng, Nd, 0.3));
  float rc = pbr_ice.w;
  float F = 0.04 + 0.96 * pow(1.0 - max(dot(Nc, V), 1e-3), 5.0);
  vec3 R = reflect(-V, Nc);
  vec3 sky = pbr_env(R, rc, -R) * pbr_lum(light) * 0.35;
  sky = mix(vec3(pbr_lum(sky)), sky, 0.5);
  col = mix(col, sky, clamp(F * (1.0 + k), 0.0, 0.6));
  vec3 Nh = normalize(mix(Ng, Nd, 0.5));
  vec3 hv = L + V;
  vec3 H = dot(hv, hv) > 1e-6 ? normalize(hv) : Nh;
  float ndh = max(dot(Nh, H), 0.0);
  float a2 = rc * rc * rc * rc;
  float dd = ndh * ndh * (a2 - 1.0) + 1.0;
  float Fh = 0.04 + 0.96 * pow(1.0 - max(dot(V, H), 0.0), 5.0);
  float hl = (a2 * a2 / (dd * dd)) * Fh * smoothstep(0.0, 0.2, dot(Nh, L)) * sv * 8.0 * k * w;
  col += Lcol * 3.0 * (1.0 - exp(-hl / 3.0));
  float back = clamp(-dot(Ng, L), 0.0, 1.0);
  float toward = pow(clamp(dot(-V, L), 0.0, 1.0), 6.0);
  float thin = mix(0.4, 1.0, (1.0 - ndv) * (1.0 - ndv));
  vec3 tcol = tc * tc / max(max(tc.r * tc.r, tc.g * tc.g), max(tc.b * tc.b, 1e-3));
  vec3 tr = pbr_lum(Lcol) * tcol * back * (0.35 + 1.5 * toward) * thin * sv * pbr_ice_tr * k * w;
  float tm = max(max(tr.r, tr.g), max(tr.b, 1e-4));
  col += tr * (1.5 * (1.0 - exp(-tm / 1.5)) / tm);
  return col;
}

vec3 pbr_merc(vec3 rgb, vec3 albedo, vec3 lighting, float key_scale) {
  int c = pbr_mat & 31;
  // lava class (lakes drawn as actors): crust shades as stone, molten texels glow
  float lava_m = -1.0;
  if (c == 10 && (lava_on & 1) != 0) {
    lava_m = lava_molten(albedo);
    merc_lava_m = lava_m;
    c = 1;
  }
  vec3 Ng = normalize(pbr_mnrm);
  vec3 V = normalize(-pbr_mpos) * pbr_vsign;
  // cotangent frame from screen derivatives (no tangents in MercVertex)
  vec4 d = pbr_uv_detail(Ng, pbr_mpos, vtx_st, vtx_st * pbr_scale[c], c);
  vec3 Nd = d.xyz;
  vec3 L = normalize(pbr_key.xyz);
  float sv = pbr_shaded(c) ? pbr_key.w : 0.0;
  vec3 hl;
  vec3 lit = pbr_respond(rgb, albedo, lighting, Ng, Nd, V, d.w, pbr_draw.x, c, sv, L,
                         pbr_kcol * key_scale,
                         0.0, L, vec3(0.0), pbr_shaded(c) ? 1.0 : 0.0, hl);
  if (c == PBR_ICE && pbr_ice.x > 0.0) {
    lit = pbr_ice_merc(lit, rgb, albedo, lighting, Ng, Nd, V, L, pbr_kcol * key_scale, sv);
  }
  if (lava_m >= 0.0) {
    // lakes (facing up) keep the darkened crust; anything else is never darker than lava off
    float lava_floor = smoothstep(0.6, 0.9, dot(Ng, pbr_up));
    lit = lava_glow_mix(lit, rgb, lava_m, albedo, lava_floor);
    lit = mix(max(lit, rgb), lit, lava_floor);
  }
  if (pbr_view == 4) {
    return dot(Ng, V) > 0.0 ? vec3(0.2, 1.0, 0.2) : vec3(1.0, 0.2, 0.2);
  }
  // molten lava is no stone: no highlight
  if (llh_merc.x > 0.0 && shadow_on == 1 && merc_emitter == 0 && pbr_shaded(c) && lava_m <= 0.0) {
    lit += merc_light_spec(shadow_cpos, Nd, V);
  }
  return pbr_debug_view(c, rgb, Nd, hl, vec3(sv), d.w, lit);
}

// inland water (InlandWater.h): per draw 0 off, 1 clear, 2 mud, + 4 on a level with a sky
uniform int iw_kind;
uniform sampler2D iw_depth;  // SSAO depth copy: camera z (m) of the bed
uniform int iw_depth_ok;
uniform int iw_view;  // debug: 1 depth factor as opaque grey, 2-7 the sun glint's inputs
uniform vec3 iw_lin;         // fragment depth to camera z, as the depth copy
uniform vec4 iw_proj;        // pixel to camera xy per metre of z
uniform vec4 iw_water;       // absorption length (m), reflection, reflection cap, glint
uniform vec4 iw_mud;         // darkening, reflection cap, roughness, glint
uniform vec4 iw_sun;         // merc camera frame, toward the sun; w 1 when there is one
uniform vec3 iw_sun_col;
uniform mat4 iw_c2w;         // merc camera space to world (game units), with the glint on
uniform float iw_time;       // water clock (s)
uniform float tm_toon_knee;  // the final pass's toon shoulder (encoded luminance)

WATER_SUN_GLINT

/// Share of the game's water alpha this pixel's view ray earns through the water (Beer-Lambert).
float iw_column() {
  if (iw_depth_ok == 0) {
    return 1.0;
  }
  ivec2 p = ivec2(gl_FragCoord.xy);
  float bed = texelFetch(iw_depth, min(p, textureSize(iw_depth, 0) - ivec2(1)), 0).r;
  float surf = abs(iw_lin.x / (iw_lin.y * (2.0 * gl_FragCoord.z - 1.0) - iw_lin.z)) / 4096.0;
  if (bed > 5000.0) {
    return 1.0;
  }
  vec2 xy = (vec2(p) + 0.5) * iw_proj.xy + iw_proj.zw;
  float ray = max(bed - surf, 0.0) * length(vec3(xy, 1.0));
  return 1.0 - exp(-ray / iw_water.x);
}

/// The sky the ocean reflects: mood environment along R at the fog colour's brightness.
vec3 iw_sky(vec3 R, float r) {
  vec3 L = iw_sun.w > 0.5 ? iw_sun.xyz : pbr_up;
  vec3 e = pbr_env(R, r, L);
  return min(e * pbr_lum(fog_color.rgb) / max(pbr_lum(pbr_env_hor), 1e-3), vec3(0.97));
}

/// Inland sun glint (linear) over the encoded colour `base`: a broad sheen and the sharp lobe's
/// body held just under the toon shoulder, and only the sharp lobe's top 15% hot.
vec3 iw_glint(vec3 N, vec3 V, vec3 L, float var, vec3 base) {
  vec3 sheen = 0.25 * wsg_radiance(N, V, L, iw_sun_col, pbr_up, var);
  float a2 = 0.0625 * sun_glint.y * sun_glint.y + var;
  vec3 spark = 0.4 * wsg_lobe(N, V, L, iw_sun_col, pbr_up, a2, sun_glint.z);
  float sy = pbr_lum(spark);
  float k = 0.34 * sun_glint.z;
  vec3 hot = sy > k ? spark * ((sy - k) / (0.15 * sy)) : vec3(0.0);
  sheen += sy > 1e-6 ? spark * (min(sy, k) / sy) : vec3(0.0);
  float knee = pow(clamp(tm_toon_knee - 0.02, 0.0, 1.0), 2.2);
  float room = max(knee - pbr_lum(pow(max(base, vec3(0.0)), vec3(2.2))), 0.0);
  float y = pbr_lum(sheen);
  sheen *= y > 1e-6 ? room * (1.0 - exp(-y / max(room, 1e-4))) / y : 0.0;
  return sheen + hot;
}

/// Clear water or mud over the game's colour `c`; returns the colour and alpha to blend.
float iw_wv = -2.0;  // ws_water_vis at the fragment, read once (-2: not yet)

float iw_water_vis_raw() {
  if (iw_wv < -1.5) {
    iw_wv = ws_water_vis(shadow_cpos);
  }
  return iw_wv;
}

vec4 inland_water(vec4 c) {
  // shadow_cpos is true camera space (w = -1 divided out): no pbr_vsign here (symptoms #297)
  vec3 V = normalize(-shadow_cpos);
  vec3 N = normalize(shadow_cnrm);
  if (dot(N, V) < 0.0) {
    N = -N;
  }
  // the glint's normal: world up and the ripples (the mesh's smooth normals lean)
  vec3 Ng = N;
  float glint_var = 0.0;
  if (sun_glint.w > 0.5) {
    mat3 w2c = transpose(mat3(iw_c2w));
    vec3 up = normalize(pbr_up);
    vec3 ex = normalize(w2c[0] - dot(w2c[0], up) * up);
    vec3 ez = normalize(w2c[2] - dot(w2c[2], up) * up);
    vec2 p = (iw_c2w * vec4(shadow_cpos, 1.0)).xz / 4096.0;
    float lost;
    vec2 g = wsg_ripple(p, dFdx(p), dFdy(p), iw_time, lost);
    Ng = normalize(up - g.x * ex - g.y * ez);
    glint_var = wsg_pixel_var(Ng) + lost;
  }
  bool sky = (iw_kind & 4) != 0;
  float ndv = max(dot(N, V), 1e-3);
  float schlick = 0.02 + 0.98 * pow(1.0 - ndv, 5.0);
  vec3 L = iw_sun.xyz;
  float sun = sky ? iw_sun.w : 0.0;
  // the world sun map takes every sun term on the water, the sky's sun glow too
  float wv = sun > 0.0 ? iw_water_vis_raw() : -1.0;  // open where the map is off or out of reach
  pbr_env_sun_vis = wv < 0.0 ? 1.0 : wv;
  sun *= pbr_env_sun_vis;
  if ((iw_kind & 2) != 0) {
    float r = iw_mud.z;
    float F = min(schlick, iw_mud.y) * (sky ? 1.0 : 0.0);
    vec3 rgb = mix(c.rgb * iw_mud.x, iw_sky(reflect(-V, N), r), F);
    rgb += pbr_lobe(N, V, L, r * r, vec3(0.02), 1.0) * iw_sun_col * sun * iw_mud.w;
    return vec4(rgb, c.a);
  }
  // seen from below the surface (Jak diving): the game's sheet
  if (dot(V, pbr_up) <= 0.0) {
    return c;
  }
  float col = iw_column();
  if (iw_view == 1) {
    return vec4(vec3(col), 1.0);
  }
  if (iw_view >= 2 && iw_view <= 7) {
    // 2 sun, 3 glint normal, 4 up, 5 mesh normal (merc camera space), 6 glint, 7 sun the
    // world map leaves (blue: no cascade reaches)
    vec3 sg = iw_glint(Ng, V, L, glint_var, vec3(0.0)) * sun;
    float wv = ws_water_vis(shadow_cpos);
    vec3 o = iw_view == 2 ? L : (iw_view == 3 ? Ng : (iw_view == 4 ? pbr_up : N));
    o = iw_view == 6 ? sg : (iw_view == 7 ? (wv < 0.0 ? vec3(0, 0, 1) : vec3(wv)) : o * 0.5 + 0.5);
    return vec4(o, 1.0);
  }
  float a = c.a * col;
  if (!sky) {
    return vec4(c.rgb, a);
  }
  float F = min(schlick * iw_water.y, iw_water.z);
  // glint capped so a grazing peak stays a highlight, not a firefly; added, not coverage
  vec3 g = min(pbr_lobe(N, V, L, 0.06, vec3(0.02), 1.0) * iw_sun_col * sun * iw_water.w, vec3(0.5));
  float alpha = max(1.0 - (1.0 - a) * (1.0 - F), 0.02);
  if (sun_glint.w > 0.5) {
    // the analytic sun glint replaces the capped lobe, added in linear light
    vec3 base = c.rgb * a * (1.0 - F) + iw_sky(reflect(-V, N), 0.05) * F;
    vec3 sg = iw_glint(Ng, V, L, glint_var, base) * sun;
    // a bright glint covers the bed
    alpha = max(alpha, smoothstep(0.0, 0.5, pbr_lum(sg)));
    return vec4(wsg_add(base, sg) / alpha, alpha);
  }
  vec3 rgb = c.rgb * a * (1.0 - F) + iw_sky(reflect(-V, N), 0.05) * F + g;
  return vec4(rgb / alpha, alpha);
}

void main() {
  ws_begin(shadow_cpos);
  // strand coverage, before any discard so the derivatives stay defined
  float fur_cov = 1.0;
  if (fur_on == 1) {
    vec2 g = vtx_st * fur_tex.x;
    vec2 c = floor(g);
    float len = 0.35 + 0.65 * fur_hash(c);
    vec2 f = fract(g) - 0.5 + 0.25 * (vec2(fur_hash(c + 7.1), fur_hash(c + 3.7)) - 0.5);
    float d = length(f) - 0.6 * (1.0 - fur_h / len);
    float aa = max(fwidth(length(f)), 1e-4);
    fur_cov = fur_h > len ? 0.0 : 1.0 - smoothstep(-aa, aa, d);
    float facing = abs(dot(normalize(shadow_cpos), normalize(shadow_cnrm)));
    fur_cov *= mix(1.0, fur_body.y, smoothstep(0.15, 0.6, facing));
  }
  ssao_normal = ssao_normal_value();
  vec4 T0_pbr = vec4(1.0);
  // the light the PBR response reflects: the vanilla rig, or the shadowed and relit one below
  vec3 pbr_lighting = vtx_color.rgb * 2.0;
  float pbr_key_scale = 1.0;
  if (gfx_hack_no_tex == 0) {
    vec4 T0 = texture(tex_T0, vtx_st);
    T0_pbr = T0;
    // all merc is tcc=rgba and modulate
    if (decal_enable == 0) {
      color = vtx_color * T0 * 2;
      if (shadow_on == 1) {
        // normal offset against self-shadow acne (about 6 cm)
        vec3 p = shadow_cpos + normalize(shadow_cnrm) * 256.0;
        float occ = shadow_sun(p, 0.0006);
        if ((ws_on & 2) != 0 && merc_emitter == 0) {
          // the world map per pixel, in place of the roof cover (shadow_cover is 0 then); water
          // bodies take the water penumbra, as their glint does
          float wv = iw_kind != 0 ? iw_water_vis_raw() : -1.0;
          occ = max(occ, (1.0 - (wv >= 0.0 ? wv : ws_sun_vis(shadow_cpos))) * shadow_rig_sun);
        }
        vec3 canopy = merc_emitter == 0 ? shadow_canopy_factor(shadow_cpos, occ) : vec3(1.0);
        occ = max(occ, shadow_cover);
        vec3 lit = max(vtx_color.rgb - shadow_strength * occ * vtx_sun.rgb, vec3(0.0)) * canopy;
        vec3 local_dbg = vec3(0.0);
        float dark = cave_dark(shadow_cpos);
        if (merc_emitter == 0) {
          lit = lit * dark +
                vtx_base * shadow_actor_lights(shadow_cpos, shadow_cnrm, local_dbg);
        }
        pbr_lighting = lit * 2.0;
        pbr_key_scale =
            merc_emitter == 0 ? dark * max(1.0 - shadow_strength * occ, 0.0) * dot(canopy, vec3(0.299, 0.587, 0.114)) : 1.0;
        if (shadow_view == 4) {
          // local lights: grey base, each light's contribution in its tint (1 red .. 4 yellow)
          color.rgb = vec3(dot(vtx_color.rgb, vec3(0.299, 0.587, 0.114))) * 0.3 + local_dbg * 2.0;
        } else if (shadow_view == 1) {
          color.rgb = vec3(1.0 - occ);
        } else if (shadow_view == 3) {
          color.rgb = (shadow_mtx_sun * vec4(shadow_cpos, 1.0)).xyz * 0.5 + 0.5;
        } else if (shadow_view == 2) {
          color.rgb = vec3(dot(vtx_sun.rgb, vec3(0.299, 0.587, 0.114)) /
                           max(dot(vtx_color.rgb, vec3(0.299, 0.587, 0.114)), 1e-3));
        } else {
          color.rgb = lit * T0.rgb * 2;
        }
      }
    } else {
      color = T0;
    }
    color.a *= 2;
  } else {
    color.rgb = vtx_color.rgb;

    if (decal_enable == 0) {
      color.a = vtx_color.a * 2;
    } else {
      color.a = 1;
    }
  }

  if (light_dir1_fade_en.w > 0) {
    color.a = light_dir0_fade.w;
  } else if (light_dir1_fade_en.w < 0) {
    color.a *= light_dir0_fade.w;
  }


  if (ignore_alpha == 0 && color.w < 0.128) {
    discard;
  }
  if (fur_on == 1) {
    // one strand per cell, random length, tapering to its tip; soft edge for alpha to coverage
    // without MSAA alpha to coverage is a no-op: fall back to a 50 % alpha test
    if (fur_cov <= 0.0 || (fur_tex.w == 0.0 && fur_cov < 0.5)) {
      discard;
    }
    color.rgb *= mix(1.0 - fur_body.x, 1.0, fur_h);
    if (fur_tex.z == 1.0) {
      color.rgb = vec3(fur_h, 1.0 - fur_h, 0.2);
    }
    color.a = fur_tex.w == 0.0 ? 1.0 : fur_cov;
  }

  if (iw_kind != 0 && pbr_on != 0) {
    color = inland_water(color);
  } else if (pbr_on != 0 && gfx_hack_no_tex == 0 && decal_enable == 0) {
    color.rgb = pbr_merc(color.rgb, T0_pbr.rgb, pbr_lighting, pbr_key_scale);
    // characters over lava: the lava field's light from below (LavaField.h)
    // only what the field gives beyond vanilla's own dir2 under-light (no double count)
    if ((lava_on & 2) != 0 && (pbr_mat & 31) != 10) {
      vec3 wp = (lava_c2w * vec4(shadow_cpos, 1.0)).xyz * (1.0 / 4096.0);
      vec3 wn = normalize((lava_c2w * vec4(shadow_cnrm, 0.0)).xyz);
      vec3 f = lava_under(wp, wn).rgb * lava_char;
      vec3 v = lava_vanilla * lava_light.rgb / max(lava_gain, 1e-3) * max(-wn.y, 0.0);
      vec3 add = T0_pbr.rgb * max(f - v, vec3(0.0));
      color.rgb += lava_view == 2 ? add - color.rgb : lava_fit(color.rgb, add);
    }
  }
   color.xyz = mix(color.xyz, fog_color.rgb, clamp(fog_color.a * fog, 0, 1));
  color.xyz = mix(color.xyz, haze_color, haze_f);
  ssao_normal = ssao_normal_fog(ssao_normal, color.a, (1. - clamp(fog_color.a * fog, 0, 1)) * (1. - haze_f));
  // em_mat 1 without a material written (no PBR, decal, no texture): a = 1, none
  vec2 hl = vec2(0.0, em_mat >= 1 ? 1.0 : color.a);
  if (em_mat == 1 && pbr_on != 0 && gfx_hack_no_tex == 0 && decal_enable == 0 &&
      merc_lava_m <= 0.0) {
    hl = pbr_hl_pack(pbr_hl_r, pbr_hl_f0);
  }
  emissive_out =
      vec4(max(em_value, lava_e), em_value * em_unlit + max(lava_e - em_value, 0.0), hl);
  inspect_write(vtx_st);
}
