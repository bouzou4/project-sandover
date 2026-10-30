// Gerstner waves of the ocean shading (Materials.cpp water_setup_ocean), shared by the ocean
// shaders (ocean_common.vert/.frag, ocean_water.vert/.tese). World xz in metres, y up.
uniform vec4 water_w0[5];  // direction x, direction z, wavenumber (1/m), amplitude (m)
uniform vec4 water_w1[5];  // angular speed (1/s), horizontal amplitude (m), range scale
uniform vec4 water_wnoise;  // amplitude noise, phase noise (turns), 1 / cell (1/m), drift (1/s)
uniform vec4 water_warp;    // crest warp amplitude (m), 1 / cell (1/m); amplitude 0 is off
uniform float water_time;
uniform vec4 water_range;  // full amplitude up to x (m), none from y (m)
uniform int water_disp_on;
uniform mat4 water_cam;  // world to GS homogeneous, as the collision shader uses it
uniform mat4 water_cam_inv;
uniform vec4 water_hvdf;  // hvdf offset xyz, fog constant
uniform vec4 water_eye;   // camera position (game units), ocean plane height
uniform int water_plane_ok;
uniform vec2 water_ray_z;  // GS depths at one and two units of view depth (water_ray_depths)
uniform mat3 water_rot;   // camera space to world
uniform vec4 water_proj;  // hvdf x, y; GS pixels to camera-space x/z, y/z
uniform int water_depth_ok;
uniform sampler2D water_depth;  // opaque scene depth
uniform sampler2D water_shoal_depth;  // the same before the mid ocean drew (vertex shoaling)
uniform vec3 water_lin;         // see make_new_cam_mat
uniform float water_shoal;      // waves are gone where the water is this shallow (m); 0 off
uniform float water_shoal_occ;  // ring weight of a sample under a float or deck
uniform vec2 water_shore_fade;  // shallow tint, shore foam and shoaling fade out over this distance (m)
uniform vec2 stereo_gs;         // per-eye GS x shift: x += stereo_gs.x * (z - stereo_gs.y)
uniform uint perf_opt;           // pc-set-perf-opt; water bits 12-15
uniform vec2 water_wave_aa;     // a wave's slope is whole above x pixels per wavelength, gone
                                // below y; y <= 0 is off
// rest-position derivatives per pixel (fragment shader only; zero elsewhere, which is no fade)
vec2 water_xz_dx = vec2(0.);
vec2 water_xz_dy = vec2(0.);
// chop waves' weight (all but the last, the swell): ocean_common.frag's water_k, else 1
float water_chop_k = 1.;
// mean square slope the range falloff and the wave AA took out of the last water_gerstner sum
float water_slope_lost = 0.;

float water_whash(vec2 p) {
  uvec2 q = uvec2(ivec2(floor(p))) * uvec2(1597334673u, 3812015801u);
  return float((q.x ^ q.y) * 1597334673u) * (1. / 4294967295.);
}

/// Smooth value noise in 0..1, one lattice cell per unit of p.
float water_wnoise1(vec2 p) {
  vec2 i = floor(p);
  vec2 f = fract(p);
  vec2 u = f * f * (3. - 2. * f);
  return mix(mix(water_whash(i), water_whash(i + vec2(1., 0.)), u.x),
             mix(water_whash(i + vec2(0., 1.)), water_whash(i + vec2(1., 1.)), u.x), u.y);
}

/// water_wnoise1 and its gradient: x the value, yz d/dp.
vec3 water_wnoise1d(vec2 p) {
  vec2 i = floor(p);
  vec2 f = fract(p);
  vec2 u = f * f * (3. - 2. * f);
  vec2 du = 6. * f * (1. - f);
  float a = water_whash(i);
  float b = water_whash(i + vec2(1., 0.));
  float c = water_whash(i + vec2(0., 1.));
  float d = water_whash(i + vec2(1., 1.));
  float k = a - b - c + d;
  return vec3(a + (b - a) * u.x + (c - a) * u.y + k * u.x * u.y,
              du * vec2(b - a + k * u.y, c - a + k * u.x));
}

