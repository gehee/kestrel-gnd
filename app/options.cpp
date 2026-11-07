#include "app/options.hpp"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "app/main.hpp"   // the command line's parser
#include "app/settings.hpp"
#include "base/frame_mode.hpp"
#include "base/sd_log.hpp"
#include "bsp/bsp.hpp"
#include "display/screen_id.h"
#include "display/settings.hpp"
#include "dvr/settings.hpp"
#include "platform/platform.hpp"
#include "hud/osd.hpp"
#include "hud/settings.hpp"
#include "kestrel_gnd_config.h"
#include "kestrel_gnd_features.h"
#include "radio/bb_watchdog.hpp"
#include "radio/settings.hpp"
#include "region/zones.hpp"
#include "render/gl_context.hpp"

static void printHelp() {
  printf(
    "\n\t\tkestrel-gnd (v%s)\n"
    "\n"
    "  Usage:\n"
    "    kestrel-gnd [Arguments]\n"
    "\n"
    "  Arguments:\n"
    "    --link ar8030            - Take video from the AR8030 baseband on this board\n"
    "                               (the only supported source; also settable as 'link:'\n"
    "                               in kestrel-gnd.yaml)\n"
    "\n"
    "    --menu                   - Open the settings menu at startup. For looking at the\n"
    "                               menu from a workstation: it is otherwise reachable\n"
    "                               only by pressing a button on the goggle, which makes\n"
    "                               it the one screen that cannot be captured.\n"
    "\n"
    "    --bbwatchdog=N           - Bring-up watchdog: 0 disables it, 1 uses the default\n"
    "                               deadline, any larger N is the deadline in seconds.\n"
    "                               bb_socket_open() has no timeout, so a lost reply from\n"
    "                               the baseband daemon parks the bring-up for good; the\n"
    "                               watchdog notices and re-execs. Disable it to see the\n"
    "                               parks rather than the recovery.\n"
    "\n"
    "    --osd          		- Enable OSD\n"
    "\n"
    "\n"
    "\n"
    "    --osd-refresh  		- Defines the delay between osd refresh (Default: 1000 ms)\n"
    "    --ui-scale N            - OSD scale factor (default 1.0); also in HUD > UI Scaling\n"
    "    --stats                 - Print link/video stats to the console\n"
    "\n"
    "    (recording is on the REC button; files land in dvr_dir, default /media/dvr)\n"
    "\n"
    "    --dvr-framerate        - Force the dvr framerate fro smoother dvr\n"
    "\n"
    "    --dvr-format           - Dvr format: raw, mp4, fmp4 (default mp4)\n"
    "\n"
    "    --screen-mode   		- Override default screen mode. ex:1920x1080@120\n"
    "\n"
    "    --vsync              		- Enable V-Sync (tear-free, adds latency)\n"
    "\n"
    "    --no-display         		- Disable rendering (decode-only benchmarking)\n"
    "\n"
    "	 --decoder-name 		- Decoder to use, ex: rkmpp, hevc_cuvid, hevc_qsv, vaapi...\n"
    "    --codec h264|h265       - Video codec to decode (default h265)\n"
    "    --record [File]         - Record raw to File (same as the REC button)\n"
    "    --disable-vrr           - Force disable VRR (Variable Refresh Rate)\n"
    "    --ar8030-chan-manual    - Pin the AR8030 to --ar8030-freq instead of scanning\n"
    "    --ar8030-ap-index N     - Use bb_mac_addr_N from user_cfg.json as the AP (default 0)\n"
    "    --ar8030-pair           - Enter AR8030 pairing mode for ~30s at startup\n"
    "    --ar8030-power DBM      - AR8030 TX power in dBm (default 11, i.e. 25mW)\n"
    "    --ar8030-mode [C:]WxH@F - Retune the camera, e.g. 1920x1080@60 (default: leave as-is)\n"
    "    --ar8030-freq KHZ       - AR8030 link frequency in kHz (default 5740000)\n"
    "    --ar8030-bw N           - AR8030 bandwidth index (default 0)\n"
    "    --ar8030-slot N         - AR8030 BB slot (default 0)\n"
    "    --ar8030-port N         - AR8030 video plane port (default 3)\n"
    "    --no-osd                - Disable the GL/EGL OSD entirely (video plane only, GPU idle)\n"
    "    --no-decode             - (accepted; no effect)\n"
    "    --simulation            - Simulation mode: no graphics, reports packet loss\n"
    "    --verbose, --verbose-vid, --verbose-rf - (accepted; no effect)\n"
    "    --version               - Print the version and exit\n"
    "\n"
    "    Development-only flags share a --debug-* prefix and are not listed here\n"
    "    (baseband dumps, RF/mode probes, handshake and standby overrides).\n"
    "\n", KESTREL_GND_VERSION
  );
}

