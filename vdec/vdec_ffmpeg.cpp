#include <bitset>
#include <iostream>
#include <iomanip>
#include <algorithm>
#ifdef USE_VAAPI
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#endif
#include "vdec_ffmpeg.hpp"

//#define ERRSTR strerror(errno)

bool VdecFfmpeg::init_decoder(void* data_p,int data_len){
    frame_count = 0;
    cleanup_device(); // FULL cleanup of hw_device_ctx and related buffers
    
    if(av_ctx) {
        avcodec_free_context(&av_ctx);
        av_ctx = NULL;
    }

    if (av_parser) {
        av_parser_close(av_parser);
        av_parser = av_parser_init(codec->id);
    }

    av_ctx = avcodec_alloc_context3(codec);
    if (!av_ctx) {
        fprintf(stderr, "Could not allocate video codec context\n");
        return false;
    }

    // qsv acceleration
    if(use_qsv){
        bool ok = false;
        char card_name[50];
        // 1. Try direct QSV creation on render nodes
        for(int i=128; i<=132; i++){
            sprintf(card_name, "/dev/dri/renderD%d", i);
            printf("Trying direct qsv (vaapi-backend) with %s...\n", card_name);
            AVDictionary *dopts = NULL;
            av_dict_set(&dopts, "child_device_type", "vaapi", 0);
            if (av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_QSV, card_name, dopts, 0) == 0) {
                ok = true;
                printf("Using qsv with %s\n", card_name);
                av_dict_free(&dopts);
                break;
            }
            av_dict_free(&dopts);
        }

        // 2. Fallback: Try deriving QSV from VAAPI (OneVPL/MSDK on Linux often requires this)
        if (!ok) {
            printf("Direct QSV creation failed, trying via VAAPI derivation...\n");
            for(int i=128; i<=132; i++){
                sprintf(card_name, "/dev/dri/renderD%d", i);
                AVBufferRef* vaapi_dev = NULL;
                // Try to create VAAPI device first
                if (av_hwdevice_ctx_create(&vaapi_dev, AV_HWDEVICE_TYPE_VAAPI, card_name, NULL, 0) == 0) {
                     printf("Created VAAPI device on %s. Deriving QSV...\n", card_name);
                     if (av_hwdevice_ctx_create_derived(&hw_device_ctx, AV_HWDEVICE_TYPE_QSV, vaapi_dev, 0) == 0) {
                         ok = true;
                         printf("Successfully derived QSV from VAAPI on %s\n", card_name);
                         av_buffer_unref(&vaapi_dev); 
                         break;
                     } else {
                         printf("Failed to derive QSV from VAAPI on %s\n", card_name);
                     }
                     av_buffer_unref(&vaapi_dev);
                }
            }
        }

        if (!ok) {
            printf("Trying QSV default with VAAPI child...\n");
            AVDictionary *dopts = NULL;
            av_dict_set(&dopts, "child_device_type", "vaapi", 0);
            if (av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_QSV, NULL, dopts, 0) == 0) {
                ok = true;
                printf("Using qsv with default vaapi child\n");
            }
            av_dict_free(&dopts);
        }

        // 4. Last resort: Default device
        if (!ok) {
            fprintf(stderr, "Explicit QSV/VAAPI-derived setup failed, trying default qsv...\n");
            if (av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_QSV, NULL, NULL, 0) == 0) {
                 printf("Using qsv with default device\n");
            } else {
                fprintf(stderr, "Failed to create QSV device\n");
                return false;
            }
        }
    
        av_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
    }

#ifdef USE_VAAPI
    // vaapi acceleration
    if(use_vaapi){
        bool ok = false;
        char card_name[50];
        for(int i=128; i<=132; i++){
            //sprintf(card_name, "/dev/dri/card%d", i);
            sprintf(card_name, "/dev/dri/renderD%d", i);
            
            printf("Trying vaapi with %s...\n", card_name);
            if (av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI, card_name, NULL, 0) == 0) {
                ok = true;
                printf("Using vaapi with %s\n", card_name);
                break;
            }
        }
        if (!ok) {
            fprintf(stderr, "Failed to create VAAPI device\n");
            return false;
            // exit(1);
        }
    
        av_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
    }
#else
    if (use_vaapi) {
        fprintf(stderr, "VAAPI requested but not supported in this build\n");
        return false;
    }
