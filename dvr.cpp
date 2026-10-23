
#define MINIMP4_IMPLEMENTATION

#include "dvr.hpp"
#include <unistd.h>
#include <time.h>
#include <atomic>
#include <thread>
#include <deque>
#include <sched.h>
#include <cerrno>
#include <cstring>
#include <pthread.h>
#include "webstream.hpp"
#include "utils/time_util.h"
#include "drm.hpp"
#include "renderer.hpp"   // DecodedUnit, for the RGA recorder
#include "settings.hpp"
#include "frame_mode.hpp"   // g_display_lock, g_dl_camera_fps

// Queued this far, a write waits for the card: ten seconds of the heaviest
// recording, far more than a sync ever takes.
constexpr size_t kWriterMaxQueued = 64u << 20;

DvrWriter::DvrWriter(int fd) : fd_(fd) {
	t_ = std::thread([this] { run(); });
}

DvrWriter::~DvrWriter() {
	{
		std::lock_guard<std::mutex> lk(m_);
		done_ = true;
	}
	work_.notify_one();
	t_.join();
}

int DvrWriter::write(int64_t offset, const void* buf, size_t size) {
	const uint8_t* p = static_cast<const uint8_t*>(buf);
	{
		std::unique_lock<std::mutex> lk(m_);
		room_.wait(lk, [this] { return queued_ < kWriterMaxQueued || failed_; });
		q_.push_back({offset, std::vector<uint8_t>(p, p + size)});
		queued_ += size;
	}
	work_.notify_one();
	return failed_ ? 1 : 0;
}

void DvrWriter::run() {
	pthread_setname_np(pthread_self(), "dvr-writer");
	uint64_t last_sync_us = get_time_us();
	for (;;) {
		std::deque<Chunk> batch;
		bool done;
		{
			std::unique_lock<std::mutex> lk(m_);
			work_.wait_for(lk, std::chrono::milliseconds(250), [this] { return !q_.empty() || done_; });
			batch.swap(q_);
			queued_ = 0;
			done = done_;
		}
		room_.notify_all();
		for (const Chunk& c : batch) {
			size_t off = 0;
			while (off < c.data.size() && !failed_) {
				ssize_t n = pwrite(fd_, c.data.data() + off, c.data.size() - off, (off_t)(c.offset + off));
				if (n > 0) off += (size_t)n;
				else if (!(n < 0 && errno == EINTR)) {
					printf("DVR: write failed: %s\n", n < 0 ? strerror(errno) : "no progress");
					failed_ = true;
					room_.notify_all();
				}
			}
		}
		// Once a second, push what has been written onto the card: until then
		// it is only in the page cache, and on the exFAT DVR partition even the
		// file's length is not updated - a power cut would lose all of it. With
		// fragmented MP4 this bounds the loss to about the last second.
		const uint64_t now = get_time_us();
		if (done || now - last_sync_us >= 1000000) {
			last_sync_us = now;
			fsync(fd_);
		}
		if (done) {
			std::lock_guard<std::mutex> lk(m_);
			if (q_.empty()) return;
		}
	}
}

int write_callback(int64_t offset, const void *buffer, size_t size, void *token){
    return static_cast<DvrWriter*>(token)->write(offset, buffer, size);
}


