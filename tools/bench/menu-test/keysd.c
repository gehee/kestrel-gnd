// keysd: a uinput keyboard for bench tests on the goggle (tools/bench/menu-test).
//
// One device for the whole test, so kestrel-gnd's input rescan (every 2 s)
// finds it once. Commands come one per line from a FIFO:
//   <key> [hold_ms]   press and release (KEY_ name without the prefix:
//                     ENTER, ESC, UP, DOWN, LEFT, RIGHT, BACK, MENU, ...)
//   sleep <ms>        wait
//   quit              remove the device and exit
// Each command is acknowledged by a line "ok <n>" appended to the ack file,
// so the host knows a key has been sent before it reads the log.
//
//   keysd /tmp/keys.fifo /tmp/keys.ack &
//
// Static build: aarch64-...-gcc -O2 -static -o keysd keysd.c
#include <errno.h>
#include <fcntl.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct named { const char *name; int code; };
static const struct named keys[] = {
    {"ENTER", KEY_ENTER}, {"ESC", KEY_ESC}, {"UP", KEY_UP}, {"DOWN", KEY_DOWN},
    {"LEFT", KEY_LEFT}, {"RIGHT", KEY_RIGHT}, {"BACK", KEY_BACK}, {"MENU", KEY_MENU},
    {"OK", KEY_OK}, {"SELECT", KEY_SELECT}, {"SPACE", KEY_SPACE}, {"RECORD", KEY_RECORD},
    {"A", KEY_A}, {"B", KEY_B}, {"M", KEY_M}, {"R", KEY_R}, {"Q", KEY_Q},
};

static void sleep_ms(long ms) {
    struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};
    while (nanosleep(&t, &t) && errno == EINTR) {}
}

static int emit(int fd, int type, int code, int value) {
    struct input_event e;
    memset(&e, 0, sizeof e);
    e.type = type; e.code = code; e.value = value;
    return write(fd, &e, sizeof e) == (ssize_t)sizeof e ? 0 : -1;
}

static int code_of(const char *name) {
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
        if (!strcmp(keys[i].name, name)) return keys[i].code;
    return -1;
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: keysd FIFO ACKFILE\n"); return 2; }
    int ui = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (ui < 0) { perror("/dev/uinput"); return 1; }
    ioctl(ui, UI_SET_EVBIT, EV_KEY);
    ioctl(ui, UI_SET_EVBIT, EV_SYN);
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) ioctl(ui, UI_SET_KEYBIT, keys[i].code);
    struct uinput_setup s;
    memset(&s, 0, sizeof s);
    s.id.bustype = BUS_VIRTUAL; s.id.vendor = 0x1d6b; s.id.product = 0x7e57;
    snprintf(s.name, sizeof s.name, "kestrel-bench-keys");
    if (ioctl(ui, UI_DEV_SETUP, &s) || ioctl(ui, UI_DEV_CREATE)) { perror("uinput setup"); return 1; }
    unlink(argv[1]);
    if (mkfifo(argv[1], 0600)) { perror(argv[1]); return 1; }
    FILE *ack = fopen(argv[2], "w");
    if (!ack) { perror(argv[2]); return 1; }
    setvbuf(ack, NULL, _IOLBF, 0);
    fprintf(ack, "ready\n");
    unsigned n = 0;
    for (;;) {
        FILE *in = fopen(argv[1], "r");   // blocks until a writer opens it
        if (!in) { perror(argv[1]); break; }
        char line[128];
        while (fgets(line, sizeof line, in)) {
            char word[32] = {0};
            long arg = 0;
            int k = sscanf(line, "%31s %ld", word, &arg);
            if (k < 1) continue;
            if (!strcmp(word, "quit")) { fclose(in); goto done; }
            if (!strcmp(word, "sleep")) { sleep_ms(arg > 0 ? arg : 0); fprintf(ack, "ok %u sleep\n", ++n); continue; }
            int code = code_of(word);
            if (code < 0) { fprintf(ack, "err %u unknown %s\n", ++n, word); continue; }
            long hold = k == 2 && arg > 0 ? arg : 60;
            emit(ui, EV_KEY, code, 1); emit(ui, EV_SYN, SYN_REPORT, 0);
            sleep_ms(hold);
            emit(ui, EV_KEY, code, 0); emit(ui, EV_SYN, SYN_REPORT, 0);
            fprintf(ack, "ok %u %s\n", ++n, word);
        }
        fclose(in);
    }
done:
    ioctl(ui, UI_DEV_DESTROY);
    close(ui);
    unlink(argv[1]);
    fprintf(ack, "quit\n");
    return 0;
}
