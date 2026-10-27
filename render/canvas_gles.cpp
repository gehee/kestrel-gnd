#include "render/canvas_gles.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

// The OSD's own 2D drawing, moved here unchanged from hud/osd.cpp and
// hud/hud_canopy.cpp: each primitive sets the 2D program's uniforms, fills the
// shared vertex buffer and draws. The program is the OSD's (render/gles_2d),
// current whenever these run.

namespace render {

// Edge feathering: a cheap stand-in for multisampling. The canopy's shapes
// are drawn flat and then slanted and put in perspective by the panel
// matrices, so their edges land on the screen as diagonals with a hard
// staircase. Instead of 4x MSAA (four times the blending work on this
// one-core GPU - see drm.cpp), each rail, block and wedge edge gets a border
// about a pixel wide whose alpha runs from the shape's down to zero, and the
// ordinary blend smooths the step. feather_ is that width in canopy units
// (0 = off; set per frame by the canopy, set_feather()).
namespace {
// Quad a-b-c-d (in loop order) as two triangles, each corner with its alpha.
void quad6(std::vector<float>& v, float ax, float ay, float aa, float bx, float by, float ba,
           float cx, float cy, float ca, float dx, float dy, float da) {
    v.insert(v.end(), {ax, ay, 0.0f, 0.5f, 0.5f, aa, bx, by, 0.0f, 0.5f, 0.5f, ba,
                       cx, cy, 0.0f, 0.5f, 0.5f, ca, ax, ay, 0.0f, 0.5f, 0.5f, aa,
                       cx, cy, 0.0f, 0.5f, 0.5f, ca, dx, dy, 0.0f, 0.5f, 0.5f, da});
}
// A feathered axis-aligned rectangle: the body at alpha a, then a ring f
// wide fading to zero around it.
void feathered_rect(std::vector<float>& v, float x0, float y0, float x1, float y1, float a, float f) {
    quad6(v, x0, y0, a, x1, y0, a, x1, y1, a, x0, y1, a);
    if (f <= 0.0f) return;
    quad6(v, x0 - f, y0 - f, 0, x1 + f, y0 - f, 0, x1, y0, a, x0, y0, a);   // bottom
    quad6(v, x0, y1, a, x1, y1, a, x1 + f, y1 + f, 0, x0 - f, y1 + f, 0);   // top
    quad6(v, x0 - f, y0 - f, 0, x0, y0, a, x0, y1, a, x0 - f, y1 + f, 0);   // left
    quad6(v, x1, y0, a, x1 + f, y0 - f, 0, x1 + f, y1 + f, 0, x1, y1, a);   // right
}
}  // namespace


void CanvasGles::panel(float x, float y, float w, float h, float alpha, float r, float g, float b) {
    float verts[] = {
        x,   y,   0,   0, 0,
        x+w, y,   0,   1, 0,
        x,   y+h, 0,   0, 1,
        x+w, y+h, 0,   1, 1
    };
    
    glUniform1i(gl_.u_is_text, 0);
    glUniform1i(gl_.u_use_shading, 0);
    glUniform4f(gl_.u_color, r, g, b, 0.85); 
    glUniform1f(gl_.u_alpha, alpha);
    
    glBindBuffer(GL_ARRAY_BUFFER, gl_.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    
    GLint pos_loc = gl_.a_pos;
    glEnableVertexAttribArray(pos_loc);
    glVertexAttribPointer(pos_loc, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), 0);
    
    GLint uv_loc = gl_.a_uv;
    glEnableVertexAttribArray(uv_loc);
    glVertexAttribPointer(uv_loc, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
    
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void CanvasGles::icon(ImageId tex, float x, float y, float w, float h, float r, float g, float b, float alpha) {
    if (tex == 0) return;
    float verts[] = {
        x,   y,   0,   0, 1,
        x+w, y,   0,   1, 1,
        x,   y+h, 0,   0, 0,
        x+w, y+h, 0,   1, 0
    };
    
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(gl_.u_tex, 0);
    glUniform1i(gl_.u_is_text, 1);
    glUniform1i(gl_.u_use_shading, 0);
    glUniform4f(gl_.u_color, r, g, b, 1.0); 
    glUniform1f(gl_.u_alpha, alpha);
    
    glBindBuffer(GL_ARRAY_BUFFER, gl_.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    
    GLint pos_loc = gl_.a_pos;
    glEnableVertexAttribArray(pos_loc);
    glVertexAttribPointer(pos_loc, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), 0);
    
    GLint uv_loc = gl_.a_uv;
    glEnableVertexAttribArray(uv_loc);
    glVertexAttribPointer(uv_loc, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
    
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

// Draw a texture over an arbitrary quad (bl, br, tr, tl) rather than an
// axis-aligned rect, so text can be warped by the menu's perspective: the
// bottom edge of a line ends up wider than its top edge, matching the panel.
void CanvasGles::icon_quad(ImageId tex, math::Vec2 bl, math::Vec2 br,
                         math::Vec2 tr, math::Vec2 tl, float r, float g, float b) {
    if (tex == 0) return;
    float verts[] = {
        bl.x, bl.y, 0,   0, 1,
        br.x, br.y, 0,   1, 1,
        tl.x, tl.y, 0,   0, 0,
        tr.x, tr.y, 0,   1, 0
    };

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(gl_.u_tex, 0);
    glUniform1i(gl_.u_is_text, 1);
    glUniform1i(gl_.u_use_shading, 0);
    glUniform4f(gl_.u_color, r, g, b, 1.0);
    glUniform1f(gl_.u_alpha, 1.0);

    glBindBuffer(GL_ARRAY_BUFFER, gl_.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);

    GLint pos_loc = gl_.a_pos;
    glEnableVertexAttribArray(pos_loc);
    glVertexAttribPointer(pos_loc, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), 0);

    GLint uv_loc = gl_.a_uv;
    glEnableVertexAttribArray(uv_loc);
    glVertexAttribPointer(uv_loc, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void CanvasGles::trapezoid(float x, float y, float w, float h, float top_scale, float slant, float alpha, float r, float g, float b) {
    float top_w = w * top_scale;
    float inset = (w - top_w) / 2.0f;
    
    // Slant shifts the whole top row
    float slant_x = h * slant;

    float verts[] = {
        x,           y,   0,   0, 0,
        x+w,         y,   0,   1, 0,
        x+inset+slant_x,       y+h, 0,   0, 1,
        x+w-inset+slant_x,     y+h, 0,   1, 1
    };
    
    glUniform1i(gl_.u_is_text, 0);
    glUniform1i(gl_.u_use_shading, 0);
    glUniform4f(gl_.u_color, r, g, b, 0.85); 
    glUniform1f(gl_.u_alpha, alpha);
    
    glBindBuffer(GL_ARRAY_BUFFER, gl_.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    
    GLint pos_loc = gl_.a_pos;
    glEnableVertexAttribArray(pos_loc);
    glVertexAttribPointer(pos_loc, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), 0);
    
    GLint uv_loc = gl_.a_uv;
    glEnableVertexAttribArray(uv_loc);
    glVertexAttribPointer(uv_loc, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
    
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void CanvasGles::poly(const std::vector<math::Vec2>& points, float alpha, float r, float g, float b) {
    if (points.size() < 3) return;
    
    std::vector<float> verts;
    verts.reserve(points.size() * 3 * 5); 
    
    math::Vec2 center = {0,0};
    for(const auto& p : points) { center.x += p.x; center.y += p.y; }
    center.x /= points.size(); center.y /= points.size();

    for (size_t i = 0; i < points.size(); ++i) {
        size_t next = (i + 1) % points.size();
        verts.insert(verts.end(), {center.x, center.y, 0, 0.5f, 0.5f});
        verts.insert(verts.end(), {points[i].x, points[i].y, 0, 0, 0});
        verts.insert(verts.end(), {points[next].x, points[next].y, 0, 1, 1});
    }

    glUniform1i(gl_.u_is_text, 0);
    glUniform1i(gl_.u_use_shading, 0);
    glUniform4f(gl_.u_color, r, g, b, 1.0f);
    glUniform1f(gl_.u_alpha, alpha);

    glBindBuffer(GL_ARRAY_BUFFER, gl_.vbo);
    glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_DYNAMIC_DRAW);

    GLint pos_loc = gl_.a_pos;
    glEnableVertexAttribArray(pos_loc);
    glVertexAttribPointer(pos_loc, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), 0);

    GLint uv_loc = gl_.a_uv;
    glEnableVertexAttribArray(uv_loc);
    glVertexAttribPointer(uv_loc, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));

    glDrawArrays(GL_TRIANGLES, 0, points.size() * 3);
}

// The video link warning's glow: a rail along the top of the picture and one
// along the bottom, each a solid line at the frame edge whose light falls off
// quickly and then trails away inboard. The sides are left alone - that is
// where the canopy's wings sit - and each rail fades out before the corners
// rather than stopping square, so it reads as light, not as a painted bar.
void CanvasGles::rail_glow(float fw, float fh, float depth, float alpha,
                         float r, float g, float b) {
    if (depth <= 0.0f || alpha <= 0.0f) return;
    // Down the rail, from the edge inboard: x depth, x alpha.
    static const float kInset[] = { 0.0f, 0.04f, 0.12f, 0.45f, 1.0f };
    static const float kAlpha[] = { 1.0f, 1.00f, 0.55f, 0.18f, 0.0f };
    // Along it, as a fraction of fw: the rail spans 88% of the width and
    // ramps up over its outer 14% at each end.
    static const float kX[]  = { -0.88f, -0.6336f, 0.6336f, 0.88f };
    static const float kXa[] = {  0.0f,   1.0f,    1.0f,    0.0f  };
    const int nv = (int)(sizeof(kInset) / sizeof(kInset[0]));
    const int nh = (int)(sizeof(kX) / sizeof(kX[0]));

    std::vector<float> v;
    v.reserve(2 * (nv - 1) * (nh - 1) * 6 * 6);
    for (int side = 0; side < 2; side++) {
        const float edge = side ? fh : -fh, dir = side ? -1.0f : 1.0f;
        for (int k = 0; k + 1 < nv; k++) {
            float y0 = edge + dir * kInset[k] * depth;
            float y1 = edge + dir * kInset[k + 1] * depth;
            for (int c = 0; c + 1 < nh; c++) {
                float x0 = kX[c] * fw, x1 = kX[c + 1] * fw;
                float a00 = alpha * kAlpha[k]     * kXa[c];
                float a10 = alpha * kAlpha[k]     * kXa[c + 1];
                float a01 = alpha * kAlpha[k + 1] * kXa[c];
                float a11 = alpha * kAlpha[k + 1] * kXa[c + 1];
                const float q[6][3] = {{x0, y0, a00}, {x1, y0, a10}, {x1, y1, a11},
                                       {x0, y0, a00}, {x1, y1, a11}, {x0, y1, a01}};
                for (int m = 0; m < 6; m++)
                    v.insert(v.end(), {q[m][0], q[m][1], 0.0f, 0.5f, 0.5f, q[m][2]});
            }
        }
    }

    glUniform1i(gl_.u_is_text, 0);
    glUniform1i(gl_.u_use_shading, 0);
    glUniform4f(gl_.u_color, r, g, b, 1.0f);
    glUniform1f(gl_.u_alpha, 1.0f);

    glBindBuffer(GL_ARRAY_BUFFER, gl_.vbo);
    glBufferData(GL_ARRAY_BUFFER, v.size() * sizeof(float), v.data(), GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(gl_.a_pos);
    glVertexAttribPointer(gl_.a_pos, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), 0);
    glEnableVertexAttribArray(gl_.a_uv);
    glVertexAttribPointer(gl_.a_uv, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    if (gl_.a_alpha_factor != -1) {
        glEnableVertexAttribArray(gl_.a_alpha_factor);
        glVertexAttribPointer(gl_.a_alpha_factor, 1, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(5 * sizeof(float)));
    }
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(v.size() / 6));
    if (gl_.a_alpha_factor != -1) {
        glDisableVertexAttribArray(gl_.a_alpha_factor);
        glVertexAttrib1f(gl_.a_alpha_factor, 1.0f);
    }
}

// Helper to draw a glowing line by stacking transparent lines
void CanvasGles::glow_line(const std::vector<math::Vec2>& points, float width, float alpha, float r, float g, float b) {
    if (points.size() < 2) return;
    
    // Disable shading logic for lines
    glUniform1i(gl_.u_use_shading, 0);

    // Compute normals at each point
    std::vector<math::Vec2> normals(points.size());
    for (size_t i = 0; i < points.size(); i++) {
        float dx, dy;
        if (i == 0) {
            dx = points[1].x - points[0].x;
            dy = points[1].y - points[0].y;
        } else if (i == points.size() - 1) {
            dx = points[i].x - points[i-1].x;
            dy = points[i].y - points[i-1].y;
        } else {
            float dx1 = points[i].x - points[i-1].x;
            float dy1 = points[i].y - points[i-1].y;
            float dx2 = points[i+1].x - points[i].x;
            float dy2 = points[i+1].y - points[i].y;
            float len1 = sqrtf(dx1*dx1 + dy1*dy1);
            float len2 = sqrtf(dx2*dx2 + dy2*dy2);
            if (len1 > 0) { dx1 /= len1; dy1 /= len1; }
            if (len2 > 0) { dx2 /= len2; dy2 /= len2; }
            dx = dx1 + dx2;
            dy = dy1 + dy2;
        }
        float len = sqrtf(dx*dx + dy*dy);
        if (len > 0) {
            normals[i].x = -dy / len;
            normals[i].y = dx / len;
        } else {
            normals[i].x = 0.0f;
            normals[i].y = 1.0f;
        }
    }

    // Render 3 passes for glow (each pass is a SINGLE draw call)
    for (int j = 0; j < 3; j++) {
        float w_scale = 1.0f + j * 2.0f;
        float w = width * w_scale * 0.001f;
        float a = alpha / (j * 2.0f + 1.0f);
        if (j == 0) a = alpha;

        std::vector<float> verts;
        verts.reserve(points.size() * 2 * 5);

        for (size_t i = 0; i < points.size(); i++) {
            float nx = normals[i].x * w;
            float ny = normals[i].y * w;
            
            verts.push_back(points[i].x + nx);
            verts.push_back(points[i].y + ny);
            verts.push_back(0.0f);
            verts.push_back(0.0f);
            verts.push_back(0.0f);
            
            verts.push_back(points[i].x - nx);
            verts.push_back(points[i].y - ny);
            verts.push_back(0.0f);
            verts.push_back(0.0f);
            verts.push_back(0.0f);
        }

        glUniform1i(gl_.u_is_text, 0);
        glUniform4f(gl_.u_color, r, g, b, a);
        glUniform1f(gl_.u_alpha, a);
        glBindBuffer(GL_ARRAY_BUFFER, gl_.vbo);
        glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_DYNAMIC_DRAW);
        GLint pos_loc = gl_.a_pos;
        glEnableVertexAttribArray(pos_loc);
        glVertexAttribPointer(pos_loc, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), 0);
        GLint uv_loc = gl_.a_uv;
        glEnableVertexAttribArray(uv_loc);
        glVertexAttribPointer(uv_loc, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
        glDrawArrays(GL_TRIANGLE_STRIP, 0, (int)verts.size() / 5);
    }
}

void CanvasGles::hex_panel(float x, float y, float w, float h, float alpha, float r, float g, float b, bool outline, int num_h, int num_v, float grid_y, float grid_h, bool uniform_x) {
    if (!outline) return; // Background now handled inside the outline block for integration
    
    // side estimation: left wing x is negative, right wing x is positive
    bool is_left = (x < 0);
    
    auto get_pos = [&](float tx, float ty) {
        float ratio_y = (ty - y) / h;
        float span_scale = 1.0f - (ratio_y * 0.3f);
        float sw = w * span_scale;
        float sx = x + (w - sw) / 2.0f;
        float ratio_x = (tx - x) / w;
        return math::Vec2{sx + ratio_x * sw, ty};
    };

    // 4. Smooth Gradient Background
    // We draw a single quad (or two depending on wing shape) with vertex colors for smooth interpolation.
    // Left Wing: Fades out towards Top-Right. Max alpha at Bottom-Left.
    // Right Wing: Fades out towards Top-Left. Max alpha at Bottom-Right.
    
    // Coordinates
    float x1 = x;
    float x2 = x + w;
    float y1 = y;
    float y2 = y + h;
    
    // Get actual polygon corners based on trapezoid shape
    math::Vec2 p_bl = get_pos(x1, y1);
    math::Vec2 p_br = get_pos(x2, y1);
    math::Vec2 p_tr = get_pos(x2, y2);
    math::Vec2 p_tl = get_pos(x1, y2);

    float max_a = alpha * 0.85f;

    // Vertex Alphas based on Wing Side
    float a_bl, a_br, a_tr, a_tl;
    
    if (is_left) {
        // Max at Bottom-Left (p_bl)
        a_bl = max_a;
        a_br = max_a * 0.4f;
        a_tr = 0.0f;          // Fade out top-right
        a_tl = max_a * 0.4f;
    } else {
        // Max at Bottom-Right (p_br)
        a_bl = max_a * 0.4f;
        a_br = max_a;
        a_tr = max_a * 0.4f;
        a_tl = 0.0f;          // Fade out top-left
    }
    
    // Draw directly using gl buffer to support per-vertex color/alpha if shader supports it.
    // Our 'draw_poly' uses uniform alpha. We need a way to pass vertex attributes or just standard lerp.
    // Since our simple shader uses uniform alpha, drawing a smooth gradient requires modifying the drawing logic 
    // OR just drawing a bunch of overlapping quads which is what caused the blockiness.
    // Wait, the shader 'f_shader_str' currently handles 'v_uv' but uses uniform color/alpha.
    
    // Hack: We can simulate a gradient by drawing many thin overlapping slices? No that's what we just removed.
    // Better: Update the shader to support vertex colors or use UVs to create a gradient.
    // The shader has v_uv. We can use UVs to generate the gradient in the Fragment Shader.
    // UVs are (0,0) to (1,1).
    
    // Let's use 'draw_poly' but rely on a new shader uniform or just custom implementation here?
    // 'draw_poly' sets UVs: (0,0) (0,1) (1,1)... wait, draw_poly sets UVs:
    // center (0,0.5), p1(0,0), pnext(1,1)?? No draw_poly sets weird UVs.
    
    // Look at 'draw_trapezoid' or 'draw_panel' - they set specific UVs.
    // Let's implement 'draw_gradient_quad' locally here or use a custom block.
    
    // Actually, 'draw_panel' sets UVs (0,0)..(1,1) correctly?
    // In 'draw_hex_panel':
    // We already calculated p_bl, p_br, p_tr, p_tl. 
    // We can feed these into a VBO.
    // NOTE: Our fragment shader "f_shader_str" lines 215-217 has some "use_shading" logic.
    // If we enable 'use_shading', it creates a radial gradient from center 0.5. This isn't what we want.
    
    // Simplest fix without changing shader:
    // Create a dense mesh (e.g. 10x10) for the background so vertex interpolation (if we had vertex colors) works? 
    // No, we only have uniform color.
    
    // Real fix: Update Shader to support a 'gradient_mode'.
    // BUT modifying shader might break other things or require recompile of everything.
    
    // Alternate: Use 100s of very small overlapping transparent layers? Expensive.
    
    auto smoothstep = [](float edge0, float edge1, float x) {
        float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };


    // BATCHED SMOOTH BACKGROUND RENDER (Gouraud Shading)
    // We generate a mesh where each vertex has its own calculated alpha factor.
    // This removes the "squares" artifact by letting GL interpolate alpha.
    
    int bg_steps = 20; // Enough for smooth interpolation
    std::vector<float> bg_verts;
    bg_verts.reserve(bg_steps * bg_steps * 6 * 6); // x,y,z,u,v,a * 6 verts/quad
    
    glUniform1i(gl_.u_is_text, 0);
    glUniform1i(gl_.u_use_shading, 0);
    // Use the base color, alpha will be modulated by the attribute
    glUniform4f(gl_.u_color, r, g, b, 1.0f);
    glUniform1f(gl_.u_alpha, alpha); // Base alpha, attribute multiplies this

    for (int j = 0; j < bg_steps; j++) {
        for (int i = 0; i < bg_steps; i++) {
            float rx1 = (float)i/bg_steps;
            float rx2 = (float)(i+1)/bg_steps;
            float ry1 = (float)j/bg_steps;
            float ry2 = (float)(j+1)/bg_steps;

            float x1_l = x + rx1 * w;
            float x2_l = x + rx2 * w;
            float y1_l = y + ry1 * h;
            float y2_l = y + ry2 * h;

            math::Vec2 p1 = get_pos(x1_l, y1_l); // BL
            math::Vec2 p2 = get_pos(x2_l, y1_l); // BR
            math::Vec2 p3 = get_pos(x2_l, y2_l); // TR
            math::Vec2 p4 = get_pos(x1_l, y2_l); // TL

            // Helper to calc alpha factor for a logical coordinate (0..1)
            auto calc_a = [&](float rx, float ry) {
                float fade_x = uniform_x ? 0.5f : (is_left ? (1.0f - rx) : rx);
                float fade_top = 1.0f - ry;
                return 1.25f * fade_x * fade_top;
            };

            float a1 = calc_a(rx1, ry1);
            float a2 = calc_a(rx2, ry1);
            float a3 = calc_a(rx2, ry2);
            float a4 = calc_a(rx1, ry2);

            // Quad as 2 triangles: p1-p2-p3, p1-p3-p4
            auto push = [&](math::Vec2 p, float a) {
                bg_verts.push_back(p.x); bg_verts.push_back(p.y); bg_verts.push_back(0); // pos
                bg_verts.push_back(0.5f); bg_verts.push_back(0.5f); // dummy UV
                bg_verts.push_back(a); // alpha factor
            };

            push(p1, a1); push(p2, a2); push(p3, a3);
            push(p1, a1); push(p3, a3); push(p4, a4);
        }
    }

    if (!bg_verts.empty()) {
        glBindBuffer(GL_ARRAY_BUFFER, gl_.vbo);
        glBufferData(GL_ARRAY_BUFFER, bg_verts.size() * sizeof(float), bg_verts.data(), GL_DYNAMIC_DRAW);

        GLint pos_loc = gl_.a_pos;
        glEnableVertexAttribArray(pos_loc);
        glVertexAttribPointer(pos_loc, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), 0);

        GLint uv_loc = gl_.a_uv;
        glEnableVertexAttribArray(uv_loc);
        glVertexAttribPointer(uv_loc, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));

        GLint alpha_loc = gl_.a_alpha_factor;
        if (alpha_loc != -1) {
            glEnableVertexAttribArray(alpha_loc);
            glVertexAttribPointer(alpha_loc, 1, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(5 * sizeof(float)));
        }

        glDrawArrays(GL_TRIANGLES, 0, bg_verts.size() / 6);

        if (alpha_loc != -1) {
            glDisableVertexAttribArray(alpha_loc);
            glVertexAttrib1f(alpha_loc, 1.0f); // Reset to default
        }
    }

    // 2. Converging Grid
    float grid_a_base = alpha * 0.40f; // Toned down grid (0.45 -> 0.40)
    float gy = (grid_y >= 0.0f) ? grid_y : y;
    float gh = (grid_h >= 0.0f) ? grid_h : h;

    // Horizontal Grid Lines
    for (int i = 0; i <= num_h; i++) {
        float ry = (float)i / num_h;
        float ly = gy + ry * gh;
        
        // Horizontal lines fade towards the far side and top
        int h_steps = 10;
        for (int k = 0; k < h_steps; k++) {
            float rx1 = (float)k / h_steps;
            float rx2 = (float)(k+1) / h_steps;
            math::Vec2 p1 = get_pos(x + rx1 * w, ly);
            math::Vec2 p2 = get_pos(x + rx2 * w, ly);
            
            float mid_rx = (rx1 + rx2) * 0.5f;
            float fade_x = uniform_x ? 0.5f : (is_left ? (1.0f - mid_rx) : mid_rx);
            
            float fade_top = 1.0f - ry;
            float a = grid_a_base * fade_x * fade_top;
            
            if (a > 0.01f) {
                poly({p1, {p1.x, p1.y + 0.003f}, {p2.x, p2.y + 0.003f}, p2}, a, 1.0f, 1.0f, 1.0f);
            }
        }
    }

    // Vertical Grid Lines
    for (int i = 0; i <= num_v; i++) {
        float rx = (float)i / num_v;
        float lx = x + rx * w;
        
        int v_segments = 8;
        for (int j = 0; j < v_segments; j++) {
            float ry1 = (float)j / v_segments;
            float ry2 = (float)(j+1) / v_segments;
            math::Vec2 p1 = get_pos(lx, gy + ry1 * gh);
            math::Vec2 p2 = get_pos(lx, gy + ry2 * gh);
            float mid_ry = (ry1 + ry2) * 0.5f;

            float fade_x = uniform_x ? 0.5f : (is_left ? (1.0f - rx) : rx);
            
            float fade_top = 1.0f - mid_ry;
            float a = grid_a_base * fade_x * fade_top;
            
            if (a > 0.01f) {
                poly({p1, {p1.x + 0.002f, p1.y}, {p2.x + 0.002f, p2.y}, p2}, a, 1.0f, 1.0f, 1.0f);
            }
        }
    }
}

void CanvasGles::arc(float cx, float cy, float r_in, float r_out, float start_angle, float end_angle, float alpha, float r, float g, float b) {
    int segments = 24;
    float step = (end_angle - start_angle) / segments;
    
    std::vector<float> verts;
    verts.reserve(segments * 6 * 5);

    for (int i = 0; i < segments; i++) {
        float a1 = start_angle + i * step;
        float a2 = start_angle + (i + 1) * step;
        
        float cos1 = cosf(a1), sin1 = sinf(a1);
        float cos2 = cosf(a2), sin2 = sinf(a2);
        
        float x1_in = cx + r_in * cos1, y1_in = cy + r_in * sin1;
        float x1_out = cx + r_out * cos1, y1_out = cy + r_out * sin1;
        float x2_in = cx + r_in * cos2, y2_in = cy + r_in * sin2;
        float x2_out = cx + r_out * cos2, y2_out = cy + r_out * sin2;
        
        verts.insert(verts.end(), {x1_in, y1_in, 0, 0, 0});
        verts.insert(verts.end(), {x1_out, y1_out, 0, 1, 0});
        verts.insert(verts.end(), {x2_in, y2_in, 0, 0, 1});
        
        verts.insert(verts.end(), {x1_out, y1_out, 0, 1, 0});
        verts.insert(verts.end(), {x2_out, y2_out, 0, 1, 1});
        verts.insert(verts.end(), {x2_in, y2_in, 0, 0, 1});
    }

    glUniform1i(gl_.u_is_text, 0);
    glUniform1i(gl_.u_use_shading, 0);
    glUniform4f(gl_.u_color, r, g, b, 0.85);
    glUniform1f(gl_.u_alpha, alpha);

    glBindBuffer(GL_ARRAY_BUFFER, gl_.vbo);
    glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_DYNAMIC_DRAW);

    GLint pos_loc = gl_.a_pos;
    glEnableVertexAttribArray(pos_loc);
    glVertexAttribPointer(pos_loc, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), 0);