#endif

    // Always request low delay decoding
    av_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    // av_log_set_level(AV_LOG_DEBUG); // Uncomment for driver-level diagnostics

    // Note: AV_CODEC_FLAG_OUTPUT_CORRUPT and AV_CODEC_FLAG2_SHOW_ALL are removed 
    // because they cause stability issues with some VAAPI drivers when joinning mid-stream.
    
    // HOWEVER: For Intra-Refresh (GDR) to work, we MUST output corrupt frames initially
    // as the picture cleans up. Without this, the decoder waits forever for a keyframe.
    av_ctx->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT;
    av_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    av_ctx->flags2 |= AV_CODEC_FLAG2_SHOW_ALL;
    
    // Aggressively relax compliance for robust GDR startup on VAAPI
    av_ctx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
    av_ctx->err_recognition = 0; // AV_EF_IGNORE_ERR / 0
    // av_ctx->skip_frame = AVDISCARD_NONREF; // Optional: might skip non-ref frames if overloaded

#ifdef USE_VAAPI
    if (use_vaapi) {
        av_ctx->hwaccel_flags |= AV_HWACCEL_FLAG_ALLOW_PROFILE_MISMATCH;
        av_ctx->hwaccel_flags |= AV_HWACCEL_FLAG_IGNORE_LEVEL;
        av_ctx->hwaccel_flags |= AV_HWACCEL_FLAG_ALLOW_HIGH_DEPTH; 
        av_ctx->extra_hw_frames = 64; // Increased even more for Alder Lake N stability
    }
#endif

    av_ctx->flags2 |= AV_CODEC_FLAG2_FAST;
    av_ctx->has_b_frames = 0; // Force zero reordering
    av_ctx->max_b_frames = 0;
    av_ctx->thread_count = 1; // Single thread for lowest latency and more deterministic start
    av_ctx->active_thread_type = 0; // FF_THREAD_SLICE; 
    av_ctx->pkt_timebase = (AVRational){1, 90000};
    
    // Allow ffmpeg to output corrupt frames (necessary for GDR startup where references are missing)
    av_ctx->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT;
    av_ctx->flags2 |= AV_CODEC_FLAG2_SHOW_ALL;
    
    if (data_len>0){
        av_ctx->extradata = (uint8_t*)av_malloc(data_len + AV_INPUT_BUFFER_PADDING_SIZE);
        av_ctx->extradata_size= data_len;
        memcpy(av_ctx->extradata, data_p, data_len);
        memset(av_ctx->extradata + data_len, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    }

    AVDictionary *opts = NULL;
    // hevc_rkmpp
    // https://github.com/nyanmisaka/ffmpeg-rockchip/blob/57d5befee96f229b05fa09334a4d7a6f95a324bd/libavcodec/rkmppdec.h#L46
    av_dict_set(&opts, "buf_mode", "half", 0);
    av_dict_set(&opts, "low_latency", "1", 0); // Changed to 1
    av_dict_set(&opts, "deint", "0", 0);
    av_dict_set(&opts, "async_depth", "1", 0); // Minimize pipeline depth
    // av_dict_set(&opts, "fast_play", "0", 0); // causes to only return frame when dec buf is full
    // av_dict_set(&opts, "fast_parse", "0", 0);
    av_dict_set(&opts, "immediate_out", "0", 0);
    av_dict_set(&opts, "disable_error", "0", 0);
    // Robustness for GDR / Negative POC
    av_dict_set(&opts, "strict_gop", "0", 0);          // Allow non-closed GOPs (vital for GDR)
    av_dict_set(&opts, "hevc-param-change-ignore", "1", 0); // Ignore negligible param changes
    av_dict_set(&opts, "err_detect", "ignore_err", 0); // Aggressively ignore errors
    av_dict_set(&opts, "flags", "+output_corrupt", 0);
    av_dict_set(&opts, "flags2", "+showall", 0);

    
    // QSV specific options
    if (use_qsv) {
        av_dict_set(&opts, "async_depth", "1", 0);
    }

    if (avcodec_open2(av_ctx, codec, &opts) < 0) {
        fprintf(stderr, "Could not open codec\n");
        return false;
        // exit(1);
    }
    av_dict_free(&opts);

    printf("Decoder succesfully initialized.\n");
    return true;
}

std::string av_error_to_string(int errnum) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(errnum, errbuf, AV_ERROR_MAX_STRING_SIZE);
    return std::string(errbuf);
}

