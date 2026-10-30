#version 410 core
/*
  GLSL port of XeGTAO_MainPass from XeGTAO.hlsli (https://github.com/GameTechDev/XeGTAO).
  XeGTAO is based on GTAO/GTSO "Jimenez et al. / Practical Real-Time Strategies for Accurate
  Indirect Occlusion". Implementation: Filip Strugar, Steve Mccalla.

  MIT License

  Copyright (C) 2016-2021, Intel Corporation

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.
*/
// Ported for the Jak 1 renderer: y-up pixels, metres, half or full res, normal target normals.
// Horizons start at the depth facet's plane and visibility is relative to that facet's own.

// ao-raw: XeGTAO visibility, linear (the response curve is the composite's), 1 = unoccluded
out float visibility;

#define PI 3.1415926535897932
#define PI_HALF 1.5707963267948966
#define MAX_MIP_LEVEL 4

uniform sampler2D csz;   // camera-space z in metres, mips 0..4
uniform vec4 proj_info;  // full-res pixel -> camera-space xy per metre of z
uniform float far_m;
uniform float radius;           // effect radius (m), before radius_mul
uniform float radius_mul;       // XE_GTAO_DEFAULT_RADIUS_MULTIPLIER
uniform float falloff_range;    // share of the radius over which samples fade out
uniform float sample_power;     // sample distribution power
uniform float thin_comp;        // thin occluder compensation
uniform float mip_offset;       // depth MIP sampling offset
uniform int slices;
uniform int steps;
uniform int noise_index;        // frame % 64 under a temporal AA, else 0 (a fixed pattern)
uniform int full_res;           // 1: one texel per full-res pixel
// exclude mask (SsaoRenderer::make_exclude_mask): 1 where stencil bit 7 keeps AO off the sample
uniform sampler2D ex_ss;
uniform sampler2DMS ex_ms;
uniform int ex_samples;  // 0: no mask

// any sample of the pixel excluded: the composite discards this texel's AO
bool excluded(ivec2 p) {
  if (ex_samples == 0) {
    return false;
  }
  if (ex_samples == 1) {
    return texelFetch(ex_ss, p, 0).r > 0.75;
  }
  for (int i = 0; i < ex_samples; i++) {
    if (texelFetch(ex_ms, p, i).r > 0.75) {
      return true;
    }
  }
  return false;
}

uniform sampler2D nrm_ss;
uniform sampler2DMS nrm_ms;
uniform int nrm_samples;  // 0: no target, 1: single-sample, else the sample count
uniform vec2 nrm_scale;   // clip-space normal x, y to view space

NORMAL_DECODE_FRAG
vec3 targetNormal1(ivec2 p) {
  p = clamp(p, ivec2(0), textureSize(csz, 0) - ivec2(1));
  vec3 n = nrm_samples == 1 ? decodeNormal(texelFetch(nrm_ss, p, 0))
                            : decodeNormal(texelFetch(nrm_ms, p, 0));
  return vec3(n.xy * nrm_scale, n.z);
}

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

vec3 getPosition(ivec2 ssP) {
  ssP = clamp(ssP, ivec2(0), textureSize(csz, 0) - ivec2(1));
  return reconstructCSPosition(vec2(ssP) + vec2(0.5), texelFetch(csz, ssP, 0).r);
}

