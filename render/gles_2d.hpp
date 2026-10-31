#pragma once
// The GLES programs the HUD draws with: the main 2D one and the vertex buffer
// every 2D draw fills, the background's warp, the video's (external
// textures), the gallery's snapshot and the flat-colour one; made by
// init_programs() on the HUD's thread, used by CanvasGles.

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

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

    GLuint warp = 0;          // the background image's warp-zoom transition
    GLuint ext_plain = 0;     // a video picture (samplerExternalOES), as it is
    GLuint ext_warp = 0;      // ... with the warp-zoom
    GLuint snap = 0;          // a texture of premultiplied RGBA as it is (the gallery's copy of the screen)
    GLuint flat = 0;          // a colour per vertex, premultiplied (the stats charts, pixel-space fills)
    GLuint flat_vbo = 0;
    GLint  flat_mvp = -1, flat_pos = -1, flat_col = -1;

    // EGL's DMA-BUF import (a video picture as a texture, no copy).
    PFNEGLCREATEIMAGEKHRPROC            eglCreateImageKHR = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC           eglDestroyImageKHR = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES = nullptr;
};

// Compiles and links them, and looks up what they use; on the HUD's thread,
// with its GL context current. A program that fails is logged and left 0.
void init_programs(Gles2d& gl);

}  // namespace render
