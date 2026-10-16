#version 410 core

// SSAO debug views, drawn over the whole frame
out vec4 color;

uniform sampler2D ao;
uniform sampler2D csz;
// 1 raw AO, 2 blurred AO, 3 normals AO uses, 4 depth after 56, 5 flat colour,
// 6 normals from depth, 7 normal source (green target, red depth), 8 emissive,
// 9 fog and mist transmission
uniform int mode;
SWAMP_MIST_GLSL
SSAO_FOG_T
uniform int mist_on;  // view 9 includes the ground mist drawn after the AO composite
uniform float view_far_m;
uniform vec4 flat_color;
uniform int em_samples;
uniform sampler2D em_ss;
uniform sampler2DMS em_ms;

// normal target (SsaoRenderer.h): clip-space normals per sample, (0, 0, 0) where none
uniform sampler2D nrm_ss;
uniform sampler2DMS nrm_ms;
uniform int nrm_samples;  // 0: no target, 1: single-sample, else the sample count
uniform vec2 nrm_scale;   // clip-space normal x, y to view space

vec3 decodeNormal(vec4 e) {
  return dot(e.rgb, e.rgb) < 0.01 ? vec3(0.0) : normalize(e.rgb * 2.0 - 1.0);
}

// view-space normal from the target, averaged over the pixel's samples; zero if none
vec3 targetNormal(ivec2 p) {
  vec3 sum = vec3(0.0);
  if (nrm_samples == 1) {
    sum = decodeNormal(texelFetch(nrm_ss, p, 0));
  } else {
    for (int i = 0; i < nrm_samples; i++) {
      sum += decodeNormal(texelFetch(nrm_ms, p, i));
    }
  }
  return vec3(sum.xy * nrm_scale, sum.z);
}

vec3 pos(ivec2 p) {
  p = clamp(p, ivec2(0), textureSize(csz, 0) - ivec2(1));
  float z = texelFetch(csz, p, 0).r;
  return vec3(((vec2(p) + vec2(0.5)) * proj_info.xy + proj_info.zw) * z, z);
}

void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  if (mode == 5) {
    color = flat_color;
    return;
  }
  if (mode == 8) {
    float e = 0.0;
    if (em_samples <= 1) {
      e = texelFetch(em_ss, p, 0).r;
    } else {
      for (int i = 0; i < em_samples; i++) {
        e += texelFetch(em_ms, p, i).r;
      }
      e /= float(em_samples);
    }
    color = vec4(vec3(e), 0.25 * (1.0 - e));
    return;
  }
  if (mode == 9) {
    // grey: combined transmission over the pixel (white clear, black all haze); red tint: no normal
    vec4 e = nrm_samples == 1 ? texelFetch(nrm_ss, p, 0) : texelFetch(nrm_ms, p, 0);
    float t = nrm_samples == 0 ? 1.0 : ssao_fog_t(e);
    if (mist_on == 1) {
      vec3 v;
      t *= 1.0 - swamp_mist_alpha(p, texelFetch(csz, p, 0).r, v);
    }
    bool none = nrm_samples == 0 || dot(e.rgb, e.rgb) < 0.01;
    color = vec4(none ? vec3(0.6, 0.15, 0.15) + 0.4 * t : vec3(t), 1.0);
    return;
  }
  if (mode == 1 || mode == 2) {
    float a = texelFetch(ao, min(p >> 1, textureSize(ao, 0) - ivec2(1)), 0).r;
    color = vec4(vec3(a), 1.0);
    return;
  }
  vec3 C = pos(p);
  if (C.z >= far_m * 0.999) {
    color = vec4(0.15, 0.1, 0.3, 1.0);
    return;
  }
  vec3 t = mode == 3 || mode == 7 ? targetNormal(p) : vec3(0.0);
  if (mode == 7) {
    color = dot(t, t) > 0.0 ? vec4(0.1, 0.8, 0.2, 1.0) : vec4(0.9, 0.1, 0.1, 1.0);
    return;
  }
  if (mode == 3 && dot(t, t) > 0.0) {
    t = normalize(t);
    color = vec4((dot(t, C) > 0.0 ? -t : t) * 0.5 + 0.5, 1.0);
    return;
  }
  if (mode == 3 || mode == 6) {
    vec3 L = pos(p - ivec2(1, 0)), R = pos(p + ivec2(1, 0));
    vec3 D = pos(p - ivec2(0, 1)), U = pos(p + ivec2(0, 1));
    vec3 dx = abs(R.z - C.z) < abs(C.z - L.z) ? R - C : C - L;
    vec3 dy = abs(U.z - C.z) < abs(C.z - D.z) ? U - C : C - D;
    vec3 n = cross(dy, dx);
    n = dot(n, C) > 0.0 ? -n : n;
    color = vec4(normalize(n) * 0.5 + 0.5, 1.0);
    return;
  }
  float g = sqrt(clamp(C.z / view_far_m, 0.0, 1.0));
  color = vec4(vec3(1.0 - g), 1.0);
}
