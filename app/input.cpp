#include "app/input.hpp"

#include <errno.h>
#include <poll.h>
#include <unistd.h>

#include <cstdio>
#include <vector>

#include "app/signals.hpp"
#include "app/term.h"
#include "base/prof.hpp"
#include "base/scheduling_helper.hpp"
#include "base/time_util.h"
#include "bsp/bsp.hpp"
#include "bsp/vrx_buttons.hpp"
#include "dvr/dvr.hpp"
#include "hud/osd.hpp"
#include "system/gnd/radio.hpp"
// After the OSD: <linux/input.h> defines KEY_UP and the like as macros,
// which its own key names are.
#include "app/input_reader.hpp"

// Our radio, made in main() before the button threads start: the BIND button
// opens its pairing window.
static gnd::Radio* g_radio = nullptr;

void input_set_radio(gnd::Radio* r) { g_radio = r; }

// The VRX Pro's own front buttons are a SARADC resistor ladder, not an input
// device (see bsp/vrx_buttons.hpp), so they need their own poller. Mapped onto
// the same keys the keyboard path already uses, so menu behaviour is identical:
//   up/down    'w'/'s'  move between rows
//   left/right 'a'/'d'  change the selected value
//   ok         Enter    apply; on the live picture, open the menu
//   esc        Back     on the live picture, open the gallery; in the menu, close it
//   rec        'r'      currently unbound in OSD::handle_key - the app has no
//                       runtime record toggle yet, so this is a hook, not a
//                       working record button.
void* vrx_button_thread_func(void* arg) {
	OSD* osd = (OSD*)arg;
	SchedulingHelper::set_thread_params_max_realtime("VRX_BTN", 10);
	VrxButtons buttons(bsp::board().buttons);
	// The signal handler only sets a flag: DvrRecorder::toggle() takes a lock
	// and opens files, none of which is safe to do inside a signal.
	auto service_rec_signal = [&]() {
		if (!signals_take_rec_toggle()) return;
		bool on = DvrRecorder::instance().toggle();
		printf("SIGUSR1: rec -> %s\n", on ? "RECORDING" : "stopped");
		fflush(stdout);
		if (osd) osd->signal_render(prof::kWakeInput);
	};
	if (!buttons.available()) {
		printf("VRX buttons: no front-panel ADC channel on this board, front panel disabled\n");
		return nullptr;
	}
	printf("VRX buttons: front panel active (ADC ladder)\n");
	// The BIND button is a separate ADC channel, not part of the ladder above.
	// Polled on this same thread because it is the same kind of device and the
	// cadence suits it; see bsp/vrx_buttons.hpp for why it is read directly
	// rather than through its input device.
	VrxBindButton bind_button(bsp::board().bind);
	const bool have_bind = bind_button.available();
	printf("VRX buttons: bind button %s\n",
	       have_bind ? "active (ADC)" : "NOT readable - bind via the menu");
	while (!signal_stop) {
		service_rec_signal();
		if (have_bind && bind_button.pressed()) {
			// Binding is not a menu action - it has to work with the menu
			// shut, which is the state anyone reaching for this button is in.
			printf("VRX buttons: BIND pressed -> requesting bind\n");
			fflush(stdout);
			g_radio->bind();
			if (osd) osd->signal_render(prof::kWakeInput);
		}
		VrxButtons::Key b = buttons.poll(get_time_ms());
		if (b != VrxButtons::VB_NONE) {
			int key = 0;
			switch (b) {
				case VrxButtons::VB_UP:    key = 'w'; break;
				case VrxButtons::VB_DOWN:  key = 's'; break;
				case VrxButtons::VB_LEFT:  key = 'a'; break;
				case VrxButtons::VB_RIGHT: key = 'd'; break;
				case VrxButtons::VB_OK:    key = 13;  break;
				case VrxButtons::VB_ESC:   key = OSD::kKeyBack; break;
				case VrxButtons::VB_REC: {
					// Toggle recording directly: the OSD has no runtime record
					// action, and routing this through handle_key would just be
					// indirection. The indicator reads DvrRecorder's state.
					bool on = DvrRecorder::instance().toggle();
					printf("VRX buttons: rec -> %s\n", on ? "RECORDING" : "stopped");
					if (osd) osd->signal_render(prof::kWakeInput);
					key = 0;
					break;
				}
				default: break;
			}
			if (key) osd->handle_key(key);
		}
		usleep(25000);   // 25 ms; stock polls at 20 ms
	}
	return nullptr;
}