int DVR::init(int frm_width, int frm_height) {
	if (!record_screen) {
		if(format == DvrFormat::RAW) {
			return 0;
		}
		writer.reset(new DvrWriter(fileno(dvr_file)));
		mux = MP4E_open(0 /*sequential_mode*/, format == DvrFormat::FMP4, writer.get(), write_callback);
		// Fragmented while recording, so a power cut loses a second at most;
		// plain once stopped, so it plays anywhere - phone browsers will not
		// play a fragmented file from its URL.
		if (format == DvrFormat::FMP4)
			MP4E_set_finalize(mux);
		// The camera's 100-144 fps: QuickTime played it as slow motion, slowed
		// down between a normal-speed first and last few seconds.
		MP4E_set_full_frame_rate(mux);
		// MP4E_open returns NULL when the file refuses its first write (a full
		// card), and add_track would dereference it before checking.
		if (!mux || MP4E_STATUS_OK != mp4_h26x_write_init(&mp4wr, mux, frm_width, frm_height, codec==VideoCodec::H265))
		{
			printf("error: mp4_h26x_write_init failed\n");
			// No recording then: the muxer closed before the file it writes
			// into, and the file too, so run_dvr does not write raw NAL units
			// into an .mp4 (it only writes with a file and no muxer for RAW).
			close_mux();
			if (dvr_file) { fclose(dvr_file); dvr_file = NULL; }
		}
		return 0;
	}

	// Screen recording: each slot (every rga_div-th refresh) the screen is
	// rebuilt from what the display scanned out - the decoded picture and the
	// OSD - by RGA into the encoder's own input buffer (screen_tap.hpp), and
	// encoded on the VEPU. Nothing touches the display, so the live picture
	// pays nothing. No RGA, no screen recording: the only other way to see the
	// video plane, a DRM writeback capture, cost the live picture, and
	// glReadPixels sees the OSD alone.
	//
	// ffmpeg is not an option for the encode: this board's libavcodec has
	// only h264_v4l2m2m and no /dev/video* node for it to bind to.
	//
	// The file is on the slot grid (H264Cfr makes an unchanged slot a skip
	// frame): at most enc_fps_cap a second - for a recording dvr_screen_fps,
	// 100 unless set: a 100 Hz screen records every refresh, a 120 Hz one
	// every other (60), 240 Hz every third (80).
	const int bitrate = enc_bitrate > 0 ? enc_bitrate
	                                    : Settings::getInstance().getInt("dvr_screen_kbps", 30000) * 1000;
	if (!disp_dev) {
		printf("DVR: no DRM device - screen recording unavailable\n");
		return -1;
	}
	if (!RgaCompositor::available()) {
		printf("DVR: no RGA (/dev/rga) - screen recording unavailable\n");
		return -1;
	}
	int fps;
	if (leader) {
		// Every follow_k-th of the leader's slots, as near enc_fps_cap as that gets.
		follow_k = (leader->screen_fps + enc_fps_cap - 1) / enc_fps_cap;
		if (follow_k < 1) follow_k = 1;
		rga_div = leader->rga_div * follow_k;
		fps = (leader->screen_fps + follow_k / 2) / follow_k;
		if (fps < 1) fps = 1;
		rga_slot_ticks = leader->rga_slot_ticks * follow_k;
	} else {
		// Refresh Sync FREE holds each refresh as long as a camera frame: the
		// screen's rate is the camera's, whatever its mode says. Taken from the
		// lock's target, not the refresh of the moment - a recording started
		// while the lock was not holding (the video back after a drop) read the
		// mode's 120 Hz and kept every 2nd refresh, 60 fps, for the whole file.
		double period_us = disp_dev->frame_period_us();
		const float cam_fps = g_dl_camera_fps.load(std::memory_order_relaxed);
		if (g_display_lock.load(std::memory_order_relaxed) == 2 && cam_fps > 0 &&
		    1e6 / cam_fps >= period_us * 0.999)
			period_us = 1e6 / cam_fps;
		const int hz = period_us > 0 ? (int)(1e6 / period_us + 0.5) : disp_dev->refresh_hz();
		const int want = enc_fps_cap;
		rga_div = (hz + want - 1) / want;
		if (rga_div < 1) rga_div = 1;
		fps = (hz + rga_div / 2) / rga_div;
		if (fps > enc_fps_cap) fps = enc_fps_cap;
		if (fps < 1) fps = 1;
		rga_slot_ticks = period_us > 0 ? (int)(period_us * rga_div * 0.09 + 0.5) : 90000 / fps;
	}
	screen_fps = fps;
	if (leader) screen_enc.set_input_count(0);   // it reads the leader's
	// H.265 (dvr_screen_codec): the same picture in fewer bits. No skip
	// frames for it yet - an unchanged refresh lengthens the picture before.
	screen_enc.set_hevc(codec == VideoCodec::H265);
	if (!screen_enc.init(frm_width, frm_height, fps, bitrate, true /*BT.601, RGA's*/, enc_max_bps)) {
		printf("DVR: hardware encoder unavailable - screen recording disabled\n");
		return -1;
	}
	if (!screen_enc.hevc() && !screen_cfr.init(screen_enc.header())) {
		printf("DVR: the encoder's stream is one H264Cfr cannot hold on the slot grid - screen recording disabled\n");
		screen_enc.deinit();
		return -1;
	}
	if (!leader) {
		std::vector<int> fds;
		for (int i = 0; i < screen_enc.input_count(); i++) fds.push_back(screen_enc.input_dmabuf_fd(i));
		if (!rga.init(frm_width, frm_height, fds)) {
			printf("DVR: RGA unusable - screen recording unavailable\n");
			screen_enc.deinit();
			return -1;
		}
	}

	if (to_file) {
		if (!dvr_file) {
			printf("DVR: output file not open - screen recording disabled\n");
			return -1;
		}
		writer.reset(new DvrWriter(fileno(dvr_file)));
		mux = MP4E_open(0 /*sequential_mode*/, format == DvrFormat::FMP4, writer.get(), write_callback);
		if (format == DvrFormat::FMP4)
			MP4E_set_finalize(mux);     // as in the camera recording
		// Above 60 fps Apple's players took it for slow motion (as the camera's).
		if (mux && fps > 60) MP4E_set_full_frame_rate(mux);
		if (!mux || MP4E_STATUS_OK != mp4_h26x_write_init(&mp4wr, mux, frm_width, frm_height, screen_enc.hevc())) {
			printf("DVR: mp4_h26x_write_init failed (screen)\n");
			close_mux();   // the file is closed by discard() or the destructor
			return -1;
		}
	}
	// SPS/PPS first, exactly as the FPV path primes its parameter sets.
	if (mux && !screen_enc.header().empty())
		mp4_h26x_write_nal(&mp4wr, screen_enc.header().data(),
		                   (int)screen_enc.header().size(), 90000 / fps);
	if (!to_file)
		printf("DVR: screen encoder for the web view, %dx%d at %d fps, %d kbit/s\n",
		       frm_width, frm_height, screen_fps, bitrate / 1000);
	else if (leader)
		; // said by DvrRecorder ("follows the screen recording")
	else
		printf("DVR: screen recording %dx%d at %d fps, RGA composite of every %s\n",
		       frm_width, frm_height, screen_fps,
		       rga_div == 1 ? "refresh" : rga_div == 2 ? "2nd refresh" : rga_div == 3 ? "3rd refresh"
		                    : (std::to_string(rga_div) + "th refresh").c_str());
	return 0;
}

