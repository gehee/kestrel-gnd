#include "vdec_rk.hpp"
#include <atomic>
#include "../utils/ltrace.hpp"

#include <assert.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <sys/ioctl.h>

#include "common.hpp"
#include "utils/scheduling_helper.hpp"
#include "utils/time_util.h"

// Which set of decoder buffers prime_fds refer to (DecodedUnit::buf_epoch).
static std::atomic<uint32_t> s_buf_epoch{1};


// void VdecRK::init_buffer(MppFrame frame) {
// 	RK_U32 width = mpp_frame_get_width(frame);
//     RK_U32 height = mpp_frame_get_height(frame);
//     RK_U32 stride = mpp_frame_get_hor_stride(frame);
//     MppFrameFormat fmt = mpp_frame_get_fmt(frame);

//     assert(fmt == MPP_FMT_YUV420SP);

//     int ret = mpp_buffer_group_get_external(&mpi.frm_grp, MPP_BUFFER_TYPE_DRM);
//     assert(ret == 0);

//     for (int i = 0; i < MAX_FRAMES; i++) {
//         DrmBuffer out;
//         if (!renderer->create_drm_nv12_buffer(stride, height, out, true)) {
// 			printf("create_drm_nv12_buffer failed\n");
//             exit(1);
//         }

//         mpi.frame_to_drm[i].handle = out.handle;
//         mpi.frame_to_drm[i].fb_id = out.fb_id;

//         MppBufferInfo info = {};
//         info.type = MPP_BUFFER_TYPE_DRM;
//         info.size = stride * height * 3 / 2;
//         info.fd = out.prime_fd;
//         ret = mpp_buffer_commit(mpi.frm_grp, &info);
//         assert(ret == 0);
//         mpi.frame_to_drm[i].prime_fd = info.fd;
//         if (info.fd != out.prime_fd) close(out.prime_fd);
//     }

//     mpi.mpi->control(mpi.ctx, MPP_DEC_SET_EXT_BUF_GROUP, mpi.frm_grp);
//     mpi.mpi->control(mpi.ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
// 	printf("init_buffer done\n");
// }

