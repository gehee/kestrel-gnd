#pragma once
// The OSD's main 2D program and the vertex buffer every 2D draw fills: made
// in OSD::init_shaders(), used by CanvasGles and by the OSD's own GL drawing
// (the gallery, the screen view, the background).

#include <GLES2/gl2.h>

namespace render {

struct Gles2d {
    GLuint program = 0;
    GLuint vbo = 0;

    // Uniform/attribute locations, looked up once in init_shaders() instead
    // of via glGet*Location() on every draw call - the OSD recomposites the
    // whole HUD per frame, so those per-draw string lookups were a measurable
    // chunk of the render hot path.
    GLint u_mvp = -1, u_is_text = -1, u_use_shading = -1,
          u_color = -1, u_alpha = -1, u_tex = -1;
    GLint a_pos = -1, a_uv = -1, a_alpha_factor = -1;
};

}  // namespace render
