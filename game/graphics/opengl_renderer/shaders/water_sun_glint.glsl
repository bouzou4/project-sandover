// Sun glint on water (ocean_common.frag, merc2.frag): GGX, Smith, Schlick F0 0.02, in linear
// light, shadowed by the world sun map (world_shadow_frag.glsl must come before this file).
uniform vec4 sun_glint;  // strength, roughness (GGX alpha), peak (linear luminance), 1 on
uniform vec3 sun_glint_dir;  // ocean: world, toward the sun of the water's own level
uniform vec3 sun_glint_col;

/// GGX glint of the sun (`L` toward it, `col` its colour, `up` the world up) along `V` on normal
/// `N` with alpha^2 `a2`, soft-capped at `peak` (linear luminance); 0 under the horizon.
vec3 wsg_lobe(vec3 N, vec3 V, vec3 L, vec3 col, vec3 up, float a2, float peak) {
  float sun_up = smoothstep(-0.02, 0.08, dot(L, up));
  float ndl = dot(N, L);
  float ndv = max(dot(N, V), 1e-3);
  if (sun_up <= 0. || ndl <= 0.) {
    return vec3(0.);
  }
  vec3 H = L + V;
  H *= inversesqrt(max(dot(H, H), 1e-8));
  a2 = clamp(a2, 1e-4, 0.25);
  float ndh = max(dot(N, H), 0.);
  float d = ndh * ndh * (a2 - 1.) + 1.;
  float D = a2 / (3.14159265 * d * d);
  float k = 0.5 * sqrt(a2);
  float vis = 0.25 / ((ndl * (1. - k) + k) * (ndv * (1. - k) + k));
  float F = 0.02 + 0.98 * pow(1. - max(dot(V, H), 0.), 5.);
  vec3 g = col * (sun_glint.x * sun_up * D * vis * F * ndl);
  float y = dot(g, vec3(0.2126, 0.7152, 0.0722));
  return g / (1. + y / max(peak, 1e-3));
}

/// Linear glint radiance of the sun seen along `V` on normal `N`; `var` widens alpha^2 by the
/// slope variance the surface lost below the pixel.
vec3 wsg_radiance(vec3 N, vec3 V, vec3 L, vec3 col, vec3 up, float var) {
  return wsg_lobe(N, V, L, col, up, sun_glint.y * sun_glint.y + var, sun_glint.z);
}

/// Normal variance under the pixel from the screen derivatives of N (specular anti-aliasing).
float wsg_pixel_var(vec3 N) {
  vec3 dx = dFdx(N);
  vec3 dy = dFdy(N);
  return min(0.5 * (dot(dx, dx) + dot(dy, dy)), 0.18);
}

/// Display-encoded colour `c` with linear radiance `g` added.
vec3 wsg_add(vec3 c, vec3 g) {
  vec3 lin = pow(max(c, vec3(0.)), vec3(2.2));
  return pow(lin + g, vec3(1. / 2.2));
}

/// Smooth value noise in 0..1 on unit cells.
float wsg_vnoise(vec2 x) {
  vec2 c = floor(x);
  vec2 f = x - c;
  f = f * f * (3. - 2. * f);
  uvec2 q = uvec2(ivec2(c) + ivec2(32768));
  uint h00 = (q.x * 1597334673u) ^ (q.y * 3812015801u);
  uint h10 = ((q.x + 1u) * 1597334673u) ^ (q.y * 3812015801u);
  uint h01 = (q.x * 1597334673u) ^ ((q.y + 1u) * 3812015801u);
  uint h11 = ((q.x + 1u) * 1597334673u) ^ ((q.y + 1u) * 3812015801u);
  vec4 h = vec4(uvec4(h00, h10, h01, h11) * 1103515245u >> 8) / 16777216.;
  return mix(mix(h.x, h.y, f.x), mix(h.z, h.w, f.x), f.y);
}

/// Height gradient (d/dx, d/dz) of six crossing capillary-to-short gravity waves at world xz `p`
/// (m), time `t` (s); a wave finer than a few pixels fades and its slope variance goes to `lost`.
vec2 wsg_ripple(vec2 p, vec2 dpx, vec2 dpy, float t, out float lost) {
  vec2 g = vec2(0.);
  lost = 0.;
  for (int i = 0; i < 6; i++) {
    float fi = float(i);
    float lam = 2.4 * pow(0.69, fi);
    float s = 0.12 - 0.01 * fi;
    float ang = 0.7 + 2.39996 * fi;
    vec2 d = vec2(cos(ang), sin(ang));
    float k = 6.2831853 / lam;
    float px = lam / max(max(abs(dot(d, dpx)), abs(dot(d, dpy))), 1e-6);
    float keep = smoothstep(2., 6., px);
    // slow noise on each wave's phase and amplitude breaks the lattice the crossing waves make
    vec2 nq = p / (2.5 * lam) + vec2(17.3, 5.9) * fi;
    float ph = k * dot(d, p) - sqrt(9.81 * k) * t + 6.2831853 * wsg_vnoise(nq);
    g += d * (s * keep * (0.4 + 1.2 * wsg_vnoise(nq.yx + 41.7)) * cos(ph));
    lost += 0.5 * s * s * (1. - keep * keep);
  }
  return g;
}

/// Sun the world shadow map leaves at water point `p` (receiver space of the program); open
/// where the map is off or no cascade reaches. Call ws_begin(p) first, in uniform control flow.
float wsg_sun_vis(vec3 p) {
  float v = ws_water_vis(p);
  return v < 0. ? 1. : v;
}
