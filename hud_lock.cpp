// Signal lock — the screen between "something is out there" and "there is a
// picture".
//
// The canopy panels are already live by the time this draws: telemetry alone is
// enough to reach CONNECTED, so the HUD is up and the video plane is still
// empty. That gap used to show the idle KESTREL title, which says the opposite
// of what is happening.
//
// The motion is a lock closing, not a sweep spinning. Rings contract onto the
// aircraft rather than radiating out of it - rings going outward read as this
// box transmitting, and the thing being shown is the opposite. The reticle
// walks in, the arc fills, and the whole thing snaps at the lock.
//
// One honest half and one decorative half, deliberately. The reticle has no
// idea how close a frame is; a progress bar keyed to nothing would be a lie the
// pilot could act on. The caption under it is the true part - it names the
// stage the pipeline is actually sitting in, and every one of those is
// observable. See lock_stage().

#include "osd.hpp"
#include "vec_icons.hpp"
#include "hud_theme.hpp"
#include "utils/time_util.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

// The search loops for as long as the search does. Acquisition has no known
// length - on this radio it can be two seconds or twenty - and neither does
// binding, so the figure must not reach a lock on its own. It used to: the
// pass ran to the snap every 4.2s and started over, showing a lock several
// times before anything had been found. The reticle is the decorative half of
// this screen, but decorative is not the same as untrue, and a lock means
// something specific.
//
// So: the scan loops, and the lock plays exactly once, when the caller says
// the real event happened - a keyframe decoded, or an air unit answering.
constexpr float kScanUs     = 2800000.0f;   // one narrowing pass
constexpr float kLockAnimUs =  900000.0f;   // the snap, played once

// Phase boundaries within a pass.
constexpr float kSearch = 0.26f;
constexpr float kNarrow = 0.68f;
constexpr float kLock   = 0.76f;

// Same roles as the canopy's, from the same theme - the lock screen and the
// panels are one instrument and must not disagree about what lime means.

float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
float ease(float t)    { return t * t * (3.0f - 2.0f * t); }
float easeOut(float t) { return 1.0f - powf(1.0f - t, 3.0f); }

}  // namespace

// Which stage the pipeline is actually in. Read, never animated - the caption
// is the half of this screen that tells the truth, and a stall is worth seeing:
// stuck on SEARCHING and stuck on WAITING FOR KEYFRAME are completely different
// problems, and from the front of the goggle they used to look identical.
int OSD::lock_stage(char* out, size_t out_len) {
    artosyn_stats rf;
    uint64_t hs;
    pthread_mutex_lock(&osd_mutex);
    rf = osd_vars.artosyn;
    hs = handshake_sent_us_;
    pthread_mutex_unlock(&osd_mutex);

    BfTelem bf = msp_osd.get_telem();
    const uint64_t now = get_time_us();
    const bool bf_fresh = bf.stamp_us && (now - bf.stamp_us) < 3000000ULL;

    if (rf.state != 2 && !bf_fresh) {
        snprintf(out, out_len, "SEARCHING");
        return 0;
    }
    if (rf.state == 2 && !hs) {
        if (rf.tx_freq > 0) snprintf(out, out_len, "LINK %d MHz", rf.tx_freq);
        else                snprintf(out, out_len, "LINK UP");
        return 1;
    }
    if (!bf_fresh) {
        snprintf(out, out_len, "HANDSHAKE");
        return 2;
    }
    // Telemetry is flowing and the stream has been asked for, so the only thing
    // still missing is a frame the decoder can start on.
    snprintf(out, out_len, "WAITING FOR KEYFRAME");
    return 3;
}

