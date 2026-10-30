// atmo.x 2: fog is max(game, unified + haze); only the excess takes the tint and haze colour
float atmo_combine(float van, float af, inout float hz, out float tw) {
  float v = van / 255., te = 1. - (1. - af / 255.) * (1. - hz), a1 = max(v, af / 255.);
  tw = te > v ? (te - v) / te : 0.;
  hz = te > v ? max(1. - (1. - te) / max(1. - a1, 1e-4), 0.) : 0.;
  return a1 * 255.;
}