// The RGA screen recording. Every rga_div-th vblank is one frame of the file
// (a slot). A sampler thread wakes just after that vblank, asks the screen
// tap which picture and OSD the display showed on it, and has RGA composite
// them into a free encoder buffer; this thread encodes them in order. Nothing
// here touches the display, so the live picture pays nothing but memory
// traffic. A slot whose screen did not change is a skip frame - nothing
// composited, nothing encoded - and so is one the sampler could not fill
// (encoder behind, woke too late), so the file stays on the slot grid.
void DVR::run_screen_rga() {
	{
		std::lock_guard<std::mutex> lk(qm);
		shots.clear();
		free_bufs.clear();
		for (int i = 0; i < screen_enc.input_count(); i++) { free_bufs.push_back(i); buf_refs[i] = 0; }
	}
	std::atomic<bool> sampling{true};
	ScreenTap& tap = disp_dev->tap;
	auto fresh_wanted = [&] {
		if (want_fresh.load(std::memory_order_relaxed)) return true;
		std::lock_guard<std::mutex> lk(follow_m);
		return follower && follower->want_fresh.load(std::memory_order_relaxed);
	};
	// Seeded with what the screen shows now, so a recording started with no
	// video flowing records the frozen picture, not black.
	if (!leader) disp_dev->start_screen_tap();

	// Sampler counters, read after it is joined.
	uint64_t n_slots = 0, n_same = 0, n_late = 0, n_busy = 0, n_fail = 0, n_nothing = 0;
	uint32_t blend_hist[64] = {0};   // RGA time, 0.25 ms buckets
	uint32_t blend_max = 0;

	// A follower takes its slots from the leader's encoding thread (hand_on).
	if (leader) {
		std::lock_guard<std::mutex> lk(leader->follow_m);
		leader->follower = this;
	}
	std::thread sampler;
	if (!leader) sampler = std::thread([&] {
		pthread_setname_np(pthread_self(), "dvr-sampler");
		const int div = rga_div;
		int parity = 0;
		uint32_t n = 0, seq0 = 0;
		bool have0 = false;
		int64_t last_slot = -1;
		bool have_last = false, last_video = false;
		uint64_t last_pts = 0, last_dec = 0, last_gen = 0;
		uint32_t last_epoch = 0;
		int lvx = 0, lvy = 0, lvw = 0, lvh = 0;
		while (sampling) {
			uint32_t seq;
			uint64_t vb;
			if (!disp_dev->wait_vblank(&seq, &vb)) { usleep(10000); continue; }
			tap.drain_graveyard();
			if (div > 1) {
				// Sample the refresh after the one pictures land on: 60 fps
				// video on a 120 Hz screen is then taken once per picture,
				// each on the second refresh it is shown.
				if ((n++ % 32) == 0) {
					int dom = tap.dominant_parity(div);
					if (dom >= 0) parity = (dom + 1) % div;
				}
				if ((int)(seq % div) != parity) continue;
			}
			if (!have0) { seq0 = seq; have0 = true; }
			const int64_t slot = ((int64_t)(uint32_t)(seq - seq0) + div / 2) / div;
			if (slot <= last_slot) continue;      // the sampled parity moved
			// Just after the vblank, so its flip event is in.
			const uint64_t at = vb + 1500;
			struct timespec ts = { (time_t)(at / 1000000), (long)((at % 1000000) * 1000) };
			clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
			ScreenPair p;
			if (!tap.sample(seq, vb, p, 3000)) {
				if (have_last) n_late++; else n_nothing++;
				continue;
			}
			n_slots++;
			const DecodedUnit* du = p.video.get();
			bool same = have_last && (du != nullptr) == last_video &&
			                  (!du || (du->pts == last_pts && du->dec_end_ts == last_dec &&
			                           du->buf_epoch == last_epoch)) &&
			                  p.osd_gen == last_gen && p.vx == lvx && p.vy == lvy && p.vw == lvw && p.vh == lvh;
			// A viewer waiting for a keyframe (ours, or our follower's) gets
			// one only from a picture that is encoded: this slot is, anyway.
			if (same && fresh_wanted()) same = false;
			if (same) {
				p.video.reset();
				tap.sample_done();
				n_same++;
				{ std::lock_guard<std::mutex> lk(qm); shots.push_back({-1, slot}); }
				qcv.notify_one();
				last_slot = slot;
				continue;
			}
			int idx = -1;
			{
				std::lock_guard<std::mutex> lk(qm);
				if (!free_bufs.empty()) { idx = free_bufs.front(); free_bufs.pop_front(); buf_refs[idx] = 1; }
			}
			if (idx < 0) { p.video.reset(); tap.sample_done(); n_busy++; continue; }   // encoder behind
			uint32_t stride = 0;
			const int osd_fd = p.osd_fb ? disp_dev->osd_fb_dmabuf(p.osd_fb, &stride) : -1;
			uint32_t us = 0;
			const bool ok = rga.compose(p, osd_fd, stride, idx, &us);
			// The picture goes back to the decoder from here: let go before
			// telling the tap, which a resolution change waits on.
			p.video.reset();
			tap.sample_done();
			if (!ok) {
				release_buf(idx);
				n_fail++;
				continue;
			}
			blend_hist[us / 250 < 63 ? us / 250 : 63]++;
			if (us > blend_max) blend_max = us;
			have_last = true;
			last_video = du != nullptr;
			last_pts = du ? du->pts : 0;
			last_dec = du ? du->dec_end_ts : 0;
			last_epoch = du ? du->buf_epoch : 0;
			last_gen = p.osd_gen;
			lvx = p.vx; lvy = p.vy; lvw = p.vw; lvh = p.vh;
			{ std::lock_guard<std::mutex> lk(qm); shots.push_back({idx, slot}); }
			qcv.notify_one();
			last_slot = slot;
		}
	});

	const int period = rga_slot_ticks;   // 90 kHz ticks
	// A gap of missed slots is filled up to this; the slot numbers come from
	// the vblank counter, so a bigger one is not a real gap.
	const int64_t kMaxGap = (int64_t)screen_fps * 600;
	int64_t last_written = -1;
	uint64_t frames = 0, skips = 0;
	auto write = [&](const uint8_t* d, size_t len, int dur) {
		to_sink(d, len, dur);
		if (!mux) return;
		auto r = mp4_h26x_write_nal(&mp4wr, d, (int)len, dur);
		if (!(MP4E_STATUS_OK == r || MP4E_STATUS_BAD_ARGUMENTS == r))
			printf("DVR: rga mp4_h26x_write_nal err %d\n", r);
	};
	int held = 0;   // H.265: slots the next picture's duration takes in (no skip frames)
	auto skip = [&]() {
		if (screen_enc.hevc()) { held += period; skips++; return true; }
		std::vector<uint8_t> sk = screen_cfr.skip_frame();
		if (sk.empty()) return false;
		write(sk.data(), sk.size(), period);
		skips++;
		return true;
	};
	// Leader: the latest composite, kept back from the sampler while a
	// follower may still want it, and what the follower was last handed.
	int latest = -1;
	uint64_t latest_seq = 0, taken_seq = 0;
	int64_t f_slot = -1;
	auto keep_latest = [&](int idx) {
		bool following;
		{ std::lock_guard<std::mutex> lk(follow_m); following = follower != nullptr; }
		if (!following) { release_buf(idx); return; }
		if (latest >= 0) release_buf(latest);
		latest = idx;
		latest_seq++;
	};
	auto hand_on = [&](int64_t slot) {
		std::lock_guard<std::mutex> lk(follow_m);
		if (!follower) {
			if (latest >= 0) { release_buf(latest); latest = -1; }
			f_slot = -1;
			return;
		}
		const int64_t fs = slot / follower->follow_k;
		if (fs <= f_slot) return;
		f_slot = fs;
		if (latest >= 0 && latest_seq != taken_seq) {
			// At most two of our inputs in its hands: one stalled must not
			// leave our sampler without a buffer (our pictures lost). Its slot
			// then becomes a skip frame, and the next picture goes to it.
			bool room;
			{
				std::lock_guard<std::mutex> q(qm);
				room = f_held < 2;
				if (room) { buf_refs[latest]++; f_held++; }
			}
			if (room) {
				taken_seq = latest_seq;
				follower->push_shot(latest, fs);
			}
		} else {
			follower->push_shot(-1, fs);    // unchanged since its last
		}
	};
	auto handle = [&](const Shot& s) {
		if (s.idx < 0 && last_written < 0) { if (!leader) hand_on(s.slot); return; }   // nothing to repeat yet
		// Slots nobody filled (the sampler late, the encoder behind) and
		// unchanged ones are skip frames: the file stays on the slot grid.
		if (last_written >= 0) {
			int64_t gap = s.slot - last_written - 1;
			if (gap > kMaxGap) gap = kMaxGap;
			for (int64_t i = 0; i < gap; i++) if (!skip()) break;
		}
		if (s.idx < 0) skip();
		if (s.idx >= 0) {
			idr_if_wanted();
			auto out = [&](const uint8_t* p_hw, size_t len, bool) {
				// One bulk copy out of the encoder's uncached output first (see run_screen).
				pkt_copy_.assign(p_hw, p_hw + len);
				if (screen_enc.hevc()) {
					write(pkt_copy_.data(), len, period + held);
					held = 0;
					return;
				}
				std::vector<uint8_t> au = screen_cfr.rewrite(pkt_copy_.data(), len);
				write(au.data(), au.size(), period);
			};
			if (leader) {
				// The leader's composite, read in place, then let go.
				screen_enc.encode_external(leader->screen_enc.input_buffer(s.idx), out);
				leader->follower_done(s.idx);
			} else {
				screen_enc.encode_buffer(s.idx, out);
				keep_latest(s.idx);
			}
			frames++;
		}
		last_written = s.slot;
		if (!leader) hand_on(s.slot);   // after the encode: its picture is the latest
	};
	auto pop = [&](Shot& s, int wait_ms) {
		std::unique_lock<std::mutex> lk(qm);
		if (wait_ms > 0) qcv.wait_for(lk, std::chrono::milliseconds(wait_ms), [&] { return !shots.empty(); });
		if (shots.empty()) return false;
		s = shots.front();
		shots.pop_front();
		return true;
	};

	for (;;) {
		{
			std::lock_guard<std::mutex> lock(mtx);
			if (should_exit) break;
		}
		if (*should_stop) break;
		Shot s;
		if (pop(s, 50)) handle(s);
	}
	sampling = false;
	if (sampler.joinable()) sampler.join();
	if (leader) {
		std::lock_guard<std::mutex> lk(leader->follow_m);
		leader->follower = nullptr;
		leader->follower_draining = true;   // the drain below still reads its inputs
	}
	// Frames already composited are still part of the recording.
	Shot s;
	while (pop(s, 0)) handle(s);
	if (leader) {
		{ std::lock_guard<std::mutex> lk(leader->follow_m); leader->follower_draining = false; }
		leader->follow_cv.notify_all();
	} else {
		tap.set_recording(false);
		// Both stopping at once (exit): a follower still reading our inputs
		// is waited for before they go (run_dvr deinits the encoder next).
		std::unique_lock<std::mutex> lk(follow_m);
		follow_cv.wait_for(lk, std::chrono::seconds(3), [&] { return !follower && !follower_draining; });
	}
	if (latest >= 0) { release_buf(latest); latest = -1; }

	uint64_t total = 0, med_i = 0, p99_i = 0, acc = 0;
	for (uint32_t c : blend_hist) total += c;
	for (int i = 0; i < 64; i++) {
		acc += blend_hist[i];
		if (!med_i && acc * 2 >= total) med_i = i + 1;
		if (!p99_i && acc * 100 >= total * 99) p99_i = i + 1;
	}
	if (leader) {
		printf("DVR: web view encoder (following the recording): %llu frames encoded, %llu skip frames (%d fps)\n",
		       (unsigned long long)frames, (unsigned long long)skips, screen_fps);
		fflush(stdout);
		return;
	}
	printf("DVR: %llu frames encoded, %llu skip frames (%d fps). Sampled %llu slots: %llu unchanged, "
	       "%llu missed (sampler late, or the picture changing size), %llu with the encoder behind, "
	       "%llu composite failures, %llu "
	       "before anything was on screen. RGA ~%.2f ms median, ~%.2f ms p99, %.2f ms max\n",
	       (unsigned long long)frames, (unsigned long long)skips, screen_fps,
	       (unsigned long long)n_slots, (unsigned long long)n_same, (unsigned long long)n_late,
	       (unsigned long long)n_busy, (unsigned long long)n_fail, (unsigned long long)n_nothing,
	       med_i * 0.25, p99_i * 0.25, blend_max / 1000.0);
	fflush(stdout);
}