void* kb_thread_func(void* arg) {
    OSD* osd = (OSD*)arg;
    SchedulingHelper::set_thread_params_max_realtime("KB_THREAD", 10); // Low RT priority
    // Only treat stdin as a key source when it's an interactive terminal.
    // Under systemd (StandardInput=null) stdin is /dev/null, which polls as
    // always-readable and reads as EOF — polling it would spin this thread at
    // 100% CPU. As a service we rely solely on the evdev devices below.
    bool stdin_is_tty = isatty(STDIN_FILENO);
    if (stdin_is_tty) set_terminal_mode(1);

    InputReader input_reader;
    std::vector<unsigned char> esc_buf;
    int rescan_tick = 0;

    while (!signal_stop) {
        // Pick up input devices that appear at runtime (SSH menu bridge, USB
        // keyboard hotplug) — the startup scan alone would miss them.
        if (++rescan_tick >= 20) {   // every ~2s at the 100ms poll cadence
            rescan_tick = 0;
            input_reader.rescan();
        }

        std::vector<struct pollfd> pfds;
        int stdin_idx = -1;
        if (stdin_is_tty) {
            stdin_idx = (int)pfds.size();
            pfds.push_back({ STDIN_FILENO, POLLIN, 0 });
        }
        size_t evdev_start = pfds.size();
        for (int fd : input_reader.get_fds()) {
            pfds.push_back({ fd, POLLIN, 0 });
        }

        if (pfds.empty()) {
            // No input sources (service with no evdev keyboard attached) —
            // sleep instead of spinning on a zero-fd poll.
            usleep(100000); // 100ms, matches the poll timeout below
            continue;
        }

        int ret = poll(pfds.data(), pfds.size(), 100); // 100ms timeout
        if (ret < 0 && errno == EINTR) continue;
        if (ret <= 0) {
            if (!esc_buf.empty()) {
                for (auto c : esc_buf) osd->handle_key(c);
                esc_buf.clear();
            }
            continue;
        }

        // 1. Check STDIN (Normal terminal)
        if (stdin_idx >= 0 && (pfds[stdin_idx].revents & POLLIN)) {
            unsigned char buf[16];
            int n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n > 0) {
                for (int i = 0; i < n; i++) {
                    unsigned char c = buf[i];
                    if (!esc_buf.empty() || c == 27) {
                        esc_buf.push_back(c);
                        if (esc_buf.size() >= 3) {
                            if (esc_buf[0] == 27 && esc_buf[1] == 91) {
                                int code = esc_buf[2];
                                if (code == 65) osd->handle_key(0x101); // Up
                                else if (code == 66) osd->handle_key(0x102); // Down
                                else if (code == 67) osd->handle_key(0x103); // Right
                                else if (code == 68) osd->handle_key(0x104); // Left
                                else osd->handle_key(code);
                            } else {
                                // Not an arrow sequence we recognize, flush buffer
                                for (auto b : esc_buf) osd->handle_key(b);
                            }
                            esc_buf.clear();
                        } else if (esc_buf.size() == 2 && esc_buf[1] != 91) {
                            // Sequence like Alt+Key, flush
                            for (auto b : esc_buf) osd->handle_key(b);
                            esc_buf.clear();
                        }
                    } else {
                        osd->handle_key(c);
                    }
                }
            }
        }

        // 2. Check Evdev devices (Raw USB Keyboard/HID, even if service)
        std::vector<int> dead_fds;
        for (size_t i = evdev_start; i < pfds.size(); i++) {
            if (pfds[i].revents & (POLLERR | POLLHUP)) {
                dead_fds.push_back(pfds[i].fd);   // device node went away
                continue;
            }
            if (pfds[i].revents & POLLIN) {
                struct input_event ev;
                while (read(pfds[i].fd, &ev, sizeof(ev)) > 0) {
                    if (ev.type == EV_KEY) {
                        printf("[main-evdev] Event: code=%d, value=%d\n", ev.code, ev.value);
                        fflush(stdout);
                    }
                    if (ev.type == EV_KEY && ev.value == 1) { // Key Down
                        // The VRX Pro's BIND button. It is not on the front
                        // panel's SARADC ladder (see bsp/vrx_buttons.hpp) but
                        // on the separate "adc-keys" input device, SARADC
                        // channel 0 - and the device tree labels that node's
                        // keys "volume up/down/menu/back", a stock RK3568 node
                        // Caddx reused rather than renamed. This goggle has no
                        // volume control, so KEY_VOLUMEUP is the bind button
                        // and nothing else. Verified by capture: press drives
                        // channel 0 from 1023 to ~10 and emits code 115.
                        //
                        // Handled here rather than through handle_key() because
                        // binding is not a menu action - it has to work with
                        // the menu shut, which is the state anyone reaching for
                        // this button is already in.
                        if (ev.code == KEY_VOLUMEUP) {
                            printf("[main-evdev] BIND button -> requesting bind\n");
                            fflush(stdout);
                            g_radio->bind();
                            if (osd) osd->signal_render(prof::kWakeInput);
                            continue;
                        }
                        int key = 0;
                        switch (ev.code) {
                            case KEY_UP: key = 0x101; break;
                            case KEY_DOWN: key = 0x102; break;
                            case KEY_RIGHT: key = 0x103; break;
                            case KEY_LEFT: key = 0x104; break;
                            case KEY_ENTER: key = 13; break;
                            case KEY_ESC: key = OSD::kKeyBack; break; // Esc is the Back button
                            case KEY_M: key = 'm'; break;
                            case KEY_W: key = 'w'; break;
                            case KEY_S: key = 's'; break;
                            case KEY_A: key = 'a'; break;
                            case KEY_D: key = 'd'; break;
                            case KEY_G: key = 'g'; break;
                            case KEY_TAB: key = 9; break;
                            case KEY_BACKSPACE: key = 8; break;
                        }
                        if (key != 0) {
                            printf("[main-evdev] Handling key: 0x%x\n", key);
                            fflush(stdout);
                            osd->handle_key(key);
                        }
                    }
                }
            }
        }
        for (int fd : dead_fds) input_reader.remove(fd);
    }
    if (stdin_is_tty) set_terminal_mode(0);
    return NULL;
}