/// Displacement D (m) of the rest point xz, and its derivatives along x and z. `dist` is the
/// horizontal distance to the camera (m): each wave keeps its full amplitude to the full range
/// and falls off smoothly to none at the zero range (shorter waves sooner). `edge` scales every
/// wave (0 on the mid ocean's outer edge).
void water_gerstner(vec2 xz, float dist, float edge, out vec3 D, out vec3 ddx, out vec3 ddz) {
  D = vec3(0.);
  ddx = vec3(0.);
  ddz = vec3(0.);
  water_slope_lost = 0.;
  for (int i = 0; i < 5; i++) {
    vec2 dir = water_w0[i].xy;
    float k = water_w0[i].z;
    float s = water_w1[i].z;
    float fr = 1. - smoothstep(water_range.x * s, water_range.y * s, dist);
    if (i < 4) {
      fr *= water_chop_k;
    }
    float f = edge * fr;
    // slow world-space noise on each wave's amplitude and phase breaks the sum's repeat; cells
    // are much longer than the waves, so the derivatives leave it out
    vec2 np = xz * water_wnoise.z + vec2(37.1, 11.3) * float(i) + water_wnoise.w * water_time;
    float m = 1. + water_wnoise.x * (2. * water_wnoise1(np) - 1.);
    if (f <= 0. && (perf_opt & (1u << 27)) != 0u) {
      // adds nothing to D or its derivatives; its whole slope is lost
      float am0 = water_w0[i].w * m * k * edge;
      water_slope_lost += 0.5 * am0 * am0;
      continue;
    }
    float ph = water_wnoise.y > 0. ? 6.2831853 * water_wnoise.y *
                                         water_wnoise1(np.yx + vec2(5.7, 23.9)) : 0.;
    float a = water_w0[i].w * f * m;
    float q = water_w1[i].y * f * min(m, 1.);  // never steeper than the sum allows
    float th = k * dot(dir, xz) - water_w1[i].x * water_time + ph;
    vec2 g = dir;  // phase gradient / k
    if (water_warp.x > 0.) {
      // crest warp: two octaves of noise shift each wave along itself, varying 3x faster
      // across it than along it, so crests bend and the waves drift out of step
      vec2 pe = vec2(-dir.y, dir.x);
      vec2 wp = vec2(dot(pe, xz), dot(dir, xz) / 3.) * water_warp.y +
                vec2(7.31, 3.17) * float(i) + vec2(water_wnoise.w * water_time, 0.);
      vec3 n1 = water_wnoise1d(wp);
      vec3 n2 = water_wnoise1d(wp * 3.4641 + vec2(17.7, 8.85));
      th += k * water_warp.x * (2. * n1.x - 1. + 0.25 * (2. * n2.x - 1.));
      vec2 gw = 2. * water_warp.x * water_warp.y * (n1.yz + 0.866 * n2.yz);
      g += gw.x * pe + gw.y / 3. * dir;
    }
    vec2 kg = k * g;
    float kept = fr;
    if (water_wave_aa.y > 0.) {
      // slope terms only: a wave thinner than a few pixels breaks its crest lines into dashes
      float pw = max(abs(dot(kg, water_xz_dx)), abs(dot(kg, water_xz_dy)));
      float sa = smoothstep(water_wave_aa.y, water_wave_aa.x, 6.2831853 / max(pw, 1e-6));
      kg *= sa;
      kept *= sa;
    }
    float am = water_w0[i].w * m * k * edge;
    water_slope_lost += 0.5 * am * am * (1. - kept * kept);
    float sn = sin(th);
    float c = cos(th);
    D += vec3(q * dir.x * c, a * sn, q * dir.y * c);
    ddx += vec3(-q * dir.x * sn * kg.x, a * c * kg.x, -q * dir.y * sn * kg.x);
    ddz += vec3(-q * dir.x * sn * kg.y, a * c * kg.y, -q * dir.y * sn * kg.y);
  }
}

