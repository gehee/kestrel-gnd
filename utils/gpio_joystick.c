// kestrel joystick → uinput mapper. Two front-ends, same virtual keyboard:
//   GPIO mode (default): 7 discrete buttons shorted to GND (original behavior).
//   ADC mode (--adc):    resistor-ladder joystick (e.g. RunCam KEYA21C) on one
//                        SARADC channel — each direction presents a distinct
//                        resistance, read via /sys/bus/iio. 5 positions map to
//                        UP/DOWN/LEFT/RIGHT/ENTER; a LONG press of ENTER
//                        (>600ms) emits 'M' (menu toggle) as the 6th input.
// Calibration: run `--adc-probe`, note the raw value per direction (and idle),
// then run `--adc <chan> --adc-map idle,up,down,left,right,enter`.
// WIRING: SARADC inputs are 1.8V MAX — pull the ladder up to 1V8, never 3V3.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
#include <linux/uinput.h>
#include <time.h>
#include <stdint.h>

#define NUM_BUTTONS 7

// Pin definitions (using the Rockchip sysfs GPIO pin numbers)
// Formula: 32 * Bank + 8 * Group + Index (A=0, B=1, C=2, D=3)
// GPIO1_D2 = 32*1 + 8*3 + 2 = 58
// GPIO1_D3 = 32*1 + 8*3 + 3 = 59
// GPIO1_B0 = 32*1 + 8*1 + 0 = 40
// GPIO1_A7 = 32*1 + 8*0 + 7 = 39
// GPIO1_B4 = 32*1 + 8*1 + 4 = 44
// GPIO1_B5 = 32*1 + 8*1 + 5 = 45
// GPIO1_B3 = 32*1 + 8*1 + 3 = 43
static const int GPIO_PINS[NUM_BUTTONS] = {
    58, // UP (GPIO1_D2)
    59, // DOWN (GPIO1_D3)
    40, // LEFT (GPIO1_B0)
    39, // RIGHT (GPIO1_A7)
    44, // MID (GPIO1_B4) -> Map to SELECT/ENTER
    45, // SET (GPIO1_B5) -> Map to MENU ('m')
    43  // RST (GPIO1_B3) -> Map to TAB
};

static const int KEY_CODES[NUM_BUTTONS] = {
    KEY_UP,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_ENTER,  // MID -> KEY_ENTER
    KEY_M,      // SET -> KEY_M
    KEY_TAB     // RST -> KEY_TAB
};

static void setup_gpio(int pin) {
    char buf[128];
    // Export pin
    int fd = open("/sys/class/gpio/export", O_WRONLY);
    if (fd >= 0) {
        sprintf(buf, "%d", pin);
        write(fd, buf, strlen(buf));
        close(fd);
    }

    // Wait a moment for sysfs to populate
    usleep(100000);

    // Set direction to input
    sprintf(buf, "/sys/class/gpio/gpio%d/direction", pin);
    fd = open(buf, O_WRONLY);
    if (fd >= 0) {
        write(fd, "in", 2);
        close(fd);
    } else {
        perror("Failed to set direction");
    }

    // Set active_low to 1 (active low since buttons short to GND)
    sprintf(buf, "/sys/class/gpio/gpio%d/active_low", pin);
    fd = open(buf, O_WRONLY);
    if (fd >= 0) {
        write(fd, "1", 1);
        close(fd);
    }

    // Set edge to both (so poll detects both press and release)
    sprintf(buf, "/sys/class/gpio/gpio%d/edge", pin);
    fd = open(buf, O_WRONLY);
    if (fd >= 0) {
        write(fd, "both", 4);
        close(fd);
    }
}

static void send_event(int uinput_fd, int type, int code, int val) {
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.code = code;
    ev.value = val;
    write(uinput_fd, &ev, sizeof(ev));
}

static void send_key_click(int uinput_fd, int code) {
    send_event(uinput_fd, EV_KEY, code, 1);
    send_event(uinput_fd, EV_SYN, SYN_REPORT, 0);
    send_event(uinput_fd, EV_KEY, code, 0);
    send_event(uinput_fd, EV_SYN, SYN_REPORT, 0);
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
}

