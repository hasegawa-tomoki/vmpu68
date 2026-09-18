/* SPDX-License-Identifier: MIT
 * Minimal newlib syscall stubs for the bare-metal build.  Musashi pulls in
 * newlib's stdio (fprintf/sscanf in rarely-used paths) and setjmp; these
 * stubs satisfy the syscall layer.  stdout/stderr output is discarded.
 */
#include <sys/stat.h>
#include <stddef.h>

int _write(int fd, const void *buf, size_t n) { (void)fd; (void)buf; return (int)n; }
int _read(int fd, void *buf, size_t n) { (void)fd; (void)buf; (void)n; return -1; }
int _close(int fd) { (void)fd; return -1; }
long _lseek(int fd, long off, int whence) { (void)fd; (void)off; (void)whence; return 0; }
int _fstat(int fd, struct stat *st) { (void)fd; st->st_mode = S_IFCHR; return 0; }
int _isatty(int fd) { (void)fd; return 1; }
int _getpid(void) { return 1; }
int _kill(int pid, int sig) { (void)pid; (void)sig; return -1; }
void _exit(int code) { (void)code; for (;;) ; }
void *_sbrk(long incr) { (void)incr; return (void *)-1; }

/* newlib's __libc_init_array/__libc_fini_array expect these; nothing to run */
void _init(void) {}
void _fini(void) {}
