// Screen-space history (ScreenHistory.h): the previous frame's RGBA16F term, read at the
// previous-frame position of a world point.
#ifndef SO_SCREEN_HISTORY
#define SO_SCREEN_HISTORY
uniform sampler2D sh_prev;  // previous history, consumer-defined channels
uniform mat3 sh_prev_k;     // previous camera offset (world axes, game units) to clip x, y, w
uniform vec3 sh_prev_cam;   // previous camera position, world game units
uniform int sh_valid;       // 0: no usable history (first frame, cut, resize, frame gap)

/// Previous-frame UV of world point `pw` and its view depth then (metres); false when off screen,
/// behind the camera or the history is invalid.
bool sh_reproject(vec3 pw, out vec2 uv, out float depth_m) {
  uv = vec2(0.0);
  depth_m = 0.0;
  if (sh_valid == 0) {
    return false;
  }
  vec3 c = sh_prev_k * (pw - sh_prev_cam);
  if (c.z <= 1.0) {
    return false;
  }
  uv = c.xy / c.z * 0.5 + 0.5;
  depth_m = c.z / 4096.0;
  return all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)));
}
#endif
