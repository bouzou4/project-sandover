#version 410 core
/**
  \file data-files/shader/AmbientOcclusion/AmbientOcclusion_blur.pix

  \brief 7-tap 1D cross-bilateral blur using a packed depth key

  DX11 HLSL port by Leonardo Zide, Treyarch
  
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
// Ported for the Jak 1 renderer: the depth key is read from the half-res camera-space z mip
// instead of a packed channel, and the radius is a uniform.

// ao-blur: 1D cross-bilateral blur at half resolution, run once per axis
out float result;

uniform sampler2D source;
uniform sampler2D csz;  // level 1 matches this pass's resolution
uniform ivec2 axis;
uniform int R;
uniform float edge_sharpness;
uniform float radius;
uniform float far_m;

void main() {
  ivec2 ssC = ivec2(gl_FragCoord.xy);
  ivec2 hi = textureSize(source, 0) - ivec2(1);
  float sum = texelFetch(source, ssC, 0).r;
  float key = texelFetch(csz, ssC, 1).r;
  if (key >= far_m * 0.999 || R <= 0) {
    // Sky pixel
    result = sum;
    return;
  }
  float sigma = max(1.0, 0.5 * float(R));
  float BASE = 1.0;
  float totalWeight = BASE;
  sum *= totalWeight;
  // the reference's key scale (2000 * 1.5 / 200 m far plane), made relative to the radius
  float scale = 15.0 * edge_sharpness / max(radius, 0.02 * key);

  for (int r = -R; r <= R; ++r) {
    if (r != 0) {
      ivec2 tapLoc = clamp(ssC + axis * r, ivec2(0), hi);
      float weight = 0.3 + exp(-float(r * r) / (2.0 * sigma * sigma));
      float tapKey = texelFetch(csz, tapLoc, 1).r;
      float value = texelFetch(source, tapLoc, 0).r;
      weight *= max(0.0, 1.0 - scale * abs(tapKey - key));
      sum += value * weight;
      totalWeight += weight;
    }
  }
  const float epsilon = 0.0001;
  result = sum / (totalWeight + epsilon);
}
