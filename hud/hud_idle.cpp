// The idle screen and what stands in the HUD's place without a picture: the
// background (or the warp into the picture), the black and the lock while
// acquiring, and the wordmark.
#include "hud/osd_detail.hpp"

void OSD::draw_backdrop(Frame& f) {
    auto& mvp_c = f.mvp_c;
    auto& frustum_h = f.frustum_h;
    auto& frustum_w = f.frustum_w;
    auto& is_menu_open = f.is_menu_open;
    auto& vs = f.vs;
    auto& trans_t = f.trans_t;
    auto& seen_this_link = f.seen_this_link;
    auto& acquiring = f.acquiring;
    auto& holding = f.holding;
    auto& lock_screen = f.lock_screen;
    auto& lock_fade = f.lock_fade;
    // Draw background or transition
    canvas_->set_transform(mvp_c);
    // Acquiring: there is an aircraft, and no picture from it yet. Deliberately
    // not gated on VideoState - that is one-way (BACKGROUND -> TRANSITION ->
    // LIVE) and never returns, so keying off it meant the lock played on the
    // first acquisition of a session and never again. A link that drops and
    // comes back is acquiring video just as much as one that never had it.
    //
    // Reaching LIVE with stale frames is exactly the reacquire case: the video
    // plane still holds the frozen last frame, and painting over it is right -
    // that picture is history, and leaving it up while the radio hunts for a
    // new one is the same lie as a panel at full brightness over a dead link.
    //
    // The lock screen then owns the whole frame. The background plate is
    // scenery for an idle goggle; behind an acquisition it is just busy ground
    // for thin rings and hairline brackets to lose themselves in.
    //
    // "Acquiring" is not the same as "no picture". On the way OUT of a link the
    // video stops seconds before the HUD concedes the aircraft is gone - frames
    // go stale after 1.5s, the three evidence sources need 3s, and the drop
    // hold adds 1.5s more - so a rule of "connected and no video" would raise a
    // lock screen on every dropout, on the way to IDLE_2. What separates the
    // two is whether a frame has ever arrived from THIS connection.
    seen_this_link = frames_since_connect_ >= kFramesForVideo;
    // Acquiring: connected, and no video worth the name yet.
    //
    // Deliberately NOT gated on video_active. That goes true on a single frame
    // and stays true for 1.5s after it, so one straggler arriving ahead of the
    // real stream was enough to drop the black ground - leaving the animation
    // playing over the frozen last frame, then that one stale picture, and only
    // then the black it should have had from the start.
    //
    // seen_this_link is the honest test and the one the rest of this already
    // uses: ten frames from THIS connection. It also behaves correctly the
    // other way round - once a connection has produced real video the black
    // never returns, so a mid-flight stall dims the panels rather than blacking
    // out a picture that is merely late.
    // Not while the menu is up. The lock screen owns the centre of the frame
    // and so does the menu; drawn together the reticle runs straight through
    // the rows, which the first capture of the two at once showed plainly.
    // The menu is the one the pilot is looking at, so it wins.
    // Gated on vs==LIVE, not just "not TRANSITION": vs is one-way and
    // BACKGROUND only until the very first connection's first keyframe ever
    // decodes, so hud_connected_ can go true well before that (telemetry
    // proves an aircraft while the decoder is still hunting for a clean
    // frame). That gap used to raise the black panel and reticle before
    // there was anything to acquire yet; now the plain background loop
    // stays up through it, same as before there was ever an aircraft, and
    // the warp takes over the instant
    // video actually arrives, unfought (974c86e turned the warp off instead
    // of fixing this fight; fixed here instead). Every later reconnect
    // finds vs already at LIVE, since it never reverts, so acquiring still
    // plays on every one of those - only the very first connection skips it.
    acquiring = hud_connected_ && !seen_this_link && !is_menu_open &&
                            vs == VideoState::LIVE;
    // ...and once video does arrive, the screen still owes the eye a moment.
    // It fades out over the live picture rather than being cut mid-animation,
    // so nothing is ever hidden to buy the time.
    const uint64_t lk_now = get_time_us();
    holding = hud_connected_ && !acquiring && lk_now < lock_hold_until_us_ &&
                          vs == VideoState::LIVE;
    lock_screen = acquiring || holding;
    lock_fade =
        acquiring ? 1.0f
                  : (lock_hold_until_us_ > lk_now
                         ? fminf(1.0f, (float)(lock_hold_until_us_ - lk_now) / (float)kLockFadeUs)
                         : 0.0f);
    if (acquiring) {
        // Opaque black, drawn rather than left clear: the OSD plane is blended
        // over the video plane at scanout, and leaving it transparent shows
        // whatever that plane last held. Only while acquiring - once there is a
        // picture the fading lock rides over it, because holding black to buy
        // the animation time would hide video we already have.
        canvas_->panel(-frustum_w, -frustum_h, frustum_w * 2.0f, frustum_h * 2.0f,
                   1.0f, 0.0f, 0.0f, 0.0f);
    } else if (!lock_screen && vs == VideoState::BACKGROUND) {
        if (bg_video_enabled && bg_have_frame_) {
            draw_bg_video(frustum_w, frustum_h, mvp_c, 0.0f);
            // No signal_render() here — on_new_frame callback drives wakeups
        } else {
            draw_bg_png(frustum_w, frustum_h, mvp_c);
        }
    } else if (vs == VideoState::TRANSITION) {
        if (bg_video_enabled && bg_have_frame_) {
            draw_bg_video(frustum_w, frustum_h, mvp_c, trans_t);
        } else {
            draw_bg_png(frustum_w, frustum_h, mvp_c, trans_t);
        }
        signal_render(prof::kWakeAnim); // keep redrawing to animate the warp
    }
    // VideoState::LIVE draws nothing here. The FPV frame is on its own DRM
    // plane, which the display controller composites with this one at scanout;
    // a screen recording rebuilds the same from both with RGA (screen_tap.hpp).
    //
    // This used to call draw_live_video() when dvr_screen was set, because
    // glReadPixels could not see the video plane and the capture needed a copy
    // of the video inside the GL layer. The side effect was that the video was
    // drawn TWICE - once by the hardware plane, once here on top - which made
    // the picture visibly brighter and the OSD grid show through as soon as the
    // Record Screen toggle was switched on.

    // --- IDLE TITLE: "KESTREL" ---
    // BACKGROUND: plain draw with shadow (main shader).
    // TRANSITION: stretched out from the sides on a quad of its own, fading
    //             with the same curve as the background's warp-zoom, so the
    //             letters visibly stretch away as the picture comes in.
    // The idle title and the lock screen are the same slot in two different
    // states. KESTREL means "nothing to fly"; once there is an aircraft it says
    // the opposite of what is happening, and the lock takes the slot instead.
    // Only BACKGROUND - the transition is already the picture arriving, and
    // drawing a lock over it would be acquiring something we have.
    if (lock_screen) {
        // The lock plays when video actually arrives, not on a timer. That is
        // precisely the acquiring -> holding edge: `holding` only becomes true
        // once a frame from THIS connection has been decoded.
        if (holding && !lock_locked_us_)  lock_locked_us_ = get_time_us();
        if (acquiring)                    lock_locked_us_ = 0;
        draw_signal_lock(frustum_w, frustum_h, mvp_c, lock_fade,
                         1.0f, 0.09f, 1.0f, nullptr, nullptr, nullptr,
                         lock_locked_us_);
    } else {
        lock_locked_us_ = 0;
        // Restart the sequence next time rather than resuming mid-lock: a
        // second aircraft should get a whole lock, not the tail of the last one.
        lock_since_us_ = 0;
    }
}