void DVR::release_buf(int idx) {
	std::lock_guard<std::mutex> lk(qm);
	if (idx < 0 || idx >= MppH264Encoder::kInputBuffers) return;
	if (--buf_refs[idx] <= 0) { buf_refs[idx] = 0; free_bufs.push_back(idx); }
}

void DVR::follower_done(int idx) {
	{ std::lock_guard<std::mutex> lk(qm); if (f_held > 0) f_held--; }
	release_buf(idx);
}

void DVR::push_shot(int idx, int64_t slot) {
	{ std::lock_guard<std::mutex> lk(qm); shots.push_back({idx, slot}); }
	qcv.notify_one();
}

void DVR::idr_if_wanted() {
	{
		std::lock_guard<std::mutex> lk(sink_m);
		if (!sink_wants_key) return;
		sink_wants_key = false;
	}
	const uint64_t now = get_time_us();
	if (now - last_forced_idr_us < 1000000) return;
	last_forced_idr_us = now;
	screen_enc.force_idr();
}

void DVR::stop() {
	{
		std::lock_guard<std::mutex> lock(mtx);
		should_exit = true;
	}
	cv.notify_one();
}

void DVR::run_dvr() {
	printf("DVR::run_dvr start.\n");
	if (record_screen) {
		run_screen_rga();
		if (mux) {
			MP4E_close(mux);
			mp4_h26x_write_close(&mp4wr);
			mux = NULL;
		}
		rga.deinit();
		screen_enc.deinit();
		writer.reset();             // everything written and synced
		if (dvr_file) {
			fclose(dvr_file);
			dvr_file = NULL;
		}
		printf("DVR::run_dvr done.\n");
		return;
	}

	// A sample's duration is the time since the previous picture, which is
	// the stream's real frame rate - not the configured one. A fixed 1/fps
	// played a 14 fps stream back four times too fast. The times are the
	// pictures' capture stamps where the source has them (the air unit's
	// clock): their arrival times carried the radio's jitter, every other
	// sample a little long or short, and stuttered in players. Within a
	// quarter of the nominal interval it snaps to nominal.
	const int nominal = 90000 / (video_framerate > 0 ? video_framerate : 60);
	auto duration_for = [&](uint64_t us) {
		int dur = nominal;
		if (last_sample_us && us > last_sample_us) {
			uint64_t d = us - last_sample_us;
			if (d > 1000000) d = 1000000;
			if (d < 1000)    d = 1000;
			int measured = (int)(d * 90000 / 1000000);
			if (abs(measured - nominal) > nominal / 4) dur = measured;
		}
		last_sample_us = us;
		return dur;
	};

	for (;;) {
		std::unique_lock<std::mutex> lock(mtx);
		cv.wait(lock, [this] { return !dvrQueue.empty() || *should_stop || should_exit; });
		// Drain what is queued before closing: stopping used to leave the
		// last pictures in the queue unwritten.
		if (dvrQueue.empty()) break;
		{
			Queued q = dvrQueue.front();
			dvrQueue.pop();
			lock.unlock();
			std::shared_ptr<std::vector<uint8_t>> frame = q.data;
			// Process the frame
			if (record_screen) {
				// Screen frames go straight from the CRTC to the encoder
				// (run_screen); nothing is queued here.
			} else if (dvr_file) {
				if(format == DvrFormat::RAW) {
					fwrite(frame->data(), frame->size(), 1, dvr_file);
					fflush(dvr_file);
				} else if (mux) {
					const uint8_t* p = frame->data();
					const bool ps = frame->size() > 4 && (codec == VideoCodec::H265
						? ((p[4] >> 1) & 0x3F) >= 32 && ((p[4] >> 1) & 0x3F) <= 34
						: ((p[4] & 0x1F) == 7 || (p[4] & 0x1F) == 8));
					auto res = mp4_h26x_write_nal(&mp4wr, frame->data(), frame->size(),
					                              ps ? nominal : duration_for(q.us));
					if (!ps) {
						if (res == MP4E_STATUS_OK) samples_written++; else samples_dropped++;
					}
					if (!(MP4E_STATUS_OK == res || MP4E_STATUS_BAD_ARGUMENTS == res)) {
						printf("mp4_h26x_write_nal failed with error %d\n", res);
					}
				}
			}
		}
	}
	if (!record_screen && format != DvrFormat::RAW)
		printf("DVR: %llu pictures written, %llu dropped%s\n",
		       (unsigned long long)samples_written, (unsigned long long)samples_dropped,
		       samples_written ? "" : " - NO VIDEO IN THIS FILE");
	if (format != DvrFormat::RAW && mux) {
		MP4E_close(mux);
		mp4_h26x_write_close(&mp4wr);
		mux = NULL;
	}
	if (record_screen) {
		screen_enc.deinit();
	}
	writer.reset();                 // everything written and synced
	if (dvr_file) {
		fclose(dvr_file);
		dvr_file = NULL;
	}
	printf("DVR::run_dvr done.\n");
}


