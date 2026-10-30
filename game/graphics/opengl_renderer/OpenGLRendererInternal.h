#pragma once

// Shared by the OpenGLRenderer translation units only.

#include "game/graphics/opengl_renderer/Fbo.h"

Fbo make_fbo(int w, int h, int msaa, bool make_zbuf_and_stencil, bool fp16 = false);
