#version 410 core
/**
  \file data-files/shader/AmbientOcclusion/AmbientOcclusion_AO.pix

  Reference implementation of the Scalable Ambient Obscurance (AmbientOcclusion) screen-space ambient obscurance algorithm. 
 
  The optimized algorithmic structure of AmbientOcclusion was published in McGuire, Mara, and Luebke, Scalable Ambient Obscurance,
  <i>HPG</i> 2012, and was developed at NVIDIA with support from Louis Bavoil.

  The mathematical ideas of AlchemyAO were first described in McGuire, Osman, Bukowski, and Hennessy, The 
  Alchemy Screen-Space Ambient Obscurance Algorithm, <i>HPG</i> 2011 and were developed at 
  Vicarious Visions.  
 
  DX11 HLSL port by Leonardo Zide of Treyarch

  <hr>

  Open Source under the "BSD" license: http://www.opensource.org/licenses/bsd-license.php

  Copyright (c) 2011-2012, NVIDIA
  All rights reserved.

  Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

  Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
  Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.
  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

  G3D Innovation Engine http://casual-effects.com/g3d
  Copyright 2000-2019, Morgan McGuire
  All rights reserved
  Available under the BSD License
*/
// Ported for the Jak 1 renderer: camera-space z is positive and in metres, normals come from the
// normal target (best-pair depth normals where it has none), and the pass runs at half resolution.

// ao-raw: scalable ambient obscurance
out float visibility;

#define LOG_MAX_OFFSET (3)
#define MAX_MIP_LEVEL (4)

// camera-space z in metres, mips 0..4
uniform sampler2D csz;
// full-res pixel -> camera-space xy per metre of z
uniform vec4 proj_info;
// pixels spanned by 1 m at 1 m, full resolution
uniform float proj_scale;
uniform float radius;
uniform float bias;
// sine of the sample elevation where the crease fade starts and ends; y <= 0 disables it
uniform vec2 crease_sin;
uniform float crease_floor;
uniform float far_m;
uniform int num_samples;
uniform float num_spiral_turns;
// temporal hook: per-frame spin of the spiral, 0 while temporal accumulation is off
uniform float frame_spin;

const float intensity = 1.0;

// normal target (SsaoRenderer.h): clip-space normals per sample, (0, 0, 0) where none
uniform sampler2D nrm_ss;
uniform sampler2DMS nrm_ms;
uniform int nrm_samples;  // 0: no target, 1: single-sample, else the sample count
uniform vec2 nrm_scale;   // clip-space normal x, y to view space

vec3 decodeNormal(vec4 e) {
  return dot(e.rgb, e.rgb) < 0.01 ? vec3(0.0) : normalize(e.rgb * 2.0 - 1.0);
}