void VdecFfmpeg::feed_packet_to_decoder(void* data_p,int data_len, int64_t pts, uint64_t recv_ts, uint8_t nal_type, uint32_t capture_delay_us, uint32_t processing_delay_us, bool is_key_override){	

    // Update stats immediately
    {
        std::lock_guard<std::mutex> lock(decoding_stats_mutex);
        decoding_stats[pts] = timing_stats_t{ 
            .recv_start_us = recv_ts,
            .decode_start_us = get_time_us(),
            .tx_capture_delay_us = capture_delay_us,
            .tx_processing_delay_us = processing_delay_us,
            .is_keyframe = is_key_override,
        };
    }

    if (data_len <= 0) return;

    // We don't request an IDR immediately anymore. 
    // Instead, we wait for the first decoding failure below.
    
    // if (!first_successful_decode && frame_count == 0 && idr_request_callback) {
    //     uint64_t now_ms = get_time_ms();
    //     if (now_ms - last_idr_request_ms > 5000) { // Throttle: max once per 2 seconds
    //         printf("VDEC: Cold start detected, requesting IDR from TX...\n");
    //         idr_request_callback();
    //         last_idr_request_ms = now_ms;
    //     }
    // }

    av_packet_unref(pkt);
    if (av_new_packet(pkt, data_len) < 0) return;
    memcpy(pkt->data, data_p, data_len);
    
    pkt->pts = pts;
    pkt->dts = pts;

    // Check if it's an IDR frame or has been manually marked
    bool is_idr = (nal_type == 19 || nal_type == 20 || nal_type == 21);
    if (is_idr || is_key_override) {
        pkt->flags |= AV_PKT_FLAG_KEY;
    }

    int ret = avcodec_send_packet(av_ctx, pkt);
    if (ret < 0) {
        consecutive_errors++;
    } else {
        frame_count++;
        // Track non-header inputs
        if (nal_type < 32) packets_input++;
    }
    
    // Watchdog check
    // if (packets_input - frames_output > 120) {
    //     printf("!!!! VDEC STARVATION DETECTED (In: %lld, Out: %lld, Lag: %lld). Resetting decoder.\n", 
    //            packets_input, frames_output, packets_input - frames_output);
    //     packets_input = 0;
    //     frames_output = 0;
    //     consecutive_errors = 0;
    //     avcodec_flush_buffers(av_ctx);
    //     // Force IDR request after flush
    //     if (idr_request_callback) idr_request_callback();
    // }
    
    receive_avframe();
    
    // Request IDR only once on startup if we encounter something we cannot decode
    if (!idr_requested && !first_successful_decode && idr_request_callback && consecutive_errors > 0) {
        printf("VDEC: First decoding failure detected on startup. Requesting IDR from TX...\n");
        idr_request_callback();
        idr_requested = true;
    }
}

AVFrame* VdecFfmpeg::next_frame_slot() {
    // 1. Advance cursor
    frame_ring_cur = (frame_ring_cur + 1) % FRAME_RING_SIZE;

    // 2. Get the frame pointer directly from the ring buffer
    AVFrame* frame = frame_ring[frame_ring_cur];

    // 3. Unreference (clean) the existing frame's data
    // This is crucial for releasing buffers associated with the previous decoded content.
    // The AVFrame structure itself is *reused*, only its internal data is reset/freed.
    av_frame_unref(frame);
    return frame;
}

