// SSAO normal target, draw buffer 1 (bound only while SSAO is on; see SsaoRenderer.h).
// Unit normal in clip (x, y, w) space, encoded n * 0.5 + 0.5; all zero means "no normal".
layout(location = 1) out vec4 ssao_normal;
uniform int ssao_nrm;  // 0 off, 1 face normal, 2 vertex normal (face normal where undefined)
uniform int ssao_fogw;  // 1: the normal's length carries the fog transmission (ssao_normal_fog)
SSAO_FOG_T
in vec3 ssao_cpos;
in vec4 ssao_vnrm;

vec4 ssao_normal_value() {
  if (ssao_nrm == 0) {
    return vec4(0.0);
  }
  vec3 f = cross(dFdx(ssao_cpos), dFdy(ssao_cpos));
  if (dot(f, ssao_cpos) > 0.0) {
    f = -f;
  }
  vec3 n = f;
  if (ssao_nrm == 2 && ssao_vnrm.w > 0.999) {
    // winding-oriented and smooth normals may point either way: take the face's side
    n = dot(ssao_vnrm.xyz, f) < 0.0 ? -ssao_vnrm.xyz : ssao_vnrm.xyz;
  }
  float l = length(n);
  return l > 0.0 ? vec4(n / l * 0.5 + 0.5, 1.0) : vec4(0.0);
}

/// Final normal-target value: alpha is the colour's (the blend weight). With ssao_fogw, a
/// fogged pixel's normal is shortened to length 0.1 + 0.9 t, where t is the share of the surface
/// colour the fog and haze left (1 = clear). Readers normalise; the AO composite reads t back
/// (ssao_fog_t).
vec4 ssao_normal_fog(vec4 n, float a, float t) {
  if (ssao_fogw != 0 && n.a > 0.0 && t < 1.0) {
    n.rgb = (n.rgb - 0.5) * (kSsaoFogL0 + (1.0 - kSsaoFogL0) * max(t, 0.0)) + 0.5;
  }
  return vec4(n.rgb, a);
}
