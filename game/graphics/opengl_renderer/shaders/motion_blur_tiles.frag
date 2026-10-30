#version 410 core

// Motion blur tiles (MotionBlur.h). mode 0: the longest blur vector of a tile x tile block of the
// relative velocity. mode 1: the longest of the 3x3 neighbouring tiles.
out vec2 v_out;

uniform sampler2D src;  // mode 0: relative velocity (MotionVectors rel_texture), 1: tile max
uniform int mode;
uniform int tile;       // px per tile
uniform float vscale;   // blur half-length per px of frame velocity
uniform float rmax;     // half-length cap, px

BLUR_VEC_GLSL
void main() {
  ivec2 t = ivec2(gl_FragCoord.xy);
  vec2 best = vec2(0.0);
  float bl = 0.0;
  if (mode == 0) {
    ivec2 lim = textureSize(src, 0) - 1;
    ivec2 o = t * tile;
    for (int y = 0; y < tile; y++) {
      for (int x = 0; x < tile; x++) {
        vec2 v = blur_vec(texelFetch(src, min(o + ivec2(x, y), lim), 0));
        float l = dot(v, v);
        if (l > bl) {
          bl = l;
          best = v;
        }
      }
    }
  } else {
    ivec2 lim = textureSize(src, 0) - 1;
    for (int y = -1; y <= 1; y++) {
      for (int x = -1; x <= 1; x++) {
        vec2 v = texelFetch(src, clamp(t + ivec2(x, y), ivec2(0), lim), 0).xy;
        float l = dot(v, v);
        if (l > bl) {
          bl = l;
          best = v;
        }
      }
    }
  }
  v_out = best;
}