// ─── ADC (resistor-ladder) mode ────────────────────────────────────────────────

static int adc_read_raw(int chan) {
    char path[128], buf[16];
    snprintf(path, sizeof(path),
             "/sys/bus/iio/devices/iio:device0/in_voltage%d_raw", chan);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = 0;
    return atoi(buf);
}

// Stream all channels so the user can find their wiring and record the raw
// value the ladder produces in each stick position.
static int adc_probe(void) {
    printf("ADC probe: streaming all SARADC channels 5x/s. Move the stick and\n"
           "note (a) which channel reacts, (b) the raw value per direction and\n"
           "at rest. Ctrl-C to stop.\n\n");
    while (1) {
        printf("\r");
        for (int c = 0; c < 8; c++) {
            int v = adc_read_raw(c);
            if (v >= 0) printf("ch%d=%-5d ", c, v);
        }
        fflush(stdout);
        usleep(200000);
    }
    return 0;
}

// Positions: 0 idle, then 5 directions. Classified by nearest calibrated center;
// tolerance = 1/3 of the smallest gap between any two centers.
enum { POS_IDLE = 0, POS_UP, POS_DOWN, POS_LEFT, POS_RIGHT, POS_ENTER, POS_N };
static const char *POS_NAME[POS_N] = { "idle", "UP", "DOWN", "LEFT", "RIGHT", "ENTER" };
static const int   POS_KEY[POS_N]  = { 0, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_ENTER };

static int adc_classify(int raw, const int *centers, int tol) {
    int best = -1, best_d = tol + 1;
    for (int i = 0; i < POS_N; i++) {
        int d = abs(raw - centers[i]);
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;   // -1 = between windows: keep previous state
}

static int adc_run(int uinput_fd, int chan, const int centers[POS_N]) {
    int tol = 1 << 30;
    for (int i = 0; i < POS_N; i++)
        for (int j = i + 1; j < POS_N; j++) {
            int gap = abs(centers[i] - centers[j]);
            if (gap / 3 < tol) tol = gap / 3;
        }
    if (tol < 8) {
        fprintf(stderr, "adc map centers too close together (tol=%d)\n", tol);
        return 1;
    }
    printf("ADC joystick on ch%d, map:", chan);
    for (int i = 0; i < POS_N; i++) printf(" %s=%d", POS_NAME[i], centers[i]);
    printf(" tol=%d\n", tol);

    int cur = POS_IDLE, cand = POS_IDLE, cand_n = 0;
    uint64_t enter_down_ms = 0;
    while (1) {
        usleep(10000);   // 100 Hz
        int raw = adc_read_raw(chan);
        if (raw < 0) { usleep(200000); continue; }
        int cls = adc_classify(raw, centers, tol);
        if (cls < 0) cls = cur;                    // dead zone between windows

        if (cls != cand) { cand = cls; cand_n = 1; continue; }
        if (cand_n < 2) { cand_n++; continue; }    // 2-sample (20ms) debounce
        if (cls == cur) continue;

        // state transition
        if (cur == POS_ENTER) {
            // ENTER is deferred to release: short = ENTER click, long = 'M'
            uint64_t held = now_ms() - enter_down_ms;
            int code = (held >= 600) ? KEY_M : KEY_ENTER;
            printf("ENTER released after %llums -> %s\n",
                   (unsigned long long)held, code == KEY_M ? "MENU" : "ENTER");
            fflush(stdout);
            send_key_click(uinput_fd, code);
        } else if (cur != POS_IDLE) {
            send_event(uinput_fd, EV_KEY, POS_KEY[cur], 0);   // release direction
            send_event(uinput_fd, EV_SYN, SYN_REPORT, 0);
        }
        if (cls == POS_ENTER) {
            enter_down_ms = now_ms();
        } else if (cls != POS_IDLE) {
            printf("ADC %d -> %s PRESSED\n", raw, POS_NAME[cls]);
            fflush(stdout);
            send_event(uinput_fd, EV_KEY, POS_KEY[cls], 1);
            send_event(uinput_fd, EV_SYN, SYN_REPORT, 0);
        }
        cur = cls;
    }
    return 0;
}

// ─── --keys: interactive stdin → uinput bridge (menu control over SSH) ────────
// Run with a tty (`ssh -t <host> kestrel-gpio-joystick --keys`); arrows/WASD
// navigate, Enter selects, m toggles the menu, Tab switches tabs, q quits.
#include <termios.h>

static struct termios keys_saved_tio;
static void keys_restore_tty(void) {
    tcsetattr(STDIN_FILENO, TCSANOW, &keys_saved_tio);
}

static int keys_run(int uinput_fd) {
    if (!isatty(STDIN_FILENO)) {
        fprintf(stderr, "--keys needs a tty (use: ssh -t <host> %s --keys)\n",
                "kestrel-gpio-joystick");
        return 1;
    }
    tcgetattr(STDIN_FILENO, &keys_saved_tio);
    atexit(keys_restore_tty);
    struct termios tio = keys_saved_tio;
    tio.c_lflag &= ~(ICANON | ECHO);
    tio.c_cc[VMIN] = 1;
    tio.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &tio);

    printf("SSH menu control: arrows/WASD move, Enter select, m menu, Tab tab, q quit\n");
    while (1) {
        unsigned char c;
        if (read(STDIN_FILENO, &c, 1) != 1) break;
        int code = 0;
        if (c == 27) {                              // ESC sequence (arrows)
            unsigned char s[2] = {0, 0};
            if (read(STDIN_FILENO, &s[0], 1) == 1 && s[0] == '[' &&
                read(STDIN_FILENO, &s[1], 1) == 1) {
                switch (s[1]) {
                    case 'A': code = KEY_UP;    break;
                    case 'B': code = KEY_DOWN;  break;
                    case 'C': code = KEY_RIGHT; break;
                    case 'D': code = KEY_LEFT;  break;
                }
            }
        } else switch (c) {
            case 'w': case 'W': code = KEY_UP;    break;
            case 's': case 'S': code = KEY_DOWN;  break;
            case 'a': case 'A': code = KEY_LEFT;  break;
            case 'd': case 'D': code = KEY_RIGHT; break;
            case '\r': case '\n': code = KEY_ENTER; break;
            case 'm': case 'M': code = KEY_M;     break;
            case '\t':          code = KEY_TAB;   break;
            case 'q': case 'Q': case 3: return 0;  // q / Ctrl-C
        }
        if (code) send_key_click(uinput_fd, code);
    }
    return 0;
}

