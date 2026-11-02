#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "app/input.hpp"
#include "app/options.hpp"
#include "app/signals.hpp"
#include "app/wiring.hpp"
#include "base/ltrace.hpp"
#include "base/restart.hpp"
#include "base/sd_log.hpp"
#include "base/settings.hpp"
#include "display/settings.hpp"
#include "dvr/dvr.hpp"
#include "dvr/dvr_library.hpp"
#include "dvr/settings.hpp"
#include "hal/wifi_ap.hpp"
#include "hud/osd.hpp"
#include "hud/settings.hpp"
#include "kestrel_gnd_config.h"
#include "platform/platform.hpp"
#include "radio/ar8030_sky.hpp"
#include "radio/bb_watchdog.hpp"
#include "radio/model.hpp"
#include "radio/settings.hpp"
#include "render/gl_context.hpp"   // g_disable_gl
#include "web/settings.hpp"
#include "web/webstream.hpp"

// The settings file: loaded first thing in main(), and handed to each part
// that keeps settings.
static Settings g_settings;

int main(int argc, char **argv)
{
	// Before any thread: every signal unblocked (a re-exec from the stop
	// watchdog's SIGALRM handler hands SIGALRM on blocked, and the next wedged
	// stop would never time out), and the scheduling the process started
	// with, which a re-exec puts back (base/restart.hpp).
	restart_init();
	signals_set_restart(argv, g_settings);
	// Line-buffer stdout. It is a terminal only when someone is watching by
	// hand; in service it is redirected to a file, where glibc switches to full
	// buffering and the log then trails reality by up to a buffer. That is
	// merely annoying until you are reading it to find out where a hang is, at
	// which point the last line printed is not where the process stopped and
	// the log actively misleads.
	setvbuf(stdout, nullptr, _IOLBF, 0);
	ltrace::start();

	int ret;

	// Absolute, deliberately: a relative path resolves against the working
	// directory, so settings silently followed wherever the app was started from
	// (an init script or a systemd unit with no WorkingDirectory would start from
	// defaults and persist somewhere else). Override with KESTREL_CONFIG for
	// testing without touching the installed file.
	{
		const char *cfg = getenv("KESTREL_CONFIG");
		g_settings.load(cfg && *cfg ? cfg : "/etc/kestrel/kestrel-gnd.yaml");
	}
	// The settings, then the command line (app/options.cpp).
	StartOptions o;
	{
		const int rc = read_start_options(argc, argv, g_settings, o);
		if (rc != kStart) return rc;
	}
	Ar8030Config& ar_cfg = o.ar;
	const uint16_t mode_width = o.mode_width, mode_height = o.mode_height;
	const uint32_t mode_vrefresh = o.mode_vrefresh;
	const std::string& screen_id_str = o.screen_id;
	int& video_zpos = o.video_zpos;
	const int enable_osd = o.enable_osd, osd_zpos = o.osd_zpos;
	const int osd_refresh_frequency_ms = o.osd_refresh_frequency_ms;
	const bool console_stats = o.console_stats, simulation_mode = o.simulation_mode;
	const RenderMode render_mode = o.render_mode;
	const std::string& dvr_filename = o.dvr_filename;
	const int dvr_framerate = o.dvr_framerate;
	const bool dvr_screen = o.dvr_screen;
	const DvrFormat dvr_format = o.dvr_format;
	const std::string& decoder_name = o.decoder_name;
	const bool enable_vrr = o.enable_vrr;
	const VideoCodec& codec = o.codec;   // the radio keeps a reference to it

	printf("kestrel-gnd v%s (built %s, git:%s)\n", KESTREL_GND_VERSION, KESTREL_GND_BUILD_TIME, KESTREL_GND_GIT_HASH);


	if (enable_osd == 0 ) {
		video_zpos = 4;
	}

	// Start decoding the heavy idle-screen assets (background.png + title) on a
	// worker thread now, so it overlaps the DRM/EGL driver bring-up below
	// instead of running serially on the OSD thread afterwards. The OSD is
	// created whenever we're not in simulation mode, so mirror that condition.
	if (!simulation_mode) {
		osd_prefetch_assets_async();
	}

	////////////////////////////////// SIGNAL SETUP
	signals_install();

	//////////////////////////////////  DISPLAY SETUP
	// The display, and what the HUD draws with: the platform's (platform/<box>.cpp).
	std::shared_ptr<display::Display> dev;
	if (!simulation_mode) {
		platform::DisplaySetup ds;
		ds.width = mode_width;
		ds.height = mode_height;
		ds.refresh = mode_vrefresh;
		ds.video_zpos = video_zpos;
		ds.osd_zpos = osd_zpos;
		ds.vrr = enable_vrr;
		ds.screen_id = screen_id_str;
		// Auto: the refresh follows the camera, from the rate last seen.
		ds.follow_camera = !(mode_width > 0 && mode_height > 0 && mode_vrefresh > 0);
		const int last_fps = g_settings.get(display::setting::camera_fps);
		ds.follow_fps = last_fps > 0 ? (uint32_t)last_fps : 0;
		const int last_lines = g_settings.get(display::setting::camera_lines);
		ds.follow_lines = last_lines > 0 ? (uint32_t)last_lines : 0;
		ds.sync_mode = o.sync_mode;
		ds.sync_slice = o.sync_slice;
		ds.sync_margin_us = o.sync_margin_us;
		dev = platform::make_display(g_settings, ds);
		if (!g_disable_gl) platform::init_graphics(*dev, g_settings.get(hud::setting::hud_msaa));
		else printf("GL/EGL disabled (--no-osd): GPU stays idle\n");

	}

	////////////////////////////////// Pipeline setup
	std::shared_ptr<DVR> dvr;
	if (!dvr_filename.empty()) {
		int dvr_fps = dvr_framerate > 0 ? dvr_framerate : 30;
		dvr = std::make_shared<DVR>(g_settings, dvr_filename, codec, dvr_fps, dvr_format, &signal_stop, dvr_screen);
	}

	// The radio's model, which implements the parts the radio carries, never
	// freed: the HUD and the web view may call them after the radio has stopped.
	radio::Model& radio_model = *new radio::Model(g_settings);
	const hud::Parts parts{radio_model.camera(), radio_model.encoder(), radio_model.air_radio(),
	                       radio_model.air_unit(), radio_model.fc(), radio_model.radio(),
	                       radio_model.timing()};
	input_set_radio(&radio_model.radio());

	std::shared_ptr<OSD> osd;
	std::shared_ptr<display::Presenter> renderer;
	std::shared_ptr<Vdec> vdec;

	if (!simulation_mode) {
		// With --no-osd there is no EGL display/context (GlContext was not made),
		// so constructing the OSD would segfault as soon as its thread calls
		// eglMakeCurrent(). The video plane is driven by the Renderer via DRM
		// and does not need GL.
		if (!g_disable_gl) {
			osd = std::make_shared<OSD>(parts, g_settings, dev, platform::make_canvas(), platform::make_hud_layer(*dev),
			                            osd_refresh_frequency_ms, &signal_stop, console_stats);
			osd->set_ui_scale(o.ui_scale);
			osd->set_decoder_name(decoder_name);
			osd->set_screen(screen_id_str);
			if (!o.screen_mode_trial.empty())
				osd->begin_screen_mode_confirm(o.screen_mode_trial);
			if (dvr_screen && dvr) {
				osd->set_dvr(dvr, true);
			}
		}
		
		// A new picture size: the --dvr recording opens at it, and the REC
		// button's recordings, made later, remember it.
		auto on_frame_size = [dvr](int w, int h) {
			if (dvr) dvr->init(w, h);
			DvrRecorder::instance().set_frame_size(w, h);
		};
		renderer = platform::make_presenter(dev, render_mode, g_settings, on_frame_size, osd, &signal_stop,
		                                    console_stats);
		vdec = platform::make_decoder(codec, decoder_name, renderer, dev, &signal_stop);
	}

    // 2. Start Video/UI threads IMMEDIATELY
    pthread_t tid_frame = 0;
    pthread_t tid_rend = 0;
    pthread_t tid_osd = 0;
    pthread_t tid_kb = 0;
    pthread_t tid_btn = 0;
    if (vdec) {
        ret = pthread_create(&tid_frame, NULL, Vdec::run_frame_thread, vdec.get());
        assert(!ret);
    }
	if (renderer) {
		ret = pthread_create(&tid_rend, NULL, [](void* r) -> void* {
			static_cast<display::Presenter*>(r)->run();
			return nullptr;
		}, renderer.get());
		assert(!ret);
	}
	if (osd) {
		ret = pthread_create(&tid_osd, NULL, OSD::run_thread, osd.get());
		assert(!ret);
		ret = pthread_create(&tid_kb, NULL, kb_thread_func, osd.get());
		assert(!ret);
	}
	// The front buttons, with or without an OSD: SIGUSR1's REC is done there.
	ret = pthread_create(&tid_btn, NULL, vrx_button_thread_func, osd.get());
	assert(!ret);
    

    // Recording is button-driven now, so give the recorder everything it needs
    // to build a DVR on demand. dvr_dir defaults to the recordings partition
    // created on first boot (S35dvrpart).
    DvrRecorder::instance().set_display(dev.get());
    // What the recorder gives the phone view: a finished recording, and its
    // screen encoder's stream.
    DvrRecorder::instance().set_web_hooks({
        [](const std::string& file) { WebStream::instance().recording_finished(file); },
        [](const std::vector<uint8_t>& header, int w, int h, int dur90k) {
            WebStream::instance().set_screen_source(header, w, h, dur90k);
        },
        [](const uint8_t* p, size_t n, int dur) { return WebStream::instance().feed_screen(p, n, dur); },
    });
    DvrRecorder::instance().set_screen_mode(
        g_settings.get(dvr::setting::dvr_screen));
    {
        char build[192];
        snprintf(build, sizeof build, "kestrel-gnd v%s (built %s, git:%s)",
                 KESTREL_GND_VERSION, KESTREL_GND_BUILD_TIME, KESTREL_GND_GIT_HASH);
        DvrRecorder::instance().set_build(build);   // into each recording's metadata
    }
    DvrRecorder::instance().configure(
        g_settings, codec, dvr_framerate > 0 ? dvr_framerate : 60, dvr_format, &signal_stop,
        g_settings.get(dvr::setting::dvr_dir));
    dvr_lib::set_dir(g_settings.get(dvr::setting::dvr_dir));

    // Live video in a browser: http://<goggle>/ on the USB link or WiFi.
    WebStream::instance().set_radio(&radio_model.radio());
    wifi_ap_set_page_port(g_settings.get(web::setting::web_port));
    if (g_settings.get(web::setting::web_stream))
        WebStream::instance().start(g_settings.get(web::setting::web_port));


    if (osd) {
        // Show the camera's own modes in the VIDEO tab instead of the
        // ArtLynk air unit's, and start on whatever --ar8030-mode asked for
        // (falling back to the mode stock boots into).
        std::vector<std::string> names;
        std::vector<uint16_t> needs;
        int cur = sky::default_fpv_mode_index();
        for (int i = 0; i < sky::kFpvModeCount; i++) {
            names.push_back(sky::kFpvModes[i].name);
            needs.push_back(sky::kFpvModes[i].need);
            if (ar_cfg.mode_w == sky::kFpvModes[i].w &&
                ar_cfg.mode_h == sky::kFpvModes[i].h &&
                ar_cfg.mode_fps == sky::kFpvModes[i].fps)
                cur = i;
        }
        osd->set_video_mode_names(names, cur, needs);
    }

    pthread_t tid_dvr;
	if (dvr) {
		if (dvr_screen && dev && dev->has_output()) {
			dvr->init(dev->mode().w, dev->mode().h);
		}
		ret = pthread_create(&tid_dvr, NULL, DVR::run_dvr_thread, dvr.get());
		assert(!ret);
	}

	////////////////////////////////////////////// MAIN LOOP
	// The AR8030 baseband is the only supported link.
	//
	// Armed before the link is brought up, because the call that wedges is
	// inside that bring-up and cannot be interrupted once it has parked.
	// Settings supply the deadline unless --bbwatchdog said otherwise on the
	// command line, so the choice survives a reboot without a rebuild:
	//   bb_watchdog: 0   off
	//   bb_watchdog: 1   built-in default
	//   bb_watchdog: N   deadline in seconds
	// It is off at the moment. The ioctl timeouts (see ar_ioctl in
	// ar8030_link.cpp) removed the parks this was containing, so it is
	// carrying no weight that anyone has measured - and every version of it so
	// far has cost more than it saved. Turn it back on if the symptom returns.
	if (!bb_watchdog_deadline_set())
		bb_watchdog_set_deadline(g_settings.get(radio::setting::bb_watchdog));
	bb_watchdog_start(argv, g_settings);
	run_radio(radio_model, ar_cfg, g_settings, codec, vdec, osd, &signal_stop, o.ar8030_slot, o.ar8030_port);

	////////////////////////////////////////////// MPI CLEANUP
	bb_watchdog_stop();   // a deliberate exit is not a wedge
	printf("Shutting down threads...\n");
	DvrRecorder::instance().shutdown();
	WebStream::instance().stop();
	if (renderer) {
		ret = pthread_join(tid_rend, NULL);
		assert(!ret);
		printf("Renderer thread joined.\n");
		// The display lock's thread reads the DRM device every vblank; it has
		// to be gone before dev->cleanup() frees that (the renderer object
		// itself lives to the end of main).
		renderer->stop_display_lock();
	}
	
	if (dev) dev->cond_signal();
	
	if (osd) {
		//osd->stop();
		ret = pthread_join(tid_osd, NULL);
		assert(!ret);
		printf("OSD thread joined.\n");
	}
	if (dvr){
		dvr->stop();
		ret = pthread_join(tid_dvr, NULL);
		assert(!ret);
	}

	if (tid_btn) pthread_join(tid_btn, NULL);
	if (osd) {
		ret = pthread_join(tid_kb, NULL);
		assert(!ret);
		printf("Keyboard thread joined.\n");
	}
    
    if (vdec) {
        printf("Senging EOS to decoder...\n");
        vdec->cleanup();
        printf("Joining frame thread...\n");
        pthread_join(tid_frame, NULL);
        printf("Frame thread joined.\n");
    }
	
	// Settings are written from a background thread; whatever is still pending
	// goes now, before a restart or the next start reads the file.
	g_settings.flush();

	////////////////////////////////////////////// DRM CLEANUP;
	if (dev) dev->cleanup();

	if (signals_restart_asked()) {
		// The pending alarm, the signals and the descriptors are reset just
		// before the exec (restart_prepare_exec); a watchdog retry's count is
		// not in the environment (bb_watchdog.cpp takes it out at start).
		printf("kestrel-gnd: restarting\n");
		fflush(nullptr);
		sd_log::flush();   // the card's log: its threads do not survive the exec
		signals_restart_now();
	}
	printf("kestrel-gnd done.\n");
	// Out with _exit(), not by returning: detached threads (a recording being
	// recovered, the web view's, the WiFi script's) may still be running, and
	// exit() would run the static destructors under them. What returning did
	// that matters is done here: the settings and the SD log written out,
	// every stdio stream flushed.
	g_settings.flush();
	sd_log::flush();
	fflush(nullptr);
	_exit(0);
}