    GLint uv_loc = gl_.a_uv;
    glEnableVertexAttribArray(uv_loc);
    glVertexAttribPointer(uv_loc, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));

    glDrawArrays(GL_TRIANGLES, 0, segments * 6);
}

void CanvasGles::grid_arc(float cx, float cy, float r_inner, float r_outer, float start_angle, float end_angle, float alpha) {
    int num_h = 10;
    int num_v = 24;
    
    // 1. Concentric Arcs (Continuous Flow)
    for (int i = 0; i <= num_h; i++) {
        float ratio = (float)i / num_h;
        float r = r_inner + (r_outer - r_inner) * ratio;
        // Exponential fade top-wards
        float a = alpha * 0.25f * (1.0f - ratio * ratio);
        arc(cx, cy, r, r + 0.004f, start_angle, end_angle, a, 1.0f, 1.0f, 1.0f);
    }
    
    // 2. Radial Spokes (Perspective Convergence)
    for (int i = 0; i <= num_v; i++) {
        float ratio = (float)i / num_v;
        float angle = start_angle + (end_angle - start_angle) * ratio;
        float c = cosf(angle), s = sinf(angle);
        
        float rx1 = cx + r_inner * c, ry1 = cy + r_inner * s;
        float rx2 = cx + r_outer * c, ry2 = cy + r_outer * s;
        
        // Slightly fade outer edges and top
        float v_alpha = alpha * 0.12f * (1.0f - (fabsf(ratio - 0.5f) * 1.5f));
        if (v_alpha < 0.01f) continue;

        std::vector<math::Vec2> line = {
            {rx1, ry1}, {rx1 + 0.003f, ry1},
            {rx2 + 0.003f, ry2}, {rx2, ry2}
        };
        glUniform1i(gl_.u_use_shading, 0);
        poly(line, v_alpha, 1.0f, 1.0f, 1.0f);
    }
}