void OSD::draw_signal_lock(float frustum_w, float frustum_h, const float* mvp,
                           float fade, float scale, float cy_frac,
                           float text_scale, const char* head_override,
                           const char* sub_override, uint64_t* clock,
                           uint64_t locked_at) {
    (void)frustum_w;
    const HudTheme& TH = hud_theme_current();
    const float kLimeR = TH.accent[0], kLimeG = TH.accent[1], kLimeB = TH.accent[2];
    const float kCyanR = TH.data[0],   kCyanG = TH.data[1],   kCyanB = TH.data[2];
    const float kMuteR = TH.quiet[0],  kMuteG = TH.quiet[1],  kMuteB = TH.quiet[2];
    const float kWhite = TH.text[0];   // the lock draws its type in one channel
    // fade < 1 means video has arrived and the lock is bowing out over the
    // live picture. Everything drawn here is scaled by it - alphas for the
    // arcs and lines, colour for the glyph and the type, which is the only
    // fade draw_text has.
    if (fade <= 0.0f) return;

    const uint64_t now = get_time_us();
    uint64_t& since = clock ? *clock : lock_since_us_;
    if (!since) since = now;
    // Two clocks, deliberately, instead of one position along a pass.
    //
    // `ph` is free-running seconds and never wraps, so everything keyed to it
    // is seamless - there is no frame where the figure restarts. A search that
    // walked a reticle inward and then jumped it back out read as a loop, and
    // a loop reads as a fixed-length process, which acquisition and binding
    // are not: both can take two seconds or twenty.
    //
    // `lp` is the lock, 0 until the caller reports the real event and then 0->1
    // once. Everything the lock changes is interpolated by it, so the snap is a
    // transition out of the idle motion rather than a different animation.
    const float ph = (float)(now - since) / 1000000.0f;
    const float lp = locked_at
                   ? clampf((float)(now - locked_at) / kLockAnimUs, 0.0f, 1.0f)
                   : 0.0f;
    const float lpE    = ease(lp);
    const bool  locked = lp > 0.75f;

    // This animates on its own clock, so it has to ask for its own frames.
    signal_render(prof::kWakeAnim);

    glUseProgram(shader_program);
    glUniformMatrix4fv(u_mvp_, 1, GL_FALSE, mvp);

    // Everything is sized off the frame height and sits in the centre third,
    // which is the band the canopy panels deliberately leave empty.
    const float U  = frustum_h * scale;
    const float cx = 0.0f;
    const float cy = frustum_h * cy_frac;
    const float rGlyph = 0.085f * U;


    // --- rings, contracting onto the aircraft ------------------------------
    for (int i = 0; i < 4; i++) {
        float p = fmodf(ph / 2.6f + (float)i / 4.0f, 1.0f);
        float r = 0.47f * U - easeOut(p) * (0.47f * U - rGlyph);
        // In as it leaves the edge, out as it lands - a ring that vanished on
        // arrival would read as something being absorbed rather than closing.
        float a = fminf(1.0f, p * 5.0f) * (1.0f - p) * 0.65f * fade;
        draw_arc(cx, cy, r, r + 0.0035f, 0.0f, 2.0f * (float)M_PI, a,
                 kCyanR, kCyanG, kCyanB);
    }

    // --- sweep, fast at first and slowing into the lock --------------------
    {
        // The angle integrates a slowing rate rather than multiplying the
        // elapsed time by it - scaling elapsed time would teleport the sweep
        // backwards the moment the rate changed.
        static float sweep_a = 0.0f;
        static float sweep_last = 0.0f;
        float dt = ph - sweep_last; sweep_last = ph;
        if (dt < 0.0f || dt > 0.5f) dt = 0.0f;      // first frame, or a stall
        sweep_a += dt * 2.3f * (1.0f - 0.75f * lpE);
        float span  = 0.55f + 0.5f * (1.0f - lpE);
        float alpha = 0.9f * (1.0f - clampf((lp - 0.75f) / 0.25f, 0.0f, 1.0f)) * fade;
        float r = 0.37f * U;
        if (alpha > 0.001f)
            draw_arc(cx, cy, r, r + 0.005f, sweep_a, sweep_a + span, alpha,
                     kLimeR, kLimeG, kLimeB);
    }

    // --- signal arc: a ring that fills to the lock -------------------------
    {
        float r = 0.163f * U;
        draw_arc(cx, cy, r, r + 0.004f, 0.0f, 2.0f * (float)M_PI, 0.28f * fade,
                 kMuteR, kMuteG, kMuteB);
        if (lp <= 0.0f) {
            // Nothing is known about how close a frame is, so this must not
            // read as progress. A segment orbiting the ring says "working"
            // without claiming a fraction of anything.
            float a = ph * 1.6f;
            draw_arc(cx, cy, r, r + 0.005f, a, a + 1.15f, 0.9f * fade,
                     kCyanR, kCyanG, kCyanB);
        }
        float fill = lpE;
        if (fill > 0.001f) {
            // From twelve o'clock, clockwise, which is the direction a gauge is
            // read even when it is decorative.
            float a0 = -(float)M_PI * 0.5f;
            draw_arc(cx, cy, r, r + 0.005f, a0, a0 + fill * 2.0f * (float)M_PI, 0.95f * fade,
                     locked ? kLimeR : kCyanR, locked ? kLimeG : kCyanG,
                     locked ? kLimeB : kCyanB);
        }
    }

    // --- reticle: four brackets walking in, then snapping ------------------
    {
        const float far = 0.33f * U, near = 0.112f * U;
        // Breathing, not walking: a cosine has no seam, so the brackets never
        // jump back out to start again. They hunt around the aircraft until
        // there is something to close on.
        const float mid = (far + near) * 0.5f, amp = (far - near) * 0.5f;
        const float d_search = mid + amp * cosf(ph * (2.0f * (float)M_PI / 3.4f));
        const float d = d_search + (near - d_search) * lpE;
        const float br = locked ? kLimeR : kWhite;
        const float bg = locked ? kLimeG : kWhite;
        const float bb = locked ? kLimeB : kWhite;
        // Plain corner lines, drawn as two bars meeting in a square mitre.
        //
        // These were glow polylines with an arm length fixed to U and a stroke
        // width that was not, so at a third scale the arms were barely longer
        // than the stroke and the four corners read as blobs. Two things fix
        // it: the arm is now a fraction of the bracket's own radius, so the
        // corner keeps its proportions at any size, and the stroke scales with
        // the figure but never below a floor that would drop under a pixel.
        const float arm = d * 0.30f;
        const float th  = fmaxf(0.0060f * U, 0.0030f) * osd_vars.ui_scale;
        const float h   = th * 0.5f;
        const float a   = 0.95f * fade;
        static const float q[4][2] = { {-1,-1}, {1,-1}, {-1,1}, {1,1} };
        for (int i = 0; i < 4; i++) {
            float qx = q[i][0], qy = q[i][1];
            float px = cx + qx * d, py = cy + qy * d;
            // Each bar starts half a stroke OUTSIDE the corner so the two meet
            // flush and the angle is square rather than notched.
            float ox = px + qx * h, oy = py + qy * h;
            // horizontal arm, running inward
            draw_poly({{ox,             oy - h},
                       {ox - qx * arm,  oy - h},
                       {ox - qx * arm,  oy + h},
                       {ox,             oy + h}}, a, br, bg, bb);
            // vertical arm, running inward
            draw_poly({{ox - h, oy},
                       {ox - h, oy - qy * arm},
                       {ox + h, oy - qy * arm},
                       {ox + h, oy}}, a, br, bg, bb);
        }
    }

    // --- the aircraft, brightening as the lock lands -----------------------
    {
        int px = (int)lroundf((rGlyph * 2.0f) / (frustum_h * 2.0f) *
                              (float)(dev && dev->output_list ? dev->output_list->mode.vdisplay : 1080));
        float asp = 1.0f;
        GLuint tex = vec_icon_texture(VecIcon::Drone, px, &asp);
        if (tex) {
            float h = rGlyph * 2.0f, w = h * asp;
            float lit = (0.55f + 0.45f * lpE) * fade;
            draw_icon(tex, cx - w * 0.5f, cy - h * 0.5f, w, h, lit, lit, lit);
        }
    }

    // --- the flash at the moment of lock -----------------------------------
    {
        // The last quarter of the lock, so the flash lands on the snap.
        float fp = clampf((lp - 0.75f) / 0.25f, 0.0f, 1.0f);
        if (lp > 0.75f && fp < 1.0f) {
            float r = rGlyph + fp * 0.19f * U;
            draw_arc(cx, cy, r, r + 0.008f, 0.0f, 2.0f * (float)M_PI,
                     (1.0f - fp) * 0.8f * fade, kWhite, kWhite, kWhite);
        }
    }

    // --- captions ----------------------------------------------------------
    {
        glUniformMatrix4fv(u_mvp_, 1, GL_FALSE, mvp);
        const float s = osd_vars.ui_scale;
        const float title = 0.088f * s * text_scale, sub = 0.050f * s * text_scale;
        const char* head = head_override ? head_override : "ACQUIRING VIDEO";
        // Clear of the reticle at its widest: the brackets reach 0.33*U from
        // the centre and their arms run 0.031 past that, so anything nearer
        // than 0.40 gets walked through on every pass.
        float ty = cy - fmaxf(0.40f * U, 0.34f * s * text_scale);
        draw_text(head, -text_width(head, title) * 0.5f, ty, title, false,
                  kWhite * fade, kWhite * fade, kWhite * fade);

        char stage[48];
        int idx = 0;
        if (sub_override) snprintf(stage, sizeof(stage), "%s", sub_override);
        else              idx = lock_stage(stage, sizeof(stage));
        // White, not the theme's `quiet` grey. This line is an instruction
        // being followed, or the pipeline stage being watched - in both cases
        // it is read, and it is read over live video of unknown brightness.
        // Grey is for words that qualify a number, which this is not.
        float sr = (locked ? kLimeR : kWhite) * fade;
        float sg = (locked ? kLimeG : kWhite) * fade;
        float sb = (locked ? kLimeB : kWhite) * fade;
        // Stage 3 is the last one this screen can show - past it there is a
        // picture, and the screen is gone.
        (void)idx;
        draw_text(stage, -text_width(stage, sub) * 0.5f,
                  ty - fmaxf(0.075f * U, 0.105f * s * text_scale), sub, false,
                  sr, sg, sb);
    }
}