/// View depth (game units) of a window depth.
float water_viewz(float d) {
  return abs(water_lin.x / (water_lin.y * (2. * d - 1.) - water_lin.z));
}

/// World point (game units) of GS pixel `gxy` at GS depth `gz`, through the inverse of the
/// matrix the displaced vertices are projected with.
vec3 water_gs_world(vec2 gxy, float gz) {
  vec4 ph = water_cam_inv * vec4((vec3(gxy, gz) - water_hvdf.xyz) / water_hvdf.w, 1.);
  return ph.xyz / ph.w;
}

/// World point (game units) of a GS vertex (x, y in GS pixels / 4096, z in depth / 2^24). On the
/// plane path it is where the vertex's pixel ray meets the ocean plane. The ray is taken through
/// two fixed, well-conditioned depths, never the vertex's own: far away one GS depth step spans
/// hundreds of metres, and past the far asymptote the point flips behind the camera.
bool water_plane_missed = false;  // set by water_plane_point: the plane path's ray missed
// the vertex's share of its own depth in its rest point: 1 on the near ocean (the game's wave
// heights) but its rim, which meets the mid on the plane (CommonOceanRenderer near_rim_on_plane)
float water_depth_w = 0.;

vec3 water_plane_point(vec3 position) {
  vec2 gxy = position.xy * 4096.;
  water_plane_missed = water_plane_ok != 0 && water_depth_w < 1.;
  if (water_plane_ok != 0 && water_depth_w < 1.) {
    vec3 A = water_gs_world(gxy, water_ray_z.x);
    vec3 d = water_gs_world(gxy, water_ray_z.y) - A;
    float s = (water_eye.w - A.y) / d.y;
    // s = -1 is the camera: only the half of the ray in front of it
    if (abs(d.y) > 1e-6 * length(d) && s > -0.999) {
      water_plane_missed = false;
      vec3 P = A + d * s;
      return water_depth_w > 0. ? mix(P, water_gs_world(gxy, position.z * 16777216.), water_depth_w)
                                : P;
    }
  }
  return water_gs_world(gxy, position.z * 16777216.);
}

/// Shoal weight from the scene depth behind one rest point P: 0 at the waterline, 1 from
/// `water_shoal` metres of water down, `water_shoal_occ` under something standing on the water.
float water_shoal_at(vec3 P) {
  vec4 c0 = -(water_cam * vec4(P, 1.));
  if (c0.w <= 0.) {
    return 1.;
  }
  vec3 g = c0.xyz * (water_hvdf.w / c0.w) + water_hvdf.xyz;
  // as gl_Position in ocean_water.tese, with this eye's shift
  float x = g.x / 4096. + stereo_gs.x * (g.z / 16777216. - stereo_gs.y);
#ifdef XR_GS_DEFINED
  vec4 xh = xr_gs(vec3(x, g.y / 4096., g.z / 16777216.));
  vec2 ndc = vec2((xh.x / xh.w - 0.5) * 16.,
                  -(xh.y / xh.w - 0.5) * 32. * SCISSOR_ADJUST * HEIGHT_SCALE);
#else
  vec2 ndc = vec2((x - 0.5) * 16.,
                  -(g.y / 4096. - 0.5) * 32. * SCISSOR_ADJUST * HEIGHT_SCALE);
#endif
  vec2 uv = ndc * 0.5 + 0.5;
  if (any(lessThan(uv, vec2(0.))) || any(greaterThan(uv, vec2(1.)))) {
    return 1.;
  }
  float ds = textureLod(water_shoal_depth, uv, 0.).r;
  if (ds <= 1e-7) {
    return 1.;  // nothing behind: open water
  }
  float vz = water_viewz(g.z / 16777216.);
  float dz = water_viewz(ds) - vz;
  if (isinf(dz) || isnan(dz) || vz <= 0.) {
    return 1.;
  }
  // vertical metres of water under the point (negative: ground above it)
  float depth = dz * (water_eye.y - P.y) / vz / 4096.;
  if (depth < -3.) {
    return 1.;  // an occluder well in front, not the bank
  }
  if (depth < 0.) {
    return water_shoal_occ;  // a float, deck or bank: it outweighs open-water samples of the ring
  }
  return smoothstep(0., water_shoal, depth);
}