vec3 getMipPosition(ivec2 ssP, int mip) {
  ivec2 mipP = clamp(ssP >> mip, ivec2(0), textureSize(csz, mip) - ivec2(1));
  return reconstructCSPosition(vec2(ssP) + vec2(0.5), texelFetch(csz, mipP, mip).r);
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

float fastACos(float inX) {
  float x = min(abs(inX), 1.0);  // sqrt of a negative is NaN
  float res = (-0.156583 * x + PI_HALF) * sqrt(1.0 - x);
  return inX >= 0.0 ? res : PI - res;
}

uint hilbertIndex(uint posX, uint posY) {
  uint index = 0u;
  for (uint curLevel = 32u; curLevel > 0u; curLevel >>= 1u) {
    uint regionX = (posX & curLevel) > 0u ? 1u : 0u;
    uint regionY = (posY & curLevel) > 0u ? 1u : 0u;
    index += curLevel * curLevel * ((3u * regionX) ^ regionY);
    if (regionY == 0u) {
      if (regionX == 1u) {
        posX = 63u - posX;
        posY = 63u - posY;
      }
      uint temp = posX;
      posX = posY;
      posY = temp;
    }
  }
  return index;
}

// SpatioTemporalNoise of the XeGTAO sample (Hilbert curve index, R2 sequence)
vec2 spatioTemporalNoise(ivec2 p, int temporalIndex) {
  uint index = hilbertIndex(uint(p.x) & 63u, uint(p.y) & 63u);
  index += 288u * uint(temporalIndex % 64);
  return fract(0.5 + float(index) * vec2(0.75487766624669276005, 0.5698402909980532659114));
}

// slice angle of `nrm` projected into the slice plane; `len` its projected length
float sliceAngle(vec3 nrm, vec3 axisVec, vec3 orthoDirectionVec, vec3 viewVec, out float len,
                 out float cosN) {
  vec3 projected = nrm - axisVec * dot(nrm, axisVec);
  float signNorm = sign(dot(orthoDirectionVec, projected));
  len = length(projected);
  cosN = clamp(dot(projected, viewVec) / max(len, 1e-6), 0.0, 1.0);
  return signNorm * fastACos(cosN);
}

float iarc(float h, float n, float cosNorm) {
  return (cosNorm + 2.0 * h * sin(n) - cos(2.0 * h - n)) * 0.25;
}

void main() {
  ivec2 ssH = ivec2(gl_FragCoord.xy);
  // the full-res pixel that mip 1 kept for this texel
  ivec2 ssC = full_res == 1 ? ssH : ssH * 2 + ivec2(ssH.y & 1, ssH.x & 1);
  if (excluded(ssC)) {
    visibility = -1.0;  // later passes skip it; readers treat it as no AO
    return;
  }
  float viewspaceZ = texelFetch(csz, clamp(ssC, ivec2(0), textureSize(csz, 0) - ivec2(1)), 0).r;
  if (viewspaceZ >= far_m * 0.999) {
    visibility = 1.0;
    return;
  }
  vec3 C0 = reconstructCSPosition(vec2(ssC) + vec2(0.5), viewspaceZ);
  vec3 n_F = bestPairNormal(ssC, C0);
  float nn = dot(n_F, n_F);
  if (nn <= 0.0) {
    visibility = 1.0;
    return;
  }
  n_F *= inversesqrt(nn);
  vec3 n_C = targetNormal(ssC);
  bool smooth_n = dot(n_C, n_C) > 0.0;
  n_C = smooth_n ? normalize(dot(n_C, C0) > 0.0 ? -n_C : n_C) : n_F;

  viewspaceZ *= 0.99999;
  vec3 pixCenterPos = reconstructCSPosition(vec2(ssC) + vec2(0.5), viewspaceZ);
  vec3 viewVec = normalize(-pixCenterPos);

  float effectRadius = radius * radius_mul;
  float falloffRange = falloff_range * effectRadius;
  float falloffFrom = effectRadius * (1.0 - falloff_range);
  float falloffMul = -1.0 / falloffRange;
  float falloffAdd = falloffFrom / falloffRange + 1.0;

  vec2 localNoise = spatioTemporalNoise(ssH, noise_index);
  float noiseSlice = localNoise.x;
  float noiseSample = localNoise.y;
  const float pixelTooCloseThreshold = 1.3;
  float screenspaceRadius = effectRadius / (viewspaceZ * proj_info.x);
  float fade = clamp((10.0 - screenspaceRadius) / 100.0, 0.0, 1.0) * 0.5;
  float minS = pixelTooCloseThreshold / screenspaceRadius;
  float sliceCount = float(slices);
  float stepsPerSlice = float(steps);

  float vis = 0.0;
  float visRef = 0.0;
  for (int slice = 0; slice < slices; slice++) {
    float sliceK = (float(slice) + noiseSlice) / sliceCount;
    float phi = sliceK * PI;
    float cosPhi = cos(phi);
    float sinPhi = sin(phi);
    // pixel y points up here, as view y does, so omega keeps sin's sign
    vec2 omega = vec2(cosPhi, sinPhi) * screenspaceRadius;
    vec3 directionVec = vec3(cosPhi, sinPhi, 0.0);
    vec3 orthoDirectionVec = directionVec - dot(directionVec, viewVec) * viewVec;
    vec3 axisVec = normalize(cross(orthoDirectionVec, viewVec));

    float projectedNormalVecLength, cosNorm;
    float n = sliceAngle(n_C, axisVec, orthoDirectionVec, viewVec, projectedNormalVecLength,
                         cosNorm);
    float lenF, cosF;
    float nF = sliceAngle(n_F, axisVec, orthoDirectionVec, viewVec, lenF, cosF);

    // horizons start at the lower of the smooth hemisphere and the facet's own plane
    float lowHorizonCos0 = max(cos(n + PI_HALF), cos(nF + PI_HALF));
    float lowHorizonCos1 = max(cos(n - PI_HALF), cos(nF - PI_HALF));
    float horizonCos0 = lowHorizonCos0;
    float horizonCos1 = lowHorizonCos1;

    for (int step = 0; step < steps; step++) {
      float stepBaseNoise = float(slice + step * steps) * 0.6180339887498948482;
      float stepNoise = fract(noiseSample + stepBaseNoise);
      float s = (float(step) + stepNoise) / stepsPerSlice;
      s = pow(s, sample_power);
      s += minS;
      vec2 sampleOffset = s * omega;
      float sampleOffsetLength = length(sampleOffset);
      float mipLevel = clamp(log2(sampleOffsetLength) - mip_offset, 0.0, float(MAX_MIP_LEVEL));
      int mip = int(floor(mipLevel + 0.5));
      ivec2 o = ivec2(round(sampleOffset));

      vec3 sampleDelta0 = getMipPosition(ssC + o, mip) - pixCenterPos;
      vec3 sampleDelta1 = getMipPosition(ssC - o, mip) - pixCenterPos;
      float sampleDist0 = length(sampleDelta0);
      float sampleDist1 = length(sampleDelta1);
      vec3 sampleHorizonVec0 = sampleDelta0 / max(sampleDist0, 1e-6);
      vec3 sampleHorizonVec1 = sampleDelta1 / max(sampleDist1, 1e-6);

      // thickness heuristic: samples behind the centre fall off sooner
      float falloffBase0 = length(vec3(sampleDelta0.xy, sampleDelta0.z * (1.0 + thin_comp)));
      float falloffBase1 = length(vec3(sampleDelta1.xy, sampleDelta1.z * (1.0 + thin_comp)));
      float weight0 = clamp(falloffBase0 * falloffMul + falloffAdd, 0.0, 1.0);
      float weight1 = clamp(falloffBase1 * falloffMul + falloffAdd, 0.0, 1.0);

      if (smooth_n) {
        // a shallow sample whose smooth normal matches ours is the same curved surface
        vec3 n0 = targetNormal1(ssC + o);
        vec3 n1 = targetNormal1(ssC - o);
        if (dot(n0, n0) > 0.0) {
          float e0 = min(dot(sampleHorizonVec0, n_C), dot(sampleHorizonVec0, n_F));
          weight0 *= 1.0 - smoothstep(0.96, 0.995, abs(dot(n_C, normalize(n0)))) *
                               (1.0 - smoothstep(0.17, 0.31, e0));
        }
        if (dot(n1, n1) > 0.0) {
          float e1 = min(dot(sampleHorizonVec1, n_C), dot(sampleHorizonVec1, n_F));
          weight1 *= 1.0 - smoothstep(0.96, 0.995, abs(dot(n_C, normalize(n1)))) *
                               (1.0 - smoothstep(0.17, 0.31, e1));
        }
      }

      float shc0 = mix(lowHorizonCos0, dot(sampleHorizonVec0, viewVec), weight0);
      float shc1 = mix(lowHorizonCos1, dot(sampleHorizonVec1, viewVec), weight1);
      horizonCos0 = max(horizonCos0, shc0);
      horizonCos1 = max(horizonCos1, shc1);
    }

    projectedNormalVecLength = mix(projectedNormalVecLength, 1.0, 0.05);
    float h0 = -fastACos(horizonCos1);
    float h1 = fastACos(horizonCos0);
    float r0 = -fastACos(lowHorizonCos1);
    float r1 = fastACos(lowHorizonCos0);
    vis += projectedNormalVecLength * (iarc(h0, n, cosNorm) + iarc(h1, n, cosNorm));
    visRef += projectedNormalVecLength * (iarc(r0, n, cosNorm) + iarc(r1, n, cosNorm));
  }
  float v = visRef > 1e-4 ? clamp(vis / visRef, 0.0, 1.0) : 1.0;
  visibility = clamp(v + fade / sliceCount, 0.0, 1.0);
}
