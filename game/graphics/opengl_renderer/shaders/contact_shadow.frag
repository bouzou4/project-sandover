#version 410 core

// contact shadows (ContactShadows.h), raw pass: a short dithered ray toward the shadow light
// through the SSAO depth copy; r = the share of the pixel's colour a hit removes (or a debug view)
out vec4 color;

uniform sampler2D csz;  // camera-space z in metres, full res at mip 0
uniform sampler2D nrm_ss;
uniform sampler2DMS nrm_ms;
uniform int nrm_samples;  // 0: no normal target
uniform int em_on;
uniform int em_samples;
uniform sampler2D em_ss;
uniform sampler2DMS em_ms;
uniform vec2 em_ramp;
uniform int fog_w;
uniform float fog_k;
uniform int mist_on;

uniform mat3 cs_k;      // camera offset (world axes, game units) to clip x, y, w
uniform mat3 cs_kinv;
uniform vec3 cs_cam;    // camera position, world game units
uniform vec3 cs_light;  // toward the light, world, unit
uniform vec4 cs_par;    // steps, length (m), thickness (m), strength x light fade
uniform float cs_cap;   // longest ray on screen, share of the frame height
uniform float cs_share; // fitted direct share of the light at N.L = 1
uniform float cs_frame; // dither rotation, 0..63
uniform int cs_view;    // ContactShadowView
uniform vec2 cs_size;   // render target pixels

SSAO_FOG_T
SWAMP_MIST_GLSL
WORLD_SHADOW_FRAG
ACTOR_SHADOW_FRAG

const float kM = 4096.0;

vec3 cam_pos(vec2 s, float z_m) {
  float w = z_m * kM;
  return cs_kinv * vec3((s / cs_size * 2.0 - 1.0) * w, w);
}

EMISSIVE_MASK_FRAG
// clip (x, y, w) normal of the pixel's samples, zero where none was written
vec3 normal_clip(ivec2 p, out float t) {
  t = 1.0;
  if (nrm_samples <= 0) {
    return vec3(0.0);
  }
  vec3 n = vec3(0.0);
  float tsum = 0.0;
  int cnt = max(nrm_samples, 1);
  for (int i = 0; i < 16; i++) {
    if (i >= cnt) {
      break;
    }
    vec4 e = nrm_samples == 1 ? texelFetch(nrm_ss, p, 0) : texelFetch(nrm_ms, p, i);
    tsum += ssao_fog_t(e);
    if (dot(e.rgb, e.rgb) >= 0.01) {
      n += normalize(e.rgb * 2.0 - 1.0);
    }
  }
  t = tsum / float(cnt);
  return n;
}

// interleaved gradient noise, rotated per frame
float dither(vec2 p) {
  p += 5.588238 * cs_frame;
  return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  float z = texelFetch(csz, p, 0).r;
  vec3 P = cam_pos(vec2(p) + 0.5, z);
  // derivatives in uniform control flow, before any return
  ws_begin(P + cs_cam);
  vec3 Nf = cross(dFdx(P), dFdy(P));
  color = vec4(0.0, 0.0, 0.0, 1.0);
  if (z >= 0.99 * far_m) {
    return;
  }
  float fog_t;
  vec3 nc = normal_clip(p, fog_t);
  vec3 N = dot(nc, nc) > 1e-4 ? transpose(cs_k) * nc : Nf;
  float nl = length(N);
  N = nl > 0.0 ? N / nl : -normalize(P);
  N = dot(N, P) > 0.0 ? -N : N;
  vec3 L = cs_light;
  float ndl = dot(N, L);

  // ray: longer far away so it keeps a few pixels, capped on screen
  float len = cs_par.y * clamp(z / 20.0, 1.0, 3.0) * kM;
  vec3 O = P + N * ((0.02 + 0.004 * z) * kM);
  vec3 c0 = cs_k * O;
  vec3 c1 = cs_k * (O + L * len);
  float px = 0.0;
  if (c0.z > 1.0 && c1.z > 1.0) {
    px = length((c1.xy / c1.z - c0.xy / c0.z) * 0.5 * cs_size);
    float cap = cs_cap * cs_size.y;
    if (px > cap) {
      len *= cap / px;
      px = cap;
    }
  }
  int steps = int(cs_par.x);
  float j = dither(vec2(p));
  float occ = 0.0;
  float thick = cs_par.z * (1.0 + 0.02 * z);
  if (ndl > 0.0) {
    for (int i = 0; i < 16; i++) {
      if (i >= steps) {
        break;
      }
      float t = (float(i) + j) / float(steps);
      vec3 c = cs_k * (O + L * (len * t));
      if (c.z <= 1.0) {
        break;
      }
      vec2 uv = c.xy / c.z * 0.5 + 0.5;
      if (any(lessThan(uv, vec2(0.0))) || any(greaterThanEqual(uv, vec2(1.0)))) {
        break;
      }
      float zs = texelFetch(csz, ivec2(uv * cs_size), 0).r;
      float zq = c.z / kM;
      float dz = zq - zs;
      if (dz > 0.01 + 0.002 * zq && dz < thick) {
        // soft at the far end of the ray and toward the thickness limit
        occ = (1.0 - t * t) * (1.0 - smoothstep(0.6 * thick, thick, dz));
        break;
      }
    }
  }
  occ *= smoothstep(0.02, 0.15, ndl) * (1.0 - smoothstep(60.0, 120.0, z));

  // direct share still at the pixel: the fitted share at this N.L, after the sun maps
  float r = 0.0;
  if (occ > 0.0 || cs_view == 4) {
    float occ_map = shadow_on == 1 ? shadow_sun(P + cs_cam, 0.0003) : 0.0;
    if ((ws_on & 1) != 0) {
      occ_map = max(occ_map, 1.0 - ws_sun_vis(P + cs_cam));
    }
    float d = cs_share * max(ndl, 0.0) * (1.0 - clamp(occ_map, 0.0, 1.0));
    r = d / max(d + 1.0 - cs_share, 1e-4);
  }
  float k = occ * r * cs_par.w;
  if (fog_w == 1) {
    float t = fog_t;
    if (mist_on == 1) {
      vec3 v;
      t *= 1.0 - swamp_mist_alpha(p, z, v);
    }
    k *= pow(clamp(t, 0.0, 1.0), fog_k);
  }
  if (em_on == 1) {
    k *= 1.0 - smoothstep(em_ramp.x, em_ramp.y, emissive(p));
  }
  float v = clamp(k, 0.0, 1.0);
  if (cs_view == 1) {
    v = occ;
  } else if (cs_view == 2) {
    v = px / max(cs_cap * cs_size.y, 1.0);
  } else if (cs_view == 4) {
    v = r;
  }
  color = vec4(v, 0.0, 0.0, 1.0);
}