float CanvasGles::text_width(const char* text, float scale) {
    return text_.width(text, scale);
}

void CanvasGles::text(const char* text, float x, float y, float scale, bool right_align, float r, float g, float b) {
    if (!text || !text[0]) return;
    const TextCache::Entry* hit = text_.get(text);
    if (!hit) return;
    const auto& cached = *hit;
    float aspect = (float)cached.w / cached.h;
    
    float tx = x;
    if (right_align) {
        float text_width_units = (cached.text_w / 64.0f) * scale;
        tx = x - text_width_units;
    }
    
    icon(cached.tex, tx, y, scale * aspect, scale, r, g, b, 1.0f);
}

// Same as draw_text, but only the left `reveal` fraction (0..1) of the
// rendered string is drawn - a texture crop, not an alpha fade, so a state
// label sweeps into view left-to-right instead of fading or popping in
// whole. draw_text caches each string as one texture (ensure_text_texture),
// so "revealing" it is just drawing a narrower sub-rect of that same quad
// rather than re-rendering glyph-by-glyph.
void CanvasGles::text_wipe(const char* text, float x, float y, float scale,
                         bool right_align, float r, float g, float b,
                         float reveal) {
    if (!text || !text[0]) return;
    reveal = fmaxf(0.0f, fminf(1.0f, reveal));
    if (reveal <= 0.0f) return;
    const TextCache::Entry* hit = text_.get(text);
    if (!hit) return;
    const auto& cached = *hit;

    // ensure_text_texture renders each string with the glyphs starting at
    // x=2 and a few pixels of margin after them, so the texture (w) is a
    // little wider than the word (text_w). Both numbers matter here and they
    // are NOT interchangeable - right-alignment is measured on the glyph
    // advance (as draw_text does), and the quad spans the whole texture.
    const float px_to_units = scale / (float)cached.h;   // 64 texture px == scale
    float tx = x;
    if (right_align) tx = x - cached.text_w * px_to_units;

    if (reveal >= 1.0f) {
        icon(cached.tex, tx, y, (float)cached.w * px_to_units, scale, r, g, b, 1.0f);
        return;
    }

    // Crop to the revealed part of the GLYPHS, not of the texture, so the
    // sweep ends exactly at the last letter.
    const float cut_px = 2.0f + cached.text_w * reveal;  // 2 = cairo's left margin
    const float w  = cut_px * px_to_units;
    const float u1 = cut_px / (float)cached.w;

    // Same vertex/uniform setup as draw_icon, just a narrower quad and a
    // matching narrower U range instead of the full 0..1 texture.
    float verts[] = {
        tx,     y,       0,  0.0f, 1,
        tx + w, y,       0,  u1,   1,
        tx,     y+scale, 0,  0.0f, 0,
        tx + w, y+scale, 0,  u1,   0
    };

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, cached.tex);
    glUniform1i(gl_.u_tex, 0);
    glUniform1i(gl_.u_is_text, 1);
    glUniform1i(gl_.u_use_shading, 0);
    glUniform4f(gl_.u_color, r, g, b, 1.0);
    glUniform1f(gl_.u_alpha, 1.0);

    glBindBuffer(GL_ARRAY_BUFFER, gl_.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);

    glEnableVertexAttribArray(gl_.a_pos);
    glVertexAttribPointer(gl_.a_pos, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), 0);
    glEnableVertexAttribArray(gl_.a_uv);
    glVertexAttribPointer(gl_.a_uv, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}