static int setup_uinput(void) {
    int uinput_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (uinput_fd < 0) {
        perror("Failed to open /dev/uinput");
        return -1;
    }

    ioctl(uinput_fd, UI_SET_EVBIT, EV_KEY);
    for (int i = 0; i < NUM_BUTTONS; i++) {
        ioctl(uinput_fd, UI_SET_KEYBIT, KEY_CODES[i]);
    }

    struct uinput_user_dev uidev;
    memset(&uidev, 0, sizeof(uidev));
    snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "kestrel-gpio-joystick");
    uidev.id.bustype = BUS_USB;
    uidev.id.vendor  = 0x1234;
    uidev.id.product = 0x5678;
    uidev.id.version = 1;

    if (write(uinput_fd, &uidev, sizeof(uidev)) < 0) {
        perror("Failed to write uinput device description");
        return -1;
    }
    if (ioctl(uinput_fd, UI_DEV_CREATE) < 0) {
        perror("Failed to create uinput device");
        return -1;
    }
    return uinput_fd;
}

int main(int argc, char **argv) {
    int adc_chan = -1;
    int centers[POS_N] = {0};
    int have_map = 0;

    for (int a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "--adc-probe")) {
            return adc_probe();
        } else if (!strcmp(argv[a], "--adc") && a + 1 < argc) {
            adc_chan = atoi(argv[++a]);
        } else if (!strcmp(argv[a], "--adc-map") && a + 1 < argc) {
            if (sscanf(argv[++a], "%d,%d,%d,%d,%d,%d",
                       &centers[0], &centers[1], &centers[2],
                       &centers[3], &centers[4], &centers[5]) == POS_N)
                have_map = 1;
        } else if (!strcmp(argv[a], "--keys")) {
            int uinput_fd = setup_uinput();
            if (uinput_fd < 0) return 1;
            usleep(300000);   // let the OSD's evdev scan pick up the new device
            return keys_run(uinput_fd);
        } else {
            printf("usage: %s                      GPIO buttons mode (default)\n"
                   "       %s --keys               menu control from this terminal (ssh -t)\n"
                   "       %s --adc-probe          stream ADC channels (calibration)\n"
                   "       %s --adc N --adc-map idle,up,down,left,right,enter\n",
                   argv[0], argv[0], argv[0], argv[0]);
            return 1;
        }
    }

    if (adc_chan >= 0) {
        if (!have_map) {
            fprintf(stderr, "--adc requires --adc-map (run --adc-probe to calibrate)\n");
            return 1;
        }
        int uinput_fd = setup_uinput();
        if (uinput_fd < 0) return 1;
        return adc_run(uinput_fd, adc_chan, centers);
    }

    // ─── GPIO mode (original behavior) ─────────────────────────────────────────
    // 1. Initialize GPIOs
    for (int i = 0; i < NUM_BUTTONS; i++) {
        setup_gpio(GPIO_PINS[i]);
    }

    // 2. Set up uinput virtual keyboard
    int uinput_fd = setup_uinput();
    if (uinput_fd < 0) return 1;

    // 3. Open value file descriptors for select/polling
    int value_fds[NUM_BUTTONS];
    for (int i = 0; i < NUM_BUTTONS; i++) {
        char buf[128];
        sprintf(buf, "/sys/class/gpio/gpio%d/value", GPIO_PINS[i]);
        value_fds[i] = open(buf, O_RDONLY);
        if (value_fds[i] < 0) {
            fprintf(stderr, "Failed to open value file for GPIO %d\n", GPIO_PINS[i]);
            return 1;
        }
    }

    printf("kestrel-gpio-joystick virtual driver running...\n");

    // 4. Event loop using select()
    while (1) {
        fd_set exceptfds;
        FD_ZERO(&exceptfds);
        int max_fd = -1;

        for (int i = 0; i < NUM_BUTTONS; i++) {
            FD_SET(value_fds[i], &exceptfds);
            if (value_fds[i] > max_fd) {
                max_fd = value_fds[i];
            }
            // Seek to beginning to clear any previous triggers
            lseek(value_fds[i], 0, SEEK_SET);
        }

        int ret = select(max_fd + 1, NULL, NULL, &exceptfds, NULL);
        if (ret < 0) {
            perror("select error");
            break;
        }

        for (int i = 0; i < NUM_BUTTONS; i++) {
            if (FD_ISSET(value_fds[i], &exceptfds)) {
                char val_char;
                lseek(value_fds[i], 0, SEEK_SET);
                if (read(value_fds[i], &val_char, 1) > 0) {
                    int val = (val_char == '1') ? 1 : 0;
                    
                    const char* key_name = "UNKNOWN";
                    switch (KEY_CODES[i]) {
                        case KEY_UP:    key_name = "UP"; break;
                        case KEY_DOWN:  key_name = "DOWN"; break;
                        case KEY_LEFT:  key_name = "LEFT"; break;
                        case KEY_RIGHT: key_name = "RIGHT"; break;
                        case KEY_M:     key_name = "MENU ('m')"; break;
                        case KEY_ENTER: key_name = "ENTER"; break;
                        case KEY_ESC:   key_name = "ESC"; break;
                        case KEY_TAB:   key_name = "TAB"; break;
                    }
                    
                    printf("GPIO %d -> Key %s: %s\n", GPIO_PINS[i], key_name, val ? "PRESSED" : "RELEASED");
                    fflush(stdout);
                    // Send corresponding key press/release event
                    send_event(uinput_fd, EV_KEY, KEY_CODES[i], val);
                    send_event(uinput_fd, EV_SYN, SYN_REPORT, 0);
                }
            }
        }
    }

    // Cleanup
    for (int i = 0; i < NUM_BUTTONS; i++) {
        close(value_fds[i]);
    }
    ioctl(uinput_fd, UI_DEV_DESTROY);
    close(uinput_fd);
    return 0;
}