// view-space normal of one sample of the target (the first); zero if none
vec3 targetNormal1(ivec2 p) {
  p = clamp(p, ivec2(0), textureSize(csz, 0) - ivec2(1));
  vec3 n = nrm_samples == 1 ? decodeNormal(texelFetch(nrm_ss, p, 0))
                            : decodeNormal(texelFetch(nrm_ms, p, 0));
  return vec3(n.xy * nrm_scale, n.z);
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

vec3 reconstructCSPosition(vec2 S, float z) {
  return vec3((S * proj_info.xy + proj_info.zw) * z, z);
}

vec2 tapLocation(int sampleNumber, float spinAngle, out float ssR) {
  // Radius relative to ssR
  float alpha = (float(sampleNumber) + mod(spinAngle, 1.0) + 0.5) * (1.0 / (float(num_samples) - 0.5));
  alpha *= alpha;
  float angle = alpha * (num_spiral_turns * 6.28) + spinAngle;
  ssR = alpha;
  return vec2(cos(angle), sin(angle));
}

vec3 getPosition(ivec2 ssP) {
  ssP = clamp(ssP, ivec2(0), textureSize(csz, 0) - ivec2(1));
  return reconstructCSPosition(vec2(ssP) + vec2(0.5), texelFetch(csz, ssP, 0).r);
}

int getMipLevel(float ssR) {
  return clamp(int(floor(log2(ssR))) - LOG_MAX_OFFSET, 0, MAX_MIP_LEVEL);
}

vec3 getOffsetPosition(ivec2 ssC, vec2 unitOffset, float ssR) {
  int mipLevel = getMipLevel(ssR);
  ivec2 ssP = ivec2(ssR * unitOffset) + ssC;
  // Manually clamp to the texture size because texelFetch bypasses the texture unit
  ivec2 mipP = clamp(ssP >> mipLevel, ivec2(0), textureSize(csz, mipLevel) - ivec2(1));
  float z = texelFetch(csz, mipP, mipLevel).r;
  return reconstructCSPosition(vec2(ssP) + vec2(0.5), z);
}

float fallOffFunction(float vv, float vn, float epsilon) {
  float f = clamp(vv * (-1.0 / (radius * radius)) + 1.0, 0.0, 1.0);
  return f * clamp(vn - bias, 0.0, 1.0) / (epsilon + vv);
}

// n_C: surface normal (smooth where the target has one), n_F: face normal from depth. A sample
// counts only as far as it rises above both planes, so a facet never occludes itself.
float aoValueFromPositionsAndNormal(vec3 C, vec3 n_C, vec3 n_F, vec3 Q, vec3 n_Q) {
  vec3 v = Q - C;
  float vv = dot(v, v);
  float vn = min(dot(v, n_C), dot(v, n_F));
  const float epsilon = 0.015;
  float f = fallOffFunction(vv, vn, epsilon);
  if (dot(n_Q, n_Q) > 0.0) {
    // a shallow sample whose smooth normal matches ours is the same curved surface, not an occluder
    float same = smoothstep(0.96, 0.995, abs(dot(n_C, normalize(n_Q))));
    float shallow = 1.0 - smoothstep(0.17, 0.31, vn * inversesqrt(vv + 1e-8));
    f *= 1.0 - same * shallow;
  }
  if (crease_sin.y > 0.0) {
    // shallow creases (faceted low-poly ground) are not treated as occlusion
    f *= mix(crease_floor, 1.0, smoothstep(crease_sin.x, crease_sin.y, vn * inversesqrt(vv + 1e-8)));
  }
  // Enhance dark areas
  return f * mix(0.9 + 0.5 * intensity, 1.2 - intensity * 0.15, f);
}

float sampleAO(ivec2 ssC, vec3 C, vec3 n_C, vec3 n_F, bool smooth_n, float ssDiskRadius,
               int tapIndex, float spin) {
  float ssR;
  vec2 unitOffset = tapLocation(tapIndex, spin, ssR);
  // Ensure that the taps are at least 1 pixel away
  ssR = max(0.75, ssR * ssDiskRadius);
  vec3 Q = getOffsetPosition(ssC, unitOffset, ssR);
  vec3 n_Q = smooth_n ? targetNormal1(ivec2(ssR * unitOffset) + ssC) : vec3(0.0);
  return aoValueFromPositionsAndNormal(C, n_C, n_F, Q, n_Q);
}

const float MIN_RADIUS = 1.0;  // pixels

// By Morgan McGuire from https://www.shadertoy.com/view/lt3XDM
float lowPrecisionHash(vec2 p) {
  vec3 p3 = fract(vec3(p.xyx) * 0.13);
  p3 += dot(p3, p3.yzx + 3.333);
  return fract((p3.x + p3.y) * p3.z);
}

// of the two neighbour pairs, take the side with the smaller depth step (no silhouette halo)
vec3 bestPairNormal(ivec2 p, vec3 C) {
  vec3 L = getPosition(p - ivec2(1, 0));
  vec3 R = getPosition(p + ivec2(1, 0));
  vec3 D = getPosition(p - ivec2(0, 1));
  vec3 U = getPosition(p + ivec2(0, 1));
  vec3 dx = abs(R.z - C.z) < abs(C.z - L.z) ? R - C : C - L;
  vec3 dy = abs(U.z - C.z) < abs(C.z - D.z) ? U - C : C - D;
  vec3 n = cross(dy, dx);
  return dot(n, C) > 0.0 ? -n : n;
}

void main() {
  ivec2 ssH = ivec2(gl_FragCoord.xy);
  // the full-res pixel that mip 1 kept for this texel
  ivec2 ssC = ssH * 2 + ivec2(ssH.y & 1, ssH.x & 1);
  vec3 C = getPosition(ssC);
  if (C.z >= far_m * 0.999) {
    visibility = 1.0;
    return;
  }
  vec3 n_F = bestPairNormal(ssC, C);
  float nn = dot(n_F, n_F);
  if (nn <= 0.0) {
    visibility = 1.0;
    return;
  }
  n_F *= inversesqrt(nn);
  vec3 n_C = targetNormal(ssC);
  bool smooth_n = dot(n_C, n_C) > 0.0;
  if (!smooth_n) {
    n_C = n_F;
  } else {
    n_C = normalize(dot(n_C, C) > 0.0 ? -n_C : n_C);
  }

  float randomPatternRotationAngle = lowPrecisionHash(vec2(ssC)) * 20.1 + frame_spin;

  float ssDiskRadius = proj_scale * radius / C.z;
  if (ssDiskRadius <= MIN_RADIUS) {
    visibility = 1.0;
    return;
  }

  float sum = 0.0;
  for (int i = 0; i < num_samples; ++i) {
    sum += sampleAO(ssC, C, n_C, n_F, smooth_n, ssDiskRadius, i, randomPatternRotationAngle);
  }
  visibility = pow(clamp(1.0 - sqrt(sum * (1.0 / float(num_samples))), 0.0, 1.0), intensity);
}
