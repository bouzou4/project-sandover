// darkcave merc light (ActorShadows.h): the level light is scaled by cave_dark_base and rises to
// full with the strongest lit crystal's glow, faded from 5 m to 70 m (cavecrystal-light.gc)
uniform float cave_dark_base;
uniform int cave_ncrystal;
uniform vec4 cave_crystal[7];  // merc camera space; w: glow (palette fade, 0..2)

float cave_dark(vec3 cpos) {
  float glow = 0.0;
  for (int i = 0; i < 7; i++) {
    if (i >= cave_ncrystal) {
      break;
    }
    vec4 c = cave_crystal[i];
    float d = length(cpos - c.xyz) * (1.0 / 4096.0);
    glow = max(glow, c.w * (1.0 - clamp((d - 5.0) / 65.0, 0.0, 1.0)));
  }
  return min(cave_dark_base + (1.0 - cave_dark_base) * glow, 1.0);
}
