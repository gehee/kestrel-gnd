#include "base/shell.hpp"

#include <cerrno>
#include <cstdlib>
#include <dirent.h>

#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

int spawn_close_descriptors(posix_spawn_file_actions_t* fa) {
#if defined(__GLIBC__) && !defined(KESTREL_TEST_PORTABLE_SPAWN)
    return posix_spawn_file_actions_addclosefrom_np(fa, 3);
#else
    // musl has no addclosefrom extension. Close the open descriptors through
    // standard spawn actions; descriptors opened concurrently must use CLOEXEC.
    DIR* fds = opendir("/proc/self/fd");
    if (!fds) return errno;
    int action_error = 0;
    while (const dirent* entry = readdir(fds)) {
        char* end = nullptr;
        const long fd = strtol(entry->d_name, &end, 10);
        if (end != entry->d_name && !*end && fd >= 3 && fd != dirfd(fds)) {
            action_error = posix_spawn_file_actions_addclose(fa, int(fd));
            if (action_error) break;
        }
    }
    closedir(fds);
    return action_error;
#endif
}

// Spawned as system() does (no copy of kestrel's memory map, which fork()
// would lock the RT threads out of while it is made).
int run_shell(const char* cmd) {
    char* const argv[] = { (char*)"sh", (char*)"-c", (char*)cmd, nullptr };
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t at;
    posix_spawn_file_actions_init(&fa);
    if (spawn_close_descriptors(&fa)) {
        posix_spawn_file_actions_destroy(&fa);
        return 127 << 8;
    }
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