void VdecFfmpeg::receive_avframe() {
    int ret = 0;
    while (ret >= 0) {
        AVFrame* frame = next_frame_slot();
        ret = avcodec_receive_frame(av_ctx, frame);
        // printf("avcodec_receive_frame ret=%d\n", ret);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            // CRITICAL FIX: Track decode errors that don't produce frames
            consecutive_errors++;
            
            // static int err_print_throttle = 0;
            // if (err_print_throttle++ % 30 == 0) {
            //     char errbuf[128];
            //     av_strerror(ret, errbuf, sizeof(errbuf));
            //     fprintf(stderr, "VDEC: Error during decoding (consecutive=%d, ret=%d: %s)\n", 
            //         consecutive_errors, ret, errbuf);
            // }
            
            // // IMMEDIATE RECOVERY CHECK: Don't wait until process_avframe
            // if (consecutive_errors >= 30 && idr_request_callback) {
            //     uint64_t now_ms = get_time_ms();
            //     if (now_ms - last_idr_request_ms > 500) {
            //         printf("VDEC: Decoder in error state (%d errors). Requesting IDR from TX...\n", consecutive_errors);
            //         idr_request_callback();
            //         last_idr_request_ms = now_ms;
            //     }
            // }
            
            // if (consecutive_errors >= 120) {
            //     printf("!!!! Persistent decoding errors (%d). Performing FULL decoder reset. !!!!\n", consecutive_errors);
            //     consecutive_errors = 0;
            //     first_successful_decode = false;
            //     avcodec_flush_buffers(av_ctx);
            //     init_decoder(NULL, 0);
            //     return;
            // }
            
            break;
        }

        uint64_t dec_end = get_time_us();
        uint64_t d_start = 0;
        uint64_t r_start = 0;
        uint32_t cap_delay = 0;
        uint32_t proc_delay = 0;
        bool is_keyframe = false;

        bool found_stats = false;
        timing_stats_t t_stats = {};
        {
            std::lock_guard<std::mutex> lock(decoding_stats_mutex);
            auto it = decoding_stats.find(frame->pts);
            if (it != decoding_stats.end()) {
                found_stats = true;
                t_stats = it->second;
                decoding_stats.erase(it);
            }
        }

        if (found_stats) {
            d_start = t_stats.decode_start_us;
            r_start = t_stats.recv_start_us;
            cap_delay = t_stats.tx_capture_delay_us;
            proc_delay = t_stats.tx_processing_delay_us;
            is_keyframe = t_stats.is_keyframe;
        }

        process_avframe(frame, d_start, dec_end, r_start, cap_delay, proc_delay, is_keyframe);
        
        // Advance to the next slot for the potentially next frame in this same call
        // (already at next slot because process_avframe just uses the frame we gave it)
    }
}

