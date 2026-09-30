extern "C"{
    #include <sys/ioctl.h>
}
#include <bitset>
#include <iostream>
#include <iomanip>

#include "renderer.hpp"
#include "settings.hpp"
#include <cmath>
#include <time.h>
#include "utils/ltrace.hpp"
#include "dvr.hpp"
#include "webstream.hpp"



void Renderer::init_buffers(DecodedUnit* du) {
    display_buffers.resize((render_mode==FrontBuffer) ? 1 : MAX_DISPLAY_BUFFERS);
	buffer_initialized = true;
}

void Renderer::copy_sw_frame(DisplayBufferInfo& buf_info, DecodedUnit* du) {
    if (!du || !buf_info.sw_base ) {
        fprintf(stderr, "copy_sw_frame: Invalid input or unmapped buffer.\n");
        return;
    }
    
    // Map source if needed (and possible)
    uint8_t* src_y = du->data[0];
    uint8_t* src_uv = du->data[1];
    int src_stride_y = du->linesize[0];
    int src_stride_uv = du->linesize[1];
    void* mapped_ptr = nullptr;
    size_t mapped_size = 0;

    if (!src_y && du->has_prime_fd) {
        // We need to map the IMPORTED frame to read from it.
        // NOTE: This usually requires the buffer to be created as mappable (dumb buffer or specific heap).
        // If it's a pure hardware secured buffer, this will fail.
        struct drm_mode_map_dumb mreq = {};
        // We can't map a prime_fd directly easily without importing it to a GEM handle first.
        // Luckily, we likely already imported it or can do so.
        // But DecodedUnit just has prime_fd.
        
        // Quick/Dirty: Import to handle, map, read, close.
        uint32_t handle = 0;
        if (drmPrimeFDToHandle(dev->drm_fd, du->prime_fd, &handle) == 0) {
           mreq.handle = handle;
           if (drmIoctl(dev->drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) == 0) {
               // Approximate size for NV12
               mapped_size = du->width * du->height * 2; 
               mapped_ptr = mmap(0, mapped_size, PROT_READ, MAP_SHARED, dev->drm_fd, mreq.offset);
               if (mapped_ptr != MAP_FAILED) {
                   src_y = (uint8_t*)mapped_ptr;
                   // Mpp/NV12 usually contiguous: Y...UV
                   // Stride? Assume aligned to width or passed keys
                   src_stride_y = du->pitches[0] ? du->pitches[0] : du->width;
                   src_stride_uv = du->pitches[1] ? du->pitches[1] : du->width; 
                   
                   // Determine offsets
                   // If offsets are 0, assume standard packed
                   uint32_t off_y = du->offsets[0];
                   uint32_t off_uv = du->offsets[1];
                   if (off_uv == 0) off_uv = src_stride_y * du->height;
                   
                   src_uv = src_y + (off_uv - off_y);
               }
           }
        }
        // Don't close handle yet if mmap needs it? DRM handles can be closed after mmap technically, 
        // but let's keep it simple and leaky for this transient scope if needed, or close at end.
        // Actually, closing handle doesn't unmap.
        if (handle) drmCloseBufferHandle(dev->drm_fd, handle);
    }
    
    if (!src_y) {
         // Still null?
         return;
    }

    int width = du->width;
    int height = du->height;

    // Copy Y plane
    for (int i = 0; i < height; ++i) {
        memcpy(buf_info.sw_base + i * buf_info.stride,
               src_y + i * src_stride_y,
               width);
    }

    // Copy UV interleaved plane (NV12)
    uint8_t* uv_dst = buf_info.sw_base + buf_info.sw_uv_offset;
    for (int i = 0; i < height / 2; ++i) {
        memcpy(uv_dst + i * buf_info.stride,
               src_uv + i * src_stride_uv,
               width);
    }
    
    if (mapped_ptr) {
        munmap(mapped_ptr, mapped_size);
    }
}