void CanvasGles::stacked_bar(float x, float y, float w, float h, float slant, bool reverse, float val1, float r1, float g1, float b1, float val2, float r2, float g2, float b2, float val3, float r3, float g3, float b3, float val4, float r4, float g4, float b4, float val5, float r5, float g5, float b5, float val6, float r6, float g6, float b6) {
    float seg_w = 0.02f;
    float spacing = 0.005f;
    
    int total_segments = (int)(w / (seg_w + spacing));
    int c1 = (int)(val1 * total_segments);
    int c2 = (int)(val2 * total_segments);
    int c3 = (int)(val3 * total_segments);
    int c4 = (int)(val4 * total_segments);
    int c5 = (int)(val5 * total_segments);
    int c6 = (int)(val6 * total_segments);
    
    if (c1 + c2 + c3 + c4 + c5 + c6 > total_segments) {
        float scale = (float)total_segments / (c1 + c2 + c3 + c4 + c5 + c6);
        c1 *= scale; c2 *= scale; c3 *= scale; c4 *= scale; c5 *= scale; c6 *= scale;
    }

    float cx = reverse ? (x + w - seg_w) : x;
    float step = reverse ? -(seg_w + spacing) : (seg_w + spacing);

    auto draw_seg = [&](int count, float r, float g, float b, float a = 0.9f) {
        if (count <= 0) return;
        for(int i=0; i<count; i++) {
             trapezoid(cx, y, seg_w, h, 1.0f, 0.0f, a, r, g, b);
             cx += step;
        }
    };
    
    draw_seg(c1, r1, g1, b1);
    draw_seg(c2, r2, g2, b2);
    draw_seg(c3, r3, g3, b3);
    draw_seg(c4, r4, g4, b4);
    draw_seg(c5, r5, g5, b5);
    draw_seg(c6, r6, g6, b6);
}