void VdecFfmpeg::process_avframe(AVFrame* frame, uint64_t dec_start_ts, uint64_t dec_end_ts, uint64_t recvts, uint32_t capture_delay_us, uint32_t processing_delay_us, bool is_keyframe) {
    DecodedUnit du;
    memset(&du, 0, sizeof(du));
    du.frame_ref = NULL;

    bool ok = false;
    AVFrame* allocated_frame = NULL;

    du.has_prime_fd = false;
    du.recv_ts = recvts;
    du.dec_start_ts = dec_start_ts;
    du.dec_end_ts = dec_end_ts;
    du.tx_capture_delay_us = capture_delay_us;
    du.tx_processing_delay_us = processing_delay_us;
    du.is_keyframe = is_keyframe;

    // printf("VDEC: process_avframe format=%d (VAAPI=%d, DRM_PRIME=%d)\n", frame->format, AV_PIX_FMT_VAAPI, AV_PIX_FMT_DRM_PRIME);

    // Convert incompatible pixel format.
    // Convert YUV420P to NV12 for software decoding to ensure correct DRM display
    if (frame->format == AV_PIX_FMT_YUV420P) {
       allocated_frame = convert_to_nv12(frame);
       frame = allocated_frame;
    } 
    
    if (frame->format == AV_PIX_FMT_DRM_PRIME) {
        AVDRMFrameDescriptor *drm_desc = (AVDRMFrameDescriptor*)frame->data[0];
        if (!drm_desc) {
            fprintf(stderr, "ERROR: Invalid AVDRMFrameDescriptor in AVFrame.\n");
            if (allocated_frame) av_frame_free(&allocated_frame);
            return;
        }

        // The PRIME FD for the first object/plane
        du.prime_fd = drm_desc->objects[0].fd;
        du.has_prime_fd = true;
        du.drm_pixel_format = DRM_FORMAT_NV12; // Default for most HW decoders
        populate_drm(frame, &du);
        ok = true;
#ifdef USE_VAAPI
    }  else if (frame->format == AV_PIX_FMT_VAAPI) {
        du.has_prime_fd = false; 
        ok = populate_drm_from_vaapi(frame, &du);
#endif
    } else if (frame->format == AV_PIX_FMT_QSV) {
         AVFrame *mapped_frame = av_frame_alloc();
         if (mapped_frame) {
             mapped_frame->format = AV_PIX_FMT_VAAPI;
#ifdef USE_VAAPI
             if (av_hwframe_map(mapped_frame, frame, 0) == 0) {
                 du.has_prime_fd = false;
                 ok = populate_drm_from_vaapi(mapped_frame, &du);
             }
#else
             (void)frame; // Suppress unused
#endif
             av_frame_free(&mapped_frame);
         }
    } else if (frame->format == AV_PIX_FMT_NV12) {
        du.drm_pixel_format = DRM_FORMAT_NV12;
        std::copy_n(frame->data, 8, du.data);
        std::copy_n(frame->linesize, 4, du.linesize);
        ok = true;
    } else if (frame->format == AV_PIX_FMT_YUV420P) {
        du.drm_pixel_format = DRM_FORMAT_YUV420;
        allocated_frame = av_frame_clone(frame);
        if (allocated_frame) {
            std::copy_n(allocated_frame->data, 8, du.data);
            std::copy_n(allocated_frame->linesize, 4, du.linesize);
            ok = true;
        }
    } else if (frame->format == 12 /* AV_PIX_FMT_YUVJ420P */ || frame->format == 71 /* QSV/Different Platform YUVJ */) {
         // Treat YUVJ420P (Full Range) same as YUV420P for simple display purposes
         du.drm_pixel_format = DRM_FORMAT_YUV420;
         allocated_frame = av_frame_clone(frame);
         if (allocated_frame) {
             std::copy_n(allocated_frame->data, 8, du.data);
             std::copy_n(allocated_frame->linesize, 4, du.linesize);
             ok = true;
         }
    } else {
        printf("VDEC: unknown format %d (VAAPI is %d)\n", frame->format, (int)AV_PIX_FMT_VAAPI);
        if (allocated_frame) av_frame_free(&allocated_frame);
        exit(1);
    }

    // RECOVERY LOGIC MUST RUN FIRST - before checking if frame processing succeeded
    // This ensures recovery triggers even when VAAPI sync or DRM export fails
    
    if (frame->decode_error_flags || !ok) {
        if (frame->decode_error_flags) {
             static int frame_err_throttle = 0;
             if (frame_err_throttle++ % 30 == 0) {
                 printf("VDEC: Received corrupt frame (flags=0x%x)\n", frame->decode_error_flags);
             }
        }
        consecutive_errors++;
    } else {
        // Successfully decoded
        frames_output++;
        if (consecutive_errors > 0) {
            printf("VDEC: Recovered clean frame after %d errors\n", consecutive_errors);
            consecutive_errors = 0;
        }
        last_successful_decode_ms = get_time_ms();
        if (!first_successful_decode) {
            first_successful_decode = true;
            printf("VDEC: First successful CLEAN decode achieved!\n");
        }
    }

    // // TIME-BASED ERROR RECOVERY (more aggressive than counter-based)
    // uint64_t now_ms = get_time_ms();
    // if (first_successful_decode && last_successful_decode_ms > 0) {
    //     uint64_t time_since_good_frame = now_ms - last_successful_decode_ms;
        
    //     if (time_since_good_frame > 2000) {  // 2 seconds without clean frame
    //         printf("!!!! No successful decode for %llu ms. Performing FULL decoder reset. !!!!\n", (unsigned long long)time_since_good_frame);
    //         consecutive_errors = 0;
    //         first_successful_decode = false;
    //         last_successful_decode_ms = now_ms;  // Prevent rapid resets
    //         avcodec_flush_buffers(av_ctx);
    //         init_decoder(NULL, 0);
    //         if (allocated_frame) av_frame_free(&allocated_frame);
    //         return;
    //     } else if (time_since_good_frame > 500 && idr_request_callback) {
    //         if (now_ms - last_idr_request_ms > 3000) {
    //             printf("VDEC: Errors for %llu ms. Requesting IDR from TX...\n", (unsigned long long)time_since_good_frame);
    //             idr_request_callback();
    //             last_idr_request_ms = now_ms;
    //         }
    //     }
    // }

    // // FALLBACK: Counter-based recovery (if time-based doesn't catch it)
    // if (consecutive_errors >= 30 && idr_request_callback) {
    //     // First line of defense: Request IDR from TX
    //     // Throttled to max once per 5 seconds to avoid flooding TX
    //     if (now_ms - last_idr_request_ms > 5000) {
    //         printf("VDEC: Decoder in error state (%d errors). Requesting IDR from TX...\n", consecutive_errors);
    //         idr_request_callback();
    //         last_idr_request_ms = now_ms;
    //     }
    // }
    
    // if (consecutive_errors >= 120) {
    //     // Second line of defense: Full decoder reset if IDR request didn't help
    //     printf("!!!! Persistent decoding errors (%d). Performing FULL decoder reset. !!!!\n", consecutive_errors);
    //     consecutive_errors = 0;
    //     first_successful_decode = false;
    //     last_successful_decode_ms = now_ms;
    //     avcodec_flush_buffers(av_ctx);
    //     init_decoder(NULL, 0);
    //     if (allocated_frame) av_frame_free(&allocated_frame);
    //     return;
    // }

    // NOW check if frame processing failed - if so, discard and return
    if (!ok) {
        if (allocated_frame) av_frame_free(&allocated_frame);
        return;
    }

    du.width = frame->width;
    du.height = frame->height;
    du.pts = frame->pts;
    
    // PROPER OWNERSHIP FIX: Move the ring buffer frame's data into a new AVFrame
    // that the DecodedUnit's shared_ptr will own. This prevents the ring buffer
    // from overwriting the frame data while it's still being rendered/presented.
    AVFrame* renderer_frame = av_frame_alloc();
    if (allocated_frame) {
        // Converted frame (SW decoding), it already owns its data
        av_frame_move_ref(renderer_frame, allocated_frame);
        av_frame_free(&allocated_frame);
    } else {
        // HW frame (VAAPI/DRM_PRIME), move reference from ring buffer slot
        av_frame_move_ref(renderer_frame, frame);
    }

    du.frame_ref = std::shared_ptr<void>(renderer_frame, [](void* ptr) {
        AVFrame* f = (AVFrame*)ptr;
        av_frame_free(&f);
    });

    renderer->queue_frame(std::make_shared<DecodedUnit>(du));
}



