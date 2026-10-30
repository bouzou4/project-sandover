// unified vista fog (vista_atmo_foreground_uniforms): x on, y start (m), z density (1/m),
// w 1/scale height (1/m); atmo_up: world y of the camera axes
uniform vec4 atmo;
uniform vec3 atmo_up;
uniform vec3 atmo_cam;
// neighbour-mode haze: x density (1/m), y start (m), z max amount
uniform vec4 haze;
out float haze_f;

float atmo_fog(vec3 cpos, float fog_min) {
  float dist_m = length(cpos) * (1. / 4096.);
  float mid_y_m = (atmo_cam.y + 0.5 * dot(atmo_up, cpos)) * (1. / 4096.);
  float od = atmo.z * max(dist_m - atmo.y, 0.) * exp(-atmo.w * max(mid_y_m, 0.));
  return (255. - fog_min) * (1. - exp(-od));
}