size_t DVR::queue_depth() {
	std::lock_guard<std::mutex> lock(mtx);
	return dvrQueue.size();
}

void DVR::enqueueDvrPacket(std::shared_ptr<std::vector<uint8_t>> frame, uint64_t us) {
	{
		std::lock_guard<std::mutex> lock(mtx);
		dvrQueue.push({frame, us ? us : get_time_us()});
	}
	cv.notify_one();
}

// ---------------------------------------------------------------- DvrRecorder

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include <errno.h>
#include <cstring>
#include <algorithm>
#include <sys/resource.h>
#include <pthread.h>
#include "dvr_recover.hpp"

namespace {

// Threads inherit their creator's scheduling. A recording is started from the
// REC button's thread, which is real-time (SCHED_FIFO 10), and so was every
// thread a recording made: its own, the file writer's and MPP's encoder
// threads. The decoder's own MPP threads (mpp_dec_parser, mpp_dec_hal) are
// ordinary ones, so whatever CPU a recording needed came ahead of the live
// picture's decode. A recording's threads are ordinary ones now: none of them
// has a deadline the pilot sees.
class OrdinaryScheduling {
    public:
        OrdinaryScheduling() {
            restore_ = pthread_getschedparam(pthread_self(), &policy_, &param_) == 0 &&
                       policy_ != SCHED_OTHER;
            if (restore_) {
                struct sched_param p = {};
                pthread_setschedparam(pthread_self(), SCHED_OTHER, &p);
            }
        }
        ~OrdinaryScheduling() {
            if (restore_) pthread_setschedparam(pthread_self(), policy_, &param_);
        }
    private:
        int policy_ = SCHED_OTHER;
        struct sched_param param_ = {};
        bool restore_ = false;
};

// Deletes the oldest fpvOS_*.mp4 / fpvOS_*.h265 recordings in dir - and
// kestrel_*, what earlier builds named them - until
// free space clears min_free_bytes, or there is nothing left to delete. Only
// called from start_recording(), before any recording is open, so there is
// never a live file among the candidates.
void purge_oldest_recordings(const std::string& dir, uint64_t min_free_bytes) {
    struct statvfs st;
    if (statvfs(dir.c_str(), &st) != 0) return;
    uint64_t avail = (uint64_t)st.f_bavail * (uint64_t)st.f_frsize;
    if (avail >= min_free_bytes) return;

    DIR* d = opendir(dir.c_str());
    if (!d) return;
    std::vector<std::pair<time_t, std::string>> files;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        std::string name = ent->d_name;
        if (name.rfind("fpvOS_", 0) != 0 && name.rfind("kestrel_", 0) != 0) continue;
        std::string path = dir + "/" + name;
        struct stat fst;
        if (stat(path.c_str(), &fst) != 0 || !S_ISREG(fst.st_mode)) continue;
        files.emplace_back(fst.st_mtime, path);
    }
    closedir(d);
    std::sort(files.begin(), files.end());