int read_start_options(int argc, char** argv, Settings& settings, StartOptions& o) {
	// What the AR8030 link will run with: the settings' values and the command
	// line's, given to it when it is made (run_radio).
	Ar8030Config& ar_cfg = o.ar;
	uint16_t& mode_width = o.mode_width;
	uint16_t& mode_height = o.mode_height;
	uint32_t& mode_vrefresh = o.mode_vrefresh;
	std::string& screen_id_str = o.screen_id;
	std::string& screen_mode_trial = o.screen_mode_trial;
	int& video_zpos = o.video_zpos;
	int& enable_osd = o.enable_osd;
	int& osd_zpos = o.osd_zpos;
	int& osd_refresh_frequency_ms = o.osd_refresh_frequency_ms;
	bool& console_stats = o.console_stats;
	bool& simulation_mode = o.simulation_mode;
	RenderMode& render_mode = o.render_mode;
	std::string& dvr_filename = o.dvr_filename;
	int& dvr_framerate = o.dvr_framerate;
	bool& dvr_screen = o.dvr_screen;
	DvrFormat& dvr_format = o.dvr_format;
	int& ar8030_slot = o.ar8030_slot;
	int& ar8030_port = o.ar8030_port;
	std::string& decoder_name = o.decoder_name;
	bool& enable_vrr = o.enable_vrr;
	float& ui_scale = o.ui_scale;
	VideoCodec& codec = o.codec;

	// SYSTEM > Time Zone: its TZ, before any thread reads the time.
	zones::load(settings);
	// HUD > Debug Info on: the log on the recordings partition too, named
	// as the recordings are (fpvOS_YYYYMMDD_HHMMSS.log), so it survives a
	// power-off; and the air unit's flight logs (sent at link-up).
	sd_log::init(settings.get(dvr::setting::dvr_dir));
	ar_cfg.air_logs = settings.get(hud::setting::hud_debug);
	sd_log::set_enabled(ar_cfg.air_logs);
	// The low-latency video features, each with a switch that survives a
	// reboot (the KESTREL_* variables and the /tmp files still work too):
	//   stream_decode: 0   pictures go to the decoder whole
	//   early_present: 0   no picture goes up before it is all decoded
	//   video_halves: 0    no picture in halves on two planes
	// A build without KESTREL_STREAM_DECODE has all three off.
	if (!KESTREL_STREAM_DECODE || settings.get(radio::setting::stream_decode) == 0) {
		setenv("KESTREL_STREAM_DECODE", "0", 1);
		ar_cfg.stream_decode = false;
	}
	if (!KESTREL_STREAM_DECODE || settings.get(display::setting::early_present) == 0)
		setenv("KESTREL_EARLY_PRESENT", "0", 1);
	if (!KESTREL_STREAM_DECODE || settings.get(display::setting::video_halves) == 0)
		setenv("KESTREL_SPLIT", "0", 1);


	mode_width = 0;
	mode_height = 0;
	mode_vrefresh = 0;
	// The display mode belongs to the screen: screen_mode_<ID>, by the ID in
	// its EDID (display/screen_id.h), and unset means Auto - the fastest 1080p
	// above 60 Hz, else 720p above 60 Hz, else 1080p60 (display/kms/
	// mode_follow.h). screen_mode is for a screen with no ID, and --screen-mode.
	// A mode picked in DISPLAY > Screen Mode is only on trial the first time:
	// screen_mode_try names the screen and the mode, and is read and removed
	// here, so a mode that leaves the screen black is gone at the next start
	// even if nobody could see the prompt to reject it (see OSD, confirm).
	char screen_id_buf[16] = "";
	const bool have_screen_id = screen_id(screen_id_buf, sizeof(screen_id_buf));
	screen_id_str = have_screen_id ? screen_id_buf : "";
	std::string screen_mode_str = settings.get(display::setting::screen_mode(screen_id_str));
	// DISPLAY > Refresh Sync is the screen's too, and off unless tried and
	// kept on it: some screens blank on the one-line timing changes it makes.
	// A screen with no EDID ID cannot be told from another, so it starts off
	// every time (the menu still turns it on for the run).
	{
		const int v = have_screen_id
			? settings.get(display::setting::display_lock(screen_id_buf)) : 0;
		o.sync_mode = v >= 0 && v <= 2 ? v : 0;   // OFF, NUDGE, FOLLOW
	}
	o.sync_slice = settings.get(display::setting::display_lock_slice) == 2 ? 2 : 1;
	{
		const int ms = settings.get(display::setting::display_lock_margin_ms);
		o.sync_margin_us = (ms >= 0 && ms <= 4 ? ms : 0) * 1000;
	}
      // on trial this run, "" for none
	{
		const std::string t = settings.get(display::setting::screen_mode_try);
		if (!t.empty()) {
			// On disk before the display is brought up: a trial mode that
			// crashes the app or blanks the screen must not come back.
			settings.remove(display::setting::screen_mode_try);
			settings.flush();
			const size_t sp = t.find(' ');
			const std::string id = sp == std::string::npos ? "" : t.substr(0, sp);
			const std::string mode = sp == std::string::npos ? "" : t.substr(sp + 1);
			if (!mode.empty() && id == (have_screen_id ? screen_id_buf : "-")) {
				screen_mode_trial = mode;
				screen_mode_str = (mode == "auto") ? "" : mode;
			}
		}
	}
	printf("display: screen %s, mode %s%s\n", have_screen_id ? screen_id_buf : "(no EDID ID)",
	       screen_mode_str.empty() ? "auto" : screen_mode_str.c_str(),
	       screen_mode_trial.empty() ? "" : " (on trial)");
	if (!screen_mode_str.empty()) {
		// Parsing: 1920x1080@60
		try {
			size_t x_pos = screen_mode_str.find('x');
			size_t at_pos = screen_mode_str.find('@');
			if (x_pos != std::string::npos && at_pos != std::string::npos) {
				mode_width = std::stoi(screen_mode_str.substr(0, x_pos));
				mode_height = std::stoi(screen_mode_str.substr(x_pos + 1, at_pos - x_pos - 1));
				mode_vrefresh = std::stoi(screen_mode_str.substr(at_pos + 1));
			}
		} catch(...) {}
	}
	
	video_zpos = settings.get(display::setting::video_zpos);
	enable_osd = settings.get(hud::setting::enable_osd);
	osd_zpos = settings.get(hud::setting::osd_zpos);
	osd_refresh_frequency_ms = settings.get(hud::setting::osd_refresh);
	console_stats = settings.get(hud::setting::stats);
	simulation_mode = false;

	// Default: FrontBuffer (lowest latency)
	render_mode = FrontBuffer;
	bool vsync_flag = settings.get(display::setting::vsync);
	bool no_display_flag = settings.get(display::setting::no_display);
	
	if (no_display_flag) render_mode = Disable;
	else if (vsync_flag) render_mode = Atomic;
	else render_mode = FrontBuffer;

	dvr_filename = settings.get(dvr::setting::dvr);
	dvr_framerate = settings.get(dvr::setting::dvr_framerate);
	dvr_screen = settings.get(dvr::setting::dvr_screen);
	
	// Fragmented MP4 by default: every frame goes out as a self-contained
	// fragment behind a header written up front, so a recording cut short -
	// the battery pulled with REC on, which is how most of them end - still
	// plays up to its last second. A plain MP4 writes its index only when it
	// is closed, and without one no player opens the file. "mp4" is still
	// available for tools that insist on it.
	dvr_format = DvrFormat::FMP4;
	std::string dvr_format_str = settings.get(dvr::setting::dvr_format);
	if (dvr_format_str == "mp4") dvr_format = DvrFormat::MP4;
	else if (dvr_format_str == "raw") dvr_format = DvrFormat::RAW;
	
	// The goggle always starts by searching for the air unit (chan_auto); a
	// channel picked on the channel page is saved (ar8030_chan_mhz) and the
	// link moves back there at every link-up. Only --ar8030-chan-manual starts
	// it pinned.
	// Decode was flag-gated because it was being isolated while chasing the
	// decode freeze; for normal use it belongs on. Settings-backed so a bare
	// "kestrel-gnd" works on hardware where the config says ar8030: 1.
	ar_cfg.decode_enabled = settings.get(radio::setting::ar8030_decode) != 0;
	ar8030_slot = settings.get(radio::setting::ar8030_slot);
	ar8030_port = settings.get(radio::setting::ar8030_port);
	// mW, in stock's encoding (N = hold, N+1 = auto capped at N). See
	// kArPwrLevels in system/air/power.hpp for the levels this board accepts.
	ar_cfg.tx_power_mw = settings.get(radio::setting::tx_power_mw);
	ar_cfg.tx_power_want_mw = ar_cfg.tx_power_mw;   // the choice, until an air unit's own
	{
		int fm = settings.get(base::setting::frame_mode);
		g_frame_mode = (fm >= 0 && fm < kFrameModeCount) ? fm : kFrameWhole;
		printf("frame mode: %s\n", frame_mode_label(g_frame_mode.load()));
	}
	ar_cfg.tx_power_dbm = kArPwrLevels[ar_pwr_index(ar_cfg.tx_power_mw)].dbm;
	// The fastest capture -> arrival the air delay is pinned to (see
	// air_delay_for). Measure it for a setup with both clocks synced.
	ar_cfg.air_floor_us =
		(int)(settings.get(radio::setting::air_floor_ms) * 1000.0f);
	// 0 = off. See drain_msp_socket(): an unserved port wedges bring-up.
	ar_cfg.msp_bb_port = settings.get(radio::setting::msp_bb_port);
	// Standby is dictated by the air unit, not the ground: fpv_sky_standby_mode_thread
	// auto-parks the air to low power when it is disarmed/idle and forces itself out
	// of standby while flying. So the ground does not command it - standby_mode stays
	// -1 (leave alone), connect_bb() skips CAM_STANDBY, and the menu just reflects the
	// air's reported state (osd air_standby). --debug-standby still overrides for
	// experiments.
	// bb_bandwidth_e gear: 0=1.25M 1=2.5M 2=5M 3=10M 4=20M 5=40M.
	ar_cfg.bandwidth = settings.get(radio::setting::ar8030_bw);
	// Stock's values; -1 on either disables the PRJ_DISPATCH call entirely.
	// Both default to -1 (off): see send_prj_rf_config(). Enabling the ground
	// side alone measurably hurts; air_bw is the other half and its units are
	// not yet verified.
	ar_cfg.prj_rf_bw     = settings.get(radio::setting::prj_rf_bw);
	ar_cfg.prj_rf_pwr_mw = settings.get(radio::setting::prj_rf_pwr_mw);
	ar_cfg.air_bw        = settings.get(radio::setting::air_bw);
	// The cap on kestrel-air's video bitrate (RF menu, Max Bitrate), kbps, 0 = none.
	ar_cfg.max_kbps      = settings.get(radio::setting::ar8030_max_kbps);
	// The cap on the air unit's auto exposure (VIDEO menu, Max Exposure), us, 0 = none.
	ar_cfg.max_exp_us    = settings.get(radio::setting::ar8030_max_exp_us);
	ar_cfg.strips        = settings.get(radio::setting::ar8030_strips);
	// The cap on the video link's bandwidth (RF menu, Max Bandwidth): 20 or 40 MHz,
	// with Open8030 here also AUTO (0: nothing sent), 5 or 10.
	{
		const int bw = settings.get(radio::setting::ar8030_max_bw_mhz);
		ar_cfg.max_bw_mhz = bw == 0 || bw == 5 || bw == 10 || bw == 20 ? bw : 40;
		const int mcs = settings.get(radio::setting::ar8030_max_mcs);
		ar_cfg.max_mcs = mcs >= 0 && mcs <= 11 ? mcs : -1;   // the menu's range, AUTO + MCS 0..11
		ar_cfg.steady = settings.get(radio::setting::ar8030_steady) == 1 ? 1 : 0;
		ar_cfg.chan_hop = settings.get(radio::setting::ar8030_chan_hop) == 1 ? 1 : 0;
		const int fixed = settings.get(radio::setting::ar8030_chan_mhz);
		ar_cfg.chan_fixed_mhz = fixed >= 2000 && fixed <= 7000 ? fixed : 0;
	}
	// RADIO > VRX Radio Firmware: Open8030 when its image is on the unit, unless
	// the menu said STOCK (0) - as bb_only.sh decides what it uploads.
	printf("board: %s\n", bsp::board().name);
	platform::caps();   // worked out once, before any thread; logged
	const char *open8030_fw = bsp::board().radio.open8030_fw;
	ar_cfg.radio_fw      = *open8030_fw && access(open8030_fw, R_OK) == 0 &&
	                              settings.get(radio::setting::ar8030_firmware) != 0 ? 1 : 0;
	// The radio's TDD frame mode (RADIO > Radio Frame): 2 short, 0 standard.
	ar_cfg.frame_mode    = settings.get(radio::setting::ar8030_frame_mode) == 0 ? 0 : 2;
	ar_cfg.replay_stock_rf = settings.get(radio::setting::replay_stock_rf);
	decoder_name = settings.get(app::setting::decoder_name);
	// VRR (FreeSync over HDMI): opt-in on Rockchip, where it needs fpvOS's
	// kernel support and a sink that declares FreeSync (or is forced to with
	// rockchipdrm.freesync_force). Used only when the connector is vrr_capable.
	enable_vrr = settings.get(display::setting::enable_vrr);
	printf("VRR %s by the settings\n", enable_vrr ? "allowed" : "off");
	
	ui_scale = settings.get(hud::setting::ui_scale);




	codec = VideoCodec::H265;

	// Load console arguments. Each is for this run only and never saved:
	// one run by hand with --no-osd or --no-display must not change every
	// later start (the menu's choices are what the settings file keeps).
	__BeginParseConsoleArguments__(printHelp) 

	__OnArgument("--codec") {
		char * codec_str = const_cast<char*>(__ArgValue);
		codec = video_codec(codec_str);
		if (codec == VideoCodec::UNKNOWN ) {
			printf("unsupported video codec");
			return -1;
		}
		continue;
	}

	__OnArgument("--dvr-framerate") {
		dvr_framerate = atoi(__ArgValue);
		continue;
	}

	__OnArgument("--dvr-format") {
		char* fmt = const_cast<char*>(__ArgValue);
		if (!strcmp(fmt, "fmp4")) {
         dvr_format = DvrFormat::FMP4;
    	} else if (!strcmp(fmt, "raw")) {
         dvr_format = DvrFormat::RAW;
    	}
		continue;
	}

	__OnArgument("--osd") {
		enable_osd = 1;
		osd_zpos = 2;
		continue;
	}

	__OnArgument("--decoder-name") {
		char* name = const_cast<char*>(__ArgValue);
		decoder_name = std::string(name);
		continue;
	}

	__OnArgument("--osd-refresh") {
		osd_refresh_frequency_ms = atoi(__ArgValue);
		continue;
	}
	
	__OnArgument("--screen-mode") {
		char* mode = const_cast<char*>(__ArgValue);
		char* pt = strtok(mode, "x");
		if (pt) mode_width = atoi(pt);
		
		pt = strtok(NULL, "@");
		if (pt) mode_height = atoi(pt);
		
		pt = strtok(NULL, "@");
		if (pt) mode_vrefresh = atoi(pt);
		continue;
	}

	__OnArgument("--vsync") {
		vsync_flag = true;
		render_mode = Atomic;
		continue;
	}
	
	__OnArgument("--no-display") {
		no_display_flag = true;
		render_mode = Disable;
		continue;
	}



	if (!strncmp(Arg, "--menu=", 7) || !strcmp(Arg, "--menu")) {
		osd_set_menu_at_start(true);
		if (Arg[6] == '=') osd_set_menu_start_tab(atoi(Arg + 7));
		continue;
	}

	// --bbwatchdog=N - see bb_watchdog.cpp. 0 off, 1 default deadline, N>1 the
	// deadline in seconds. Kept as a flag rather than only an environment
	// variable because the thing it guards against happens at bring-up, which
	// is exactly when you are launching the binary by hand to watch it.
	if (!strncmp(Arg, "--bbwatchdog=", 13) || !strcmp(Arg, "--bbwatchdog")) {
		std::string spec = (Arg[12] == '=') ? std::string(Arg + 13) : std::string(__ArgValue);
		int n = atoi(spec.c_str());
		if (n < 0) {
			printf("--bbwatchdog: expected 0 (off), 1 (default), or seconds; got '%s'\n",
			       spec.c_str());
			return 1;
		}
		bb_watchdog_set_deadline(n);
		continue;
	}

	// --link selects the video source. Only "ar8030" exists; the flag is kept
	// so existing command lines and init scripts keep working. Both
	// "--link x" and "--link=x" parse.
	if (!strncmp(Arg, "--link=", 7) || !strcmp(Arg, "--link")) {
		std::string spec = (Arg[6] == '=') ? std::string(Arg + 7) : std::string(__ArgValue);
		if (spec != "ar8030") {
			printf("--link: unknown source '%s' (only 'ar8030' is supported)\n", spec.c_str());
			return 1;
		}
		continue;
	}





	__OnArgument("--stats") {
		console_stats = true;
		continue;
	}
	
	__OnArgument("--disable-vrr") {
		enable_vrr = false;
		continue;
	}

	__OnArgument("--ui-scale") {
		ui_scale = atof(__ArgValue);
		continue;
	}

	__OnArgument("--ar8030-chan-manual") {
		ar_cfg.chan_auto = false;
		ar_cfg.chan_manual_cli = true;
		continue;
	}

	__OnArgument("--ar8030-ap-index") {
		ar_cfg.ap_index = std::stoi(__ArgValue);
		continue;
	}

	__OnArgument("--ar8030-pair") {
		ar_cfg.do_pair = true;
		continue;
	}

	__OnArgument("--debug-no-handshake") {
		ar_cfg.skip_handshake = true;
		continue;
	}

	__OnArgument("--ar8030-mode") {
		// WxH@FPS, optionally prefixed with the channel: "1:1920x1080@60".
		// Channel 0 is the FPV stream we decode, 1 the air unit's recording.
		std::string v = __ArgValue;
		size_t colon = v.find(':');
		if (colon != std::string::npos) {
			ar_cfg.mode_chn = std::stoi(v.substr(0, colon));
			v = v.substr(colon + 1);
		}
		size_t x = v.find('x'), at = v.find('@');
		if (x == std::string::npos || at == std::string::npos || at < x) {
			printf("--ar8030-mode: expected [CHN:]WxH@FPS, e.g. 1920x1080@60\n");
			return 1;
		}
		ar_cfg.mode_w   = std::stoi(v.substr(0, x));
		ar_cfg.mode_h   = std::stoi(v.substr(x + 1, at - x - 1));
		ar_cfg.mode_fps = std::stoi(v.substr(at + 1));
		continue;
	}

	__OnArgument("--debug-probe-rf") {
		ar_cfg.probe_rf = true;
		continue;
	}

	__OnArgument("--debug-probe-modes") {
		ar_cfg.probe_modes = true;
		continue;
	}

	__OnArgument("--ar8030-power") {
		ar_cfg.tx_power_dbm = std::stoi(__ArgValue);
		continue;
	}

	__OnArgument("--ar8030-freq") {
		ar_cfg.freq_khz = (unsigned)std::stoul(__ArgValue);
		continue;
	}

	__OnArgument("--ar8030-bw") {
		ar_cfg.bandwidth = std::stoi(__ArgValue);
		continue;
	}

	__OnArgument("--ar8030-slot") {
		ar8030_slot = std::stoi(__ArgValue);
		continue;
	}

	__OnArgument("--ar8030-port") {
		ar8030_port = std::stoi(__ArgValue);
		continue;
	}

	__OnArgument("--debug-msp-bb-port") {
		ar_cfg.msp_bb_port = std::stoi(__ArgValue);
		continue;
	}

	__OnArgument("--debug-standby") {
		ar_cfg.standby_mode = std::stoi(__ArgValue);
		continue;
	}

	__OnArgument("--debug-bb-dump") {
		ar_cfg.dump_path = __ArgValue;
		continue;
	}

	__OnArgument("--debug-replay") {
		ar_cfg.replay_path = __ArgValue;
		continue;
	}

	__OnArgument("--debug-video-dump") {
		ar_cfg.video_dump_path = __ArgValue;
		continue;
	}

	__OnArgument("--no-osd") {
		enable_osd = 0;
		g_disable_gl = true;   // skip EGL/GLES entirely - GPU stays idle
		continue;
	}

	// Accepted, for the command lines that have them; nothing reads them.
	__OnArgument("--no-decode") {
		continue;
	}

	__OnArgument("--record") {
		dvr_filename = const_cast<char*>(__ArgValue);
		dvr_format = DvrFormat::RAW;
		continue;
	}

	__OnArgument("--verbose") {
		continue;
	}

	__OnArgument("--verbose-vid") {
		continue;
	}

	__OnArgument("--verbose-rf") {
		continue;
	}

	__OnArgument("--simulation") {
		simulation_mode = true;
		console_stats = true;
		enable_osd = 0;
		render_mode = Disable;
		continue;
	}

	__OnArgument("--version") {
		printf("kestrel-gnd v%s\n", KESTREL_GND_VERSION);
		return 0;
	}

	__EndParseConsoleArguments__;

	if (!dvr_filename.empty() && dvr_format != DvrFormat::RAW && dvr_framerate < 0 ) {
		printf("--dvr-framerate must be provided when (f)mp4 dvr is enabled.\n");
		return 0;
	}

	return kStart;
}
