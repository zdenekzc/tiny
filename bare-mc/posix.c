/* Minimal POSIX stub implementation
 * For now this file only provides placeholder definitions required to link
 * the kernel. Full file‑system support will be added later.
 */

#include <stddef.h>
#include <stdint.h>
#include <unistd.h>      /* ssize_t, off_t, uid_t, gid_t */
#include <sys/types.h>  /* uid_t, gid_t */
#include <sys/stat.h>   /* struct stat */
#include <sys/time.h>   /* struct timeval */
#include <time.h>       /* struct timespec */
#include <dirent.h>

/* Stub for open – always fail */
int open(const char *pathname, int flags, ...) {
    (void)pathname; (void)flags; return -1;
}

/* Stub for read – always return 0 bytes */
ssize_t read(int fd, void *buf, size_t count) {
    (void)fd; (void)buf; (void)count; return 0;
}

/* Write – if fd is 1 or 2 (stdout/stderr) write to VGA, otherwise ignore */
ssize_t write(int fd, const void *buf, size_t count) {
    extern void vga_write(const char *buf, size_t len, uint8_t attr);
    if (fd == 1 || fd == 2) {
        vga_write((const char *)buf, count, 0x07); /* white on black */
        return (ssize_t)count;
    }
    return -1;
}

int close(int fd) { (void)fd; return 0; }
off_t lseek(int fd, off_t offset, int whence) { (void)fd; (void)offset; (void)whence; return 0; }
int fstat(int fd, struct stat *buf) { (void)fd; (void)buf; return -1; }
int stat(const char *pathname, struct stat *buf) { (void)pathname; (void)buf; return -1; }

/* Stubs for directory handling – not supported yet */
DIR *opendir(const char *name) { (void)name; return NULL; }
struct dirent *readdir(DIR *dirp) { (void)dirp; return NULL; }
int closedir(DIR *dirp) { (void)dirp; return 0; }

/* Misc stubs */
int getpid(void) { return 1; }
uid_t getuid(void) { return 0; }
uid_t getgid(void) { return 0; }
int gettimeofday(struct timeval *tv, void *tz) { (void)tv; (void)tz; return 0; }
int nanosleep(const struct timespec *req, struct timespec *rem) { (void)req; (void)rem; return 0; }
void _exit(int status) { (void)status; for (;;) __asm__("hlt"); }
