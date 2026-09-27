#include <termios.h>
#include <poll.h>
#include <unistd.h>
#include <stdio.h>

#ifndef FPVUE_TERM_UTIL_H
#define FPVUE_TERM_UTIL_H


inline int getch() {
    struct termios oldt, newt;
    int ch;

    // Get the current terminal settings
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;

    // Disable canonical mode and echo
    newt.c_lflag &= ~(ICANON | ECHO);

    // Set the new terminal settings
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);

    // Read a single character
    ch = getchar();

    // Restore the original terminal settings
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);

    return ch;
}

inline int getch_timeout(int timeout_ms) {
    struct pollfd pfd = { STDIN_FILENO, POLLIN, 0 };
    int ret = poll(&pfd, 1, timeout_ms);
    if (ret > 0 && (pfd.revents & POLLIN)) {
        return getch();
    }
    return -1;
}

inline void set_terminal_mode(int enable) {
    if (!isatty(STDIN_FILENO)) return;
    static struct termios oldt, newt;

    if (enable) {
        if (tcgetattr(STDIN_FILENO, &oldt) < 0) return; // Get current terminal attributes
        newt = oldt;
        newt.c_lflag &= ~(ICANON | ECHO);         // Disable canonical mode and echo
        tcsetattr(STDIN_FILENO, TCSANOW, &newt);  // Apply changes immediately
    } else {
        tcsetattr(STDIN_FILENO, TCSANOW, &oldt);  // Restore original terminal settings
    }
}

#endif //FPVUE_TERM_UTIL_H