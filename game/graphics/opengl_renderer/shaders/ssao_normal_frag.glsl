// SSAO normal target, draw buffer 1 (bound only while SSAO is on; see SsaoRenderer.h).
// Unit normal in clip (x, y, w) space, encoded n * 0.5 + 0.5; all zero means "no normal".
layout(location = 1) out vec4 ssao_normal;
uniform int ssao_nrm;  // 0 off, 1 face normal, 2 vertex normal (face normal where undefined)
uniform int ssao_fogw;  // 1: the normal's length carries the fog transmission (ssao_normal_fog)
SSAO_FOG_T
in vec3 ssao_cpos;
in vec4 ssao_vnrm;
// detail normal target, draw buffer 6 (tile lights only): the PBR mapped normal as its tangential
// part around the normal in ssao_dn_frame, stored 128 + 127 t of 255; 128 = unperturbed
layout(location = 6) out vec4 ssao_detail;
uniform int ssao_dn;       // 1: the target is bound and this draw's PBR shading writes it
uniform mat3 ssao_dn_rot;  // the shading frame to the tile pass's view space

/// Tangent frame around unit view-space n, continuous except at +z (facing away from the camera);
/// the same in emissive_light.frag.
void ssao_dn_frame(vec3 n, out vec3 b1, out vec3 b2) {
  float a = 1.0 / max(1.0 - n.z, 1e-4);
  float b = -n.x * n.y * a;
  b1 = vec3(1.0 - n.x * n.x * a, b, n.x);
  b2 = vec3(b, 1.0 - n.y * n.y * a, n.y);
}

/// Mapped normal Nd around the shading normal Ng (both unit, shading frame; V toward the eye).
void ssao_detail_set(vec3 Ng, vec3 Nd, vec3 V) {
  if (ssao_dn == 0) {
    return;
  }
  vec3 g = normalize(ssao_dn_rot * Ng);
  vec3 d = normalize(ssao_dn_rot * Nd);
  if (dot(g, ssao_dn_rot * V) < 0.0) {
    g = -g;
    d = -d;
  }
  vec3 b1, b2;
  ssao_dn_frame(g, b1, b2);
  vec2 t = clamp(vec2(dot(d, b1), dot(d, b2)), -1.0, 1.0);
  ssao_detail.rg = (128.0 + 127.0 * t) / 255.0;
}

vec4 ssao_normal_value() {
  ssao_detail = vec4(128.0 / 255.0, 128.0 / 255.0, 0.0, 1.0);
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
  ssao_detail.a = a;
  if (ssao_fogw != 0 && n.a > 0.0 && t < 1.0) {
    n.rgb = (n.rgb - 0.5) * (kSsaoFogL0 + (1.0 - kSsaoFogL0) * max(t, 0.0)) + 0.5;
  }
  return vec4(n.rgb, a);
}