void CanvasGles::signal_bar(float x, float y, float w, float h, float value, float min_val, float max_val) {
    float norm = (value - min_val) / (max_val - min_val);
    if (norm < 0) norm = 0;
    if (norm > 1) norm = 1;

    // Color based on strength
    float r = 1.0f - norm;
    float g = norm;
    float b = 0.2f;
    
    panel(x, y, w * norm, h, 0.9f, r, g, b);
}

// A run of tapering blocks. The tall, opaque end sits at the screen edge and
// the run shrinks and fades inward, which is what gives the panel its depth
// without dimming anything (dimming reads as less important, never as further
// away). Segments past `filled` stay drawn, very faint, so the track's length
// is always legible and a falling gauge retreats towards the edge rather than
// disappearing.
void CanvasGles::seg_track(float x, float y, float w, float h, int segs,
                         float filled, bool edge_left,
                         float r, float g, float b, float a_lo, float a_hi,
                         float reveal) {
    if (segs <= 0 || w <= 0.0f || reveal <= 0.0f) return;
    int shown = (int)(reveal * segs + 0.5f);
    if (shown < 1)     shown = 1;
    if (shown > segs)  shown = segs;
    const float taper = 0.32f;
    // Keep the 10:3 block-to-gap ratio whatever the segment count, and solve
    // the pitch so the tapered run ends up exactly `w` wide.
    const float pitch = w / (segs * (1.0f - taper / 2.0f));
    const float seg_w = pitch * 10.0f / 13.0f;
    const float gap_w = pitch *  3.0f / 13.0f;

    int lit = (int)(filled * segs + 0.5f);
    if (lit < 0) lit = 0;
    if (lit > segs) lit = segs;

    std::vector<float> v;
    v.reserve(segs * 6 * 6);
    float cx = edge_left ? x : x + w;
    for (int i = 0; i < shown; i++) {
        float t  = (segs > 1) ? (float)i / (float)(segs - 1) : 0.0f;
        float sc = 1.0f - taper * t;
        float sw = seg_w * sc, sh = h * sc, sg = gap_w * sc;
        float x0 = edge_left ? cx : cx - sw;
        float x1 = x0 + sw;
        float y0 = y + (h - sh) * 0.5f;
        float y1 = y0 + sh;
        cx = edge_left ? (cx + sw + sg) : (cx - sw - sg);

        float a = (i < lit) ? (a_hi - (a_hi - a_lo) * t) : 0.13f;
        feathered_rect(v, x0, y0, x1, y1, a, feather_);
    }

    glUniform1i(gl_.u_is_text, 0);
    glUniform1i(gl_.u_use_shading, 0);
    glUniform4f(gl_.u_color, r, g, b, 1.0f);
    glUniform1f(gl_.u_alpha, 1.0f);

    glBindBuffer(GL_ARRAY_BUFFER, gl_.vbo);
    glBufferData(GL_ARRAY_BUFFER, v.size() * sizeof(float), v.data(), GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(gl_.a_pos);
    glVertexAttribPointer(gl_.a_pos, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), 0);
    glEnableVertexAttribArray(gl_.a_uv);
    glVertexAttribPointer(gl_.a_uv, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    if (gl_.a_alpha_factor != -1) {
        glEnableVertexAttribArray(gl_.a_alpha_factor);
        glVertexAttribPointer(gl_.a_alpha_factor, 1, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(5 * sizeof(float)));
    }
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(v.size() / 6));
    if (gl_.a_alpha_factor != -1) {
        glDisableVertexAttribArray(gl_.a_alpha_factor);
        glVertexAttrib1f(gl_.a_alpha_factor, 1.0f);   // restore for every other draw
    }
}