void OSD::draw_idle_title(Frame& f) {
    auto& mvp_c = f.mvp_c;
    auto& mvp_hud_c = f.mvp_hud_c;
    auto& frustum_h = f.frustum_h;
    auto& frustum_w = f.frustum_w;
    auto& vs = f.vs;
    auto& trans_t = f.trans_t;
    auto& lock_screen = f.lock_screen;
    // It stays with the canopy awake: a blade unfolding up its side moves the
    // wordmark into the part of the picture it leaves free, and back as it
    // folds (draw_canopy_ui: an unfolded blade is 0.34 of the width).
    // "Ready" sits where the channel band unfolds, so it goes while that is up.
    if (idle_title_tex_ && !lock_screen) {
        const HudTheme& TT = hud_theme_current();
        auto ease = [](float t) { return t * t * (3.0f - 2.0f * t); };
        const float open_l = ease(ui_open_[0]), open_r = ease(ui_open_[1]);
        const float ready_a = 1.0f - ease(ui_band_);
        const float kPanel = 0.34f;
        const float title_dx = (open_l - open_r) * kPanel * frustum_w;
        float base_h = frustum_h * 0.663f;
        float base_w = base_h * (float)idle_title_w_ / (float)idle_title_h_;
        {
            // The ink is half the texture's width (render_idle_title); fit it
            // to 80% of what the panel leaves.
            const float fit = std::min(1.0f, 0.8f * (1.0f - kPanel) * 2.0f * frustum_w / (base_w * 0.5f));
            const float s = 1.0f + (fit - 1.0f) * std::max(open_l, open_r);
            base_h *= s;
            base_w *= s;
        }
        float cx = title_dx - base_w * 0.5f;
        float cy = -base_h * 0.5f + frustum_h * 0.08f; // slightly above centre

        if (vs == VideoState::BACKGROUND) {
            canvas_->set_transform(mvp_c);

            // Softer than the old title's: the wordmark is set larger, so the
            // same fraction of its height threw a slab rather than a shadow.
            float sh = base_h * 0.018f;
            struct { float dx, dy, r, g, b; } shadow_passes[] = {
                { sh * 2.0f, -sh * 2.0f, 0.00f, 0.00f, 0.05f },
                { sh,        -sh,        0.00f, 0.00f, 0.03f },
            };
            // The shadow is cast by the word, not by either half of it, so
            // both layers drop it before either is inked.
            for (auto& p : shadow_passes) {
                canvas_->set_fade(1.0f);
                canvas_->icon(idle_title_tex_,    cx + p.dx, cy + p.dy, base_w, base_h, p.r, p.g, p.b);
                canvas_->icon(idle_title_os_tex_, cx + p.dx, cy + p.dy, base_w, base_h, p.r, p.g, p.b);
            }
            canvas_->set_fade(1.0f);
            canvas_->icon(idle_title_tex_,    cx, cy, base_w, base_h,
                      TT.text[0],   TT.text[1],   TT.text[2]);
            canvas_->icon(idle_title_os_tex_, cx, cy, base_w, base_h,
                      TT.accent[0], TT.accent[1], TT.accent[2]);

            // "Ready" in the theme's accent, sitting where the boot splash's
            // progress bar does (splash.c: 85% down the frame) - so the
            // splash's loading bar and this label read as the same spot
            // finishing its job, not two unrelated elements.
            if (ready_a > 0.003f) {
                float ready_scale    = 0.18f * osd_vars.ui_scale;   // 3x - the first size read too small
                float ready_y_centre = -frustum_h * 0.7f;
                float ready_w        = canvas_->text_width("Ready", ready_scale);
                float ready_x        = title_dx - ready_w * 0.5f;
                float ready_y        = ready_y_centre - ready_scale * 0.5f;

                // Same soft two-pass shadow as the wordmark above, scaled to
                // this label's own (much smaller) size - without it "Ready"
                // washed out over the brighter parts of the photo. Lighter
                // than the wordmark's: at a 6% offset and full opacity it
                // read as a second, dark "Ready" rather than a shadow, so the
                // offset is shorter and both passes are part-transparent.
                float rsh = ready_scale * 0.035f;
                struct { float dx, dy, r, g, b, a; } ready_shadow[] = {
                    { rsh * 2.0f, -rsh * 2.0f, 0.00f, 0.00f, 0.05f, 0.35f },
                    { rsh,        -rsh,        0.00f, 0.00f, 0.03f, 0.60f },
                };
                for (auto& p : ready_shadow) {
                    canvas_->set_fade(p.a * ready_a);
                    canvas_->text("Ready", ready_x + p.dx, ready_y + p.dy, ready_scale, false, p.r, p.g, p.b);
                }
                canvas_->set_fade(ready_a);
                canvas_->text("Ready", ready_x, ready_y, ready_scale, false,
                          TT.accent[0], TT.accent[1], TT.accent[2]);
                canvas_->set_fade(1.0f);
            }

        } else if (vs == VideoState::TRANSITION) {
            // "Stretched from sides" effect:
            //   - Quad is pinned to full screen width (no geometry expansion).
            //   - UV range in X shrinks toward the centre over time, so the same
            //     texture content is spread across a wider screen area → letters
            //     appear to be pulled outward from both sides.
            //   - At t=0 the UV exactly matches what BACKGROUND draws, so there
            //     is no visual pop at the moment the transition starts.
            //   - Height grows slowly so letters become taller too.
            //
            // u_half: at t=0 = frustum_w / base_w (0.335 at 16:9 with no blade
            //         unfolded: the same UV slice BACKGROUND shows via its large
            //         overflowing quad), centred where BACKGROUND has the word
            //         (title_dx). Shrinks as 1/(1+8t²) so letters are ≈2× wider
            //         at t=0.4, ≈5× at t=0.7.
            float t2 = trans_t * trans_t;
            float title_alpha = std::max(0.0f, 1.0f - t2 * 1.35f);

            float u_half = (frustum_w / base_w) / (1.0f + t2 * 8.0f);
            float u_mid  = 0.5f - title_dx / base_w;
            float u0 = u_mid - u_half;
            float u1 = u_mid + u_half;

            // Full-screen-width quad; height grows
            float tw = frustum_w * 2.0f;
            float th = base_h * (1.0f + t2 * 2.0f);
            float qx = -frustum_w;
            float qy = -th * 0.5f + frustum_h * 0.08f;

            float verts[] = {
                qx,      qy,      0.0f,  u0, 1.0f,
                qx + tw, qy,      0.0f,  u1, 1.0f,
                qx,      qy + th, 0.0f,  u0, 0.0f,
                qx + tw, qy + th, 0.0f,  u1, 0.0f,
            };

            canvas_->set_transform(mvp_hud_c);
            canvas_->set_fade(title_alpha);
            // Both halves ride the same quad and the same stretch, so the word
            // comes apart as one thing - only their colours differ.
            const render::ImageId parts[2] = { idle_title_tex_, idle_title_os_tex_ };
            const float* cols[2]  = { TT.text, TT.accent };
            for (int k = 0; k < 2; k++)
                if (parts[k]) canvas_->image_quad(parts[k], verts, cols[k][0], cols[k][1], cols[k][2], 1.0f);
            canvas_->set_fade(1.0f);
        }
    }
}
