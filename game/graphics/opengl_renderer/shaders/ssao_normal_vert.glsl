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

// Clip (x, y, w) image of object direction d under the etie projection (etie.vert, etie_base.vert).
vec3 ssao_etie_axis(mat4 cam, vec4 p0, vec4 p1, float sy, vec3 d) {
  vec3 v = mat3(cam) * d;
  float w = p0.w * v.z;
  return vec3((p1.x * v.x + p0.x * v.z - 2048.0 * w) / 256.0,
              -(p1.y * v.y + p0.y * v.z - 2048.0 * w) / 128.0 * sy, w);
}

/// Smooth sidecar normal for an etie vertex (record rec), or zero where undefined.
vec4 ssao_etie_vnrm(mat4 cam, vec4 p0, vec4 p1, float sy, uvec4 rec) {
  if (ssao_nrm != 2 || (rec.w & 0x100u) == 0u) {
    return vec4(0.0);
  }
  return vec4(ssao_clip_normal(ssao_etie_axis(cam, p0, p1, sy, vec3(1, 0, 0)),
                               ssao_etie_axis(cam, p0, p1, sy, vec3(0, 1, 0)),
                               ssao_etie_axis(cam, p0, p1, sy, vec3(0, 0, 1)), ssao_oct_decode(rec.y)),
              1.0);
}
