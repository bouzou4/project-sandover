#version 410 core

// Motion vector debug views (MotionVectors.h View), drawn over the whole frame
out vec4 color;

uniform sampler2D vel;   // RG16F velocity, px
uniform sampler2D aux;   // camera-only velocity xy, clip w / 4096, source
uniform sampler2D cur;   // this frame's scene colour
uniform sampler2D prev;  // the previous frame's
uniform int mode;        // 1 velocity, 2 reprojection error, 3 source, 4 error with camera-only
uniform vec2 size;      // render target pixels (the velocity grid)
uniform vec2 out_size;  // the framebuffer drawn into (display size after an upscale)
uniform float scale;     // px per frame at full brightness
uniform float gain;

vec3 hue(float a) {
  vec3 k = vec3(0.0, 2.0, 4.0) / 6.0;
  return clamp(abs(fract(a + k) * 6.0 - 3.0) - 1.0, 0.0, 1.0);
}

void main() {
  vec2 uv = gl_FragCoord.xy / out_size;
  // legend, bottom left: colour wheel (direction) and a ramp with a tick per scale / 4 px
  vec2 lp = gl_FragCoord.xy - vec2(24.0, 24.0);
  float r = length(lp - vec2(64.0));
  if (mode == 1 && r < 64.0) {
    vec2 d = lp - vec2(64.0);
    color = vec4(hue(atan(d.y, d.x) / 6.2831853 + 0.5) * (r / 64.0), 1.0);
    return;
  }
  if (mode == 1 && lp.x >= 144.0 && lp.x < 400.0 && lp.y >= 0.0 && lp.y < 24.0) {
    float m = (lp.x - 144.0) / 256.0;
    float tick = step(fract(m * 4.0 + 0.004), 0.008);
    color = vec4(vec3(m) * (1.0 - tick) + vec3(1.0, 0.2, 0.2) * tick, 1.0);
    return;
  }
  vec2 v = texture(vel, uv).xy;
  vec4 a = texture(aux, uv);
  if (mode == 1) {
    float m = length(v);
    color = vec4(m > 0.0 ? hue(atan(v.y, v.x) / 6.2831853 + 0.5) * min(m / scale, 1.0)
                         : vec3(0.0), 1.0);
  } else if (mode == 3) {
    int s = int(a.w + 0.5);
    vec3 c = s == 1 ? vec3(0.1, 0.9, 0.1) : s == 2 ? vec3(0.8, 0.1, 0.1)
             : s == 3 ? vec3(0.9, 0.1, 0.9) : vec3(0.1, 0.2, 0.8);
    color = vec4(mix(texture(cur, uv).rgb, c, 0.5), 1.0);
  } else {
    vec2 mv = mode == 4 ? a.xy : v;
    vec3 p = texture(prev, uv + mv / size).rgb;
    vec3 c = texture(cur, uv).rgb;
    color = vec4(abs(p - c) * gain, 1.0);
  }
}
