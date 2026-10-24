// Behaviour tests for credential redaction, bounded rotation and crash receipts.
#include "diagnostics.h"
#include "util.h"

#include <cassert>
#include <atomic>
#include <cerrno>
#include <fstream>
#include <iostream>
#include <sstream>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

// Fail a real asynchronous checkpoint sync without touching the prior file.
// This verifies error behavior that a successful host write cannot exercise.
static std::atomic<bool> fail_checkpoint_sync{false};
static std::atomic<unsigned> failed_checkpoint_syncs{0};
static std::atomic<bool> unsupported_directory_sync{false};
static std::atomic<unsigned> skipped_directory_syncs{0};
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd) {
    struct stat info{};
    if (unsupported_directory_sync.load() && ::fstat(fd, &info) == 0 && S_ISDIR(info.st_mode)) {
        ++skipped_directory_syncs;
        errno = ENOTSUP;
        return -1;
    }
    if (fail_checkpoint_sync.load() && ::fstat(fd, &info) == 0 && S_ISREG(info.st_mode)) {
        ++failed_checkpoint_syncs;
        errno = EIO;
        return -1;
    }
    return __real_fsync(fd);
}

static std::string contents(const std::string& path) {
    std::string text;
    (void)read_file(path, text);
    return text;
}
static void excludes(const std::string& actual, const char* secret) {
    if (actual.find(secret) != std::string::npos) {
        std::cerr << "Redaction assertion failed\n";
        std::abort();
    }
}
static void matches_build(const json& metadata) {
    assert(metadata["application"] == "Stremio Plus");
    assert(metadata["app_version"] == STREMIO_VERSION);
    assert(metadata["title_id"] == STREMIO_TITLE_ID);
    assert(metadata["build_id"].get<std::string>().starts_with(std::string(STREMIO_VERSION) + " "));
}
static void ffmpeg_event(int level, const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    diagnostics_ffmpeg_log(nullptr, level, format, arguments);
    va_end(arguments);
}

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string root = argv[1];
    const std::pair<const char*, const char*> cases[] = {
        {"Request https://person:my-password@addon.test/config-value/meta/movie/item.json?token=very-secret#fragment", "config-value"},
        {"https://person:my-password@addon.test/config/meta/movie/item.json", "my-password"},
        {"https:\\/\\/addon.test\\/private-token\\/manifest.json", "private-token"},
        {"https%253A%252F%252Faddon.test%252Fsecret-config%252Fmanifest.json", "secret-config"},
        {"Authorization: Bearer short-sensitive-token", "short-sensitive-token"},
        {"%41uthorization%3a%20Bearer%20header-secret", "header-secret"},
        {"Cookie: SID=cookie-value; second=also-sensitive", "also-sensitive"},
        {"{\"auth_key\":\"account-private-key\",\"email\":\"person@private.example\"}", "account-private-key"},
        {"{\"\\u0061uth\\u005fkey\":\"escaped-key-secret\"}", "escaped-key-secret"},
        {"API_KEY='short-api-key' status=403", "short-api-key"},
        {"Reply addressed to person+tag@private.example.", "private.example"},
        {"GET /addon/private-value/manifest.json status=404", "private-value"},
        {"magnet:?xt=urn:btih:1234567890abcdef1234567890abcdef12345678&dn=private-title", "private-title"},
        {"failure AbCd0123456789_secret_token_value_xyz", "AbCd0123456789_secret_token_value_xyz"},
        {"Video opened\nAuthorization: Basic cHJpdmF0ZTpzZWNyZXQ=\nnetwork error 401", "cHJpdmF0ZTpzZWNyZXQ="},
    };
    for (const auto& pair : cases) excludes(diagnostics_redact(pair.first), pair.second);
    const auto normal = diagnostics_redact("Video HEVC 3840x2160, pixel NV12, decode 8.5 ms, HTTP 503");
    assert(normal.find("HEVC 3840x2160") != std::string::npos);
    assert(normal.find("HTTP 503") != std::string::npos);

    unsupported_directory_sync = true;
    assert(diagnostics_start(root + "/Stremio/"));
    assert(diagnostics_flush());
    assert(skipped_directory_syncs > 0);
    unsupported_directory_sync = false;
    matches_build(json::parse(contents(root + "/Stremio/session.json")));
    log_open(root + "/Stremio/log.txt"); // compatibility entry point uses the same exact folder
    assert(!file_exists(root + "/Stremio/logs/session.json"));
    diagnostics_set_runtime({{"display", {{"width", 3840}, {"height", 2160}, {"Authorization", "runtime-auth-secret"}}},
                             {"gpu_yuv", true}, {"gl_renderer", "AMD"}, {"auth_key", "runtime-secret"},
                             {"url", "https://example.test/private-url"}, {"email", "private-runtime@account.example"},
                             {"video", {{"codec", "HEVC"}, {"token", "nested-secret"}}},
                             {"display_refresh", {{"preference", 2}, {"available", true}, {"active", true}, {"refresh_code", 13}}},
                             {"video_processing", {{"upscaler", "fsr1"}, {"smoothing", "hdr_input"},
                                  {"source_fps", 23.976}, {"dolby_vision_reshape", true},
                                  {"accepted_frame_fps", 22.3}, {"upload_path", "rectangle-planes"},
                                  {"audio_output", "active_eac3"}, {"token", "nested-secret"}}},
                             {"video_frame_accepted", true},
                             {"presentation_timing", {{"scope", "CPU timing, not physical scanout"},
                                  {"histogram_upper_ms", json::array({8.4, 17.5, 25.0, 34.0, 50.0, 100.0})},
                                  {"plane_transfer", {{"samples", 750}, {"mean_ms", 2.5}, {"max_ms", 4.0},
                                       {"histogram", json::array({750, 0, 0, 0, 0, 0, 0})}, {"token", "nested-secret"}}},
                                  {"conversion_submit", {{"samples", 750}, {"mean_ms", .2}}},
                                  {"loop", {{"histogram", json::array({"nested-secret"})}}},
                                  {"allocation", {{"histogram", json::array({1, 2, 3, 4, 5, 6, 7, 8, 9})}}},
                                  {"upload", {{"histogram", json::array({-1, 2})}}},
                                  {"private_array", json::array({"nested-secret"})}}}});
    for (const auto& pair : cases) dlog("%s", pair.first);
    ffmpeg_event(8, "Fatal codec test https://addon.test/fatal-private-value/manifest.json");
    const auto fatal = contents(root + "/Stremio/log.txt");
    assert(fatal.find("Fatal codec test") != std::string::npos); // callback drains before returning
    excludes(fatal, "fatal-private-value");
    assert(diagnostics_flush());
    const auto startup = contents(root + "/Stremio/log.txt") + contents(root + "/Stremio/events.jsonl");
    for (const auto& pair : cases) excludes(startup, pair.second);
    const auto runtime = contents(root + "/Stremio/runtime.json");
    excludes(runtime, "runtime-auth-secret"); excludes(runtime, "runtime-secret");
    excludes(runtime, "private-url"); excludes(runtime, "private-runtime"); excludes(runtime, "nested-secret");
    const auto runtime_json = json::parse(runtime);
    matches_build(runtime_json);
    assert(runtime_json["runtime"]["display"]["width"] == 3840);
    assert(runtime_json["runtime"]["video"]["codec"] == "HEVC");
    assert(runtime_json["runtime"]["display_refresh"]["refresh_code"] == 13);
    assert(runtime_json["runtime"]["display_refresh"]["active"] == true);
    assert(runtime_json["runtime"]["video_processing"]["upscaler"] == "fsr1");
    assert(runtime_json["runtime"]["video_processing"]["smoothing"] == "hdr_input");
    assert(runtime_json["runtime"]["video_processing"]["audio_output"] == "active_eac3");
    assert(runtime_json["runtime"]["video_processing"]["dolby_vision_reshape"] == true);
    assert(runtime_json["runtime"]["video_processing"]["source_fps"] == 23.976);
    assert(runtime_json["runtime"]["video_processing"]["accepted_frame_fps"] == 22.3);
    assert(runtime_json["runtime"]["video_processing"]["upload_path"] == "rectangle-planes");
    assert(runtime_json["runtime"]["video_frame_accepted"] == true);
    const auto& timing = runtime_json["runtime"]["presentation_timing"];
    assert(timing["histogram_upper_ms"].size() == 6);
    assert(timing["plane_transfer"]["samples"] == 750);
    assert(timing["plane_transfer"]["histogram"].size() == 7);
    assert(timing["plane_transfer"]["mean_ms"] == 2.5);
    assert(timing["conversion_submit"]["mean_ms"] == .2);
    assert(!timing["loop"].contains("histogram"));
    assert(!timing["allocation"].contains("histogram"));
    assert(!timing["upload"].contains("histogram"));
    assert(!timing.contains("private_array"));

    fail_checkpoint_sync = true;
    diagnostics_set_runtime({{"video", {{"width", 1920}, {"height", 1080}}}});
    assert(diagnostics_flush());
    assert(failed_checkpoint_syncs > 0);
    assert(contents(root + "/Stremio/runtime.json") == runtime);
    assert(!file_exists(root + "/Stremio/runtime.json.tmp"));
    fail_checkpoint_sync = false;
    diagnostics_set_runtime({{"video", {{"width", 3840}, {"height", 2160}}}});
    assert(diagnostics_flush());
    const auto durable = json::parse(contents(root + "/Stremio/runtime.json"));
    assert(durable["runtime"]["video"]["width"] == 3840);
    assert(durable["log_io_failures"].get<unsigned>() >= 1);

    std::string payload;
    for (int i = 0; i < 32; ++i) payload += "Frame decoded at 3840x2160 with 8 ms latency; ";
    for (int batch = 0; batch < 24; ++batch) {
        for (int event = 0; event < 32; ++event) diagnostics_note("decoder", payload);
        assert(diagnostics_flush());
    }
    diagnostics_stop();
    diagnostics_stop();
    assert(file_exists(root + "/Stremio/log.1.txt"));
    assert(file_exists(root + "/Stremio/log.2.txt"));
    assert(file_exists(root + "/Stremio/events.1.jsonl"));
    assert(file_exists(root + "/Stremio/events.2.jsonl"));
    assert(!file_exists(root + "/Stremio/log.3.txt"));
    assert(!file_exists(root + "/Stremio/events.3.jsonl"));
    for (const char* name : {"log.txt", "log.1.txt", "log.2.txt", "events.jsonl", "events.1.jsonl", "events.2.jsonl"}) {
        struct stat info{};
        assert(stat((root + "/Stremio/" + name).c_str(), &info) == 0);
        assert(info.st_size > 0 && info.st_size <= STREMIO_DIAGNOSTICS_ROTATE_BYTES);
    }
    for (const char* name : {"events.jsonl", "events.1.jsonl", "events.2.jsonl"}) {
        std::istringstream lines(contents(root + "/Stremio/" + name));
        std::string line;
        while (std::getline(lines, line)) {
            const auto entry = json::parse(line);
            assert(entry.contains("utc") && entry.contains("sequence") && entry.contains("session"));
            assert(entry["message"].is_string());
        }
    }
    auto marker = json::parse(contents(root + "/Stremio/session.json"));
    matches_build(marker);
    assert(marker["clean_shutdown"] == true);
    marker["clean_shutdown"] = false;
    assert(write_file(root + "/Stremio/session.json", marker.dump()));
    assert(diagnostics_start(root + "/Stremio/"));
    assert(diagnostics_flush());
    marker = json::parse(contents(root + "/Stremio/session.json"));
    matches_build(marker);
    assert(marker["previous_shutdown_unclean"] == true);
    diagnostics_stop();

    // A separate host child deliberately raises a fatal signal. This never
    // contacts a console and creates no core dump or memory-content archive.
    const auto crash_root = root + "/crash-child";
    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        const struct rlimit no_core{0, 0};
        (void)setrlimit(RLIMIT_CORE, &no_core);
        if (!diagnostics_start(crash_root + "/logs") || !diagnostics_flush()) _exit(9);
        raise(SIGSEGV);
        _exit(10);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
    const auto receipt = contents(crash_root + "/logs/crash-current.txt");
    const auto expected_identity = std::string("Stremio Plus ") + STREMIO_VERSION + " (" + STREMIO_TITLE_ID + ") build=" + STREMIO_VERSION + " ";
    assert(receipt.starts_with(expected_identity));
    assert(receipt.find("signal=0x") != std::string::npos);
    assert(receipt.find(" pc=0x") != std::string::npos);
    assert(receipt.find(" sp=0x") != std::string::npos);
    assert(receipt.find(" pc=0x0000000000000000") == std::string::npos);
    assert(receipt.find(" sp=0x0000000000000000") == std::string::npos);
    assert(diagnostics_start(crash_root + "/logs"));
    assert(file_exists(crash_root + "/logs/crash-last.txt"));
    marker = json::parse(contents(crash_root + "/logs/session.json"));
    matches_build(marker);
    assert(marker["previous_shutdown_unclean"] == true);
    diagnostics_stop();
    std::cout << "DIAGNOSTICS_TESTS_OK\n";
}