int Renderer::get_or_create_display_buffer(DecodedUnit* du, DisplayBufferInfo& out_info) {
    if (!du) {
        fprintf(stderr, "Null frame passed to get_or_create_display_buffer.\n");
        return -1;
    }

    if (render_mode == FrontBuffer) {
        int slot_idx = 0;
        if(display_buffers[slot_idx].drm_fb_id != 0) {
            out_info = display_buffers[slot_idx];
            return slot_idx;
        }
        // Fallthrough to create SW buffer at index 0
        // We trick the subsequent logic by temporarily clearing has_prime_fd if it's set
        // But we can't modify 'du' destructively safely.
        // Instead, we just jump to Case 2 logic.
        goto create_sw_buffer;
    }

    // -------------------------
    // Case 1: Hardware buffer (with prime_fd)
    // -------------------------
    if (du->has_prime_fd) {
        // Find available slot (Match prime_fd or reuse oldest)
        int slot_idx = -1;
        for (int i = 0; i < display_buffers.size(); ++i) {
            if (display_buffers[i].prime_fd == du->prime_fd && display_buffers[i].drm_fb_id != 0) {
                out_info = display_buffers[i];
                return i;
            }
        }

        // No match? Use FIFO reuse
        slot_idx = next_display_buffer_idx;
        next_display_buffer_idx = (next_display_buffer_idx + 1) % display_buffers.size();

        // Clean up previous
        if (display_buffers[slot_idx].drm_fb_id != 0) {
            cleanup_display_buffer(display_buffers[slot_idx]);
        }

        DisplayBufferInfo new_buf_info;
        new_buf_info.prime_fd = du->prime_fd;

        if (drmPrimeFDToHandle(dev->drm_fd, du->prime_fd, &new_buf_info.gem_handle)) {
            perror("Failed to import prime FD to GEM handle");
            return -1;
        }

        du->handles[0] = new_buf_info.gem_handle;
        du->handles[1] = new_buf_info.gem_handle;
        du->handles[2] = 0;
        du->handles[3] = 0;

        if (drmModeAddFB2WithModifiers(dev->drm_fd, du->width, du->height,
                    du->drm_pixel_format,
                    du->handles, du->pitches, du->offsets,
                    du->modifiers,
                    &new_buf_info.drm_fb_id, DRM_MODE_FB_MODIFIERS)) {
            perror("drmModeAddFB2WithModifiers failed");
            drmCloseBufferHandle(dev->drm_fd, new_buf_info.gem_handle);
            return -1;
        }

        display_buffers[slot_idx] = new_buf_info;
        out_info = new_buf_info;
        return slot_idx;
    }

    // -------------------------
    // Case 2: Software-rendered buffer (no prime_fd) OR FrontBuffer
    // -------------------------
    create_sw_buffer:

    int slot_idx = -1;
    for (int i = 0; i < display_buffers.size(); ++i) {
        // todo for live copy just skip the in_use check
        if (display_buffers[i].drm_fb_id == 0 || !display_buffers[i].in_use) {
            slot_idx = i;
            break;
        }
    }

    if (slot_idx == -1) {
        fprintf(stderr, "No available SW display buffer slots.\n");
        exit(1);
        return -1;
    }

    if(display_buffers[slot_idx].drm_fb_id != 0) {
        // buffer has already been created, reuse.
        out_info = display_buffers[slot_idx];
        return slot_idx;
    }

    // Determine DRM format based on input frame pixel format
    // Force NV12 because the hardware (N100/i915) seemingly rejected DRM_FORMAT_YUV420 for AddFB2.
    // We will perform the conversion (interleaving) during copy if needed.
    uint32_t drm_fmt = DRM_FORMAT_NV12;
    // if (du->drm_pixel_format == DRM_FORMAT_YUV420) {
    //    drm_fmt = DRM_FORMAT_YUV420;
    // }

    DrmBuffer out;
    if (!create_drm_buffer(du->width, du->height, drm_fmt, out)) {
        fprintf(stderr, "Failed to create SW DRM buffer (fmt=0x%x).\n", drm_fmt);
        return -1;
    }

    // Map dumb buffer to memory
    struct drm_mode_map_dumb mreq = {};
    mreq.handle = out.handle;

    if (drmIoctl(dev->drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq)) {
        perror("DRM_IOCTL_MODE_MAP_DUMB failed");
        return -1;
    }

    uint8_t* base = (uint8_t*)mmap(0, out.size, PROT_READ | PROT_WRITE, MAP_SHARED, dev->drm_fd, mreq.offset);
    if (base == MAP_FAILED) {
        perror("mmap failed");
        return -1;
    }

    // Fill DisplayBufferInfo
    DisplayBufferInfo sw_buf;
    sw_buf.gem_handle = out.handle;
    sw_buf.drm_fb_id = out.fb_id;
    sw_buf.width = du->width;
    sw_buf.height = du->height;
    sw_buf.stride = out.stride; // driver-aligned dumb-buffer pitch (matches the FB)
    sw_buf.sw_base = base;

    // Calculate offsets based on format (in terms of the aligned pitch, so the
    // copy lands exactly where drmModeAddFB2 expects each plane).
    if (drm_fmt == DRM_FORMAT_YUV420) {
        // YUV420P: Y plane, then U plane, then V plane.
        sw_buf.sw_u_offset = out.stride * du->height;
        sw_buf.sw_v_offset = sw_buf.sw_u_offset + (out.stride / 2 * du->height / 2);
    } else {
         // NV12: Y plane, then interleaved UV plane
         sw_buf.sw_uv_offset = out.stride * du->height;
    }
    
    sw_buf.prime_fd = 0;
    sw_buf.format = drm_fmt;

    display_buffers[slot_idx] = sw_buf;
    out_info = sw_buf;
    return slot_idx;
}