    for (const auto& f : files) {
        if (avail >= min_free_bytes) break;
        struct stat fst;
        uint64_t freed = (stat(f.second.c_str(), &fst) == 0) ? (uint64_t)fst.st_size : 0;
        if (remove(f.second.c_str()) == 0) {
            printf("DVR: low on space, removed oldest recording %s\n", f.second.c_str());
            avail += freed;
        } else {
            printf("DVR: low on space, failed to remove %s: %s\n", f.second.c_str(), strerror(errno));
        }
    }
}

} // namespace

DvrRecorder& DvrRecorder::instance() {
    static DvrRecorder inst;
    return inst;
}

void DvrRecorder::configure(VideoCodec codec, int framerate, DvrFormat fmt,
                            volatile bool* stop_signal, const std::string& dir) {
    std::lock_guard<std::mutex> lk(m_);
    codec_ = codec;
    fps_   = framerate > 0 ? framerate : 60;
    fmt_   = fmt;
    stop_  = stop_signal;
    dir_   = dir.empty() ? std::string("/media/dvr") : dir;
    configured_ = true;

    // Recordings a power cut or crash left fragmented, and those from before
    // kestrel finalized them, made playable: once, at start, on a thread of
    // its own at the lowest priority - a long one takes a while to read.
    static bool recovering = false;
    if (!recovering) {
        recovering = true;
        const std::string d = dir_;
        std::thread([d] {
            pthread_setname_np(pthread_self(), "dvr-recover");
            setpriority(PRIO_PROCESS, 0, 19);   // this thread only, on Linux
            struct stat st, parent;
            if (stat(d.c_str(), &st) != 0 || stat((d + "/..").c_str(), &parent) != 0 ||
                st.st_dev == parent.st_dev)
                return;                          // no recordings partition mounted
            dvr_recover_recordings(d, [] { return DvrRecorder::instance().current_file(); });
        }).detach();
    }
}

void DvrRecorder::set_frame_size(int w, int h) {
    if (w <= 0 || h <= 0) return;
    std::lock_guard<std::mutex> lk(m_);
    fw_ = w; fh_ = h;
}

void DvrRecorder::set_parameter_sets(const std::vector<std::vector<uint8_t>>& ps) {
    if (ps.empty()) return;
    std::lock_guard<std::mutex> lk(m_);
    ps_ = ps;
}

void DvrRecorder::set_screen_size(int w, int h) {
    if (w <= 0 || h <= 0) return;
    std::lock_guard<std::mutex> lk(m_);
    sw_ = w; sh_ = h;
}

void DvrRecorder::set_display(DrmDevice* dev) {
    std::lock_guard<std::mutex> lk(m_);
    disp_dev_ = dev;
}


void DvrRecorder::set_screen_mode(bool on) {
    screen_ = on;
}

bool DvrRecorder::screen_mode() const {
    return screen_;
}

bool DvrRecorder::is_recording() const {
    return running_;
}

std::string DvrRecorder::current_file() const {
    std::lock_guard<std::mutex> lk(m_);
    return file_;
}

