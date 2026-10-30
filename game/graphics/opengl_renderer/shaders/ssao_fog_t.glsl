// Fog transmission t carried by the SSAO normal target: the encoded normal's length, 0.1 + 0.9 t.
const float kSsaoFogL0 = 0.1;

/// t of one normal-target texel, 1 where there is no normal; lengths within 1% of 1 read as 1.
float ssao_fog_t(vec4 e) {
  return dot(e.rgb, e.rgb) < 0.01
             ? 1.0
             : clamp((length(e.rgb * 2.0 - 1.0) - kSsaoFogL0) / (0.99 - kSsaoFogL0), 0.0, 1.0);
}