bool Renderer::create_drm_buffer(uint32_t width, uint32_t height, uint32_t format, DrmBuffer& out, bool export_fd) {
    struct drm_mode_create_dumb creq = {};
    creq.width = width;
    creq.height = height; 
    
    // Calculate total bpp / height multiplier for dumb buffer size logic
    // NV12 and YUV420 = 12bpp effectively -> height * 1.5
    creq.height = height * 3 / 2; 
    creq.bpp = 8;

    if (drmIoctl(dev->drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) {
        perror("DRM_IOCTL_MODE_CREATE_DUMB failed");
        return false;
    }

    out.handle = creq.handle;

    if (export_fd) {
        struct drm_prime_handle dph = {};
        dph.handle = creq.handle;
        if (drmIoctl(dev->drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &dph) < 0) {
            perror("DRM_IOCTL_PRIME_HANDLE_TO_FD failed");
            return false;
        }
        out.prime_fd = dph.fd;
    }

    // Use the driver-aligned pitch the kernel actually allocated, not `width`.
    // amdgpu rounds the dumb-buffer pitch up to its hardware alignment (e.g. 256
    // bytes); declaring an unaligned pitch=width to drmModeAddFB2 gets the FB
    // rejected with EINVAL. The luma/chroma offsets and the copy stride must all
    // follow creq.pitch too, which is why we hand it back via out.stride.
    uint32_t pitch = creq.pitch;
    out.stride = pitch;

    uint32_t handles[4] = {}, pitches[4] = {}, offsets[4] = {};
    handles[0] = creq.handle;
    pitches[0] = pitch;
    offsets[0] = 0;

    if (format == DRM_FORMAT_YUV420) {
        // Y
        handles[1] = creq.handle;
        pitches[1] = pitch / 2;
        offsets[1] = pitch * height;

        // U (or V depending on exact fourcc, YUV420 usually I420 = Y, U, V)
        handles[2] = creq.handle;
        pitches[2] = pitch / 2;
        offsets[2] = offsets[1] + (pitch / 2 * height / 2);
    } else {
        // NV12
        handles[1] = creq.handle;
        pitches[1] = pitch;
        offsets[1] = pitch * height;
    }
    
    if (drmModeAddFB2(dev->drm_fd, width, height, format, handles, pitches, offsets, &out.fb_id, 0) < 0) {
        perror("drmModeAddFB2 failed");
        struct drm_mode_destroy_dumb dreq = { .handle = creq.handle };
        drmIoctl(dev->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
        return false;
    }

    out.size = creq.size;

    return true;
}

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#endif

void Renderer::convert_yuv420_to_nv12(DisplayBufferInfo& buf_info, DecodedUnit* du) {
    if (!du || !buf_info.sw_base) return;

    int width = du->width;
    int height = du->height;
    
    // Destination (Mapped DRM Buffer, NV12)
    uint8_t* y_dst = buf_info.sw_base;
    uint8_t* uv_dst = buf_info.sw_base + buf_info.sw_uv_offset;

    // Source (Decoded Frame, YUV420P)
    uint8_t* y_src = du->data[0];
    uint8_t* u_src = du->data[1];
    uint8_t* v_src = du->data[2];

    int y_stride_src = du->linesize[0];
    int u_stride_src = du->linesize[1];
    int v_stride_src = du->linesize[2];
    int stride_dst = buf_info.stride;

    // 1. Copy Y Plane (memcpy is usually SIMD-optimized by libc)
    if (y_stride_src == stride_dst) {
         memcpy(y_dst, y_src, width * height);
    } else {
        for (int i = 0; i < height; ++i) {
            memcpy(y_dst + i * stride_dst, y_src + i * y_stride_src, width);
        }
    }

    // 2. Interleave U/V into NV12 UV Plane using SSE2 intrinsics
    // NV12 UV layout: U0 V0 U1 V1 ...
    int uv_width = width / 2;
    int uv_height = height / 2;
    
    for (int y = 0; y < uv_height; ++y) {
        uint8_t* u_row = u_src + y * u_stride_src;
        uint8_t* v_row = v_src + y * v_stride_src;
        uint8_t* dest_row = uv_dst + y * stride_dst;
        
        int x = 0;
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
        // Process 16 UV pixels (consume 16 U bytes + 16 V bytes -> produce 32 NV12 bytes) per iter
        for (; x <= uv_width - 16; x += 16) {
            __m128i u_val = _mm_loadu_si128((const __m128i*)(u_row + x));
            __m128i v_val = _mm_loadu_si128((const __m128i*)(v_row + x));
            
            // Interleave bytes: 
            // lo: u0 v0 u1 v1 ... u7 v7
            // hi: u8 v8 u9 v9 ... u15 v15
            __m128i lo = _mm_unpacklo_epi8(u_val, v_val);
            __m128i hi = _mm_unpackhi_epi8(u_val, v_val);
            
            _mm_storeu_si128((__m128i*)(dest_row + 2 * x), lo);
            _mm_storeu_si128((__m128i*)(dest_row + 2 * x + 16), hi);
        }
#endif

        // Handle remaining pixels scalar
        for (; x < uv_width; ++x) {
            dest_row[2*x] = u_row[x];
            dest_row[2*x+1] = v_row[x];
        }
    }
}


void Renderer::cleanup_display_buffer(DisplayBufferInfo& buffer_info) {
    if (buffer_info.drm_fb_id != 0) {
        drmModeRmFB(dev->drm_fd, buffer_info.drm_fb_id);
        buffer_info.drm_fb_id = 0;
    }
    if (buffer_info.gem_handle != 0) {
        // struct drm_gem_close args = { .handle = buffer_info.gem_handle };
        // drmIoctl(dev->drm_fd, DRM_IOCTL_GEM_CLOSE, &args); // Close the GEM handle we created from import
        // More simply with libdrm >= 2.4.59:
        drmCloseBufferHandle(dev->drm_fd, buffer_info.gem_handle);
        buffer_info.gem_handle = 0;
    }
    // The original prime_fd (buffer_info.prime_fd) is owned by the decoder's AVFrame.
    // We don't close it directly unless we dup'd it.
    // It will be released when buffer_info.av_frame_ref is unref'd.
    // if (buffer_info.av_frame_ref) {
    //     av_frame_unref(buffer_info.av_frame_ref);
    //     buffer_info.av_frame_ref = nullptr;
    // }
    buffer_info.prime_fd = -1;
}

void Renderer::queue_frame(std::shared_ptr<DecodedUnit> du) {
    decoded_unit_queue->put(DataMsg<std::shared_ptr<DecodedUnit>>(du->pts, du));
}

void Renderer::run(){
    SchedulingHelper::configure_thread(SchedulingHelper::ThreadRole::Renderer);
    
    std::shared_ptr<DecodedUnit> latest_frame = nullptr;

    // Deadline commit. A picture is not committed the moment it is decoded but
    // just before the next vblank, with whatever is newest by then. The flip
    // then lands at that very vblank, so it is never still pending when the
    // next picture arrives. Committing at once instead let a picture decoded
    // while a flip was pending go out just after that flip's vblank, pending a
    // whole refresh and blocking the next picture the same way: at 120 Hz that
    // put the median flip-to-vblank wait at 6 ms, and at 100 Hz output with
    // 100 fps video it chained on every picture. Photodiode, 3 pairs each:
    // median 46.7 -> 44.4 ms, p90 68.2 -> 61.5 ms.
    //
    // The margin is how long before the vblank the commit is made. On this
    // VOP2 a commit takes 0.2 to over 0.8 ms to be latched: at 0.8 ms before
    // the vblank 23% missed it and went out a refresh later, at 1.2 ms 1%. A
    // picture less than commit_min_slack_ms from a vblank waits for the next.
    const bool deadline_commit = Settings::getInstance().getInt("deadline_commit", 1) != 0;
    const double commit_margin_us = Settings::getInstance().getFloat("commit_margin_ms", 1.2f) * 1000.0;
    const double commit_min_slack_us = Settings::getInstance().getFloat("commit_min_slack_ms", 0.6f) * 1000.0;
    if (deadline_commit && render_mode == Atomic)
        printf("renderer: committing %.1f ms before each vblank\n", commit_margin_us / 1000);

    while(!*should_stop) {
        // Handle resolution-change flush request from the decoder thread.
        // All display_buffers access happens here in the renderer thread — no mutex needed.
        if (flush_requested.load()) {
            if (dev->is_flip_pending()) dev->wait_for_flip_completion(50);
            // Drain any queued frames with stale prime_fds
            while (decoded_unit_queue->size() > 0) decoded_unit_queue->tryGet();
            latest_frame = nullptr;
            // Clean up all cached display buffer entries (gem handles + DRM FBs)
            for (auto& buf : display_buffers) {
                if (buf.drm_fb_id != 0) cleanup_display_buffer(buf);
            }
            buffer_initialized = false;
            next_display_buffer_idx = 0;
            flush_requested.store(false);
            flush_done.store(true);
            continue;
        }

        uint64_t now = get_time_us();

        // 1. DRAIN QUEUE: Find the most recent decoded frame
        bool got_new = false;
        while (decoded_unit_queue->size() > 0) {
            auto m = decoded_unit_queue->tryGet();
            if (m) {
                // static_cast, not dynamic_cast: decoded_unit_queue has exactly one
                // producer (put() below) and it only ever pushes this type. The
                // reference-form dynamic_cast threw std::bad_cast on a mismatch, which
                // in this thread is a terminate() rather than a dropped frame - and it
                // cost an RTTI lookup on every frame.
                auto& dm = static_cast<DataMsg<std::shared_ptr<DecodedUnit>>&>(*m);
                if (latest_frame) {
                    update_stats(latest_frame.get(), 0); // Mark previous as skipped
                }
                latest_frame = dm.getPayload();
                got_new = true;
            }
        }

        // 2. Statistics Heartbeat (Update even if no frames)
        update_stats(nullptr, 0);

        static uint64_t last_alive = 0;
        if (now - last_alive > 5000000) {
            // printf("Renderer: Loop heartbeat alive. flip_pending=%s, queue_size=%zu\n", 
            //        dev->is_flip_pending() ? "YES" : "NO", decoded_unit_queue->size());
            // fflush(stdout);
            last_alive = now;
        }

        // 2b. Deadline commit: hold the newest picture until just before the
        // next vblank. Sleeping returns to the top, so anything decoded in the
        // meantime replaces it (the drain above keeps only the newest).
        if (deadline_commit && render_mode == Atomic && latest_frame) {
            if (dev->is_flip_pending()) {
                dev->wait_for_flip_completion(2);
                continue;
            }
            uint64_t vb = 0;
            double period = dev->frame_period_us();
            if (period > 0 && dev->last_vblank(&vb)) {
                uint64_t t = get_time_us();
                double since = t > vb ? (double)(t - vb) : 0.0;
                double to_next = period - fmod(since, period);
                double sleep_us = 0;
                if (to_next > commit_margin_us) sleep_us = to_next - commit_margin_us;                  // early: wait
                else if (to_next < commit_min_slack_us) sleep_us = to_next + period - commit_margin_us; // too late for this one
                if (sleep_us > 50) {
                    struct timespec ts = { 0, (long)(sleep_us * 1000) };
                    nanosleep(&ts, nullptr);
                    continue;
                }
            }
        }

        // 3. If hardware is busy, wait for it to clear BEFORE attempting render
        // EXCEPTION: FrontBuffer mode does NOT wait. It renders (memcopies) immediately and uses flip ONLY for async stats.
        if (latest_frame && dev->is_flip_pending() && render_mode != FrontBuffer) {
            dev->wait_for_flip_completion(2);
            // After waiting, we MUST 'continue' to the start of the while loop 
            // so we can drain any NEWER frames that arrived while we were sleeping.
            continue; 
        }
        
        // 4. If no frames available at all, wait for the decoder
        if (!latest_frame) {
            auto m = decoded_unit_queue->get(5); 
            if (m) {
                // static_cast, not dynamic_cast: decoded_unit_queue has exactly one
                // producer (put() below) and it only ever pushes this type. The
                // reference-form dynamic_cast threw std::bad_cast on a mismatch, which
                // in this thread is a terminate() rather than a dropped frame - and it
                // cost an RTTI lookup on every frame.
                auto& dm = static_cast<DataMsg<std::shared_ptr<DecodedUnit>>&>(*m);
                latest_frame = dm.getPayload();
                if (deadline_commit && render_mode == Atomic) continue;   // through the deadline above
            } else {
                continue;
            }
        }
        
        // 5. Hardware is guaranteed free (or we have no choice but to try), so render.
        if (render_frame(latest_frame.get())) {
            latest_frame = nullptr; 
        }
    }
}

// A nominal exposure + readout for the capture end of the latency figures,
// when the source has no measured one. The AR8030 path measures its own (see
// Ar8030Source::air_delay_for) and sends it in tx_capture_delay_us.
uint64_t Renderer::sensor_offset_us(const DecodedUnit* du) {
    if (du->tx_capture_delay_us || !osd) return 0;
    uint32_t fps = osd->get_sky_framerate();
    return fps > 0 ? (uint64_t)osd->get_sky_exposure_us() + 1000000 / fps : 0;
}

bool Renderer::render_frame(DecodedUnit *du) {
	if (render_mode == Disable) {
        update_stats(du, 0);
        return true;
    }

    uint64_t render_start_ts = get_time_us();
    
    if(!buffer_initialized) {
		init_buffers(du);
	}

    DisplayBufferInfo buffer_to_display;
    int new_buf_idx = get_or_create_display_buffer(du, buffer_to_display);
    if (new_buf_idx < 0 || buffer_to_display.drm_fb_id == 0) {
        fprintf(stderr, "Failed to get/create display buffer for frame.\n");
        return false;
    }

    int fb_id = buffer_to_display.drm_fb_id;
    current_display_buffer_idx = new_buf_idx;
    if (!du->has_prime_fd || render_mode == FrontBuffer) {
        if (du->drm_pixel_format == DRM_FORMAT_YUV420 && buffer_to_display.format == DRM_FORMAT_NV12) {
             convert_yuv420_to_nv12(buffer_to_display, du);
        } else {
            copy_sw_frame(buffer_to_display, du);
        }
    }
    // FrontBuffer: the copy above IS the presentation - the display is already
    // scanning this buffer, so the frame is live now (modulo where the raster
    // happens to be, which is also why this mode tears). Record the latency
    // here. The probe flip further down is kept for render cadence only, and is
    // given recv_ts = 0 so it cannot overwrite these numbers with its own.
    // (recorded below, once the frame is actually submitted)

    bool performed_modeset = false;

    if (du->width != dev->video_frm_width || du->height != dev->video_frm_height) {
        printf("Frame info changed to %dx%d\n", du->width, du->height);
        dev->set_frame_size(du->width, du->height);
        dev->perform_modeset_video(fb_id);
        performed_modeset = true;
        if(osd) osd->set_video_resolution(du->width, du->height);
	    if(dvr) dvr->init(du->width, du->height);
        // Button-driven recordings are created later, so remember the size.
        DvrRecorder::instance().set_frame_size(du->width, du->height);
        WebStream::instance().set_frame_size(du->width, du->height);
        // Do not return here, see logic below
    }

    // Ensure OSD knows the resolution even if it didn't change from DRM default
    static bool first_frame = true;
    if (first_frame) {
        if(osd) osd->set_video_resolution(du->width, du->height);
        first_frame = false;
    }

    bool submitted = false;
    if (performed_modeset) {
        // Modeset is blocking and displays the frame effectively.
        // We skip page_flip to avoid 'flip_pending' deadlock (no event for no-op flip).
        submitted = true;
    } else if (render_mode == Atomic) {
        uint64_t hw_offset_us = sensor_offset_us(du);
        uint32_t total_tx_age = ((du->tx_capture_delay_us >> 16) & 0xFFFF) +
                                (du->tx_capture_delay_us & 0xFFFF) +
                                du->tx_processing_delay_us + hw_offset_us;
        if (ltrace::on()) ltrace::rec(ltrace::kRender, get_time_us(), (uint32_t)du->pts, (uint64_t)fb_id);
        submitted = dev->page_flip(fb_id, du->recv_ts, du->dec_start_ts, du->dec_end_ts, render_start_ts, total_tx_age);
    } else if (render_mode == FrontBuffer) {
        // FrontBuffer: Always "submitted" because we already wrote to memory.
        uint64_t hw_offset_us = sensor_offset_us(du);
        if (ltrace::on()) ltrace::rec(ltrace::kRender, get_time_us(), (uint32_t)du->pts, (uint64_t)fb_id);
        dev->record_direct_frame(du->recv_ts, du->dec_start_ts, du->dec_end_ts,
                                 render_start_ts,
                                 ((du->tx_capture_delay_us >> 16) & 0xFFFF) +
                                 (du->tx_capture_delay_us & 0xFFFF) +
                                 du->tx_processing_delay_us + hw_offset_us);
        submitted = true;
    }
        
    if (submitted) {
        if (osd) osd->signal_render(prof::kWakeVideo);
        if (osd) osd->notify_video_frame(du->is_keyframe); // drives background→live transition
        update_stats(du, render_start_ts);
        
        // FrontBuffer mode: Asynchronously request a flip event for stats ONLY, after rendering is complete.
        // This is fire-and-forget - if flip is pending, we just skip it. No blocking whatsoever.
        if (render_mode == FrontBuffer && !dev->is_flip_pending()) {
             uint64_t hw_offset_us = 0;
             if (osd) {
                 uint32_t exp_us = osd->get_sky_exposure_us();
                 uint32_t fps = osd->get_sky_framerate();
                 if (fps > 0) {
                     uint32_t readout_us = 1000000 / fps;
                     hw_offset_us = (uint64_t)exp_us + readout_us;
                 }
             }
             // recv_ts = 0 on purpose: the flip handler skips its stats block
             // when recv_ts is 0, so this probe only feeds render cadence and
             // leaves record_direct_frame()'s numbers alone.
             dev->page_flip(fb_id, 0, 0, 0, 0, 0);
        }
        
        return true;
    }
    
    // Track failed submissions (retries), but don't spam as 'drops'
    static uint64_t last_retries_report = 0;
    static int retry_count = 0;
    retry_count++;
    uint64_t now_ms = get_time_ms();
    if (now_ms - last_retries_report > 5000) {
        if (retry_count > 500) { // Only log if we are retrying excessively
             // This is a debug log, suppressed for user cleanliness
             // printf("DEBUG: %d flip retries in last 5s\n", retry_count);
        }
        retry_count = 0;
        last_retries_report = now_ms;
    }

    return false;
}

void Renderer::reset_stats(uint64_t now){
        stats_.decoding_stats_start = now;
        stats_.frame_counter = 0;
        stats_.proc_latency_max = 0;
        stats_.proc_latency_min = 1e18f;
        stats_.total_latency_max = 0;
        stats_.total_latency_min = 1e18f;
        stats_.decoding_latency_max = 0;
        stats_.decoding_latency_min = 1e18f;
        stats_.display_latency_max = 0;
        stats_.display_latency_min = 1e18f;
        stats_.tx_latency_max = 0;
        stats_.tx_latency_min = 1e18f;
        stats_.capture_latency_max = 0;
        stats_.capture_latency_min = 1e18f;
        stats_.reassemble_latency_max = 0;
        stats_.reassemble_latency_min = 1e18f;
        stats_.frame_pace_max = 0;
        stats_.frame_pace_min = 1e18f;
}

void Renderer::update_stats(DecodedUnit *du, uint64_t display_start_ts) {
    static bool first_call = true;
    if (first_call) {
        printf("Renderer: update_stats called for the first time. console_stats=%s\n", console_stats ? "TRUE" : "FALSE");
        // Initial sync reset: Set TX loop delay to 0 to start fresh
        if (cmd_cb) {
             cmd_cb(0x11, 0); 
        }
        fflush(stdout);
        first_call = false;
    }

    auto now = get_time_us();
	
    if ((now-stats_.decoding_stats_start) >= (osd ? (osd->refresh_frequency_ms*1000) : 1000000)) {
        uint64_t delta_us = now - stats_.decoding_stats_start;
        


        latency_stats osd_stats_ = {}; // Zero initialize POD fields, constructs vectors
        float current_framerate = (float)((double)stats_.frame_counter * 1000000.0 / delta_us);
        
        if (delta_us > 2000000) current_framerate = 0.0f; 

        if (stats_.frame_counter > 0) {
            // Net latency
            float proc_latency_sum = 0;
            for (int i = 0; i < stats_.frame_counter; ++i) {
                proc_latency_sum += stats_.proc_latency_avg[i];
                if (stats_.proc_latency_avg[i] > stats_.proc_latency_max) {
                    stats_.proc_latency_max = stats_.proc_latency_avg[i];
                }
                if (stats_.proc_latency_avg[i] < stats_.proc_latency_min) {
                    stats_.proc_latency_min = stats_.proc_latency_avg[i];
                }
                osd_stats_.proc_latency.values.push_back(stats_.proc_latency_avg[i] / 1000.0);
            }
            osd_stats_.proc_latency.avg = proc_latency_sum / (stats_.frame_counter) / 1000.0;
            osd_stats_.proc_latency.max = stats_.proc_latency_max / 1000.0;
            osd_stats_.proc_latency.min = stats_.proc_latency_min / 1000.0;

            // Decoding latency
            float decoding_latency_sum = 0;
            for (int i = 0; i < stats_.frame_counter; ++i) {
                decoding_latency_sum += stats_.decoding_latency_avg[i];
                if (stats_.decoding_latency_avg[i] > stats_.decoding_latency_max) {
                    stats_.decoding_latency_max = stats_.decoding_latency_avg[i];
                }
                if (stats_.decoding_latency_avg[i] < stats_.decoding_latency_min) {
                    stats_.decoding_latency_min = stats_.decoding_latency_avg[i];
                }
                osd_stats_.decoding_latency.values.push_back(stats_.decoding_latency_avg[i] / 1000.0);
            }
            osd_stats_.decoding_latency.avg = decoding_latency_sum / (stats_.frame_counter) / 1000.0;
            osd_stats_.decoding_latency.max = stats_.decoding_latency_max / 1000.0;
            osd_stats_.decoding_latency.min = stats_.decoding_latency_min / 1000.0;

            // Display latency
            float display_latency_sum = 0;
            for (int i = 0; i < stats_.frame_counter; ++i) {
                display_latency_sum += stats_.display_latency_avg[i];
                if (stats_.display_latency_avg[i] > stats_.display_latency_max) {
                    stats_.display_latency_max = stats_.display_latency_avg[i];
                }
                if (stats_.display_latency_avg[i] < stats_.display_latency_min) {
                    stats_.display_latency_min = stats_.display_latency_avg[i];
                }
                osd_stats_.display_latency.values.push_back(stats_.display_latency_avg[i] / 1000.0);
            }
            osd_stats_.display_latency.avg = display_latency_sum / (stats_.frame_counter) / 1000.0;
            osd_stats_.display_latency.max = stats_.display_latency_max / 1000.0;
            osd_stats_.display_latency.min = stats_.display_latency_min / 1000.0;

            // Total latency
            float total_latency_sum = 0;
            for (int i = 0; i < stats_.frame_counter; ++i) {
                total_latency_sum += stats_.total_latency_avg[i];
                if (stats_.total_latency_avg[i] > stats_.total_latency_max) {
                    stats_.total_latency_max = stats_.total_latency_avg[i];
                }
                if (stats_.total_latency_avg[i] < stats_.total_latency_min) {
                    stats_.total_latency_min = stats_.total_latency_avg[i];
                }
                osd_stats_.total_latency.values.push_back(stats_.total_latency_avg[i] / 1000.0);
            }
            osd_stats_.total_latency.avg = total_latency_sum / (stats_.frame_counter) / 1000.0;
            osd_stats_.total_latency.max = stats_.total_latency_max / 1000.0;
            osd_stats_.total_latency.min = stats_.total_latency_min / 1000.0;

            // TX latency
            float tx_latency_sum = 0;
            for (int i = 0; i < stats_.frame_counter; ++i) {
                tx_latency_sum += stats_.tx_latency_avg[i];
                if (stats_.tx_latency_avg[i] > stats_.tx_latency_max) {
                    stats_.tx_latency_max = stats_.tx_latency_avg[i];
                }
                if (stats_.tx_latency_avg[i] < stats_.tx_latency_min) {
                    stats_.tx_latency_min = stats_.tx_latency_avg[i];
                }
                osd_stats_.tx_latency.values.push_back(stats_.tx_latency_avg[i] / 1000.0);
            }
            osd_stats_.tx_latency.avg = tx_latency_sum / (stats_.frame_counter) / 1000.0;
            osd_stats_.tx_latency.max = stats_.tx_latency_max / 1000.0;
            osd_stats_.tx_latency.min = stats_.tx_latency_min / 1000.0;

            // Capture latency
            float capture_latency_sum = 0;
            for (int i = 0; i < stats_.frame_counter; ++i) {
                capture_latency_sum += stats_.capture_latency_avg[i];
                if (stats_.capture_latency_avg[i] > stats_.capture_latency_max) {
                    stats_.capture_latency_max = stats_.capture_latency_avg[i];
                }
                if (stats_.capture_latency_avg[i] < stats_.capture_latency_min) {
                    stats_.capture_latency_min = stats_.capture_latency_avg[i];
                }
                osd_stats_.capture_latency.values.push_back(stats_.capture_latency_avg[i] / 1000.0);
            }
            osd_stats_.capture_latency.avg = capture_latency_sum / (stats_.frame_counter) / 1000.0;
            osd_stats_.capture_latency.max = stats_.capture_latency_max / 1000.0;
            osd_stats_.capture_latency.min = stats_.capture_latency_min / 1000.0;

            // Reassemble latency
            float reassemble_latency_sum = 0;
            for (int i = 0; i < stats_.frame_counter; ++i) {
                reassemble_latency_sum += stats_.reassemble_latency_avg[i];
                if (stats_.reassemble_latency_avg[i] > stats_.reassemble_latency_max) {
                    stats_.reassemble_latency_max = stats_.reassemble_latency_avg[i];
                }
                if (stats_.reassemble_latency_avg[i] < stats_.reassemble_latency_min) {
                    stats_.reassemble_latency_min = stats_.reassemble_latency_avg[i];
                }
                osd_stats_.reassemble_latency.values.push_back(stats_.reassemble_latency_avg[i] / 1000.0);
            }
            osd_stats_.reassemble_latency.avg = reassemble_latency_sum / (stats_.frame_counter) / 1000.0;
            osd_stats_.reassemble_latency.max = stats_.reassemble_latency_max / 1000.0;
            osd_stats_.reassemble_latency.min = stats_.reassemble_latency_min / 1000.0;

            // Frame pace
            float frame_pace_sum = 0;
            for (int i = 0; i < stats_.frame_counter; ++i) {
                frame_pace_sum += stats_.frame_pace_avg[i];
                if (stats_.frame_pace_avg[i] > stats_.frame_pace_max) {
                    stats_.frame_pace_max = stats_.frame_pace_avg[i];
                }
                if (stats_.frame_pace_avg[i] < stats_.frame_pace_min) {
                    stats_.frame_pace_min = stats_.frame_pace_avg[i];
                }
                osd_stats_.frame_pace.values.push_back(stats_.frame_pace_avg[i] / 1000.0);
            }
            osd_stats_.frame_pace.avg = frame_pace_sum / (stats_.frame_counter) / 1000.0;
            osd_stats_.frame_pace.max = stats_.frame_pace_max / 1000.0;
            osd_stats_.frame_pace.min = stats_.frame_pace_min / 1000.0;

            // Stats collected successfully

            // Worst frame (frame with max total latency)
            int worst_idx = 0;
            float max_total = 0;
            for (int i = 0; i < stats_.frame_counter; ++i) {
                if (stats_.total_latency_avg[i] > max_total) {
                    max_total = stats_.total_latency_avg[i];
                    worst_idx = i;
                }
            }
            osd_stats_.proc_latency_worst = stats_.proc_latency_avg[worst_idx] / 1000.0;
            osd_stats_.decoding_latency_worst = stats_.decoding_latency_avg[worst_idx] / 1000.0;
            osd_stats_.display_latency_worst = stats_.display_latency_avg[worst_idx] / 1000.0;
            osd_stats_.tx_latency_worst = stats_.tx_latency_avg[worst_idx] / 1000.0;
            osd_stats_.reassemble_latency_worst = stats_.reassemble_latency_avg[worst_idx] / 1000.0;
            
            if (osd) osd->update_stats((int)current_framerate, osd_stats_);
            
            // Pacing/Sync logic completely removed - relying on immediate submission
            if (current_framerate > 10) {
                 // update_stats heartbeat only
            }
        } else {
            // Even if zero frames, send current state to OSD to show 0 FPS
            if (osd) {
                osd->update_stats(0, osd_stats_);
                
                // Maintain timeline speed: calculate how many frames "should" have happened 
                // in this windows to keep the 5s window consistent.
                int missing_frames = (120 * delta_us) / 1000000;
                if (missing_frames <= 0) missing_frames = 1;
                if (missing_frames > 120) missing_frames = 120; // Cap to 1s worth

                for (int i = 0; i < missing_frames; ++i) {
                    // osd->add_latency_frame({0,0,0,0,0,0});
                }
            }
        }
        reset_stats(now);
    }

    if (du && stats_.frame_counter < Stats::kMaxFrames) {
        uint64_t proc_lat = du->dec_start_ts > du->recv_ts ? du->dec_start_ts - du->recv_ts : 0;
        uint64_t dec_lat = du->dec_end_ts > du->dec_start_ts ? du->dec_end_ts - du->dec_start_ts : 0;
        
        uint64_t hw_offset_us = sensor_offset_us(du);

        uint32_t packed_tx_delays = du->tx_capture_delay_us;
        uint64_t tx_cap = ((packed_tx_delays >> 16) & 0xFFFF) + hw_offset_us;
        uint64_t tx_enc = packed_tx_delays & 0xFFFF;
        uint64_t tx_proc = du->tx_processing_delay_us;
        
        // DEFAULT: Start with software submission time
        uint64_t disp_lat = (now > du->dec_end_ts) ? now - du->dec_end_ts : 0;
        uint64_t total_lat = (now > du->recv_ts ? now - du->recv_ts : 0) + tx_cap + tx_enc + tx_proc;

        // ACCURATE: Override with hardware-accurate VSync stats from previous flips if available
        CompletedStats hw_stats = dev->get_latest_stats();
        if (hw_stats.available) {
            disp_lat = hw_stats.display_latency_us;
            total_lat = hw_stats.total_latency_us;
        }

        uint64_t pace_lat = (stats_.last_frame_ts > 0 && now > stats_.last_frame_ts) ? now - stats_.last_frame_ts : 0;

        // Sanity check: if any latency is more than 1 second, it's likely a wrap-around or clock jump
        if (proc_lat > 1000000) proc_lat = 0;
        if (dec_lat > 1000000) dec_lat = 0;
        if (disp_lat > 1000000) disp_lat = 0;
        if (total_lat > 5000000) total_lat = 0; // 5s for total is generous but safer
        // Sanity check TX-side timing: a bad clock sync can produce absurd tx_cap/tx_proc values
        // (e.g. when clock_offset_us is corrupt). Cap to 1 second each.
        if (tx_cap  > 1000000) tx_cap  = 0;
        if (tx_enc  > 1000000) tx_enc  = 0;
        if (tx_proc > 1000000) tx_proc = 0;

        stats_.proc_latency_avg[stats_.frame_counter] = tx_proc; // Network transport
        stats_.reassemble_latency_avg[stats_.frame_counter] = proc_lat; // GS reassembly
        stats_.decoding_latency_avg[stats_.frame_counter] = dec_lat; // Pure hardware decode
        stats_.display_latency_avg[stats_.frame_counter] = disp_lat;
        stats_.capture_latency_avg[stats_.frame_counter] = tx_cap; // Capture + ISP
        stats_.tx_latency_avg[stats_.frame_counter] = tx_enc; // Encoder processing
        stats_.total_latency_avg[stats_.frame_counter] = total_lat;
        stats_.frame_pace_avg[stats_.frame_counter] = pace_lat;
        stats_.last_frame_ts = now;

        // Feed the per-frame graph
        if (osd) {
            osd->add_latency_frame({
                .capture_ms = (float)tx_cap / 1000.0f,
                .processing_ms = (float)tx_enc / 1000.0f, // Encoder processing!
                .net_ms = (float)tx_proc / 1000.0f,       // Network transport!
                .reassemble_ms = (float)proc_lat / 1000.0f, // Ground reassembly!
                .dec_ms = (float)dec_lat / 1000.0f,                   // Pure hardware decode!
                .disp_ms = (float)disp_lat / 1000.0f
            });
        }

        stats_.frame_counter++; 
        
        if (stats_.frame_counter % 100 == 0) {
            // printf("Renderer: frame_counter reached %d\n", stats_.frame_counter);
            // fflush(stdout);
        }
    }
}
void Renderer::present_frame(uint32_t fb_id, uint32_t width, uint32_t height, uint64_t pts, uint64_t recv_ts, uint64_t dec_start_ts, uint64_t dec_end_ts, uint32_t tx_age_us) {
    uint64_t render_start_ts = get_time_us();
    if (width != dev->video_frm_width || height != dev->video_frm_height) {
        dev->set_frame_size(width, height);
        dev->perform_modeset_video(fb_id);
        if(osd) osd->set_video_resolution(width, height);
    }
    bool submitted = false;
    if (render_mode == Atomic) {
        uint64_t hw_offset_us = 0;
        if (osd) {
            uint32_t exp_us = osd->get_sky_exposure_us();
            uint32_t fps = osd->get_sky_framerate();
            if (fps > 0) {
                uint32_t readout_us = 1000000 / fps;
                hw_offset_us = (uint64_t)exp_us + readout_us;
            }
        }
        submitted = dev->page_flip(fb_id, recv_ts, dec_start_ts, dec_end_ts, render_start_ts, tx_age_us + hw_offset_us); 
    }
    if (osd) osd->signal_render();
}
