#version 410 core

// sky bodies (SunDisc.cpp, style 1) with Jak 3's sky sprites. Pass 0 over sky samples: sun glow,
// moon disc, halo and wash. Pass 1 over everything: starburst and sun wash (display units)
layout(location = 0) out vec4 color;
layout(location = 2) out vec4 emissive;

uniform vec4 proj_info;  // pixel to SSAO camera space per unit z
uniform vec3 up_cs;      // world up, SSAO camera space
uniform vec3 haze;       // log(1 - fog at the fog end), layer height, fog end (m)
uniform sampler2D vis_tex;  // x 0 sun, 1 moon
uniform sampler2D moon_tex;   // full-moon, alpha = coverage
uniform sampler2D glow_tex;   // sky-glow-soft, peak 1
uniform sampler2D burst_tex;  // glow (starburst), peak 1
uniform sampler2D soft_tex;   // glow-soft, peak 1

uniform vec3 sun_cs;
uniform float sun_r;       // reference radius (rad); the glow sprite's half width is twice it
uniform vec4 sun_lum;      // core, glow (paper whites), haze transmittance, day and horizon gain
uniform vec3 sun_lin;      // glow colour, linear, luminance 1
uniform vec3 burst_col;    // starburst tint, display units, strength folded in
uniform vec3 wash_col;
uniform float burst_rot;
uniform float glare_mask;
uniform int flare_pass;

uniform vec3 moon_cs;
uniform float moon_r;
uniform float moon_pad;    // moon texture with its soft margin, over the disc
uniform vec3 moon_lum;     // disc (paper whites), haze transmittance, night and horizon gain
uniform vec3 halo_col;
uniform vec3 moon_wash_col;

const float kDeg = 0.01745329;

// haze along the pixel's ray, the sun disc's thinning of the fog layer by elevation
float ray_trans(vec3 ray) {
  float s = max(dot(ray, up_cs), 0.0);
  float path = haze.y / sqrt(s * s + (haze.y / haze.z) * (haze.y / haze.z));
  return exp(haze.x * path / haze.z);
}

// tangent-plane position around c (level with the horizon), in units of tan(angle)
vec2 tangent(vec3 ray, vec3 c) {
  vec3 e1 = cross(up_cs, c);
  e1 = dot(e1, e1) < 1e-6 ? vec3(1.0, 0.0, 0.0) : normalize(e1);
  vec3 e2 = cross(c, e1);
  float d = dot(ray, c);
  return d > 1e-3 ? vec2(dot(ray, e1), dot(ray, e2)) / d : vec2(1e3);
}

vec2 sprite_uv(vec2 tp, float half_rad) {
  return tp / tan(half_rad) * 0.5 + 0.5;
}

void main() {
  vec3 ray = normalize(vec3(gl_FragCoord.xy * proj_info.xy + proj_info.zw, 1.0));
  // sprites sit in the haze: a softer fade than a body behind it
  float t_px = pow(ray_trans(ray), 0.35);
  vec3 lin = vec3(0.0);
  vec3 enc = vec3(0.0);
  float mask = 0.0;

  if (flare_pass != 0) {
    // sun_lum.w: day and horizon gain times the sun ray's haze
    vec2 tp = tangent(ray, sun_cs);
    float v = texelFetch(vis_tex, ivec2(0, 0), 0).r;
    float c = cos(burst_rot), s = sin(burst_rot);
    vec2 tb = mat2(c, s, -s, c) * tp;
    enc = burst_col * texture(burst_tex, sprite_uv(tb, 7.5 * kDeg)).r +
          wash_col * texture(soft_tex, sprite_uv(tp, 34.5 * kDeg)).r;
    enc *= sun_lum.w * v;
  } else if (sun_lum.w > 0.0) {
    vec2 tp = tangent(ray, sun_cs);
    float v = texelFetch(vis_tex, ivec2(0, 0), 0).r;
    float g = pow(texture(glow_tex, sprite_uv(tp, 2.0 * sun_r)).r, 2.2);
    lin += (vec3(sun_lum.x * pow(g, 8.0)) + sun_lin * (sun_lum.y * g)) * (sun_lum.z * sun_lum.w * v);
    mask = glare_mask * g * v;
  }

  if (flare_pass == 0 && moon_lum.z > 0.0) {
    vec2 tp = tangent(ray, moon_cs);
    float v = texelFetch(vis_tex, ivec2(1, 0), 0).r;
    vec4 m = texture(moon_tex, sprite_uv(tp, moon_r * moon_pad));
    lin += pow(m.rgb, vec3(2.2)) * (1.4 * m.a * moon_lum.x * moon_lum.y * moon_lum.z * v);
    vec3 h = halo_col * (texture(glow_tex, sprite_uv(tp, 3.4 * moon_r)).r * v) +
             moon_wash_col * texture(soft_tex, sprite_uv(tp, 17.5 * kDeg)).r;
    enc += h * (moon_lum.z * t_px);
  }

  vec3 c = pow(lin, vec3(1.0 / 2.2)) + enc;
  if (max(c.r, max(c.g, c.b)) < 1e-4 && mask <= 0.0) {
    discard;
  }
  color = vec4(c, 0.0);
  emissive = vec4(vec2(mask), 0.0, 0.0);
}