/// Wave weight over shallow ground at P, so crests never climb onto the land; averaged over a
/// ring of `water_shoal` metres, since the depth behind P jumps at every silhouette.
float water_shoal_of(vec3 P) {
  if (water_shoal <= 0. || water_depth_ok == 0 || water_eye.y <= P.y) {
    return 1.;
  }
  // far shores behind open water keep their waves, as the shore foam fades
  float near = 1. - smoothstep(water_shore_fade.x, water_shore_fade.y,
                               length(P.xz - water_eye.xz) / 4096.);
  if (near <= 0. && (perf_opt & (1u << 12)) != 0u) {
    return 1.;  // mix(1., w, 0.): the ring has no weight
  }
  float r = water_shoal * 4096.;
  float w = water_shoal_at(P);
  if (w < 0.) {
    return 1. - near;  // under a float or deck: still, so no crest rises through it
  }
  for (int i = 0; i < 8; i++) {
    float a = 0.78539816 * float(i);
    w += water_shoal_at(P + vec3(r * cos(a), 0., r * sin(a)));
  }
  return mix(1., clamp(w / 9., 0., 1.), near);
}

/// Rest xz (m) / w and 1 / w of a vertex that stays where it is (no tessellation).
vec3 water_rest_of(vec3 position) {
  vec3 P = water_plane_point(position);
  vec4 c = -(water_cam * vec4(P, 1.));
  return c.w > 0. ? vec3(P.xz / 4096., 1.) / c.w : vec3(0.);
}

/// Displaced GS position of a vertex; `rest` gets rest xz (m) / w and 1 / w for the fragment
/// shader (0 when the point is behind the camera, which keeps the vertex as it is).
/// False for NaN, infinite or absurdly large values (comparisons with NaN are false, which
/// survives drivers that fold isnan away).
bool water_finite3(vec3 v) {
  return all(lessThan(abs(v), vec3(1e30)));
}

// shoal weight given by the caller (ocean_water.tese: the patch corners' weights, interpolated)
float water_shoal_fixed = -1.;

vec3 water_displace(vec3 position, inout float edge, out vec3 rest) {
  rest = vec3(0.);
  vec3 P = water_plane_point(position);
  // no world point on the plane (a vertex the game clipped at the camera plane, or a ray that
  // misses): keep it where the game put it, as the untessellated path does; a NaN here drops
  // the whole patch
  if (water_plane_missed || !water_finite3(P)) {
    edge = 0.;
    return position;
  }
  vec2 xz = P.xz / 4096.;
  if (edge > 0. && (-(water_cam * vec4(P, 1.))).w > 0.) {
    edge *= water_shoal_fixed >= 0. ? water_shoal_fixed : water_shoal_of(P);
  }
  vec3 D, ddx, ddz;
  water_gerstner(xz, length(P.xz - water_eye.xz) / 4096., edge, D, ddx, ddz);
  vec4 c = -(water_cam * vec4(P + D * 4096., 1.));
  if (c.w <= 0.) {
    return position;
  }
  vec3 g = c.xyz * (water_hvdf.w / c.w) + water_hvdf.xyz;
  if (!water_finite3(g)) {
    edge = 0.;
    return position;
  }
  rest = vec3(xz, 1.) / c.w;
  // an undisplaced rim keeps the game's vertex, so it meets the ocean-far edge exactly
  return edge > 0. ? vec3(g.xy / 4096., g.z / 16777216.) : position;
}
