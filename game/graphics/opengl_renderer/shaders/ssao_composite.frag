#version 410 core

// composite: joint-bilateral upsample of the half-res AO and a multiply over stencil-selected pixels
out vec4 color;

uniform sampler2D ao;
uniform sampler2D csz;         // level 0 full res, level 1 matches ao
uniform sampler2D color_copy;  // resolved colour, read only when use_floor is set
uniform float strength;       // darkening at full occlusion
uniform float response;       // darkening = strength x occlusion^response
uniform int use_floor;
uniform int ao_full;           // 1: ao is full resolution (no upsample)
uniform float luma_floor;      // 0..1
uniform int em_on;             // AO fades out over emissive pixels
uniform int em_samples;
uniform sampler2D em_ss;
uniform sampler2DMS em_ms;
uniform vec2 em_ramp;
uniform int fog_w;    // 1: AO fades with the fog and haze over the pixel
uniform float fog_k;  // exponent on that transmission: 1 as laid over, 2 a stronger fade
uniform int mist_on;  // a ground mist (swamp, Misty sea) draws after this pass: fade AO under it
uniform sampler2D nrm_ss;
uniform sampler2DMS nrm_ms;
uniform int nrm_samples;  // 0: no normal target

EMISSIVE_MASK_FRAG
SSAO_FOG_T
SWAMP_MIST_GLSL

// per-surface fog and haze transmission, mean over the pixel's samples
float transmission(ivec2 p) {
  if (nrm_samples == 1) {
    return ssao_fog_t(texelFetch(nrm_ss, p, 0));
  }
  float t = 0.0;
  for (int i = 0; i < 16; i++) {
    if (i >= nrm_samples) {
      break;
    }
    t += ssao_fog_t(texelFetch(nrm_ms, p, i));
  }
  return t / float(max(nrm_samples, 1));
}

void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  float z = texelFetch(csz, p, 0).r;
  ivec2 hi = textureSize(ao, 0) - ivec2(1);
  ivec2 h0 = max((p - ivec2(1)) >> 1, ivec2(0));
  float best_dz = 1e30;
  float best_a = -1.0;
  float sum = 0.0, wsum = 0.0;
  for (int i = 0; i < 4; i++) {
    ivec2 h = min(h0 + ivec2(i & 1, i >> 1), hi);
    float ah = texelFetch(ao, h, 0).r;
    // excluded texels (negative) hold no AO
    if (ah < 0.0) {
      continue;
    }
    float dz = abs(texelFetch(csz, h, 1).r - z);
    if (dz < best_dz) {
      best_dz = dz;
      best_a = ah;
    }
    // joint bilateral: distance to the full-res pixel mip 1 kept for h, and depth agreement
    vec2 d = abs(vec2(h * 2 + ivec2(h.y & 1, h.x & 1) - p)) * 0.5;
    float ws = max(1.0 - d.x, 0.05) * max(1.0 - d.y, 0.05);
    float r = dz / max(0.02 * z, 1e-4);
    float w = ws * exp(-r * r);
    sum += ah * w;
    wsum += w;
  }
  // no texel on this surface (thin geometry): the nearest in depth
  float a = wsum > 0.02 ? sum / wsum : best_a;
  if (ao_full == 1) {
    a = texelFetch(ao, min(p, hi), 0).r;
  }
  float f = 1.0 - strength * pow(clamp(1.0 - a, 0.0, 1.0), response);
  f = a < 0.0 ? 1.0 : f;
  float t = fog_w == 1 && nrm_samples > 0 ? transmission(p) : 1.0;
  if (fog_w == 1 && mist_on == 1) {
    vec3 v;
    t *= 1.0 - swamp_mist_alpha(p, z, v);
  }
  if (t < 1.0) {
    f = mix(1.0, f, pow(t, fog_k));
  }
  if (use_floor == 1) {
    float l = dot(texelFetch(color_copy, p, 0).rgb, vec3(0.299, 0.587, 0.114));
    f = mix(1.0, f, smoothstep(luma_floor, 2.0 * luma_floor, l));
  }
  if (em_on == 1) {
    f = mix(f, 1.0, smoothstep(em_ramp.x, em_ramp.y, emissive(p)));
  }
  color = vec4(f, f, f, 1.0);
}
