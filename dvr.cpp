
#define MINIMP4_IMPLEMENTATION

#include "dvr.hpp"
#include <unistd.h>
#include <time.h>
#include <atomic>
#include <thread>
#include <cerrno>
#include <cstring>
#include <pthread.h>
#include "webstream.hpp"
#include "utils/time_util.h"
#include "drm.hpp"
#include "settings.hpp"

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
		if (MP4E_STATUS_OK != mp4_h26x_write_init(&mp4wr, mux, frm_width, frm_height, codec==VideoCodec::H265))
		{
			printf("error: mp4_h26x_write_init failed\n");
			mux = NULL;
			dvr_file = NULL;
		}
		return 0;
	}

	// Screen recording: the DRM writeback connector composites every plane
	// (video + OSD) and writes NV12 straight into the encoder's own DMA-BUF, so
	// a captured frame is never copied - the display controller writes it and
	// the VEPU reads it.
	//
	// There is deliberately no software fallback. The previous glReadPixels
	// route could only see the GL/OSD layer, because the FPV video lives on a
	// separate DRM plane blended at scanout, so its recordings came out with a
	// black video area. A capture that silently omits the video is worse than
	// refusing to record.
	//
	// ffmpeg is not an option for the encode either: this board's libavcodec has
	// only h264_v4l2m2m and no /dev/video* node for it to bind to.
	// The file runs at the display's refresh rate (H264Cfr fills the scan-outs
	// between captures), so the encoder is set up for that rate. The rate
	// control's fps only shapes the bitrate budget and the H.264 level;
	// actual sample durations come from the capture times.
	//
	// Capped at 60 fps: a 120 Hz display records at 60. Captures run at
	// dvr_screen_capture_fps() whatever the file's rate (each one costs the
	// live picture a little), so 120 would mostly add skip frames, and 60 is
	// what players and editors handle everywhere.
	int fps = 60;
	if (wb_dev && wb_dev->output_list && wb_dev->output_list->mode.vrefresh > 0)
		fps = (int)wb_dev->output_list->mode.vrefresh;
	if (fps > 60) fps = 60;
	const int bitrate = 16000000;

	if (!wb_dev) {
		printf("DVR: no DRM device for writeback - screen recording unavailable\n");
		return -1;
	}
	screen_fps = fps;
	if (!screen_enc.init(frm_width, frm_height, fps, bitrate)) {
		printf("DVR: hardware H.264 encoder unavailable - screen recording disabled\n");
		return -1;
	}
	std::vector<int> fds;
	for (int i = 0; i < screen_enc.input_count(); i++) fds.push_back(screen_enc.input_dmabuf_fd(i));
	if (!wb_dev->writeback_init(frm_width, frm_height, fds)) {
		printf("DVR: no writeback connector - screen recording unavailable\n");
		screen_enc.deinit();
		return -1;
	}
	use_writeback = true;

	if (!dvr_file) {
		printf("DVR: output file not open - screen recording disabled\n");
		return -1;
	}
	writer.reset(new DvrWriter(fileno(dvr_file)));
	mux = MP4E_open(0 /*sequential_mode*/, format == DvrFormat::FMP4, writer.get(), write_callback);
	if (format == DvrFormat::FMP4)
		MP4E_set_finalize(mux);     // as in the camera recording
	if (MP4E_STATUS_OK != mp4_h26x_write_init(&mp4wr, mux, frm_width, frm_height, 0 /*H.264*/)) {
		printf("DVR: mp4_h26x_write_init failed (screen)\n");
		mux = NULL;
		return -1;
	}
	if (!Settings::getInstance().getBool("dvr_screen_cfr", true))
		printf("DVR: constant rate off (dvr_screen_cfr) - variable-rate recording\n");
	else if (!screen_cfr.init(screen_enc.header()))
		printf("DVR: screen recording stays variable-rate\n");
	// SPS/PPS first, exactly as the FPV path primes its parameter sets.
	if (!screen_enc.header().empty())
		mp4_h26x_write_nal(&mp4wr, screen_enc.header().data(),
		                   (int)screen_enc.header().size(), 90000 / fps);

	printf("DVR: screen recording %dx%d, DRM writeback composited capture\n",
	       frm_width, frm_height);
	return 0;
}