// Function to populate DecodedUnit from an AVFrame
// Parameters:
//   unit: Pointer to the DecodedUnit struct to populate.
//   frame: The decoded AVFrame.
//   hw_device_ctx: The hardware device context (e.g., from dec_ctx->hw_device_ctx).
//   va_display: The VADisplay from your VA-API setup.
// Returns: 0 on success, <0 on failure.
#ifdef USE_VAAPI
bool VdecFfmpeg::populate_drm_from_vaapi(AVFrame *hw_frame, DecodedUnit *du) {
    VASurfaceID va_surface = (VASurfaceID)(uintptr_t)hw_frame->data[3];

    AVHWDeviceContext *dev_ctx = (AVHWDeviceContext *)hw_device_ctx->data;
    AVVAAPIDeviceContext *vaapi_ctx = (AVVAAPIDeviceContext *)dev_ctx->hwctx;
    VADisplay va_dpy = vaapi_ctx->display;

    VAStatus va_status = vaSyncSurface(va_dpy, va_surface);
    if (va_status != VA_STATUS_SUCCESS) {
        consecutive_errors++; // Hardware sync errors contribute to reset logic
        static int sync_err_count = 0;
        if (sync_err_count++ % 30 == 0) {
            fprintf(stderr, "vaSyncSurface failed: %d (%s). Frame discarded to enable recovery.\n", va_status, vaErrorStr(va_status));
        }
        // CRITICAL FIX: Return false immediately - do NOT queue corrupt frames
        // This allows consecutive_errors to accumulate and trigger proper recovery
        return false;
    }


    if (!exported_surfaces.count(va_surface)) {
        VADRMPRIMESurfaceDescriptor prime_desc;
        va_status = vaExportSurfaceHandle(
            va_dpy,                // VA display
            va_surface,
            VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
            VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS,
            &prime_desc
        );

        //vaDestroySurfaces(va_dpy, &va_surface, 1);

        if (va_status != VA_STATUS_SUCCESS) {
            fprintf(stderr, "vaExportSurfaceHandle failed: %d (%s)\n", va_status, vaErrorStr(va_status));
            return false;
        }

        if (prime_desc.num_objects < 1 || prime_desc.num_layers < 1) {
            fprintf(stderr, "Invalid VA surface descriptor\n");
            return false;    
        }

        if (prime_desc.layers[0].drm_format != DRM_FORMAT_NV12) {
            auto fmt = prime_desc.layers[0].drm_format;
            fprintf(stderr, "Unexpected prime_desc.layers[0].drm_format 0x%08X (%c%c%c%c)\n", 
                fmt,
                fmt & 0xFF,
                (fmt >> 8) & 0xFF,
                (fmt >> 16) & 0xFF,
                (fmt >> 24) & 0xFF);
            return false;    
        }

        for (unsigned int i = 0; i < prime_desc.layers[0].num_planes; ++i) {
            if (i >= 4) { // Only handle up to 4 planes for drmModeAddFB2
                fprintf(stderr, "More than 4 planes for layer 0, skipping excess.\n");
                break;
            }

            // Get the object index for the current plane from prime_desc.layers[0].object_index
            unsigned int obj_idx = prime_desc.layers[0].object_index[i];

            if (obj_idx >= prime_desc.num_objects) {
                fprintf(stderr, "Invalid object_index %u for plane %u\n", obj_idx, i);
                return false;
            }

            int prime_fd = prime_desc.objects[obj_idx].fd;
            uint64_t modifier = prime_desc.objects[obj_idx].drm_format_modifier;
            uint32_t pitch = prime_desc.layers[0].pitch[i];
            uint32_t offset = prime_desc.layers[0].offset[i];

            if (prime_fd < 0) {
                fprintf(stderr, "Invalid FD for object %u (plane %u)\n", obj_idx, i);
                return false;
            }
            du->pitches[i] = pitch;
            du->offsets[i] = offset;
            du->modifiers[i] = modifier;
        }

        // Zero out unused planes (important for drmModeAddFB2)
        for (int i = prime_desc.layers[0].num_planes; i < 4; ++i) {
            du->handles[i] = 0;
            du->pitches[i] = 0;
            du->offsets[i] = 0;
            du->modifiers[i] = 0;
        }
        du->has_prime_fd = true;
        du->prime_fd = prime_desc.objects[0].fd;
        du->width = prime_desc.width;
        du->height = prime_desc.height;
        du->drm_pixel_format = DRM_FORMAT_NV12; // FIX: Explicitly set format!
        exported_surfaces[va_surface] = *du;
        // printf("surface %d exported to %d\n", va_surface, prime_desc.objects[0].fd);
    } else {
        // reused cached du prime attributes.
        auto base_du = exported_surfaces[va_surface];
        for (int i = 0; i < 4; ++i) {
            du->handles[i] = base_du.handles[i];
            du->pitches[i] = base_du.pitches[i];
            du->offsets[i] = base_du.offsets[i];
            du->modifiers[i] = base_du.modifiers[i];
        }
        du->prime_fd = base_du.prime_fd;
        du->has_prime_fd = base_du.has_prime_fd;
        du->width = base_du.width;
        du->height = base_du.height;
        du->drm_pixel_format = base_du.drm_pixel_format;
    }
    return true;
}
#endif

