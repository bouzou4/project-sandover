#version 410 core


layout(location = 0) out vec4 color;
SSAO_NORMAL_FRAG
INSPECT_FRAG
// highlight material only: none (Generic2 masks r and g)
layout(location = 2) out vec4 emissive_out;
in vec2 tex_coord;

uniform float alpha_reject;
uniform float color_mult;
uniform vec4 fog_color;
in float fog;
in vec4 fragment_color;

flat in uvec2 tex_info;

uniform int gfx_hack_no_tex;
uniform uint warp_sample_mode;

uniform sampler2D tex_T0;

PBR_COMMON_FRAG
uniform int pbr_sun_ok;
uniform vec3 pbr_sun_dir;  // camera space, toward the light
uniform vec3 pbr_sun_col;
uniform int pbr_key_ok;
uniform vec3 pbr_key_dir;
uniform vec3 pbr_key_col;
uniform float pbr_key_scale;
in vec3 pbr_gpos;
CANOPY_FRAG
uniform int canopy_emitter;  // additive, subtractive or emissive-table draws keep their light
float canopy_key = 1.0;      // key light share kept under the canopy

/// Generic shading: face normal and UV detail (map or procedural) in camera space; no sidecar,
/// so the sun weight comes from the baked colour and the key light takes over where it is dark.
vec3 pbr_generic(vec3 rgb, vec3 albedo, vec3 baked) {
  int c = pbr_mat & 31;
  vec3 V = normalize(-pbr_gpos);
  vec3 Ng = cross(dFdx(pbr_gpos), dFdy(pbr_gpos));
  Ng = length(Ng) > 0.0 ? normalize(Ng) : V;
  if (dot(Ng, V) < 0.0) {
    Ng = -Ng;
  }
  vec4 d = pbr_uv_detail(Ng, pbr_gpos, tex_coord, tex_coord * pbr_scale[c], c);
  vec3 Nd = d.xyz;
  float lum = pbr_lum(baked);
  float sv = pbr_sun_ok != 0 && pbr_shaded(c) ? 0.6 * smoothstep(0.35, 0.9, lum) : 0.0;
  float kv = pbr_key_ok != 0 && pbr_shaded(c) && sv <= 0.0
                 ? pbr_key_scale * canopy_key * 0.5 * smoothstep(0.1, 0.6, lum)
                 : 0.0;
  // zero before the first background setup and on levels without a sidecar
  vec3 L = pbr_sun_ok != 0 ? normalize(pbr_sun_dir) : pbr_up;
  vec3 K = pbr_key_ok != 0 ? normalize(pbr_key_dir) : pbr_up;
  vec3 hl;
  vec3 lit = pbr_respond(rgb, albedo, baked, Ng, Nd, V, d.w, pbr_draw.x, c, sv, L, pbr_sun_col,
                         kv, K, pbr_key_col, pbr_shaded(c) ? 1.0 : 0.0, hl);
  return pbr_debug_view(c, rgb, Nd, hl, vec3(sv, kv, 0.0), d.w, lit);
}

// inland water (InlandWater.h): sunken pools keep a ripple query, so they draw here; depth only
uniform int iw_kind;
uniform sampler2D iw_depth;  // SSAO depth copy: camera z (m) of the bed
uniform int iw_depth_ok;
uniform int iw_view;  // 1: depth factor as opaque grey (debug)
uniform vec3 iw_lin;
uniform vec4 iw_proj;
uniform vec4 iw_water;  // absorption length (m)

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

vec4 sample_tex(vec2 coord, uint unit) {
  return texture(tex_T0, coord);
}

void main() {
  ssao_normal = ssao_normal_value();
  // 0x1 is tcc
  // 0x2 is decal
  // 0x4 is fog

  vec4 T0 = vec4(1.0);
  if (warp_sample_mode == 1 || gfx_hack_no_tex == 0) {
    T0 = sample_tex(tex_coord.xy, tex_info.x);
    if ((tex_info.y & 1u) == 0) {
      if ((tex_info.y & 2u) == 0) {
        // modulate + no tcc
        color.rgb = fragment_color.rgb * T0.rgb;
        color.a = fragment_color.a;
      } else {
        // decal + no tcc
        color.rgb = T0.rgb * 0.5;
        color.a = fragment_color.a;
      }
    } else {
      if ((tex_info.y & 2u) == 0) {
        // modulate + tcc
        color = fragment_color * T0;
      } else {
        // decal + tcc
        color.rgb = T0.rgb * 0.5;
        color.a = T0.a;
      }
    }
    color *= 2;
  } else {
    if ((tex_info.y & 1u) == 0) {
      if ((tex_info.y & 2u) == 0) {
        // modulate + no tcc
        color.rgb = fragment_color.rgb;
        color.a = fragment_color.a * 2;
      } else {
        // decal + no tcc
        color.rgb = vec3(1);
        color.a = fragment_color.a * 2;
      }
    } else {
      if ((tex_info.y & 2u) == 0) {
        // modulate + tcc
        color = fragment_color;
      } else {
        // decal + tcc
        color.rgb = vec3(0.5);
        color.a = 1.0;
      }
    }
  }
  color.rgb *= color_mult;
  float canopy_occ = 0.0;
  if (canopy_emitter == 0 && dot(pbr_gpos, pbr_gpos) > 0.0) {  // 0 under the full matrix
    vec3 canopy = shadow_canopy_factor(pbr_gpos, canopy_occ);
    color.rgb *= canopy;
    canopy_key = dot(canopy, vec3(0.299, 0.587, 0.114));
  }

  if (color.a < alpha_reject) {
    discard;
  }
  if (pbr_on != 0 && gfx_hack_no_tex == 0 && (tex_info.y & 2u) == 0) {
    color.rgb = pbr_generic(color.rgb, T0.rgb, fragment_color.rgb * 2.0);
  }
  if (iw_kind != 0 && pbr_on != 0) {
    float col = iw_column();
    color = iw_view == 1 ? vec4(vec3(col), 1.0) : vec4(color.rgb, color.a * col);
  }
  if ((tex_info.y & 4u) != 0) {
    color.xyz = mix(color.xyz, fog_color.rgb, clamp(fog_color.a * fog, 0, 1));
  }
  ssao_normal = ssao_normal_fog(ssao_normal, color.a,
                                (tex_info.y & 4u) != 0 ? 1. - clamp(fog_color.a * fog, 0, 1) : 1.);
  inspect_write(tex_coord);
  emissive_out = vec4(0.0, 0.0, 0.0, 1.0);
}
