#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>

extern "C" int _close(int file) {
    (void) file;
    errno = EBADF;
    return -1;
}

extern "C" int _fstat(int file, struct stat* st) {
    (void) file;
    st->st_mode = S_IFCHR;
    return 0;
}

extern "C" int _getpid() {
    return 1;
}

extern "C" int _isatty(int file) {
    (void) file;
    return 1;
}

extern "C" int _kill(int pid, int sig) {
    (void) pid;
    (void) sig;
    errno = EINVAL;
    return -1;
}

extern "C" off_t _lseek(int file, off_t ptr, int dir) {
    (void) file;
    (void) ptr;
    (void) dir;
    return 0;
}