// Starting and stopping run under ctl_ only - one at a time - and take m_
// just to read the configuration and to publish or retract the recording.
// They used to hold m_ throughout: a stop joins the DVR's thread, which
// writes the MP4's index and syncs the file, and a start purges old
// recordings and sets the encoder up. Meanwhile feed() on the video ingest
// thread and is_recording() on the OSD thread waited on m_ - the live
// picture or the HUD froze for as long as REC took.
bool DvrRecorder::start_recording() {
    VideoCodec codec;
    int fps, fw, fh, sw, sh;
    DvrFormat fmt;
    volatile bool* stop_signal;
    std::string dir;
    bool configured;
    const bool screen = screen_;
    DrmDevice* disp_dev;
    std::vector<std::vector<uint8_t>> ps;
    {
        std::lock_guard<std::mutex> lk(m_);
        codec = codec_; fps = fps_; fmt = fmt_; stop_signal = stop_; dir = dir_;
        configured = configured_; fw = fw_; fh = fh_; sw = sw_; sh = sh_;
        disp_dev = disp_dev_;
        if (!screen) ps = ps_;
    }
    if (!configured) {
        printf("DVR: not configured, cannot record\n");
        return false;
    }
    // The recordings partition is created and mounted on first boot
    // (S35dvrpart). If it is not there, say so rather than silently writing
    // into the rootfs and filling it - a directory existing is not enough
    // proof of that, since S35dvrpart mkdir's the mount point up front
    // regardless of whether the mount itself succeeded. Compare device IDs
    // with the parent directory instead: identical means dir is just a
    // plain directory sitting on whatever filesystem holds its parent, not a
    // separate mounted partition.
    struct stat st, parent_st;
    if (stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) ||
        stat((dir + "/..").c_str(), &parent_st) != 0 ||
        parent_st.st_dev == st.st_dev) {
        printf("DVR: %s is not a mounted recordings partition - not recording\n", dir.c_str());
        return false;
    }

    // Keep some headroom on the recordings partition itself so an unattended
    // pile of old footage never runs it to zero.
    purge_oldest_recordings(dir, 300ull * 1024 * 1024);
    // DVR::format_timestamped_filename() turns "<dir>/fpvOS.mp4" into
    // "<dir>/fpvOS_YYYYMMDD_HHMMSS.mp4".
    const char* ext = (fmt == DvrFormat::RAW) ? ".h265" : ".mp4";
    std::string base = dir + "/fpvOS" + ext;

    // One screen sampler at a time: the web view's encoder makes way for a
    // screen recording, and then follows its sampler (web_reconcile).
    if (screen) stop_web();

    // Everything made from here on - the DVR's thread, the writer's, the
    // encoder's - is scheduled as an ordinary thread (OrdinaryScheduling).
    OrdinaryScheduling ordinary;

    // Screen recordings are H.264 (or H.265, dvr_screen_codec) in MP4,
    // whatever codec the FPV link uses.
    const bool screen_hevc = Settings::getInstance().getString("dvr_screen_codec", "h265") == "h265";
    std::shared_ptr<DVR> dvr = screen
        ? std::make_shared<DVR>(base, screen_hevc ? VideoCodec::H265 : VideoCodec::H264, fps,
                                fmt == DvrFormat::MP4 ? DvrFormat::MP4 : DvrFormat::FMP4, stop_signal, true)
        : std::make_shared<DVR>(base, codec, fps, fmt, stop_signal, false);
    // The screen sampled at most this often (dvr_screen_fps): 100, every
    // refresh of a 100 Hz screen, the camera's rate in its usual modes -
    // measured on the VRX with H.265: no picture missed, none with the
    // encoder behind, RGA ~5.3 ms a frame, the live picture ~0.5 ms later.
    if (screen) dvr->set_fps_cap(Settings::getInstance().getInt("dvr_screen_fps", 100));
    // The gallery must not take the new file for a finished one while it is
    // being set up: it is the current file from the moment it exists.
    std::string file = dvr->path().substr(dvr->path().find_last_of('/') + 1);
    {
        std::lock_guard<std::mutex> lk(m_);
        file_ = file;
    }
    if (screen && disp_dev) dvr->set_display_dev(disp_dev);
    // Without this the muxer is never created and every frame the writer thread
    // dequeues is silently dropped - the file is opened, the timer runs, and
    // nothing is ever written. Renderer only calls init() on the legacy --dvr
    // object, which is always null now.
    // Screen recordings use the display size; the FPV path uses the video size.
    // A set-up that fails (no display, no RGA, the encoder or the muxer
    // refusing) is not a recording: the thread would run with nothing to
    // encode into - a null display dereferenced in screen mode, else an empty
    // file, REC on the HUD and a web view following a leader that never
    // composites until REC is pressed again.
    const int inited = (screen && sw > 0 && sh > 0) ? dvr->init(sw, sh) : dvr->init(fw, fh);
    if (inited < 0) {
        printf("DVR: not recording - the %s could not be set up (see above)\n",
               screen ? "screen encoder" : "muxer");
        dvr->discard();
        std::lock_guard<std::mutex> lk(m_);
        file_.clear();
        return false;
    }
    pthread_t tid;
    if (pthread_create(&tid, nullptr, DVR::run_dvr_thread, dvr.get()) != 0) {
        printf("DVR: failed to start writer thread\n");
        dvr->discard();
        std::lock_guard<std::mutex> lk(m_);
        file_.clear();
        return false;
    }
    // Replay the parameter sets before any picture data. Without them the
    // muxer has no decoder configuration to put in hvcC and the file plays as
    // black even though the video data is all there. Before the recording is
    // published, so no picture from feed() can get in ahead of them.
    if (!screen) {
        for (const auto& p : ps)
            dvr->enqueueDvrPacket(std::make_shared<std::vector<uint8_t>>(p));
        if (ps.empty())
            printf("DVR: no VPS/SPS/PPS seen yet - they go in ahead of the first "
                   "picture after they arrive\n");
    }

    printf("DVR: recording -> %s\n", dvr->path().c_str());
    {
        std::lock_guard<std::mutex> lk(m_);
        dvr_ = dvr;
        tid_ = tid;
        ps_sent_ = !ps.empty();
        running_ = true;
        rec_screen_ = screen;
    }
    web_reconcile();
    return true;
}