// The panel ground. The mockup's outline is a kite: tallest at the screen
// edge, its top and bottom edges converging as it runs inboard, closing to a
// point. Both edges have to slope - drawn with a constant half-height it comes
// out as a translucent slab with two hard horizontal edges, which over a
// textured video frame reads as a grey box laid on the picture rather than as
// a shape belonging to the panel.
//
// Columns are passed in already mapped to frustum space so the mockup's own
// coordinates stay legible at the call site.
void CanvasGles::fade_wedge(const float* xs, const float* tops, const float* bots,
                          const float* alphas, int n, float r, float g, float b) {
    if (n < 2) return;
    std::vector<float> v;
    v.reserve(n * 2 * 6);
    // As triangles now (was a strip), so the feathered edges can join it.
    for (int i = 0; i + 1 < n; i++) {
        quad6(v, xs[i], bots[i], alphas[i], xs[i + 1], bots[i + 1], alphas[i + 1],
                 xs[i + 1], tops[i + 1], alphas[i + 1], xs[i], tops[i], alphas[i]);
        if (feather_ > 0.0f) {
            const float f = feather_;
            quad6(v, xs[i], tops[i], alphas[i], xs[i + 1], tops[i + 1], alphas[i + 1],
                     xs[i + 1], tops[i + 1] + f, 0, xs[i], tops[i] + f, 0);
            quad6(v, xs[i], bots[i] - f, 0, xs[i + 1], bots[i + 1] - f, 0,
                     xs[i + 1], bots[i + 1], alphas[i + 1], xs[i], bots[i], alphas[i]);
        }
    }

    glUniform1i(gl_.u_is_text, 0);
    glUniform1i(gl_.u_use_shading, 0);
    glUniform4f(gl_.u_color, r, g, b, 1.0f);
    glUniform1f(gl_.u_alpha, 1.0f);

    glBindBuffer(GL_ARRAY_BUFFER, gl_.vbo);
    glBufferData(GL_ARRAY_BUFFER, v.size() * sizeof(float), v.data(), GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(gl_.a_pos);
    glVertexAttribPointer(gl_.a_pos, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), 0);
    glEnableVertexAttribArray(gl_.a_uv);
    glVertexAttribPointer(gl_.a_uv, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    if (gl_.a_alpha_factor != -1) {
        glEnableVertexAttribArray(gl_.a_alpha_factor);
        glVertexAttribPointer(gl_.a_alpha_factor, 1, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(5 * sizeof(float)));
    }
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(v.size() / 6));
    if (gl_.a_alpha_factor != -1) {
        glDisableVertexAttribArray(gl_.a_alpha_factor);
        glVertexAttrib1f(gl_.a_alpha_factor, 1.0f);
    }
}

