#include "base/shell.hpp"
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdio>
int main() {
    const int fd=open("/dev/null",O_RDONLY);
    if(fd<0)return 1;
    const int inherited=fcntl(fd,F_DUPFD,200);
    close(fd);
    if(inherited<0)return 1;
    char command[128];
    snprintf(command,sizeof(command),"test ! -e /proc/self/fd/%d",inherited);
    const int closed=run_shell(command);
    const bool parent_kept=fcntl(inherited,F_GETFD)>=0;
    close(inherited);
    const int status=run_shell("exit 23");
    if(closed!=0 || !parent_kept || !WIFEXITED(status) || WEXITSTATUS(status)!=23)return 1;
    puts("Portable spawn closes inherited descriptors and preserves exit status");
}
