#include "base/shell.hpp"

#include <cerrno>

#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

// Spawned as system() does (no copy of kestrel's memory map, which fork()
// would lock the RT threads out of while it is made).
int run_shell(const char* cmd) {
    char* const argv[] = { (char*)"sh", (char*)"-c", (char*)cmd, nullptr };
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t at;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addclosefrom_np(&fa, 3);
    posix_spawnattr_init(&at);
    sigset_t none;
    sigemptyset(&none);
    posix_spawnattr_setsigmask(&at, &none);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGMASK);
    pid_t pid = -1;
    const int err = posix_spawn(&pid, "/bin/sh", &fa, &at, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    if (err != 0) return 127 << 8;   // as system(): the shell "exited 127"
    int status = 0;
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR) return -1;
    return status;
}