void VdecFfmpeg::populate_drm(AVFrame* frame, DecodedUnit* du) {
    AVDRMFrameDescriptor *drm_desc = (AVDRMFrameDescriptor*)frame->data[0];
        
    if (drm_desc->nb_layers == 0) {
        fprintf(stderr, "ERROR: AVDRMFrameDescriptor has no layers.\n");
        return;
    }

    du->drm_pixel_format = drm_desc->layers[0].format;

    // Since object_index is 0 for both planes, they use the same imported GEM handle.
    if (drm_desc->layers[0].planes[0].object_index != 0 || drm_desc->layers[0].planes[1].object_index != 0) {
        // This case would mean planes come from different GEM objects, requiring
        // multiple drmPrimeFDToHandle calls if drm_desc->objects had multiple FDs.
        // Your descriptor shows object_index=0 for both, so this else isn't hit.
        fprintf(stderr, "ERROR: NV12 planes are expected to use the same object_index (0) from descriptor in this setup.\n");
        return;
    }

    du->pitches[0] = drm_desc->layers[0].planes[0].pitch; // Pitch for plane 0 from descriptor
    du->pitches[1] = drm_desc->layers[0].planes[1].pitch; // Pitch for plane 1 from descriptor

    du->offsets[0] = drm_desc->layers[0].planes[0].offset; // Offset for plane 0 from descriptor
    du->offsets[1] = drm_desc->layers[0].planes[1].offset; // Offset for plane 1 from descriptor
    
    // Set modifiers if available
    for (int i = 0; i < drm_desc->layers[0].nb_planes && i < 4; i++) {
        int obj_idx = drm_desc->layers[0].planes[i].object_index;
        du->modifiers[i] = drm_desc->objects[obj_idx].format_modifier;
    }

    // Sanity checks
    if (du->pitches[0] == 0 || du->pitches[1] == 0) {
        fprintf(stderr, "ERROR: Zero pitch from descriptor (Plane0_pitch=%u, Plane1_pitch=%u)\n",
                du->pitches[0], du->pitches[1]);
        return;
    }
        // offsets[0] should be 0. offsets[1] should be non-zero for NV12.
    if (du->offsets[1] == 0 && du->pitches[1] != 0) { // Check if UV plane has offset if it has pitch
            fprintf(stderr, "Warning: UV plane (plane 1) offset is 0, pitch is %u. This might be okay if it's a separate BO, but unusual for single BO NV12.\n", du->pitches[1]);
    }
}

