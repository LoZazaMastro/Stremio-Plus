// Startup storage regression: run the production PS5 resolver with POSIX calls
// redirected into a real temporary filesystem, and inject native access limits.
#include "ps5_storage.h"
#include "filesystem/elevation.hpp"
#include "download_writer/client.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
namespace {
std::string root, fail_operation, fail_path;
std::map<int, std::string> descriptors;
std::map<DIR*, std::string> directories;
int failure_errno = EPERM, helper_calls = 0, checks = 0;
bool granted = false, repair_on_grant = true, deny_until_grant = true;
bool corrupt_read = false, short_io = false, interrupt_write = false;
bool short_migration_io = false, interrupt_migration_write = false, late_target = false;
std::string configured_writer;
elevation::Status helper_status = elevation::Status::ok;

void check(bool condition, const char* message) {
    ++checks;
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

bool native_path(const char* path) {
    return std::string_view(path).starts_with("/data") ||
        std::string_view(path).starts_with("/app0") ||
        std::string_view(path).starts_with("/download0") ||
        std::string_view(path).starts_with("/mnt/sandbox/");
}

std::string mapped(const char* path) {
    if (!native_path(path)) return path;
    if (granted && (std::string_view(path).starts_with("/app0") ||
                    std::string_view(path).starts_with("/download0")))
        return root + "/unmounted" + path;
    return root + path;
}

bool denied(const char* operation, const std::string& path) {
    if (operation != fail_operation || (deny_until_grant && granted) ||
        path.find(fail_path) == std::string::npos || !native_path(path.c_str())) return false;
    errno = failure_errno;
    return true;
}

std::string descriptor_path(int descriptor) {
    auto it = descriptors.find(descriptor);
    return it == descriptors.end() ? "" : it->second;
}

bool is_probe(int descriptor) {
    return descriptor_path(descriptor).find(".storage-probe-") != std::string::npos;
}

void reset() {
    ps5_boot_close();
    fs::remove_all(root);
    for (const char* directory : {"/data/Stremio", "/download0/stremio",
         "/app0/hui/fonts", "/mnt/sandbox/PPSA74126_000/app0/hui/fonts",
         "/mnt/sandbox/PPSA74126_000/download0/stremio"})
        fs::create_directories(root + directory);
    for (const char* base : {"/app0", "/mnt/sandbox/PPSA74126_000/app0"})
        std::ofstream(root + base + "/hui/fonts/inter-regular.huifont") << "font";
    fail_operation.clear(); fail_path.clear(); failure_errno = EPERM;
    helper_calls = 0; granted = false; repair_on_grant = deny_until_grant = true;
    helper_status = elevation::Status::ok;
    corrupt_read = short_io = interrupt_write = false;
    short_migration_io = interrupt_migration_write = late_target = false;
    descriptors.clear();
    directories.clear();
}

std::string boot_text() {
    std::ifstream input(root + "/data/Stremio/boot-current.txt");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void check_no_probe() {
    for (const auto& item : fs::recursive_directory_iterator(root + "/data/Stremio")) {
        const auto name = item.path().filename().string();
        check(!name.starts_with(".storage-probe-") && !name.starts_with(".migration-"),
            "startup removes its probe and migration temporary files");
    }
}
std::string read_file(const std::string& path) {
    std::ifstream input(root + path);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void legacy_settings() {
    std::ofstream(root + "/download0/stremio/settings.json") << "{\"authKey\":\"fake-sensitive-token\"}";
}
} // namespace

extern "C" {
int __real_stat(const char*, struct stat*);
int __real_lstat(const char*, struct stat*);
int __real_mkdir(const char*, mode_t);
int __real_chmod(const char*, mode_t);
int __real_fchmod(int, mode_t);
int __real_open(const char*, int, ...);
int __real_close(int);
int __real_fstat(int, struct stat*);
int __real_fsync(int);
ssize_t __real_read(int, void*, size_t);
ssize_t __real_write(int, const void*, size_t);
int __real_rename(const char*, const char*);
int __real_unlink(const char*);
int __real_rmdir(const char*);
DIR* __real_opendir(const char*);
struct dirent* __real_readdir(DIR*);
int __real_closedir(DIR*);

int __wrap_stat(const char* path, struct stat* info) {
    return denied("stat", path) ? -1 : __real_stat(mapped(path).c_str(), info);
}
int __wrap_lstat(const char* path, struct stat* info) {
    return denied("lstat", path) ? -1 : __real_lstat(mapped(path).c_str(), info);
}
int __wrap_mkdir(const char* path, mode_t mode) {
    return denied("mkdir", path) ? -1 : __real_mkdir(mapped(path).c_str(), mode);
}
int __wrap_chmod(const char* path, mode_t mode) {
    return denied("chmod", path) ? -1 : __real_chmod(mapped(path).c_str(), mode);
}
int __wrap_fchmod(int descriptor, mode_t mode) {
    return denied("fchmod", descriptor_path(descriptor)) ? -1 : __real_fchmod(descriptor, mode);
}
int __wrap_open(const char* path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list args; va_start(args, flags); mode = va_arg(args, int); va_end(args); }
    if (denied("open", path)) return -1;
    const int descriptor = __real_open(mapped(path).c_str(), flags, mode);
    if (descriptor >= 0 && native_path(path)) descriptors[descriptor] = path;
    return descriptor;
}
int __wrap_close(int descriptor) {
    descriptors.erase(descriptor);
    return __real_close(descriptor);
}
int __wrap_fstat(int descriptor, struct stat* info) {
    return denied("fstat", descriptor_path(descriptor)) ? -1 : __real_fstat(descriptor, info);
}
int __wrap_fsync(int descriptor) {
    if (late_target && descriptor_path(descriptor).find(".migration-") != std::string::npos) {
        late_target = false;
        std::ofstream(root + "/data/Stremio/appdata/settings.json") << "newer destination";
    }
    return denied("fsync", descriptor_path(descriptor)) ? -1 : __real_fsync(descriptor);
}
ssize_t __wrap_write(int descriptor, const void* bytes, size_t length) {
    if (denied("write", descriptor_path(descriptor))) return -1;
    if (is_probe(descriptor)) {
        if (interrupt_write) { interrupt_write = false; errno = EINTR; return -1; }
        if (short_io) length = std::min(length, size_t(3));
    }
    if (descriptor_path(descriptor).find(".migration-") != std::string::npos) {
        if (interrupt_migration_write) { interrupt_migration_write = false; errno = EINTR; return -1; }
        if (short_migration_io) length = std::min(length, size_t(3));
    }
    return __real_write(descriptor, bytes, length);
}
ssize_t __wrap_read(int descriptor, void* bytes, size_t length) {
    if (denied("read", descriptor_path(descriptor))) return -1;
    if (short_io && is_probe(descriptor)) length = std::min(length, size_t(4));
    if (short_migration_io && descriptor_path(descriptor).find("/download0/stremio/") != std::string::npos)
        length = std::min(length, size_t(4));
    const auto count = __real_read(descriptor, bytes, length);
    if (corrupt_read && is_probe(descriptor) && count > 0) static_cast<char*>(bytes)[0] ^= 1;
    return count;
}
int __wrap_rename(const char* source, const char* target) {
    return denied("rename", source) ? -1 : __real_rename(mapped(source).c_str(), mapped(target).c_str());
}
int __wrap_unlink(const char* path) {
    return denied("unlink", path) ? -1 : __real_unlink(mapped(path).c_str());
}
int __wrap_rmdir(const char* path) {
    return denied("rmdir", path) ? -1 : __real_rmdir(mapped(path).c_str());
}
DIR* __wrap_opendir(const char* path) {
    if (denied("opendir", path)) return nullptr;
    DIR* directory = __real_opendir(mapped(path).c_str());
    if (directory && native_path(path)) directories[directory] = path;
    return directory;
}
struct dirent* __wrap_readdir(DIR* directory) {
    const auto found = directories.find(directory);
    if (found != directories.end() && denied("readdir", found->second)) return nullptr;
    return __real_readdir(directory);
}
int __wrap_closedir(DIR* directory) {
    directories.erase(directory);
    return __real_closedir(directory);
}
int sceKernelDebugOutText(int, const char*) { return 0; }
} // extern C

elevation::Status elevation::request(Capability capability, const char* path) noexcept {
    ++helper_calls;
    check(capability == Capability::filesystem, "only the existing filesystem capability is requested");
    check(std::strcmp(path, "/app0/sandbox-elevator.elf") == 0, "existing bundled helper path is preserved");
    if (helper_status == Status::ok && repair_on_grant) granted = true;
    return helper_status;
}

void download_writer::configure_helper(std::string path) { configured_writer = std::move(path); }

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    root = argv[1];

    // Logs/stat can succeed under ShadowMount's partial mapping. The old code
    // incorrectly treated that as a sufficient capability and skipped the helper.
    reset();
    fail_operation = "lstat"; fail_path = "/data";
    const auto partial = ps5_prepare_storage();
    check(helper_calls == 1 && partial.helper_requested, "lstat EPERM requests helper despite writable logs");
    check(partial.filesystem_available && partial.logs_available, "verified post-grant storage becomes available");
    check(std::string(partial.app) == "/mnt/sandbox/PPSA74126_000/app0", "app mount re-resolves after root changes");
    check(partial.data_available && std::string(partial.data) == "/data/Stremio/appdata", "private appdata resolves outside the removed reservation after grant");
    check(configured_writer == "/mnt/sandbox/PPSA74126_000/app0/download-writer.elf",
        "download writer uses the resolved app mount after filesystem elevation");
    check(boot_text().find("before=\"lstat /data\" before_errno=" + std::to_string(EPERM)) != std::string::npos,
        "boot diagnostics preserve original denied operation and errno");
    check(boot_text().find("after=\"ready\" after_errno=0") != std::string::npos,
        "boot diagnostics distinguish proof after grant");
    check(boot_text().find(std::string("[Stremio Plus ") + STREMIO_VERSION + " " + STREMIO_TITLE_ID + "]") != std::string::npos,
        "boot uses the same injected build identity as the application");
    check_no_probe();

    reset();
    const auto existing = ps5_prepare_storage();
    check(existing.filesystem_available && !existing.helper_requested && helper_calls == 0,
        "complete existing access skips the helper");
    check(std::string(existing.app) == "/app0", "existing sandbox app mount stays selected");
    check(ps5_prepare_storage().data_available && descriptors.size() == 1,
        "pre-init storage retry retains only the current boot receipt descriptor");
    check_no_probe();

    reset();
    fs::create_directory(root + "/data/Stremio/downloads");
    fs::create_directory(root + "/data/Stremio/appdata");
    check(::chmod((root + "/data/Stremio/appdata").c_str(), 0775) == 0, "set existing appdata permissions");
    check(::chmod((root + "/data/Stremio").c_str(), 0777) == 0 &&
          ::chmod((root + "/data/Stremio/downloads").c_str(), 0777) == 0,
        "existing user permissions are configured for the startup regression");
    check(ps5_prepare_storage().filesystem_available, "startup accepts existing user-writable folders");
    struct stat mode_info{};
    check(::lstat((root + "/data/Stremio").c_str(), &mode_info) == 0 &&
          (mode_info.st_mode & 0777) == 0777,
        "startup preserves the existing Stremio folder permissions");
    check(::lstat((root + "/data/Stremio/downloads").c_str(), &mode_info) == 0 &&
          (mode_info.st_mode & 0777) == 0777,
        "startup preserves the existing downloads folder permissions");
    check(::lstat((root + "/data/Stremio/appdata").c_str(), &mode_info) == 0 &&
          (mode_info.st_mode & 0777) == 0777,
        "startup repairs the shared appdata directory permissions");
    const auto permission_receipt = boot_text();
    check(permission_receipt.find("phase=before path=/data/Stremio mode=0777") != std::string::npos &&
          permission_receipt.find("phase=after path=/data/Stremio/downloads mode=0777") != std::string::npos,
        "boot diagnostics retain before and after directory modes");
    check(permission_receipt.find("storage_process euid_before=") != std::string::npos &&
          permission_receipt.find(" owner=") != std::string::npos,
        "boot diagnostics distinguish directory ownership from process credentials");
    check_no_probe();

    reset();
    fs::remove_all(root + "/data/Stremio");
    const mode_t saved_umask = ::umask(0077);
    const auto masked = ps5_prepare_storage();
    ::umask(saved_umask);
    check(masked.data_available && masked.logs_available, "fresh shared storage works with loader umask0077");
    for (const char* path : {"/data/Stremio", "/data/Stremio/appdata"})
        check(::lstat((root + path).c_str(), &mode_info) == 0 && (mode_info.st_mode & 0777) == 0777,
              "shared directory mode is verified after restrictive creation");
    check(::chmod((root + "/data/Stremio/appdata").c_str(), 0700) == 0, "simulate older private appdata");
    std::ofstream(root + "/data/Stremio/appdata/keep.txt") << "retained";
    check(ps5_prepare_storage().data_available, "restart repairs older restrictive storage");
    check(::lstat((root + "/data/Stremio/appdata").c_str(), &mode_info) == 0 &&
          (mode_info.st_mode & 0777) == 0777 && read_file("/data/Stremio/appdata/keep.txt") == "retained",
          "permission repair retains files across restart");
    check_no_probe();

    reset(); fail_operation = "lstat"; fail_path = "/data";
    helper_status = elevation::Status::unavailable;
    const auto missing_helper = ps5_prepare_storage();
    check(!missing_helper.filesystem_available && missing_helper.logs_available,
        "helper refusal leaves downloads unavailable but retains diagnostic logs");
    check(!missing_helper.data_available && missing_helper.data_errno == EPERM &&
          !fs::exists(root + "/data/Stremio/appdata"),
        "missing genuine data access never creates appdata or exposes empty account storage");
    check(helper_calls == 1 && missing_helper.filesystem_status == int(helper_status),
        "refused grant is recorded and never retried in a loop");

    reset(); fail_operation = "lstat"; fail_path = "/data"; repair_on_grant = false;
    const auto false_success = ps5_prepare_storage();
    check(!false_success.filesystem_available && false_success.logs_available,
        "helper success alone does not replace the real filesystem proof");
    check(boot_text().find("after=\"lstat /data\"") != std::string::npos,
        "failed post-grant proof reports the actual operation");

    for (const auto& operation : {"lstat", "opendir", "readdir", "chmod", "fchmod", "fstat", "open"}) {
        reset(); fail_operation = operation;
        fail_path = (fail_operation == "lstat" || fail_operation == "opendir" || fail_operation == "readdir") ?
            "/data/Stremio" : ".storage-probe-";
        const auto repaired = ps5_prepare_storage();
        check(helper_calls == 1 && repaired.filesystem_available,
            "each denied download directory/file operation triggers a verified grant");
        check_no_probe();
    }

    reset(); fail_operation = "fsync"; fail_path = ".storage-probe-";
    failure_errno = ENOSPC; deny_until_grant = false;
    const auto full = ps5_prepare_storage();
    check(!full.filesystem_available && full.logs_available && helper_calls == 0,
        "allocation failure is reported without an unnecessary filesystem grant");
    check(boot_text().find("fsync storage probe") != std::string::npos,
        "fsync failure has a distinct boot diagnostic");
    check_no_probe();

    reset(); fail_operation = "rename"; fail_path = ".storage-probe-";
    failure_errno = EIO; deny_until_grant = false;
    check(!ps5_prepare_storage().filesystem_available && helper_calls == 0,
        "atomic rename failure is detected before queue use");
    check_no_probe();

    reset(); corrupt_read = true;
    check(!ps5_prepare_storage().filesystem_available && helper_calls == 0,
        "readback corruption fails proof without requesting more permissions");
    check_no_probe();

    reset(); short_io = interrupt_write = true;
    check(ps5_prepare_storage().filesystem_available && helper_calls == 0,
        "partial reads/writes and EINTR do not cause false storage failures");
    check_no_probe();

    reset();
    const auto old_probe = root + "/data/Stremio/.storage-probe-" + std::to_string(getpid()) + "-0";
    fs::create_directory(old_probe);
    std::ofstream(old_probe + "/keep.txt") << "previous launch";
    check(ps5_prepare_storage().filesystem_available, "probe skips a previous launch's directory");
    check(fs::exists(old_probe + "/keep.txt"), "probe never deletes an existing directory's data");

    reset();
    fs::remove_all(root + "/download0");
    fs::remove_all(root + "/mnt/sandbox/PPSA74126_000/download0");
    const auto fresh = ps5_prepare_storage();
    check(fresh.filesystem_available && fresh.data_available && !fresh.helper_requested,
        "fresh install starts without any reserved download0 mount");
    check(std::string(fresh.data) == "/data/Stremio/appdata", "fresh install uses grow-on-demand appdata");
    check(::lstat((root + "/data/Stremio/appdata").c_str(), &mode_info) == 0 &&
          (mode_info.st_mode & 0777) == 0777, "new appdata is shared with console file tools");
    check(fs::is_empty(root + "/data/Stremio/appdata"), "fresh storage does not preallocate caches or media");
    check_no_probe();

    reset(); legacy_settings();
    std::ofstream(root + "/download0/stremio/progress.json") << "{\"resume\":123}";
    std::ofstream(root + "/download0/stremio/config.json") << "{\"preferred\":\"en\"}";
    std::ofstream(root + "/download0/stremio/cache.bin") << "never migrate cache";
    std::ofstream(root + "/download0/stremio/unknown.json") << "never migrate arbitrary data";
    const auto migrated = ps5_prepare_storage();
    check(migrated.data_available, "reachable legacy account and progress migrate");
    for (const char* name : {"settings.json", "progress.json", "config.json"}) {
        check(read_file(std::string("/data/Stremio/appdata/") + name) ==
              read_file(std::string("/download0/stremio/") + name), "whitelisted settings copy exact bytes and retain old source");
        check(::lstat((root + "/data/Stremio/appdata/" + name).c_str(), &mode_info) == 0 &&
              (mode_info.st_mode & 0777) == 0600, "migrated sensitive files are private");
    }
    check(!fs::exists(root + "/data/Stremio/appdata/cache.bin") &&
          !fs::exists(root + "/data/Stremio/appdata/unknown.json"), "migration excludes caches and all unlisted files");
    check(boot_text().find("fake-sensitive-token") == std::string::npos &&
          boot_text().find("migrated_files=3") != std::string::npos, "boot logs only migration result, never settings content");
    std::ofstream(root + "/data/Stremio/appdata/settings.json") << "new account";
    ps5_boot_close();
    check(ps5_prepare_storage().data_available, "migration repeats safely");
    check(read_file("/data/Stremio/appdata/settings.json") == "new account" &&
          read_file("/download0/stremio/settings.json").find("fake-sensitive-token") != std::string::npos,
        "repeat startup preserves current account and never deletes old source");
    check_no_probe();

    reset();
    std::ofstream(root + "/mnt/sandbox/PPSA74126_000/download0/stremio/settings.json") << "sandbox account";
    fail_operation = "lstat"; fail_path = "/data";
    check(ps5_prepare_storage().data_available &&
          read_file("/data/Stremio/appdata/settings.json") == "sandbox account",
        "legacy account still migrates through resolved sandbox mount after elevation");
    check_no_probe();

    reset(); legacy_settings(); short_migration_io = interrupt_migration_write = true;
    check(ps5_prepare_storage().data_available &&
          read_file("/data/Stremio/appdata/settings.json") == read_file("/download0/stremio/settings.json"),
        "legacy migration handles partial reads/writes and interrupted writes");
    check_no_probe();

    reset(); legacy_settings(); late_target = true;
    check(ps5_prepare_storage().data_available && read_file("/data/Stremio/appdata/settings.json") == "newer destination",
        "migration rechecks target immediately before rename and keeps a newer file");
    check_no_probe();

    for (const char* kind : {"appdata", "target", "legacy"}) {
        reset();
        fs::create_directory(root + "/outside");
        std::ofstream(root + "/outside/keep.json") << "keep outside";
        if (std::string(kind) == "appdata") fs::create_directory_symlink(root + "/outside", root + "/data/Stremio/appdata");
        else if (std::string(kind) == "target") {
            fs::create_directory(root + "/data/Stremio/appdata");
            fs::create_symlink(root + "/outside/keep.json", root + "/data/Stremio/appdata/settings.json");
        } else fs::create_symlink(root + "/outside/keep.json", root + "/download0/stremio/settings.json");
        const auto rejected = ps5_prepare_storage();
        check(rejected.filesystem_available && !rejected.data_available && rejected.data_errno == ELOOP,
            "appdata and migration refuse symlink redirection");
        check(read_file("/outside/keep.json") == "keep outside", "refused symlink leaves outside data untouched");
        check_no_probe();
    }

    reset();
    fs::create_directory(root + "/download0/stremio/settings.json");
    check(!ps5_prepare_storage().data_available, "non-regular legacy account fails explicitly");
    check_no_probe();

    reset(); legacy_settings();
    fs::resize_file(root + "/download0/stremio/settings.json", (4u << 20) + 1);
    const auto too_large = ps5_prepare_storage();
    check(!too_large.data_available && too_large.data_errno == EFBIG &&
          !fs::exists(root + "/data/Stremio/appdata/settings.json"), "oversized legacy data never publishes a partial account");
    check_no_probe();

    for (const char* operation : {"open", "read", "write", "fsync", "rename"}) {
        reset(); legacy_settings();
        fail_operation = operation;
        fail_path = (fail_operation == "read") ? "/download0/stremio/settings.json" : ".migration-";
        failure_errno = EIO; deny_until_grant = false;
        const auto failure = ps5_prepare_storage();
        check(failure.filesystem_available && !failure.data_available && failure.data_errno == EIO,
            "failed legacy migration blocks account initialization with a concrete error");
        check(!fs::exists(root + "/data/Stremio/appdata/settings.json") &&
              read_file("/download0/stremio/settings.json").find("fake-sensitive-token") != std::string::npos,
            "failed migration leaves no partial destination and retains original account");
        check_no_probe();
        fail_operation.clear(); ps5_boot_close();
        check(ps5_prepare_storage().data_available &&
              read_file("/data/Stremio/appdata/settings.json") == read_file("/download0/stremio/settings.json"),
            "retry recovers migration after the storage error is resolved");
        check_no_probe();
    }

    for (const char* operation : {"mkdir", "open", "write"}) {
        reset(); fail_operation = operation; fail_path = "/data/Stremio/appdata";
        failure_errno = EACCES; deny_until_grant = false;
        const auto denied_data = ps5_prepare_storage();
        check(denied_data.filesystem_available && !denied_data.data_available &&
              denied_data.data_errno == EACCES, "appdata permission failure is distinct from verified root access");
        check_no_probe();
    }

    ps5_boot_close();
    fs::remove_all(root);
    std::cout << "Native startup storage: " << checks << " checks passed\n";
    return 0;
}