void DvrRecorder::stop_recording() {
    std::shared_ptr<DVR> dvr;
    pthread_t tid;
    std::string file;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!running_) return;
        running_ = false;
        dvr.swap(dvr_);
        tid = tid_;
        tid_ = 0;
        file = file_;   // still the current file until it is complete
    }
    printf("DVR: stopping -> %s\n", file.c_str());
    // The web view's encoder following this recording's sampler goes first.
    if (web_leader_ == dvr) stop_web();
    dvr->stop();
    pthread_join(tid, nullptr);
    dvr.reset();
    {
        std::lock_guard<std::mutex> lk(m_);
        file_.clear();
    }
    // The file is complete now (moov written): its gallery thumbnail can be
    // made in the background, ready before the gallery is opened.
    WebStream::instance().recording_finished(file);
    // A phone still watching gets its own screen encoder back.
    {
        std::lock_guard<std::mutex> lk(m_);
        rec_screen_ = false;
    }
    web_reconcile();
}

// ---- the web view's screen encoder ----

void DvrRecorder::attach_web(DVR* d) {
    WebStream::instance().set_screen_source(d->screen_header(), d->screen_width(), d->screen_height(),
                                            d->screen_period_ticks());
    d->set_sink([](const uint8_t* p, size_t n, int dur) { return WebStream::instance().feed_screen(p, n, dur); });
}

void DvrRecorder::start_web(const std::shared_ptr<DVR>& leader) {
    if (web_dvr_) return;
    int fps, sw, sh;
    volatile bool* stop_signal;
    DrmDevice* disp_dev;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!configured_ || !disp_dev_ || sw_ <= 0 || sh_ <= 0) {
            printf("DVR: web view - the screen is not known yet, no encoder\n");
            return;
        }
        fps = fps_; sw = sw_; sh = sh_; stop_signal = stop_; disp_dev = disp_dev_;
    }
    // A follower reads its leader's inputs: exactly their size.
    if (leader) { sw = leader->screen_width(); sh = leader->screen_height(); }
    OrdinaryScheduling ordinary;
    auto dvr = std::make_shared<DVR>("", VideoCodec::H264, fps, DvrFormat::FMP4, stop_signal, true, false);
    dvr->set_display_dev(disp_dev);
    if (leader) dvr->set_leader(leader.get());
    // A phone over the goggle's WiFi: an iPhone takes ~5 Mbit/s here, and 30
    // pictures a second of the screen are plenty for a spectator. Variable
    // rate under the cap (web_kbps, which also paces each viewer's socket -
    // webstream.cpp), averaging 70% of it: room to catch up after a stall.
    const int cap = web_cap_kbps() * 1000;
    dvr->set_encoding(cap / 10 * 7, Settings::getInstance().getInt("web_fps", 30), cap / 10 * 9);
    if (dvr->init(sw, sh) != 0) {
        printf("DVR: web view - no screen encoder\n");
        return;
    }
    attach_web(dvr.get());
    pthread_t tid;
    if (pthread_create(&tid, nullptr, DVR::run_dvr_thread, dvr.get()) != 0) {
        printf("DVR: web view - no encoder thread\n");
        dvr->set_sink(nullptr);
        return;
    }
    web_dvr_ = dvr;
    web_tid_ = tid;
    web_leader_ = leader;
    if (leader) printf("DVR: web view's encoder follows the screen recording\n");
}

void DvrRecorder::stop_web() {
    if (!web_dvr_) return;
    web_dvr_->set_sink(nullptr);
    web_dvr_->stop();
    pthread_join(web_tid_, nullptr);
    web_dvr_.reset();
    web_tid_ = 0;
    web_leader_.reset();
    printf("DVR: web view's screen encoder stopped\n");
}

void DvrRecorder::web_reconcile() {
    bool want, rec_screen;
    std::shared_ptr<DVR> rec;
    {
        std::lock_guard<std::mutex> lk(m_);
        want = web_want_ && !shut_;
        rec_screen = running_ && rec_screen_;
        rec = dvr_;
    }
    if (!want) {
        stop_web();
        return;
    }
    // A screen recording owns the screen's one sampler: the web view's
    // encoder follows it.
    std::shared_ptr<DVR> lead = rec_screen && rec ? rec : nullptr;
    if (web_dvr_ && web_leader_ != lead) stop_web();
    start_web(lead);
}

int DvrRecorder::web_cap_kbps() {
    const int k = Settings::getInstance().getInt("web_kbps", 5000);
    return k < 500 ? 500 : k > 50000 ? 50000 : k;
}

void DvrRecorder::set_web_watching(bool on) {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (shut_ || web_want_ == on) return;
        web_want_ = on;
    }
    // Encoder set-up and tear-down take a moment: not on the caller's thread.
    std::thread([this] {
        std::lock_guard<std::mutex> ctl(ctl_);
        web_reconcile();
    }).detach();
}

void DvrRecorder::web_retry() {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (shut_ || !web_want_) return;
    }
    std::thread([this] {
        std::lock_guard<std::mutex> ctl(ctl_);
        web_reconcile();
    }).detach();
}

bool DvrRecorder::toggle() {
    std::lock_guard<std::mutex> ctl(ctl_);
    if (running_) { stop_recording(); return false; }
    return start_recording();
}

void DvrRecorder::feed(std::shared_ptr<std::vector<uint8_t>> frame, uint64_t us) {
    if (!running_) return;   // cheap no-op when idle
    std::shared_ptr<DVR> dvr;
    std::vector<std::vector<uint8_t>> ps;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!running_ || !dvr_) return;
        dvr = dvr_;
        if (!ps_sent_ && !ps_.empty()) {
            ps = ps_;
            ps_sent_ = true;
        }
    }
    if (!ps.empty()) {
        for (const auto& p : ps)
            dvr->enqueueDvrPacket(std::make_shared<std::vector<uint8_t>>(p));
        printf("DVR: parameter sets arrived - recording video from here\n");
    }
    dvr->enqueueDvrPacket(frame, us);
}

void DvrRecorder::shutdown() {
    std::lock_guard<std::mutex> ctl(ctl_);
    {
        std::lock_guard<std::mutex> lk(m_);
        web_want_ = false;
        shut_ = true;     // a viewer arriving now starts nothing (WebStream stops after us)
    }
    stop_recording();
    stop_web();
}
