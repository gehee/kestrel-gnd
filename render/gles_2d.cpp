// The GLES programs the HUD draws with (render/gles_2d.hpp), moved out of the
// OSD's init_shaders() as they were.
#include "render/gles_2d.hpp"

#include <cstdio>
#include <cstring>

namespace render {

static const char* v_shader_str = 
    "attribute vec3 pos;\n"
    "attribute vec2 uv;\n"
    "attribute float v_alpha_factor;\n" 
    "varying vec2 v_uv;\n"
    "varying float v_alpha_varying;\n" 
    "uniform mat4 mvp;\n"
    "void main() {\n"
    "    v_uv = uv;\n"
    "    v_alpha_varying = v_alpha_factor;\n" 
    "    gl_Position = mvp * vec4(pos, 1.0);\n"
    "}\n";

static const char* f_shader_str = 
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "varying float v_alpha_varying;\n" 
    "uniform sampler2D tex;\n"
    "uniform float alpha;\n"
    "uniform vec4 color;\n"
    "uniform bool is_text;\n"
    "uniform bool use_shading;\n"
    "void main() {\n"
    "    if (is_text) {\n"
    "        vec4 t = texture2D(tex, v_uv).bgra;\n" 
    "        gl_FragColor = t * color * alpha * v_alpha_varying;\n" 
    "    } else {\n"
    "        float final_alpha = alpha;\n"
    "        vec3 final_color = color.rgb;\n"
    "        \n"
    "        if (use_shading) {\n"
    "            float dist = distance(v_uv, vec2(0.5));\n"
    "            float shade = 1.0 - smoothstep(0.0, 0.5, dist);\n"
    "            final_color = mix(color.rgb, color.rgb + 0.1, shade);\n"
    "            final_alpha = mix(alpha * 0.4, alpha, shade);\n"
    "        }\n"
    "        \n"
    "        gl_FragColor = vec4(final_color, final_alpha) * alpha * v_alpha_varying;\n" 
    "    }\n"
    "}\n";

void init_programs(Gles2d& gl2d_) {
    auto compile = [](GLenum type, const char* src) {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, NULL);
        glCompileShader(s);
        // Check compile log
        GLint success;
        glGetShaderiv(s, GL_COMPILE_STATUS, &success);
        if (!success) {
            char log[512];
            glGetShaderInfoLog(s, 512, NULL, log);
            fprintf(stderr, "Shader Compile Error: %s\n", log);
        }
        return s;
    };
    
    GLuint vs = compile(GL_VERTEX_SHADER, v_shader_str);
    GLuint fs = compile(GL_FRAGMENT_SHADER, f_shader_str);
    
    gl2d_.program = glCreateProgram();
    glAttachShader(gl2d_.program, vs);
    glAttachShader(gl2d_.program, fs);
    glLinkProgram(gl2d_.program);
    glUseProgram(gl2d_.program);

    // Cache hot-path uniform/attribute locations once (see osd.hpp). A local
    // alias `sp` is used so the mechanical replacement of the
    // glGet*Location(gl2d_.program, ...) call sites elsewhere leaves these
    // one-time lookups intact.
    {
        GLuint sp = gl2d_.program;
        gl2d_.u_mvp         = glGetUniformLocation(sp, "mvp");
        gl2d_.u_is_text     = glGetUniformLocation(sp, "is_text");
        gl2d_.u_use_shading = glGetUniformLocation(sp, "use_shading");
        gl2d_.u_color       = glGetUniformLocation(sp, "color");
        gl2d_.u_alpha       = glGetUniformLocation(sp, "alpha");
        gl2d_.u_tex         = glGetUniformLocation(sp, "tex");
        gl2d_.a_pos          = glGetAttribLocation(sp, "pos");
        gl2d_.a_uv           = glGetAttribLocation(sp, "uv");
        gl2d_.a_alpha_factor = glGetAttribLocation(sp, "v_alpha_factor");
    }

    glGenBuffers(1, &gl2d_.vbo);
    
    // Set default value for the new attribute so existing draw calls (which don't enable it) work fine
    GLint alpha_loc = gl2d_.a_alpha_factor;
    if (alpha_loc != -1) {
        glVertexAttrib1f(alpha_loc, 1.0f);
    }

    // ---- Warp-zoom transition shader ----
    static const char* warp_vert =
        "attribute vec3 pos;\n"
        "attribute vec2 uv;\n"
        "varying vec2 v_uv;\n"
        "uniform mat4 mvp;\n"
        "void main() {\n"
        "    v_uv = uv;\n"
        "    gl_Position = mvp * vec4(pos, 1.0);\n"
        "}\n";

