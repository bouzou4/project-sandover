#version 410 core

// ground mist (Swamp.h; swamp, Misty Island): exponential height fog above the water line along
// each pixel's ray
out vec4 color;

uniform sampler2D csz;  // camera-space z in metres (SSAO depth copy), full res at mip 0
SWAMP_MIST_GLSL
uniform vec3 mist_col;
uniform vec3 sun_dir;  // world, toward the sun
uniform vec3 sun_col;  // hue, max component 1
uniform float sun_k;

void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  float z = texelFetch(csz, min(p, textureSize(csz, 0) - ivec2(1)), 0).r;
  vec3 v;
  float a = swamp_mist_alpha(p, z, v);
  float g = sun_k * pow(max(dot(v, sun_dir), 0.0), 6.0);
  color = vec4(mist_col * (1.0 + 0.6 * g * sun_col), a);
}
