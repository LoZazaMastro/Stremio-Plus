#define _GNU_SOURCE
#define pathconf shim_pathconf
#define fpathconf shim_fpathconf
#define fork shim_fork
#define openat shim_openat
#define fstatat shim_fstatat
#include "../native/shims.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
static void check(int okay, const char* message) {
    ++checks;
    if (!okay) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}
int main(void) {
    char directory[] = "/tmp/stremio-shims-XXXXXX";
    check(mkdtemp(directory) != NULL, "create temporary directory");
    char path[1024], link[1024];
    snprintf(path, sizeof(path), "%s/file", directory);
    snprintf(link, sizeof(link), "%s/link", directory);
    check(pathconf(directory, _PC_PATH_MAX) == 1024 && pathconf(directory, _PC_NAME_MAX) == 255,
          "native filesystem limits are available without a system-only import");
    errno = 0; check(pathconf(directory, -1) == -1 && errno == EINVAL, "unknown path limit fails explicitly");
    errno = 0; check(fork() == -1 && errno == ENOSYS, "process creation remains unsupported");
    const int file = openat(AT_FDCWD, path, O_CREAT | O_EXCL | O_RDWR, 0640);
    check(file >= 0, "openat forwards creation mode on absolute path");
    check(fpathconf(file, _PC_PATH_MAX) == 1024, "descriptor limit uses the native filesystem limit");
    struct stat info;
    check(fstat(file, &info) == 0 && (info.st_mode & 0777) == 0640, "variadic mode is forwarded intact");
    close(file);
    const int dir = open(directory, O_RDONLY | O_DIRECTORY);
    check(dir >= 0, "open real parent descriptor");
    errno = 0;
    check(openat(dir, "file", O_RDONLY) == -1 && errno == ENOSYS,
          "unsupported descriptor-relative path cannot silently use cwd");
    const int absolute = openat(dir, path, O_RDONLY);
    check(absolute >= 0, "absolute path ignores the parent descriptor");
    close(absolute);
    check(symlink(path, link) == 0, "create symlink fixture");
    check(fstatat(AT_FDCWD, link, &info, 0) == 0 && S_ISREG(info.st_mode), "stat follows links when requested");
    check(fstatat(AT_FDCWD, link, &info, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(info.st_mode),
          "stat preserves no-follow semantics");
    errno = 0; check(fstatat(AT_FDCWD, link, &info, 0x40000000) == -1 && errno == EINVAL,
                     "unknown stat flags cannot be ignored");
    errno = 0; check(fstatat(dir, "file", &info, 0) == -1 && errno == ENOSYS,
                     "unsupported relative stat fails explicitly");
    close(dir); unlink(link); unlink(path); rmdir(directory);
    printf("{\"result\":\"PASS\",\"checks\":%u}\n", checks);
}