// The screen recording's encoder loop, on the DVR thread - off the OSD thread,
// so encoding never delays the HUD: take each captured frame as the display
// finishes writing it, encode it, hand the buffer back.
void DVR::run_screen() {
	// Every sample lasts one scan-out. A captured frame lands on the scan-out
	// grid by its capture time; the scan-outs between it and the previous
	// capture become skip frames repeating it, so the file runs at exactly
	// the display's rate and plays back in real time. The grid
	// position is counted from the first frame rather than from the last
	// one, so rounding never accumulates into drift, and a frame always
	// advances at least one scan-out, so none is ever discarded.
	const int period = 90000 / (screen_fps > 0 ? screen_fps : 60);   // 90 kHz ticks
	const double period_us = 1e6 / (screen_fps > 0 ? screen_fps : 60);
	uint64_t t0 = 0, last_us = 0;
	int64_t  last_idx = -1;
	uint64_t frames = 0, skips = 0;
	auto write = [&](const uint8_t* p, size_t len, int dur) {
		auto r = mp4_h26x_write_nal(&mp4wr, p, (int)len, dur);
		if (!(MP4E_STATUS_OK == r || MP4E_STATUS_BAD_ARGUMENTS == r))
			printf("DVR: writeback mp4_h26x_write_nal err %d\n", r);
	};
	auto encode = [&](const DrmDevice::WbShot& shot) {
		DrmDevice::writeback_wait(shot.fence);
		int dur = period;
		if (screen_cfr.ready()) {
			if (last_idx < 0) t0 = shot.us;
			int64_t idx = (int64_t)((double)(shot.us - t0) / period_us + 0.5);
			if (idx <= last_idx) idx = last_idx + 1;
			// A second at most: the idle capture keeps gaps far shorter.
			int64_t held = last_idx < 0 ? 0 : idx - last_idx - 1;
			if (held > screen_fps) held = screen_fps;
			for (int64_t i = 0; i < held; i++) {
				std::vector<uint8_t> sk = screen_cfr.skip_frame();
				if (sk.empty()) break;
				write(sk.data(), sk.size(), period);
				skips++;
			}
			last_idx = idx;
		} else if (last_us && shot.us > last_us) {
			// Variable rate: the time since the previous capture.
			uint64_t d = shot.us - last_us;
			if (d > 1000000) d = 1000000;
			if (d < 1000)    d = 1000;
			dur = (int)(d * 90000 / 1000000);
		}
		last_us = shot.us;
		screen_enc.encode_buffer(shot.idx, [&](const uint8_t* p_hw, size_t len, bool) {
			// One bulk copy out of the encoder's output buffer first. It is
			// uncached for the CPU, and parsing it in place - a byte at a
			// time - took 15-17 ms per frame, more than the encode itself,
			// and capped screen recording near 38 fps.
			pkt_copy_.assign(p_hw, p_hw + len);
			const uint8_t* p = pkt_copy_.data();
			if (screen_cfr.ready()) {
				std::vector<uint8_t> au = screen_cfr.rewrite(p, len);
				write(au.data(), au.size(), dur);
			} else {
				write(p, len, dur);
			}
		});
		wb_dev->writeback_release(shot.idx);
		frames++;

	};

	// The captures run on a thread of their own, on a fixed schedule: a
	// capture is a display commit that grabs whatever is on screen at that
	// instant, so it needs nothing from the HUD. It used to be driven from the
	// HUD's draw loop, which forced a HUD redraw per capture (capping the rate
	// near 36/s at ~9 ms a frame, and costing battery for nothing).
	std::atomic<bool> capturing{true};
	std::thread capturer([&] {
		const int fps = dvr_screen_capture_fps();
		const long period_ns = 1000000000L / fps;
		struct timespec next;
		clock_gettime(CLOCK_MONOTONIC, &next);
		while (capturing) {
			if (use_writeback && wb_dev) wb_dev->writeback_capture();
			next.tv_nsec += period_ns;
			while (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; next.tv_sec++; }
			// Absolute deadlines: a slow commit does not push every later
			// capture back. If one ran past its slot, skip ahead rather than
			// firing a burst to catch up.
			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);
			if (now.tv_sec > next.tv_sec || (now.tv_sec == next.tv_sec && now.tv_nsec > next.tv_nsec)) {
				next = now;
				continue;
			}
			clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, nullptr);
		}
	});

	DrmDevice::WbShot shot;
	while (!*should_stop) {
		{
			std::lock_guard<std::mutex> lock(mtx);
			if (should_exit) break;
		}
		if (use_writeback && wb_dev && wb_dev->writeback_next(shot, 50)) encode(shot);
	}
	capturing = false;
	capturer.join();
	// Frames already captured are still part of the recording.
	while (use_writeback && wb_dev && wb_dev->writeback_next(shot, 0)) encode(shot);
	printf("DVR: %llu screen frames encoded, %llu held scan-outs as skip frames (%d fps)\n",
	       (unsigned long long)frames, (unsigned long long)skips, screen_fps);
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
		run_screen();
		if (mux) {
			MP4E_close(mux);
			mp4_h26x_write_close(&mp4wr);
			mux = NULL;
		}
		// Stop capturing before the encoder's buffers go away: writeback_deinit
		// also waits out any capture still being written.
		if (wb_dev) wb_dev->writeback_deinit();
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
	// played a 14 fps stream back four times too fast. Within a quarter of
	// the nominal interval it snaps to nominal, so radio jitter does not
	// turn a steady 60 fps into uneven sample times.
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