// __FRAME_THREAD__
//
// - run_frame thread.
void VdecFfmpeg::run_frame()
{
	printf("vdec::run_frame start. (Pipeline currently uses synchronous feed_packet_to_decoder)\n");
    SchedulingHelper::configure_thread(SchedulingHelper::ThreadRole::VDec);
	printf("vdec::run_frame done.\n");
}



AVFrame* VdecFfmpeg::convert_to_nv12(AVFrame* decoded_yuv420p_frame ) {
    AVFrame* nv12_frame = av_frame_alloc();
    struct SwsContext *sws_ctx = nullptr;
    // Setup NV12 frame properties
    nv12_frame->format = AV_PIX_FMT_NV12;
    nv12_frame->width  = decoded_yuv420p_frame->width;
    nv12_frame->height = decoded_yuv420p_frame->height;
    nv12_frame->pts = decoded_yuv420p_frame->pts;
    if (av_frame_get_buffer(nv12_frame, 0) < 0) { // Use alignment 0 for sws_scale compatibility
        fprintf(stderr, "Could not allocate buffer for NV12 frame\n");
        // Handle error
        return NULL;
    }

    sws_ctx = sws_getContext(
        decoded_yuv420p_frame->width, decoded_yuv420p_frame->height, (AVPixelFormat)decoded_yuv420p_frame->format,
        nv12_frame->width, nv12_frame->height, (AVPixelFormat)nv12_frame->format,
        SWS_BILINEAR, // Or other scaling algorithm if needed, SWS_POINT for no scaling quality change
        nullptr, nullptr, nullptr
    );

    if (!sws_ctx) {
        fprintf(stderr, "Could not initialize sws context\n");
        // Handle error
        return NULL;
    }

    sws_scale(
        sws_ctx,
        (const uint8_t * const *)decoded_yuv420p_frame->data, decoded_yuv420p_frame->linesize,
        0, decoded_yuv420p_frame->height,
        nv12_frame->data, nv12_frame->linesize
    );

    // Now nv12_frame contains data in AV_PIX_FMT_NV12 format
    // Copy from nv12_frame to your DRM dumb buffer
    // copy_avframe_to_drm_dumb_buffer(nv12_frame, your_drm_buffer_info_for_nv12);

    sws_freeContext(sws_ctx);
    av_frame_unref(decoded_yuv420p_frame); // Unref the original YUV420P frame
    // av_frame_free(&nv12_frame); // Or unref if you pass it on
    return nv12_frame;
}

void VdecFfmpeg::cleanup() {
    // int ret = mpi.mpi->reset(mpi.ctx);
    // assert(!ret);

    // if (mpi.frm_grp) {
    //     ret = mpp_buffer_group_put(mpi.frm_grp);
    //     assert(!ret);
    //     mpi.frm_grp = NULL;
    //     for (int i=0; i<MAX_FRAMES; i++) {
    //         ret = drmModeRmFB(dev->drm_fd, mpi.frame_to_drm[i].fb_id);
    //         if(ret > 0) {
    //             printf("drmModeRmFB(fbid=%d) failed\n", mpi.frame_to_drm[i].fb_id);
    //             continue;
    //         }
    //         struct drm_mode_destroy_dumb dmdd;
    //         memset(&dmdd, 0, sizeof(dmdd));
    //         dmdd.handle = mpi.frame_to_drm[i].handle;
    //         do {
    //             ret = ioctl(dev->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dmdd);
    //         } while (ret == -1 && (errno == EINTR || errno == EAGAIN));
    //         if(ret > 0) {
    //             printf("ioctl(DRM_IOCTL_MODE_DESTROY_DUMB, fbid=%d) failed\n", mpi.frame_to_drm[i].fb_id);
    //         }
    //     }
    // }
        
    // mpp_packet_deinit(&packet);
    // mpp_destroy(mpi.ctx);

    //avcodec_free_context(codec);
    // av_frame_free(&frame);
    av_packet_free(&pkt);
}
void VdecFfmpeg::cleanup_device() {
    if (hw_device_ctx) {
        av_buffer_unref(&hw_device_ctx);
        hw_device_ctx = NULL;
    }
}
