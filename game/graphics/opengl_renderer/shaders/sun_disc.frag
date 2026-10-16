#version 410 core

// sun disc (SunDisc.cpp), added over sky samples: luminance in paper whites falls from the core
// to the limb on a log scale, so a hot core clips white and the limb keeps the sun's colour
layout(location = 0) out vec4 color;
layout(location = 2) out vec4 emissive;

uniform vec4 proj_info;   // pixel to SSAO camera space per unit z
uniform vec3 sun_cs;      // sun direction, SSAO camera space
uniform float radius;     // angular radius (rad)
uniform float px_per_rad;
uniform vec2 lum;         // core, limb (paper whites)
uniform vec3 col_lin;     // linear colour with luminance 1
uniform float gain;       // haze, day and horizon
uniform float glare_mask; // emissive mask value for the bloom bright pass
uniform sampler2D vis_tex;

void main() {
  vec3 ray = normalize(vec3(gl_FragCoord.xy * proj_info.xy + proj_info.zw, 1.0));
  float a = 2.0 * asin(min(0.5 * length(ray - sun_cs), 1.0));
  float r = a / radius;
  float cov = clamp((1.0 - r) * radius * px_per_rad / 1.5 + 0.5, 0.0, 1.0);
  if (cov <= 0.0) {
    discard;
  }
  float v = texelFetch(vis_tex, ivec2(0), 0).r;
  float rr = min(r, 1.0);
  float l = lum.x * pow(max(lum.y, 1e-6) / max(lum.x, 1e-6), rr * rr);
  vec3 lin = col_lin * (l * gain * v);
  color = vec4(pow(lin, vec3(1.0 / 2.2)) * cov, 0.0);
  emissive = vec4(vec2(glare_mask * cov * v), 0.0, 0.0);
}
