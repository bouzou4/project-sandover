#version 410 core
/*
  GLSL port of XeGTAO_CalculateEdges and XeGTAO_Denoise from XeGTAO.hlsli
  (https://github.com/GameTechDev/XeGTAO).

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
// Ported for the Jak 1 renderer: one pixel per fragment, edges from the camera-space z mip of the
// pass's resolution (unquantised) instead of a packed edge target written by the main pass.

// ao-denoise: one XeGTAO edge-aware 3x3 pass
out float result;

uniform sampler2D source;
uniform sampler2D csz;
uniform int z_mip;  // the csz level that matches this pass's resolution
uniform float blur_beta;  // centre weight: DenoiseBlurBeta, divided by 5 on non-final passes
uniform float far_m;

ivec2 g_hi;

float depthAt(ivec2 p) {
  return texelFetch(csz, clamp(p, ivec2(0), g_hi), z_mip).r;
}

float aoAt(ivec2 p) {
  return texelFetch(source, clamp(p, ivec2(0), g_hi), 0).r;
}

// excluded texels (negative, ssao_ao.frag) give no weight
float w(float ao, float weight) {
  return ao < 0.0 ? 0.0 : weight;
}

// left, right, top (+y), bottom (-y)
vec4 edgesAt(ivec2 p) {
  float centerZ = depthAt(p);
  vec4 edgesLRTB = vec4(depthAt(p - ivec2(1, 0)), depthAt(p + ivec2(1, 0)),
                        depthAt(p + ivec2(0, 1)), depthAt(p - ivec2(0, 1))) - centerZ;
  float slopeLR = (edgesLRTB.y - edgesLRTB.x) * 0.5;
  float slopeTB = (edgesLRTB.w - edgesLRTB.z) * 0.5;
  vec4 edgesLRTBSlopeAdjusted = edgesLRTB + vec4(slopeLR, -slopeLR, slopeTB, -slopeTB);
  edgesLRTB = min(abs(edgesLRTB), abs(edgesLRTBSlopeAdjusted));
  return clamp(1.25 - edgesLRTB / (centerZ * 0.011), 0.0, 1.0);
}

void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  g_hi = textureSize(source, 0) - ivec2(1);
  float ssaoValue = aoAt(p);
  if (ssaoValue < 0.0 || depthAt(p) >= far_m * 0.999) {
    result = ssaoValue;
    return;
  }
  const float diagWeight = 0.85 * 0.5;
  ivec2 dx = ivec2(1, 0), dy = ivec2(0, 1);

  vec4 edgesL_LRTB = edgesAt(p - dx);
  vec4 edgesR_LRTB = edgesAt(p + dx);
  vec4 edgesT_LRTB = edgesAt(p + dy);
  vec4 edgesB_LRTB = edgesAt(p - dy);
  vec4 edgesC_LRTB = edgesAt(p);
  // symmetric: a left edge here must also be the right edge of the left neighbour
  edgesC_LRTB *= vec4(edgesL_LRTB.y, edgesR_LRTB.x, edgesT_LRTB.w, edgesB_LRTB.z);

  // a little AO leaks in from neighbours where there are 3 or 4 edges
  const float leak_threshold = 2.5;
  const float leak_strength = 0.5;
  float edginess = (clamp(4.0 - leak_threshold - dot(edgesC_LRTB, vec4(1.0)), 0.0, 1.0) /
                    (4.0 - leak_threshold)) * leak_strength;
  edgesC_LRTB = clamp(edgesC_LRTB + edginess, 0.0, 1.0);

  float weightTL = diagWeight * (edgesC_LRTB.x * edgesL_LRTB.z + edgesC_LRTB.z * edgesT_LRTB.x);
  float weightTR = diagWeight * (edgesC_LRTB.z * edgesT_LRTB.y + edgesC_LRTB.y * edgesR_LRTB.z);
  float weightBL = diagWeight * (edgesC_LRTB.w * edgesB_LRTB.x + edgesC_LRTB.x * edgesL_LRTB.w);
  float weightBR = diagWeight * (edgesC_LRTB.y * edgesR_LRTB.w + edgesC_LRTB.w * edgesB_LRTB.y);

  float sumWeight = blur_beta;
  float sum = ssaoValue * sumWeight;
  float a[8] = float[8](aoAt(p - dx), aoAt(p + dx), aoAt(p + dy), aoAt(p - dy), aoAt(p - dx + dy),
                        aoAt(p + dx + dy), aoAt(p - dx - dy), aoAt(p + dx - dy));
  float k[8] = float[8](edgesC_LRTB.x, edgesC_LRTB.y, edgesC_LRTB.z, edgesC_LRTB.w, weightTL,
                        weightTR, weightBL, weightBR);
  for (int i = 0; i < 8; i++) {
    float wi = w(a[i], k[i]);
    sum += a[i] * wi;
    sumWeight += wi;
  }
  result = sum / sumWeight;
}