void VdecRK::init_buffer(MppFrame frame) {
	uint64_t video_frm_width = mpp_frame_get_width(frame);
	uint64_t video_frm_height = mpp_frame_get_height(frame);
	RK_U32 hor_stride = mpp_frame_get_hor_stride(frame);
	RK_U32 ver_stride = mpp_frame_get_ver_stride(frame);
	MppFrameFormat fmt = mpp_frame_get_fmt(frame);
	assert((fmt == MPP_FMT_YUV420SP) || (fmt == MPP_FMT_YUV420SP_10BIT));

	printf("Frame info changed %lu(%d)x%lu(%d)\n", video_frm_width, hor_stride, video_frm_height, ver_stride);

	// On resolution change, free all old DRM/MPP resources before reallocating.
	// Order: flush renderer cache → remove FBs → put MPP group (closes prime fds) → destroy dumb handles.
	if (mpi.frm_grp) {
		printf("VdecRK: resolution change — flushing renderer and freeing old DRM resources\n");
		// 0. Ask renderer to drain its frame queue and clear its display_buffers cache.
		//    This prevents false prime_fd matches after fd numbers are reused by new allocations.
		if (renderer) renderer->request_flush();
		// The screen recorder's pictures too: a buffer still referenced when
		// the group is put back below is never freed by MPP 1.0.3.
		if (dev) dev->drop_screen_pictures();
		// 1. Remove DRM framebuffers (kernel ref-counts, safe even if still displayed)
		for (int i = 0; i < MAX_FRAMES; i++) {
			if (mpi.frame_to_drm[i].fb_id) {
				drmModeRmFB(dev->drm_fd, mpi.frame_to_drm[i].fb_id);
				mpi.frame_to_drm[i].fb_id = 0;
			}
		}
		// 2. Release MPP buffer group (closes prime_fds owned by MPP). Their
		// numbers may come back for the new buffers: a new epoch. Cleared
		// first, as MPP's own decoder test does on an info change: a buffer
		// something still references is marked discard, so it is freed when
		// that lets go. Put back with one still in use, MPP 1.0.3 would
		// otherwise keep it - and the orphaned group - for good.
		s_buf_epoch++;
		mpp_buffer_group_clear(mpi.frm_grp);
		mpp_buffer_group_put(mpi.frm_grp);
		mpi.frm_grp = NULL;
		// 3. Destroy underlying GEM dumb buffers
		for (int i = 0; i < MAX_FRAMES; i++) {
			if (mpi.frame_to_drm[i].handle) {
				struct drm_mode_destroy_dumb dmd = {};
				dmd.handle = mpi.frame_to_drm[i].handle;
				ioctl(dev->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dmd);
				mpi.frame_to_drm[i].handle = 0;
			}
			mpi.frame_to_drm[i].prime_fd = -1;
		}
	}

	// output_list->video_fb_x = 0;
	// output_list->video_fb_y = 0;
	// output_list->video_fb_width = output_list->mode.hdisplay;
	// output_list->video_fb_height =output_list->mode.vdisplay;

	// osd_vars.video_width = output_list->video_frm_width;
	// osd_vars.video_height = output_list->video_frm_height;

	// create new external frame group and allocate (commit flow) new DRM buffers and DRM FB
	int ret = mpp_buffer_group_get_external(&mpi.frm_grp, MPP_BUFFER_TYPE_DRM);
	assert(!ret);

	for (int i=0; i<MAX_FRAMES; i++) {
		
		// new DRM buffer
		struct drm_mode_create_dumb dmcd;
		memset(&dmcd, 0, sizeof(dmcd));
		dmcd.bpp = fmt==MPP_FMT_YUV420SP?8:10;
		dmcd.width = hor_stride;
		dmcd.height = ver_stride*2; // documentation say not v*2/3 but v*2 (additional info included)
		do {
			ret = ioctl(dev->drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &dmcd);
		} while (ret == -1 && (errno == EINTR || errno == EAGAIN));
		assert(!ret);
		// assert(dmcd.pitch==(fmt==MPP_FMT_YUV420SP?hor_stride:hor_stride*10/8));
		// assert(dmcd.size==(fmt == MPP_FMT_YUV420SP?hor_stride:hor_stride*10/8)*ver_stride*2);
		mpi.frame_to_drm[i].handle = dmcd.handle;
		
		// commit DRM buffer to frame group
		struct drm_prime_handle dph;
		memset(&dph, 0, sizeof(struct drm_prime_handle));
		dph.handle = dmcd.handle;
		dph.fd = -1;
		do {
			ret = ioctl(dev->drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &dph);
		} while (ret == -1 && (errno == EINTR || errno == EAGAIN));
		assert(!ret);
		MppBufferInfo info;
		memset(&info, 0, sizeof(info));
		info.type = MPP_BUFFER_TYPE_DRM;
		info.size = dmcd.width*dmcd.height;
		info.fd = dph.fd;
		ret = mpp_buffer_commit(mpi.frm_grp, &info);
		assert(!ret);
		mpi.frame_to_drm[i].prime_fd = info.fd; // dups fd						
		if (dph.fd != info.fd) {
			ret = close(dph.fd);
			assert(!ret);
		}

		// allocate DRM FB from DRM buffer
		uint32_t handles[4], pitches[4], offsets[4];
		memset(handles, 0, sizeof(handles));
		memset(pitches, 0, sizeof(pitches));
		memset(offsets, 0, sizeof(offsets));
		handles[0] = mpi.frame_to_drm[i].handle;
		offsets[0] = 0;
		pitches[0] = hor_stride;						
		handles[1] = mpi.frame_to_drm[i].handle;
		offsets[1] = pitches[0] * ver_stride;
		pitches[1] = pitches[0];
        
        // Debug print
        // printf("VdecRK: Creating FB %d/%d for handle %u\n", i, MAX_FRAMES, handles[0]);
		ret = drmModeAddFB2(dev->drm_fd, video_frm_width, video_frm_height, DRM_FORMAT_NV12, handles, pitches, offsets, &mpi.frame_to_drm[i].fb_id, 0);
        if (ret != 0) {
            printf("VdecRK: drmModeAddFB2 failed: %d (errno=%d)\n", ret, errno);
        }
		assert(!ret);
    }
	// printf("VdecRK: init_buffer loop complete\n");

	// register external frame group
	ret = mpi.mpi->control(mpi.ctx, MPP_DEC_SET_EXT_BUF_GROUP, mpi.frm_grp);
    // if (ret) printf("VdecRK: MPP_DEC_SET_EXT_BUF_GROUP failed ret=%d\n", ret);
    // printf("VdecRK: Set Ext Buf Group Done\n");
    
	ret = mpi.mpi->control(mpi.ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
     // if (ret) printf("VdecRK: MPP_DEC_SET_INFO_CHANGE_READY failed ret=%d\n", ret);
    // printf("VdecRK: Set Info Change Ready Done\n");

	// ret = modeset_perform_modeset(dev->drm_fd, output_list, output_list->video_request, &output_list->video_plane, mpi.frame_to_drm[0].fb_id, osd_vars.video_width, osd_vars.video_height, video_zpos);
	// assert(ret >= 0);
    // printf("VdecRK: init_buffer returning\n");
}


static int consec_put_stalls = 0;   // consecutive 1s put-packet stalls (decoder wedge detector)

void VdecRK::run_frame()
{
	printf("VdecRK run_frame thread starting...\n" );
    SchedulingHelper::configure_thread(SchedulingHelper::ThreadRole::VDec);
	int i, ret;
	MppFrame  frame  = NULL;
	uint64_t last_frame_time;

	while (!*should_stop) { // TODO frm_eos for proper closing.
		struct timespec ts, ats;
		assert(!frame);
		ret = mpi.mpi->decode_get_frame(mpi.ctx, &frame);
		if (ret != 0) {
			static int err_count = 0;
			if (err_count++ % 100 == 0) {
				printf("VdecRK: decode_get_frame returned error %d\n", ret);
			}
		}
		clock_gettime(CLOCK_MONOTONIC, &ats);
		if (frame) {
			first_successful_decode = true;
			last_decoded_frame_time_ms = get_time_ms();
			uint64_t pts =  mpp_frame_get_pts(frame);
			static int frame_count = 0;
            // if (frame_count++ % 30 == 0) {
            //     printf("VdecRK: Got decoded frame from MPP! PTS=%lu, size=%dx%d\n", pts, mpp_frame_get_width(frame), mpp_frame_get_height(frame));
            // }
            uint32_t frm_width = mpp_frame_get_width(frame);
            uint32_t frm_height = mpp_frame_get_height(frame);
            MppFrameFormat fmt = mpp_frame_get_fmt(frame);
			if (mpp_frame_get_info_change(frame)) {
                // printf("VdecRK: Calling init_buffer\n");
                init_buffer(frame);
                // printf("VdecRK: Returned from init_buffer\n");
			} else {
                // ... (omitted for brevity, assume existing else block)
				// regular frame received
				MppBuffer buffer = mpp_frame_get_buffer(frame);					
				if (buffer) {
					MppBufferInfo info;
					ret = mpp_buffer_info_get(buffer, &info);
					assert(!ret);
					for (i=0; i<MAX_FRAMES; i++) {
						if (mpi.frame_to_drm[i].prime_fd == info.fd) break;
					}
					assert(i!=MAX_FRAMES);

                    uint64_t now_us = get_time_us();
                    if (ltrace::on()) {
                        // Mean luma of an 8x4 grid, sparsely sampled: enough to
                        // see a light switched on in front of the camera.
                        const uint8_t* y = (const uint8_t*)mpp_buffer_get_ptr(buffer);
                        const uint32_t stride = mpp_frame_get_hor_stride(frame);
                        uint8_t grid[32];
                        for (int c = 0; c < 32 && y; c++) {
                            const uint32_t x0 = (c % 8) * frm_width / 8, y0 = (c / 8) * frm_height / 4;
                            uint32_t sum = 0;
                            for (int j = 0; j < 8; j++)
                                for (int i = 0; i < 8; i++)
                                    sum += y[(size_t)(y0 + (2 * j + 1) * frm_height / 64) * stride +
                                             x0 + (2 * i + 1) * frm_width / 128];
                            grid[c] = (uint8_t)(sum / 64);
                        }
                        ltrace::rec(ltrace::kDecOut, now_us, (uint32_t)pts, 0, grid, y ? 32 : 0);
                    }
                    bool found_stats = false;
                    timing_stats_t t_stats = {};
                    {
                        std::lock_guard<std::mutex> lock(decoding_stats_mutex);
                        auto it = decoding_stats.find(pts);
                        if (it != decoding_stats.end()) {
                            found_stats = true;
                            t_stats = it->second;
                            decoding_stats.erase(it);
                        }
                    }

                    if (found_stats) {
                        auto du_stat = std::make_shared<DecodedUnit>();
                        du_stat->pts = pts;
                        du_stat->width = frm_width;
                        du_stat->height = frm_height;
                        du_stat->recv_ts = t_stats.recv_start_us;
                        du_stat->dec_start_ts = t_stats.decode_start_us;
                        du_stat->dec_end_ts = now_us;
                        du_stat->tx_capture_delay_us = t_stats.tx_capture_delay_us;
                        du_stat->tx_processing_delay_us = t_stats.tx_processing_delay_us;
                        du_stat->is_keyframe = t_stats.is_keyframe;
                        
                        du_stat->has_prime_fd = true;
                        du_stat->prime_fd = info.fd;
                        du_stat->drm_pixel_format = DRM_FORMAT_NV12;
                        
                        // Pass layout info for NV12
                        du_stat->pitches[0] = mpp_frame_get_hor_stride(frame);
                        du_stat->offsets[0] = 0;
                        du_stat->pitches[1] = du_stat->pitches[0];
                        du_stat->offsets[1] = du_stat->pitches[0] * mpp_frame_get_ver_stride(frame);

                        emit_decoded(du_stat, buffer);

                    } else {
                        // This shouldn't happen if MPP preserves PTS
                        static uint32_t fail_count = 0;
                        if (fail_count++ % 100 == 0) printf("VdecRK: Warning! Received PTS %lu not found in tracking map\n", pts);
                        
                        auto du_fallback = std::make_shared<DecodedUnit>();
                        du_fallback->pts = pts;
                        du_fallback->width = frm_width;
                        du_fallback->height = frm_height;
                        du_fallback->has_prime_fd = true;
                        du_fallback->prime_fd = info.fd;
                        du_fallback->drm_pixel_format = DRM_FORMAT_NV12;
                        
                        du_fallback->pitches[0] = mpp_frame_get_hor_stride(frame);
                        du_fallback->offsets[0] = 0;
                        du_fallback->pitches[1] = du_fallback->pitches[0];
                        du_fallback->offsets[1] = du_fallback->pitches[0] * mpp_frame_get_ver_stride(frame);

                        emit_decoded(du_fallback, buffer);
                    }
				}
			}
            
			frm_eos = mpp_frame_get_eos(frame);
            // printf("VdecRK: Deinitializing frame %p\n", frame);
			mpp_frame_deinit(&frame);
            // printf("VdecRK: Frame deinitialized\n");
			frame = NULL;
		};
	}
	printf("VdecRK run_frame thread done.\n");
}

// Deliver a decoded NV12 frame either to a registered sink (background video,
// GL-texture path) or to the renderer's video plane (live feed). For the sink
// path the underlying MPP buffer is ref-held via du->frame_ref so it stays
// valid until the consumer finishes its (deferred) EGL import.
static std::shared_ptr<void> hold_mpp_buffer(void* b) {
    mpp_buffer_inc_ref((MppBuffer)b);
    return std::shared_ptr<void>(b, [](void* p) { mpp_buffer_put((MppBuffer)p); });
}

void VdecRK::emit_decoded(std::shared_ptr<DecodedUnit> du, MppBuffer buffer) {
    du->buf = buffer;
    du->hold = hold_mpp_buffer;
    du->buf_epoch = s_buf_epoch.load(std::memory_order_relaxed);
    if (frame_sink_) {
        mpp_buffer_inc_ref(buffer);
        du->frame_ref = std::shared_ptr<void>(buffer, [](void* b) {
            mpp_buffer_put((MppBuffer)b);
        });
        frame_sink_(du);
    } else {
        // While the screen is recorded, the picture is held from here, while
        // MPP still has it: the recorder reads it after the display has
        // (screen_tap.hpp), and the hold - not MPP's least-recently-used
        // reuse - is what keeps the decoder from writing into it meanwhile.
        if (dev && dev->tap.recording()) du->frame_ref = hold_mpp_buffer(buffer);
        renderer->queue_frame(du);
    }
}

void VdecRK::feed_packet_to_decoder(void* data_p, int data_len, int64_t pts, uint64_t recv_ts, uint8_t nal_type, uint32_t capture_delay_us, uint32_t processing_delay_us, bool is_key_override){
    if (data_len <= 0) return;

    // Cache H.265 VPS (32), SPS (33), PPS (34) or H.264 SPS (7), PPS (8)
    if (nal_type == 32) {
        cached_vps.assign((uint8_t*)data_p, (uint8_t*)data_p + data_len);
        return;
    }
    if (nal_type == 33 || nal_type == 7) {
        cached_sps.assign((uint8_t*)data_p, (uint8_t*)data_p + data_len);
        return;
    }
    if (nal_type == 34 || nal_type == 8) {
        cached_pps.assign((uint8_t*)data_p, (uint8_t*)data_p + data_len);
        return;
    }
    // Skip SEI entirely
    if (nal_type == 39 || nal_type == 40 || nal_type == 6) {
        return;
    }

    if (data_len > READ_BUF_SIZE) {
        printf("VdecRK: packet/slice too large %d\n", data_len);
        return;
    }

    static int feed_count = 0;
    feed_count++;

    // Copy to a circular buffer to keep the data pointer valid for asynchronous parsing/decoding
    uint8_t* dest_buf = slices_buffers[current_slice_buffer_idx];
    current_slice_buffer_idx = (current_slice_buffer_idx + 1) % NUM_SLICES_BUFFERS;

    int final_len = 0;
    bool is_key = is_key_override || (nal_type >= 19 && nal_type <= 21) || nal_type == 5;

    if (is_key) {
        size_t vps_sz = cached_vps.size();
        size_t sps_sz = cached_sps.size();
        size_t pps_sz = cached_pps.size();

        if (vps_sz + sps_sz + pps_sz + data_len > READ_BUF_SIZE) {
            printf("VdecRK: combined packet too large %zu\n", vps_sz + sps_sz + pps_sz + data_len);
            return;
        }

        uint8_t* p = dest_buf;
        if (vps_sz > 0) {
            memcpy(p, cached_vps.data(), vps_sz);
            p += vps_sz;
        }
        if (sps_sz > 0) {
            memcpy(p, cached_sps.data(), sps_sz);
            p += sps_sz;
        }
        if (pps_sz > 0) {
            memcpy(p, cached_pps.data(), pps_sz);
            p += pps_sz;
        }
        memcpy(p, data_p, data_len);
        final_len = vps_sz + sps_sz + pps_sz + data_len;
    } else {
        memcpy(dest_buf, data_p, data_len);
        final_len = data_len;
    }

    // if (feed_count % 100 == 1) {
    //     printf("VdecRK: feed_packet_to_decoder - size=%d (orig=%d), PTS=%ld, NAL=%d, Key=%d (Total feed calls: %d)\n", 
    //            final_len, data_len, pts, nal_type, is_key, feed_count);
    // }

    MppPacket pkt = nullptr;
    int ret = mpp_packet_init(&pkt, dest_buf, final_len);
    if (ret != MPP_OK) {
        printf("VdecRK: mpp_packet_init failed: %d\n", ret);
        return;
    }
    mpp_packet_set_pts(pkt, (RK_S64)pts);

    // Feed the data to mpp until either timeout (in which case the decoder might have stalled)
    // or success
    uint64_t data_feed_begin = get_time_ms();

    uint64_t now_ms = get_time_ms();
    if (first_successful_decode && (now_ms - last_decoded_frame_time_ms > 2000)) {
        first_successful_decode = false;
        idr_requested = false;
    }

    if (!first_successful_decode) {
        if (now_ms - last_idr_request_ms > 1000) {
            if (idr_request_callback) {
                idr_request_callback();
                last_idr_request_ms = now_ms;
                idr_requested = true;
            }
        }
    } else if (!idr_requested && idr_request_callback) {
       idr_request_callback();
       idr_requested = true;
    }

    {
        std::lock_guard<std::mutex> lock(decoding_stats_mutex);
        decoding_stats[pts] = timing_stats_t{ 
            .recv_start_us = recv_ts,
            .decode_start_us = get_time_us(), 
            .tx_capture_delay_us = capture_delay_us,
            .tx_processing_delay_us = processing_delay_us,
            // is_key, not is_key_override: the override is only a hint from the
            // caller. Recording the raw override here left is_keyframe false for
            // every source that relies on NAL-type detection (the AR8030 path
            // passes no override), so Renderer::render_frame never told the OSD
            // a keyframe had landed, the OSD stayed in BACKGROUND state, and the
            // fps/resolution/bitrate labels were never drawn.
            .is_keyframe = is_key,
        };
        // MPP drops the pictures it cannot decode, and their entries were
        // never collected: over a broken stream this grew by every picture
        // sent. No picture spends 128 others inside the decoder.
        if (decoding_stats.size() > 256) {
            for (auto it = decoding_stats.begin(); it != decoding_stats.end();)
                it = (it->first < pts - 128) ? decoding_stats.erase(it) : std::next(it);
        }
    }
    
    const uint64_t put_start_us = get_time_us();
    int attempt = 0;
    while (MPP_OK != (ret = mpi.mpi->decode_put_packet(mpi.ctx, pkt))) {
        attempt++;
        uint64_t elapsed = get_time_ms() - data_feed_begin;
        if (attempt % 500 == 1) {
            printf("VdecRK: decode_put_packet returned %d, elapsed=%lu ms, attempt=%d, pts=%ld, len=%d\n", ret, elapsed, attempt, pts, data_len);
        }
        if (elapsed > 1000 || *should_stop) { // Increased from 8ms to 1000ms to avoid deadlock under heavy slice load
            printf("VdecRK: decode_put_packet failed/stalled with ret=%d after %lu ms, attempt=%d\n", ret, elapsed, attempt);
            decoder_stalled_count++;
            mpp_packet_deinit(&pkt);
            // A wedged decoder (output backpressure after e.g. a PTS reset from
            // an air reboot) never recovers by dropping packets — observed
            // stuck in the -1012 loop for tens of thousands of packets. Hard
            // reset MPP after 3 consecutive stalls; the air re-sends
            // params+IDR (link-up burst / gnd retry) so decode resumes on the
            // next keyframe within a second or two.
            if (++consec_put_stalls >= 3) {
                printf("VdecRK: %d consecutive stalls — resetting the MPP decoder\n",
                       consec_put_stalls);
                mpi.mpi->reset(mpi.ctx);
                {
                    std::lock_guard<std::mutex> lock(decoding_stats_mutex);
                    decoding_stats.clear();
                }
                consec_put_stalls = 0;
            }
            return;
        }
        // Do NOT sched_yield() here: these threads run SCHED_FIFO, so yielding
        // returns immediately when no equal-prio thread is runnable and the loop
        // busy-spins, burning RT bandwidth (triggers "sched: RT throttling
        // activated") and starving the frame/renderer threads that free the
        // decode buffers -> the -1012 never clears. Sleep briefly instead so the
        // drain threads get the CPU; 200us keeps well under a 60fps frame time.
        usleep(200);
    }
    consec_put_stalls = 0;
    if (ltrace::on()) ltrace::rec(ltrace::kDecPut, put_start_us, (uint32_t)pts, get_time_us());
    
    if (attempt > 1) {
        printf("VdecRK: decode_put_packet succeeded after attempt=%d, elapsed=%lu ms\n", attempt, get_time_ms() - data_feed_begin);
    }
    mpp_packet_deinit(&pkt);
}

void VdecRK::cleanup() {
	printf("Feeding eos\n");
    mpp_packet_set_eos(packet);
    mpp_packet_set_length(packet, 0);
    int ret=0;
    int retry_count = 0;
    while (MPP_OK != (ret = mpi.mpi->decode_put_packet(mpi.ctx, packet))) {
        usleep(10000);
        if (retry_count++ > 10) {
            printf("VdecRK: cleanup timeout feeding EOS\n");
            break; 
        }
    }
}

void VdecRK::set_control_verbose(MpiCmd control,RK_U32 enable){
    RK_U32 res = mpi.mpi->control(mpi.ctx, control, &enable);
    if(res){
        printf("Could not set control %d %d\n",control,enable);
        assert(false);
    }
}

void VdecRK::set_mpp_decoding_parameters() {
    // config for runtime mode
    MppDecCfg cfg       = NULL;
    mpp_dec_cfg_init(&cfg);
    // get default config from decoder context
    int ret = mpi.mpi->control(mpi.ctx, MPP_DEC_GET_CFG, cfg);
    if (ret) {
        printf("%p failed to get decoder cfg ret %d\n", mpi.ctx, ret);
        assert(false);
    }
    // split_parse is to enable mpp internal frame spliter when the input
    // packet is not aplited into frames.
    RK_U32 need_split   = 0;
    ret = mpp_dec_cfg_set_u32(cfg, "base:split_parse", need_split);
    if (ret) {
        printf("%p failed to set split_parse ret %d\n", mpi.ctx, ret);
        assert(false);
    }
    ret = mpi.mpi->control(mpi.ctx, MPP_DEC_SET_CFG, cfg);
    if (ret) {
        printf("%p failed to set cfg %p ret %d\n", mpi.ctx, cfg, ret);
        assert(false);
    }
	int mpp_split_mode = 0;
    set_control_verbose(MPP_DEC_SET_PARSER_SPLIT_MODE, mpp_split_mode);
    set_control_verbose(MPP_DEC_SET_DISABLE_ERROR, 1);
    set_control_verbose(MPP_DEC_SET_IMMEDIATE_OUT, 1);
    set_control_verbose(MPP_DEC_SET_ENABLE_FAST_PLAY, 1);
    //set_control_verbose(mpi,ctx,MPP_DEC_SET_ENABLE_DEINTERLACE, 1);
    // Docu fast mode:
    // and improve the
    // parallelism of decoder hardware and software
    // we probably don't want that, since we don't need pipelining to hit our bitrate(s)
    int fast_mode = 0;  // pipelining off: ride through corrupt/partial frames instead of HW timeout+reset storm
    set_control_verbose(MPP_DEC_SET_PARSER_FAST_MODE,fast_mode);
}