void DVR::enqueueDvrPacket(std::shared_ptr<std::vector<uint8_t>> frame) {
	{
		std::lock_guard<std::mutex> lock(mtx);
		dvrQueue.push({frame, get_time_us()});
	}
	cv.notify_one();
}
int dvr_screen_capture_fps() {
	int fps = Settings::getInstance().getInt("dvr_screen_fps", kDvrScreenFpsDefault);
	return fps < 1 ? 1 : fps > 60 ? 60 : fps;
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

// Deletes the oldest fpvOS_*.mp4 / fpvOS_*.h265 recordings in dir - and
// kestrel_*, what earlier builds named them - until
// free space clears min_free_bytes, or there is nothing left to delete. Only
// called from start_locked(), before any recording is open, so there is
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

void DvrRecorder::set_writeback(DrmDevice* dev) {
    std::lock_guard<std::mutex> lk(m_);
    wb_dev_ = dev;
}


void DvrRecorder::set_screen_mode(bool on) {
    std::lock_guard<std::mutex> lk(m_);
    screen_ = on;
}

bool DvrRecorder::screen_mode() const {
    std::lock_guard<std::mutex> lk(m_);
    return screen_;
}

bool DvrRecorder::is_recording() const {
    std::lock_guard<std::mutex> lk(m_);
    return running_;
}

std::string DvrRecorder::current_file() const {
    std::lock_guard<std::mutex> lk(m_);
    return file_;
}

bool DvrRecorder::start_locked() {
    if (!configured_) {
        printf("DVR: not configured, cannot record\n");
        return false;
    }
    // The recordings partition is created and mounted on first boot
    // (S35dvrpart). If it is not there, say so rather than silently writing
    // into the rootfs and filling it - a directory existing is not enough
    // proof of that, since S35dvrpart mkdir's the mount point up front
    // regardless of whether the mount itself succeeded. Compare device IDs
    // with the parent directory instead: identical means dir_ is just a
    // plain directory sitting on whatever filesystem holds its parent, not a
    // separate mounted partition.
    struct stat st, parent_st;
    if (stat(dir_.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) ||
        stat((dir_ + "/..").c_str(), &parent_st) != 0 ||
        parent_st.st_dev == st.st_dev) {
        printf("DVR: %s is not a mounted recordings partition - not recording\n", dir_.c_str());
        return false;
    }

    // Keep some headroom on the recordings partition itself so an unattended
    // pile of old footage never runs it to zero.
    purge_oldest_recordings(dir_, 300ull * 1024 * 1024);
    // DVR::format_timestamped_filename() turns "<dir>/fpvOS.mp4" into
    // "<dir>/fpvOS_YYYYMMDD_HHMMSS.mp4".
    const char* ext = (fmt_ == DvrFormat::RAW) ? ".h265" : ".mp4";
    std::string base = dir_ + "/fpvOS" + ext;

    // Screen recordings are always H.264/MP4 (that is what the VEPU encodes),
    // regardless of what codec the FPV link is using.
    dvr_ = screen_
        ? std::make_shared<DVR>(base, VideoCodec::H264, fps_,
                                fmt_ == DvrFormat::MP4 ? DvrFormat::MP4 : DvrFormat::FMP4, stop_, true)
        : std::make_shared<DVR>(base, codec_, fps_, fmt_, stop_, false);
    if (screen_ && wb_dev_) dvr_->set_writeback_dev(wb_dev_);
    // Without this the muxer is never created and every frame the writer thread
    // dequeues is silently dropped - the file is opened, the timer runs, and
    // nothing is ever written. Renderer only calls init() on the legacy --dvr
    // object, which is always null now.
    // Screen recordings use the display size; the FPV path uses the video size.
    if (screen_ && sw_ > 0 && sh_ > 0) dvr_->init(sw_, sh_);
    else                               dvr_->init(fw_, fh_);
    if (pthread_create(&tid_, nullptr, DVR::run_dvr_thread, dvr_.get()) != 0) {
        printf("DVR: failed to start writer thread\n");
        dvr_ = nullptr;
        return false;
    }
    // Replay the parameter sets before any picture data. Without them the
    // muxer has no decoder configuration to put in hvcC and the file plays as
    // black even though the video data is all there.
    ps_sent_ = false;
    if (!screen_) {
        for (const auto& p : ps_)
            dvr_->enqueueDvrPacket(std::make_shared<std::vector<uint8_t>>(p));
        ps_sent_ = !ps_.empty();
        if (ps_.empty())
            printf("DVR: no VPS/SPS/PPS seen yet - they go in ahead of the first "
                   "picture after they arrive\n");
    }

    running_ = true;
    // The name alone: what the gallery lists, serves and asks about.
    file_    = dvr_->path().substr(dvr_->path().find_last_of('/') + 1);
    printf("DVR: recording -> %s\n", dvr_->path().c_str());
    return true;
}

void DvrRecorder::stop_locked() {
    if (!running_) return;
    printf("DVR: stopping -> %s\n", file_.c_str());
    dvr_->stop();
    pthread_join(tid_, nullptr);
    tid_     = 0;
    dvr_     = nullptr;
    running_ = false;
    // The file is complete now (moov written): its gallery thumbnail can be
    // made in the background, ready before the gallery is opened.
    WebStream::instance().recording_finished(file_);
    file_.clear();
}

bool DvrRecorder::toggle() {
    std::lock_guard<std::mutex> lk(m_);
    if (running_) { stop_locked(); return false; }
    return start_locked();
}

void DvrRecorder::feed(std::shared_ptr<std::vector<uint8_t>> frame) {
    std::lock_guard<std::mutex> lk(m_);
    if (!running_ || !dvr_) return;   // cheap no-op when idle
    if (!ps_sent_ && !ps_.empty()) {
        for (const auto& p : ps_)
            dvr_->enqueueDvrPacket(std::make_shared<std::vector<uint8_t>>(p));
        ps_sent_ = true;
        printf("DVR: parameter sets arrived - recording video from here\n");
    }
    dvr_->enqueueDvrPacket(frame);
}

void DvrRecorder::shutdown() {
    std::lock_guard<std::mutex> lk(m_);
    stop_locked();
}
