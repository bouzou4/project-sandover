uniform sampler2D tex_T0;
vec4 pbr_albedo_grad(vec2 uv, vec2 dx, vec2 dy) { return textureGrad(tex_T0, uv, dx, dy); }
