// SSAO normal target inputs for the fragment stage (see ssao_normal_frag.glsl).
uniform int ssao_nrm;
out vec3 ssao_cpos;  // gl_Position x, y, w
out vec4 ssao_vnrm;  // vertex normal in clip (x, y, w) space, w = 1 where defined

// Normal n given the clip (x, y, w) images a0..a2 of the three object axes (cofactor matrix).
vec3 ssao_clip_normal(vec3 a0, vec3 a1, vec3 a2, vec3 n) {
  return cross(a1, a2) * n.x + cross(a2, a0) * n.y + cross(a0, a1) * n.z;
}

vec3 ssao_oct_decode(uint packed_xy) {
  vec2 e = vec2(float(packed_xy & 0xffffu), float(packed_xy >> 16u)) / 32767.5 - 1.0;
  vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
  if (n.z < 0.0) {
    n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
  }
  return normalize(n);
}
