// clip (x, y, w) image of a camera-space direction, following the transform in main()
vec3 merc_clip_axis(vec4 t) {
  return vec3((t.x * fog_constants.x + (hvdf_offset.x - 2048.) * t.w) / 256.,
              -(t.y * fog_constants.x + (hvdf_offset.y - 2048.) * t.w) / 128. * SCISSOR_ADJUST *
                  HEIGHT_SCALE,
              t.w);
}