    // t=0: full frame, no distortion
    // t=1: fully zoomed / barrel-distorted / transparent → reveals FPV beneath
    //
    // Distortion model:
    //   - Centre zooms in hard (× up to 9)
    //   - Horizontal edges barrel outward much more than vertical
    //     → sides appear to stretch/warp outward as the centre rushes through
    //   - Clamp-to-edge instead of black so extreme UV wraps smear rather than cut
    static const char* warp_frag =
        "precision mediump float;\n"
        "varying vec2 v_uv;\n"
        "uniform sampler2D tex;\n"
        "uniform float warp_t;\n"
        "void main() {\n"
        "    float t2   = warp_t * warp_t;\n"
        "    float zoom = 1.0 + t2 * 8.0;\n"
        // Asymmetric barrel: horizontal ×20, vertical ×4
        // At screen edge (u=±0.5): horizontal expansion ratio ≈ (1+20×0.25)/zoom
        // >> vertical (1+4×0.25)/zoom → sides visibly stretch outward
        "    float bx = t2 * 20.0;\n"
        "    float by = t2 *  4.0;\n"
        "    vec2 uv = v_uv - 0.5;\n"
        "    uv.x = uv.x * (1.0 + bx * uv.x * uv.x) / zoom;\n"
        "    uv.y = uv.y * (1.0 + by * uv.y * uv.y) / zoom;\n"
        // Clamp to [0,1] — edge pixels smear rather than going black
        "    uv = clamp(uv + 0.5, 0.0, 1.0);\n"
        // Chromatic aberration — BGRA texture: .b=R, .g=G, .r=B
        "    float fringe = t2 * 0.04;\n"
        "    float r = texture2D(tex, clamp(uv + vec2(fringe, 0.0), 0.0, 1.0)).b;\n"
        "    float g = texture2D(tex, uv).g;\n"
        "    float b = texture2D(tex, clamp(uv - vec2(fringe, 0.0), 0.0, 1.0)).r;\n"
        "    float a = texture2D(tex, uv).a;\n"
        "    float alpha = max(0.0, 1.0 - t2 * 1.35);\n"
        "    gl_FragColor = vec4(r, g, b, a) * alpha;\n"
        "}\n";

