// Stand-ins for optional hooks that the payload SDK's libraries reference
// weakly. A payload gets them from the ELF loader; a native app has none, and
// the converter needs every symbol resolved. Each reports "not available".
#include <stddef.h>

// (zstd's tracing hooks are in console_curl.c.)

// Dynamic loading (libc.a's dlopen family): not supported in the app.
void* __dlopen(const char* path, int mode) { (void)path; (void)mode; return NULL; }
void* __dlsym(void* handle, const char* name) { (void)handle; (void)name; return NULL; }
int __dlclose(void* handle) { (void)handle; return -1; }
char* __dlerror(void) { return (char*)"dynamic loading is not supported"; }
int __dladdr(const void* addr, void* info) { (void)addr; (void)info; return 0; }

// Kernel memory protection from the jailbreak's kernel helpers.
int kernel_mprotect(int pid, unsigned long addr, unsigned long len, int prot) {
	(void)pid; (void)addr; (void)len; (void)prot;
	return -1;
}

// ---- Calls only the system-process kernel library exports ---------------------------------
// pathconf, fpathconf, openat, fstatat and fork exist in libkernel_sys only.
// A title runs with the plain libkernel, so those imports stay at address 0
// and a call crashes: VLC's vlc_getcwd called pathconf(".", _PC_PATH_MAX)
// when it cached a file's embedded cover art (seen on hardware 2026-10-27).
// Each returns the "not supported" answer the callers already handle, or
// forwards to the plain call where the meaning is the same.
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

long pathconf(const char* path, int name) { (void)path; (void)name; errno = EINVAL; return -1; }
long fpathconf(int fd, int name) { (void)fd; (void)name; errno = EINVAL; return -1; }
pid_t fork(void) { errno = EAGAIN; return -1; }

int openat(int dirfd, const char* path, int flags, ...) {
	mode_t mode = 0;
	if (flags & O_CREAT) {
		va_list ap;
		va_start(ap, flags);
		mode = (mode_t)va_arg(ap, int);
		va_end(ap);
	}
	if (dirfd == AT_FDCWD || (path && path[0] == '/')) return open(path, flags, mode);
	errno = ENOSYS;
	return -1;
}

int fstatat(int dirfd, const char* path, struct stat* st, int flags) {
	if (dirfd == AT_FDCWD || (path && path[0] == '/'))
		return (flags & AT_SYMLINK_NOFOLLOW) ? lstat(path, st) : stat(path, st);
	errno = ENOSYS;
	return -1;
}
