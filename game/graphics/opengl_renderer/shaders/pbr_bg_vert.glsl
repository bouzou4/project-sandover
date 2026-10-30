// Stylized material shading inputs (see Materials.h); sidecar records as in background/Unbake.h.
uniform int pbr_on;
uniform int pbr_rec;     // 1: tex_T11 holds this tree's sidecar records
uniform float pbr_sun_l;  // blended day sun luminance, palette steps
uniform float pbr_amb_l;  // blended ambient luminance
uniform float pbr_w[8];   // day-slot weights for visibility, summing to 1
uniform float pbr_day;    // day share of the palette blend
uniform float pbr_emw[10];
uniform float pbr_amb_all;    // blended ambient luminance over every live slot
uniform float pbr_key_scale;
out vec3 pbr_wpos;
out vec3 pbr_view_v;
out vec4 pbr_vn;
uniform int pbr_pxb;  // 1: tex_T18 holds this tree's parallax basis (Unbake.h)
uniform usamplerBuffer tex_T18;
out vec4 pbr_pxn;  // parallax normal toward the camera, w = 1 where defined
out vec3 pbr_pxu;  // world gradients of u and v, UV per metre
out vec3 pbr_pxv;
out float pbr_sv;
out float pbr_e;
out float pbr_kv;  // key-light weight where the sun share is ~0
out float pbr_ksun;  // the bake's sun term at N.L = 1 (luminance, vertex colour units), 0: none

float pbr_h2f(uint h) {
  uint e = (h >> 10u) & 31u;
  float m = float(h & 1023u);
  float v = e == 0u ? m * exp2(-24.0) : (1.0 + m / 1024.0) * exp2(float(e) - 15.0);
  return (h & 0x8000u) != 0u ? -v : v;
}

void pbr_vertex(vec3 pos, vec3 cam, uvec4 rec) {
  pbr_wpos = pos * (1.0 / 4096.0);
  pbr_view_v = (cam - pos) * (1.0 / 4096.0);
  pbr_pxn = vec4(0.0);
  pbr_pxu = vec3(0.0);
  pbr_pxv = vec3(0.0);
  if (pbr_on != 0 && pbr_pxb != 0) {
    uvec4 b = texelFetch(tex_T18, gl_VertexID);
    if (b != uvec4(0u)) {
      vec3 n = ssao_oct_decode(b.x);
      pbr_pxn = vec4(dot(n, pbr_view_v) < 0.0 ? -n : n, 1.0);
      pbr_pxu = vec3(pbr_h2f(b.y & 0xffffu), pbr_h2f(b.y >> 16u), pbr_h2f(b.z & 0xffffu));
      pbr_pxv = vec3(pbr_h2f(b.z >> 16u), pbr_h2f(b.w & 0xffffu), pbr_h2f(b.w >> 16u));
    }
  }
  pbr_vn = vec4(0.0);
  pbr_sv = -1.0;
  pbr_e = 0.0;
  pbr_kv = -1.0;
  pbr_ksun = 0.0;
  if (pbr_on == 0 || pbr_rec == 0) {
    return;
  }
  if ((rec.w & 0x100u) != 0u) {
    pbr_vn = vec4(ssao_oct_decode(rec.y), 1.0);
  }
  float a = float(rec.x & 0xffffu) / 16384.0;
  float k = float(rec.x >> 16u) / 16384.0;
  float v = 0.0;
  for (int s = 0; s < 8; s++) {
    v += pbr_w[s] * float((rec.z >> uint(4 * s)) & 0xfu) / 15.0;
  }
  float sun = k * pbr_sun_l;
  float share = sun / max(sun + a * pbr_amb_l, 1e-3);
  pbr_sv = v * clamp(share * 2.0, 0.0, 1.0) * pbr_day;
  pbr_ksun = sun * v * pbr_day / 127.5;
  float sun_f = clamp(share * 2.0, 0.0, 1.0) * pbr_day;
  pbr_kv = (1.0 - sun_f) * clamp(a * pbr_amb_all / 128.0, 0.0, 1.0) * pbr_key_scale;
  pbr_e = float((rec.w >> 16u) & 0xffu) / 255.0 * pbr_emw[min(int((rec.w >> 24u) & 0xfu), 9)];
}

// sunken caustics (Caustics.h): live weight sum of the four caustic slots, 0 = off
uniform vec3 cau_w;
out vec3 pbr_cau;  // caustic light (envelope x cau_w), vertex-colour units before the shader's scale

/// Caustic envelope of palette entry `idx` (mean of the caustic slots, stored after the colours),
/// faded out where the original barely animates it (relative swing in alpha, range 2, under 0.1).
vec3 pbr_caustic_env(int idx) {
  if (cau_w == vec3(0.0)) {
    return vec3(0.0);
  }
  vec4 e = texelFetch(tex_T10, idx + 8192, 0);
  return e.rgb * cau_w * min(2.0 * e.a / 0.1, 1.0);
}