    auto compile_s = [](GLenum type, const char* src) {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, NULL);
        glCompileShader(s);
        GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[256]; glGetShaderInfoLog(s, 256, NULL, log);
            fprintf(stderr, "[warp shader] %s\n", log);
        }
        return s;
    };
    GLuint wvs = compile_s(GL_VERTEX_SHADER,   warp_vert);
    GLuint wfs = compile_s(GL_FRAGMENT_SHADER, warp_frag);
    gl2d_.warp = glCreateProgram();
    glAttachShader(gl2d_.warp, wvs);
    glAttachShader(gl2d_.warp, wfs);
    glLinkProgram(gl2d_.warp);
    GLint wok; glGetProgramiv(gl2d_.warp, GL_LINK_STATUS, &wok);
    if (!wok) {
        char log[256]; glGetProgramInfoLog(gl2d_.warp, 256, NULL, log);
        fprintf(stderr, "[warp shader link] %s\n", log);
        glDeleteProgram(gl2d_.warp);
        gl2d_.warp = 0;
    }
    glUseProgram(gl2d_.program); // restore main program

    // ---- EGL external-texture shaders (NV12 zero-copy, DRM_PRIME path) ----
    // These use samplerExternalOES so the Mali GPU converts NV12→RGB in hardware.

    static const char* ext_vert =
        "attribute vec3 pos;\n"
        "attribute vec2 uv;\n"
        "varying vec2 v_uv;\n"
        "uniform mat4 mvp;\n"
        "void main() {\n"
        "    v_uv = uv;\n"
        "    gl_Position = mvp * vec4(pos, 1.0);\n"
        "}\n";

    // Plain full-opacity draw (BACKGROUND state)
    static const char* ext_frag_plain =
        "#extension GL_OES_EGL_image_external : require\n"
        "precision mediump float;\n"
        "varying vec2 v_uv;\n"
        "uniform samplerExternalOES tex;\n"
        "void main() {\n"
        "    gl_FragColor = texture2D(tex, v_uv);\n"
        "}\n";

    // Warp-zoom + chromatic aberration (TRANSITION state)
    // samplerExternalOES on Mali outputs standard RGB — no .bgra swizzle needed.
    // Same distortion model as warp_frag above.
    static const char* ext_frag_warp =
        "#extension GL_OES_EGL_image_external : require\n"
        "precision mediump float;\n"
        "varying vec2 v_uv;\n"
        "uniform samplerExternalOES tex;\n"
        "uniform float warp_t;\n"
        "void main() {\n"
        "    float t2   = warp_t * warp_t;\n"
        "    float zoom = 1.0 + t2 * 8.0;\n"
        "    float bx = t2 * 20.0;\n"
        "    float by = t2 *  4.0;\n"
        "    vec2 uv = v_uv - 0.5;\n"
        "    uv.x = uv.x * (1.0 + bx * uv.x * uv.x) / zoom;\n"
        "    uv.y = uv.y * (1.0 + by * uv.y * uv.y) / zoom;\n"
        "    uv = clamp(uv + 0.5, 0.0, 1.0);\n"
        // samplerExternalOES → standard RGB (NV12→RGB done in hardware by Mali)
        "    float fringe = t2 * 0.04;\n"
        "    float r = texture2D(tex, clamp(uv + vec2(fringe, 0.0), 0.0, 1.0)).r;\n"
        "    float g = texture2D(tex, uv).g;\n"
        "    float b = texture2D(tex, clamp(uv - vec2(fringe, 0.0), 0.0, 1.0)).b;\n"
        "    float alpha = max(0.0, 1.0 - t2 * 1.35);\n"
        "    gl_FragColor = vec4(r, g, b, 1.0) * alpha;\n"
        "}\n";

    auto build_prog = [&](const char* vs_src, const char* fs_src) -> GLuint {
        GLuint vs = compile_s(GL_VERTEX_SHADER,   vs_src);
        GLuint fs = compile_s(GL_FRAGMENT_SHADER, fs_src);
        GLuint p  = glCreateProgram();
        glAttachShader(p, vs); glAttachShader(p, fs);
        glLinkProgram(p);
        GLint ok; glGetProgramiv(p, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[256]; glGetProgramInfoLog(p, 256, NULL, log);
            fprintf(stderr, "[ext shader link] %s\n", log);
            glDeleteProgram(p); return 0;
        }
        return p;
    };
    gl2d_.ext_plain   = build_prog(ext_vert, ext_frag_plain);
    gl2d_.ext_warp = build_prog(ext_vert, ext_frag_warp);
    // A texture of premultiplied RGBA as it is, for the gallery's copy of the
    // screen. (The main program reads its textures as BGRA.)
    static const char* snap_frag =
        "precision mediump float;\n"
        "varying vec2 v_uv;\n"
        "uniform sampler2D tex;\n"
        "uniform float alpha;\n"
        "void main() {\n"
        "    gl_FragColor = texture2D(tex, v_uv) * alpha;\n"
        "}\n";
    gl2d_.snap = build_prog(ext_vert, snap_frag);
    // Flat colour per vertex (premultiplied, as the OSD plane holds it): the stats charts.
    static const char* flat_vert =
        "attribute vec2 pos;\n"
        "attribute vec4 col;\n"
        "uniform mat4 mvp;\n"
        "varying vec4 v_col;\n"
        "void main() {\n"
        "    gl_Position = mvp * vec4(pos, 0.0, 1.0);\n"
        "    v_col = col;\n"
        "}\n";
    static const char* flat_frag =
        "precision mediump float;\n"
        "varying vec4 v_col;\n"
        "void main() { gl_FragColor = v_col; }\n";
    gl2d_.flat = build_prog(flat_vert, flat_frag);
    if (gl2d_.flat) {
        gl2d_.flat_mvp = glGetUniformLocation(gl2d_.flat, "mvp");
        gl2d_.flat_pos = glGetAttribLocation(gl2d_.flat, "pos");
        gl2d_.flat_col = glGetAttribLocation(gl2d_.flat, "col");
    }

    // Load EGL DMA-BUF extension function pointers
    gl2d_.eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)
        eglGetProcAddress("eglCreateImageKHR");
    gl2d_.eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)
        eglGetProcAddress("eglDestroyImageKHR");
    gl2d_.glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
        eglGetProcAddress("glEGLImageTargetTexture2DOES");

    if (gl2d_.eglCreateImageKHR && gl2d_.glEGLImageTargetTexture2DOES)
        fprintf(stderr, "[OSD] EGL DMA-BUF import available — zero-copy bg video enabled\n");
    else
        fprintf(stderr, "[OSD] EGL DMA-BUF import NOT available — CPU fallback\n");

    glUseProgram(gl2d_.program);
}

}  // namespace render