// A hairline that starts solid at (x0,y0) and fades to nothing at (x1,y1).
void CanvasGles::fade_rule(float x0, float y0, float x1, float y1, float thick,
                         float a0, float r, float g, float b, float a1) {
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len <= 0.0f) return;
    float ux = -dy / len, uy = dx / len;              // unit normal
    float nx = ux * thick * 0.5f, ny = uy * thick * 0.5f;
    float fx = ux * feather_, fy = uy * feather_;
    // Body, plus a feather strip either side along the rule's length.
    std::vector<float> v;
    v.reserve(18 * 6);
    quad6(v, x0 - nx, y0 - ny, a0, x1 - nx, y1 - ny, a1, x1 + nx, y1 + ny, a1, x0 + nx, y0 + ny, a0);
    if (feather_ > 0.0f) {
        quad6(v, x0 - nx - fx, y0 - ny - fy, 0, x1 - nx - fx, y1 - ny - fy, 0,
                 x1 - nx, y1 - ny, a1, x0 - nx, y0 - ny, a0);
        quad6(v, x0 + nx, y0 + ny, a0, x1 + nx, y1 + ny, a1,
                 x1 + nx + fx, y1 + ny + fy, 0, x0 + nx + fx, y0 + ny + fy, 0);
    }

    glUniform1i(gl_.u_is_text, 0);
    glUniform1i(gl_.u_use_shading, 0);
    glUniform4f(gl_.u_color, r, g, b, 1.0f);
    glUniform1f(gl_.u_alpha, 1.0f);

    glBindBuffer(GL_ARRAY_BUFFER, gl_.vbo);
    glBufferData(GL_ARRAY_BUFFER, v.size() * sizeof(float), v.data(), GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(gl_.a_pos);
    glVertexAttribPointer(gl_.a_pos, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), 0);
    glEnableVertexAttribArray(gl_.a_uv);
    glVertexAttribPointer(gl_.a_uv, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    if (gl_.a_alpha_factor != -1) {
        glEnableVertexAttribArray(gl_.a_alpha_factor);
        glVertexAttribPointer(gl_.a_alpha_factor, 1, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(5 * sizeof(float)));
    }
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(v.size() / 6));
    if (gl_.a_alpha_factor != -1) {
        glDisableVertexAttribArray(gl_.a_alpha_factor);
        glVertexAttrib1f(gl_.a_alpha_factor, 1.0f);
    }
}


}  // namespace render
