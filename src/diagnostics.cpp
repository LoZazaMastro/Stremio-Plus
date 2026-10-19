// Stremio Plus - Bounded asynchronous diagnostics with credential redaction.
// Copyright (C) 2026 Stremio PS5 contributors.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "diagnostics.h"
#include "util.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <mutex>
#include <signal.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>
#if defined(__linux__) || defined(PLATFORM_PS5)
#include <sys/ucontext.h>
#endif

#ifdef PLATFORM_PS5
extern "C" int sceKernelDebugOutText(int channel, const char* text);
#endif

#ifndef STREMIO_DIAGNOSTICS_ROTATE_BYTES
#define STREMIO_DIAGNOSTICS_ROTATE_BYTES (4u * 1024u * 1024u)
#endif
#ifndef STREMIO_VERSION
#error "Diagnostics require the application version supplied by the build"
#endif
#ifndef STREMIO_TITLE_ID
#error "Diagnostics require the title ID supplied by the build"
#endif
#ifndef STREMIO_BUILD_ID
#define STREMIO_BUILD_ID STREMIO_VERSION " " __DATE__ " " __TIME__
#endif

namespace {
constexpr std::size_t kFileBytes = STREMIO_DIAGNOSTICS_ROTATE_BYTES;
constexpr std::size_t kMessageBytes = 4096;
constexpr std::size_t kQueueEntries = 1024;
constexpr std::size_t kQueueBytes = 2u * 1024u * 1024u;
constexpr int kGenerations = 3; // active plus two older files
constexpr int kSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};

bool ascii_alnum(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}
std::string ascii_lower(std::string text) {
    for (char& c : text) if (c >= 'A' && c <= 'Z') c = char(c + ('a' - 'A'));
    return text;
}
int unhex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Decode ordinary URL/JSON escaping before finding secret labels. Decoding is
// bounded and is used only for log text, never for a request or configuration.
std::string unescape_for_redaction(const std::string& input) {
    std::string current = input.substr(0, kMessageBytes);
    for (int pass = 0; pass < 2; ++pass) {
        std::string next;
        next.reserve(current.size());
        for (std::size_t i = 0; i < current.size(); ++i) {
            if (current[i] == '%' && i + 2 < current.size() && unhex(current[i + 1]) >= 0 && unhex(current[i + 2]) >= 0) {
                next += char(unhex(current[i + 1]) * 16 + unhex(current[i + 2]));
                i += 2;
            } else if (current[i] == '\\' && i + 1 < current.size()) {
                const char escaped = current[i + 1];
                if (escaped == '/' || escaped == '"' || escaped == '\\') { next += escaped; ++i; }
                else if (escaped == 'n' || escaped == 'r' || escaped == 't') { next += '\n'; ++i; }
                else if (escaped == 'u' && i + 5 < current.size() && current[i + 2] == '0' && current[i + 3] == '0' &&
                         unhex(current[i + 4]) >= 0 && unhex(current[i + 5]) >= 0) {
                    next += char(unhex(current[i + 4]) * 16 + unhex(current[i + 5]));
                    i += 5;
                } else next += current[i];
            } else next += current[i];
        }
        if (next == current) break;
        current = std::move(next);
    }
    return current;
}

bool url_end(unsigned char c) {
    return c <= 32 || c == '"' || c == '\'' || c == '<' || c == '>' || c == ')' || c == ']' || c == '}';
}

std::string redact_urls(const std::string& input) {
    const std::string folded = ascii_lower(input);
    static const char* schemes[] = {"https://", "http://", "stremio://", "magnet:?", "ftp://", "file://", "wss://", "ws://", "socks5://"};
    std::string output;
    for (std::size_t i = 0; i < input.size();) {
        std::size_t prefix = 0;
        for (const char* scheme : schemes) {
            if (folded.compare(i, std::strlen(scheme), scheme) == 0) { prefix = std::strlen(scheme); break; }
        }
        // Relative request paths can also carry an add-on configuration token.
        const bool relative = input[i] == '/' && (i == 0 || url_end(static_cast<unsigned char>(input[i - 1])) || input[i - 1] == '=');
        if (!prefix && !relative) { output += input[i++]; continue; }
        std::size_t end = i + (prefix ? prefix : 1);
        while (end < input.size() && !url_end(static_cast<unsigned char>(input[end]))) ++end;
        if (prefix && folded.compare(i, 7, "magnet:") != 0 && folded.compare(i, 7, "file://") != 0) {
            const std::size_t authority_begin = i + prefix;
            std::size_t authority_end = authority_begin;
            while (authority_end < end && input[authority_end] != '/' && input[authority_end] != '?' && input[authority_end] != '#') ++authority_end;
            std::string authority = input.substr(authority_begin, authority_end - authority_begin);
            const auto userinfo = authority.rfind('@');
            if (userinfo != std::string::npos) authority.erase(0, userinfo + 1);
            bool valid_host = !authority.empty() && authority.size() < 254;
            for (unsigned char c : authority) valid_host = valid_host && (ascii_alnum(c) || c == '.' || c == '-' || c == ':' || c == '[' || c == ']');
            if (valid_host) output += input.substr(i, prefix) + authority + "/[redacted]";
            else output += "[url:redacted]";
        } else output += relative ? "[path:redacted]" : "[url:redacted]";
        i = end;
    }
    return output;
}

std::string redact_labels(const std::string& input) {
    // Drop the rest of a secret-bearing line. In particular, Cookie contains
    // several semicolon-separated credentials, and a JSON value can contain
    // spaces or punctuation; masking one whitespace-delimited word is unsafe.
    static const char* labels[] = {
        "proxy-authorization", "authorization", "set-cookie", "cookies", "cookie", "headers",
        "client_secret", "clientsecret", "access_token", "accesstoken", "refresh_token", "refreshtoken",
        "auth_key", "authkey", "api_key", "api-key", "apikey", "password", "passwd", "passphrase",
        "secret", "bearer", "basic", "token", "jwt", "email", "username", "user_name", "credential"
    };
    std::string output;
    std::size_t begin = 0;
    while (begin < input.size()) {
        const auto newline = input.find_first_of("\r\n", begin);
        const auto end = newline == std::string::npos ? input.size() : newline;
        const std::string line = input.substr(begin, end - begin);
        const auto folded = ascii_lower(line);
        std::size_t cut = std::string::npos;
        for (const char* label : labels) {
            std::size_t pos = 0;
            while ((pos = folded.find(label, pos)) != std::string::npos) {
                const auto after = pos + std::strlen(label);
                const bool before_ok = pos == 0 || (!ascii_alnum(static_cast<unsigned char>(folded[pos - 1])) && folded[pos - 1] != '_');
                const bool after_ok = after == folded.size() || !ascii_alnum(static_cast<unsigned char>(folded[after]));
                if (before_ok && after_ok) { cut = std::min(cut, pos); break; }
                pos = after;
            }
        }
        if (cut == std::string::npos) output += line;
        else output += line.substr(0, cut) + "[credentials:redacted]";
        if (newline == std::string::npos) break;
        output += " | ";
        begin = end + 1;
    }
    return output;
}

std::string redact_emails_and_tokens(const std::string& input) {
    std::string output = input;
    auto local = [](unsigned char c) { return ascii_alnum(c) || std::strchr(".!#$%&'*+-/=?^_`{|}~", c) != nullptr; };
    std::size_t at = 0;
    while ((at = output.find('@', at)) != std::string::npos) {
        std::size_t begin = at, end = at + 1;
        while (begin && local(static_cast<unsigned char>(output[begin - 1]))) --begin;
        while (end < output.size() && (ascii_alnum(static_cast<unsigned char>(output[end])) || output[end] == '.' || output[end] == '-')) ++end;
        if (begin < at && end > at + 1) {
            output.replace(begin, end - begin, "[email:redacted]");
            at = begin + 16;
        } else ++at;
    }
    // Unlabelled opaque IDs may be debrid keys or JWT segments. Their exact
    // contents are never needed to diagnose transport or rendering failures.
    std::string safe;
    for (std::size_t i = 0; i < output.size();) {
        if (!ascii_alnum(static_cast<unsigned char>(output[i])) && output[i] != '_' && output[i] != '-') { safe += output[i++]; continue; }
        std::size_t end = i;
        while (end < output.size() && (ascii_alnum(static_cast<unsigned char>(output[end])) || output[end] == '_' || output[end] == '-')) ++end;
        if (end - i >= 24) safe += "[opaque:redacted]";
        else safe += output.substr(i, end - i);
        i = end;
    }
    for (char& c : safe) if (static_cast<unsigned char>(c) < 32 || c == 127) c = ' ';
    if (safe.size() > kMessageBytes) safe.resize(kMessageBytes);
    return safe;
}

std::int64_t epoch_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string utc_stamp(std::int64_t milliseconds) {
    const time_t seconds = time_t(milliseconds / 1000);
    struct tm time{};
    gmtime_r(&seconds, &time);
    char date[40];
    std::strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &time);
    char complete[56];
    std::snprintf(complete, sizeof(complete), "%s.%03dZ", date, int(milliseconds % 1000));
    return complete;
}
std::string json_text(const json& value, int indent = -1) {
    return value.dump(indent, ' ', false, json::error_handler_t::replace);
}
bool write_small(const std::string& path, const std::string& data) {
    const auto temporary = path + ".tmp";
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;
    std::size_t done = 0;
    while (done < data.size()) {
        const auto count = ::write(fd, data.data() + done, data.size() - done);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { ::close(fd); ::unlink(temporary.c_str()); return false; }
        done += std::size_t(count);
    }
    // A rename alone does not make the newly written checkpoint durable.
    // Sync its contents first so a failed writeback retains the previous
    // complete runtime snapshot. This runs on the writer, never per frame.
    int synced;
    do { synced = ::fsync(fd); } while (synced != 0 && errno == EINTR);
    const bool closed = ::close(fd) == 0;
    if (synced != 0 || !closed) { ::unlink(temporary.c_str()); return false; }
    if (::rename(temporary.c_str(), path.c_str()) != 0) { ::unlink(temporary.c_str()); return false; }
    const auto separator = path.find_last_of('/');
    const auto parent = separator == std::string::npos ? "." : separator == 0 ? "/" : path.substr(0, separator);
    const int directory = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY);
    if (directory < 0) return false;
    do { synced = ::fsync(directory); } while (synced != 0 && errno == EINTR);
    const int sync_error = synced != 0 ? errno : 0;
    const bool directory_closed = ::close(directory) == 0;
    // Match the storage probe's handling of mounts without directory fsync.
    // File data was synced and rename succeeded; keep logging available even
    // where the OS cannot additionally confirm directory-entry durability.
    return directory_closed && (synced == 0 || sync_error == EINVAL || sync_error == ENOTSUP);
}

json build_metadata() {
    return {{"application", "Stremio Plus"}, {"app_version", STREMIO_VERSION}, {"title_id", STREMIO_TITLE_ID},
            {"build_id", STREMIO_BUILD_ID}, {"compiler", __VERSION__},
#ifdef PLATFORM_PS5
            {"platform", "PS5 native"},
#else
            {"platform", "Linux host preview"},
#endif
            {"log_file_bytes", kFileBytes}, {"log_generations", kGenerations}, {"redaction", "enabled"}};
}
bool runtime_key(const std::string& key) {
    static const char* allowed[] = {
        "display", "video", "audio", "gpu", "network", "width", "height", "ui_width", "ui_height",
        "output_width", "output_height", "framebuffer_width", "framebuffer_height", "ui_resolution",
        "configured_width", "configured_height", "requested_width", "requested_height", "automatic",
        "gl_version", "gl_vendor", "gl_renderer", "gl_error", "renderer", "driver", "gpu_yuv", "gpu_status",
        "hardware_decoder", "decoder", "codec", "pixel_format", "bit_depth", "color_primaries", "color_transfer",
        "color_matrix", "color_range", "hdr_mode", "tone_mapping", "video_width", "video_height", "video_fps",
        "audio_codec", "sample_rate", "audio_channels", "addon_count", "enabled_addon_count", "server_configured",
        "network_state", "connection_type", "buffer_bytes", "buffer_seconds", "download_bytes_per_second",
        "fps", "frame_ms", "dropped_frames", "decoded_frames", "presented_frames", "rebuffer_count",
        "queue_depth", "audio_underruns", "decoder_errors", "http_status", "error_code", "state", "status",
        "hardware_decode_enabled", "output_mode", "vsync", "reduced_motion", "subtitle_language",
        "audio_language", "interface_language", "language", "log_io_failures", "log_dropped_events",
        "display_refresh", "preference", "available", "active", "refresh_code", "video_processing",
        "source_fps", "upscaler_preference", "upscaler", "sharpness", "smoothing_preference", "smoothing",
        "dolby_vision_reshape", "audio_passthrough_requested", "audio_output"
    };
    for (const char* name : allowed) if (key == name) return true;
    return false;
}
json safe_runtime(const json& source, int depth = 0) {
    json result = json::object();
    if (!source.is_object() || depth > 3) return result;
    unsigned count = 0;
    for (auto it = source.begin(); it != source.end() && count < 64; ++it) {
        if (!runtime_key(it.key())) continue;
        ++count;
        if (it.value().is_object()) result[it.key()] = safe_runtime(it.value(), depth + 1);
        else if (it.value().is_string()) result[it.key()] = diagnostics_redact(it.value().get<std::string>());
        else if (it.value().is_number() || it.value().is_boolean() || it.value().is_null()) result[it.key()] = it.value();
    }
    return result;
}
std::string safe_category(const std::string& value) {
    static const char* allowed[] = {"app", "http", "player", "video", "display", "lifecycle", "decoder", "audio", "subtitles", "addon", "account", "settings", "ui", "session", "diagnostics", "crash", "ffmpeg", "gpu"};
    for (const char* name : allowed) if (value == name) return value;
    return "app";
}

struct Event {
    std::uint64_t sequence = 0;
    std::int64_t time = 0;
    std::string category;
    std::string message;
};
struct LogFile {
    std::string stem, extension, directory;
    FILE* stream = nullptr;
    std::size_t bytes = 0;
    std::string path(int generation = 0) const {
        return directory + "/" + stem + (generation ? "." + std::to_string(generation) : "") + extension;
    }
    bool open() {
        const int fd = ::open(path().c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (fd < 0) return false;
        stream = ::fdopen(fd, "a");
        if (!stream) { ::close(fd); return false; }
        ::setvbuf(stream, nullptr, _IOFBF, 64u * 1024u);
        struct stat info{};
        bytes = ::fstat(fd, &info) == 0 ? std::size_t(info.st_size) : 0;
        return true;
    }
    void close() { if (stream) ::fclose(stream); stream = nullptr; }
    bool put(const std::string& text) {
        if (!stream) return false;
        if (bytes && bytes + text.size() > kFileBytes) {
            close();
            if (::unlink(path(kGenerations - 1).c_str()) != 0 && errno != ENOENT) return false;
            for (int generation = kGenerations - 2; generation >= 0; --generation) {
                if (::rename(path(generation).c_str(), path(generation + 1).c_str()) != 0 && errno != ENOENT) return false;
            }
            if (!open()) return false;
        }
        const auto written = ::fwrite(text.data(), 1, text.size(), stream);
        bytes += written;
        return written == text.size();
    }
    bool flush() { return !stream || ::fflush(stream) == 0; }
};
struct State {
    std::mutex lifecycle;
    std::mutex mutex;
    std::condition_variable changed;
    std::thread writer;
    std::vector<Event> queue;
    std::atomic<bool> running{false};
    std::atomic<std::uint64_t> dropped{0};
    std::atomic<std::uint64_t> dropped_total{0};
    std::size_t queued_bytes = 0;
    std::uint64_t next_sequence = 0, flush_requested = 0, flush_done = 0;
    std::uint64_t io_failures = 0;
    bool stopping = false, runtime_dirty = false;
    std::string directory, session;
    json runtime = json::object();
    json marker = json::object();
    LogFile text, events;
};
State& state() {
    // Native-title static teardown is unreliable. This small control object
    // has process lifetime; explicit stop joins the worker and closes its files.
    static State* instance = new State;
    return *instance;
}

static_assert(std::atomic<unsigned>::is_always_lock_free);
static_assert(std::atomic<int>::is_always_lock_free);
static_assert(std::atomic<std::size_t>::is_always_lock_free);
static_assert(std::atomic<std::uintptr_t>::is_always_lock_free);
std::atomic<int> crash_fd{-1};
std::atomic<unsigned> handling_crash{0};
std::atomic<unsigned> allocation_failures{0};
std::atomic<std::uintptr_t> last_abort_caller{0}, last_assert_caller{0};
std::atomic<std::size_t> heap_capacity{0}, heap_used{0}, heap_available{0}, heap_observed_peak{0};
int crash_rotation_error = 0;
std::array<struct sigaction, 5> previous_actions{};
std::array<bool, 5> installed_actions{};
struct sigaction default_action{};
alignas(16) unsigned char crash_stack[64u * 1024u];
stack_t previous_stack{};
bool installed_stack = false;
char crash_header[256];
std::size_t crash_header_size = 0;

struct CrashRecord {
    char data[1024];
    std::size_t size = 0;

    void character(char value) {
        if (size < sizeof(data) - 1) data[size++] = value;
    }
    void literal(const char* value) {
        for (std::size_t i = 0; i < sizeof(data) && value[i]; ++i) character(value[i]);
    }
    void header() {
        for (std::size_t i = 0; i < crash_header_size; ++i) character(crash_header[i]);
    }
    void hex(std::uintptr_t value) {
        static constexpr char digits[] = "0123456789abcdef";
        literal("0x");
        for (int nibble = int(sizeof(value) * 2) - 1; nibble >= 0; --nibble)
            character(digits[(value >> (nibble * 4)) & 15u]);
    }
    void number(std::uint64_t value) {
        char digits[20];
        unsigned count = 0;
        do { digits[count++] = char('0' + value % 10); value /= 10; } while (value);
        while (count) character(digits[--count]);
    }
    void signed_number(int value) {
        if (value < 0) {
            character('-');
            number(std::uint64_t(-std::int64_t(value)));
        } else number(unsigned(value));
    }
    void source_text(const char* value, std::size_t limit, bool basename = false) {
        if (!value) { literal("[unknown]"); return; }
        std::size_t begin = 0, length = 0;
        const std::size_t scan_limit = basename ? 512 : limit;
        while (length < scan_limit && value[length]) {
            if (basename && (value[length] == '/' || value[length] == '\\')) begin = length + 1;
            ++length;
        }
        // Assertion inputs are source text. Never emit string literals, URLs,
        // control bytes, or the directory prefix of a source filename.
        for (std::size_t i = begin; i + 2 < length; ++i) {
            if (value[i] == ':' && value[i + 1] == '/' && value[i + 2] == '/') {
                literal("[redacted source text]"); return;
            }
        }
        char quote = 0;
        bool escaped = false;
        const auto end = begin + limit < length ? begin + limit : length;
        for (std::size_t i = begin; i < end; ++i) {
            const unsigned char c = static_cast<unsigned char>(value[i]);
            if (quote) {
                if (!escaped && c == static_cast<unsigned char>(quote)) quote = 0;
                if (!escaped && c == '\\') escaped = true;
                else escaped = false;
            } else if (c == '"' || c == '\'') {
                quote = char(c);
                literal("[literal]");
            } else character(c >= 32 && c < 127 ? char(c) : '?');
        }
        if (end < length || length == scan_limit) literal("...");
    }
    void cached_heap() {
        literal(" heap_capacity="); number(heap_capacity.load(std::memory_order_relaxed));
        literal(" heap_used_snapshot="); number(heap_used.load(std::memory_order_relaxed));
        literal(" heap_free_snapshot="); number(heap_available.load(std::memory_order_relaxed));
        literal(" heap_observed_peak="); number(heap_observed_peak.load(std::memory_order_relaxed));
    }
    void finish(bool sync) {
        // Each event is one bounded append on an already open descriptor.
        // No stdio, allocation, logging locks, or stack unwinding is involved.
        data[size++] = '\n';
        const int fd = crash_fd.load(std::memory_order_relaxed);
        if (fd >= 0) {
            (void)::write(fd, data, size);
            if (sync) (void)::fsync(fd);
        }
    }
};

void crash_handler(int signal_number, siginfo_t* info, void* context) {
    if (handling_crash.exchange(1, std::memory_order_relaxed) == 0) {
        CrashRecord record;
        record.header();
        record.literal("signal=");
        record.hex(std::uintptr_t(signal_number));
        record.literal(" si_code=");
        record.signed_number(info ? info->si_code : 0);
        std::uintptr_t pc = 0, sp = 0;
#if defined(__linux__) && defined(__x86_64__)
        if (context) {
            const auto* machine = static_cast<const ucontext_t*>(context);
            pc = std::uintptr_t(machine->uc_mcontext.gregs[REG_RIP]);
            sp = std::uintptr_t(machine->uc_mcontext.gregs[REG_RSP]);
        }
#elif defined(PLATFORM_PS5) && defined(__x86_64__)
        if (context) {
            const auto* machine = static_cast<const ucontext_t*>(context);
            pc = std::uintptr_t(machine->uc_mcontext.mc_rip);
            sp = std::uintptr_t(machine->uc_mcontext.mc_rsp);
        }
#else
        (void)context;
#endif
        record.literal(" pc="); record.hex(pc);
        record.literal(" sp="); record.hex(sp);
        if (signal_number == SIGSEGV || signal_number == SIGBUS) {
            record.literal(" fault=");
            record.hex(info ? reinterpret_cast<std::uintptr_t>(info->si_addr) : 0);
        }
        record.literal(" anchor="); record.hex(reinterpret_cast<std::uintptr_t>(&diagnostics_start));
        record.literal(" abort_caller="); record.hex(last_abort_caller.load(std::memory_order_relaxed));
        record.literal(" assert_caller="); record.hex(last_assert_caller.load(std::memory_order_relaxed));
        record.cached_heap();
        record.finish(true);
    }
    // Preserve the platform's normal fatal-signal handling after saving the
    // small receipt. No allocation, logging framework, locks, or stdio here.
    (void)::sigaction(signal_number, &default_action, nullptr);
    (void)::kill(::getpid(), signal_number);
}

enum class CrashRecordKind { Nonfatal, Fatal, Unreadable };

CrashRecordKind inspect_crash_record(const char* path, int& error) {
    // Startup only. Keep the last real fatal receipt across healthy sessions
    // whose current file contains only heap snapshots or allocation failures.
    const int fd = ::open(path, O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) return CrashRecordKind::Nonfatal;
        error = errno;
        return CrashRecordKind::Unreadable;
    }
    char buffer[4096], prefix[16];
    std::size_t prefix_size = 0;
    for (unsigned block = 0; block < 16; ++block) {
        const auto count = ::read(fd, buffer, sizeof(buffer));
        if (count <= 0) {
            error = count < 0 ? errno : 0;
            (void)::close(fd);
            return count < 0 ? CrashRecordKind::Unreadable : CrashRecordKind::Nonfatal;
        }
        for (ssize_t i = 0; i < count; ++i) {
            if (buffer[i] == '\n') { prefix_size = 0; continue; }
            if (prefix_size == sizeof(prefix)) continue;
            prefix[prefix_size++] = buffer[i];
            if ((prefix_size == 7 && std::memcmp(prefix, "signal=", 7) == 0) ||
                (prefix_size == 11 && std::memcmp(prefix, "event=exit ", 11) == 0) ||
                (prefix_size == 12 && std::memcmp(prefix, "event=abort ", 12) == 0) ||
                (prefix_size == 13 && std::memcmp(prefix, "event=assert ", 13) == 0)) {
                (void)::close(fd);
                return CrashRecordKind::Fatal;
            }
        }
    }
    // Unexpectedly large or unreadable files may contain evidence outside the
    // bounded scan. Preserve them instead of assuming they are safe to truncate.
    error = EFBIG;
    (void)::close(fd);
    return CrashRecordKind::Unreadable;
}

bool install_crash_capture(const std::string& directory, const std::string& session) {
    const auto current = directory + "/crash-current.txt";
    crash_rotation_error = 0;
    const auto previous = inspect_crash_record(current.c_str(), crash_rotation_error);
    int flags = O_WRONLY | O_CREAT | O_APPEND;
    if (previous == CrashRecordKind::Nonfatal) flags |= O_TRUNC;
    else if (previous == CrashRecordKind::Fatal) {
        if (::rename(current.c_str(), (directory + "/crash-last.txt").c_str()) == 0) flags |= O_TRUNC;
        else crash_rotation_error = errno; // Keep the original current file intact.
    }
    crash_fd = ::open(current.c_str(), flags, 0600);
    if (crash_fd < 0) return false;
    handling_crash = 0;
    allocation_failures = 0;
    last_abort_caller = 0;
    last_assert_caller = 0;
    heap_capacity = 0;
    heap_used = 0;
    heap_available = 0;
    heap_observed_peak = 0;
    const int count = std::snprintf(crash_header, sizeof(crash_header), "Stremio Plus %s (%s) build=%s session=%s\n",
                                    STREMIO_VERSION, STREMIO_TITLE_ID, STREMIO_BUILD_ID, session.c_str());
    crash_header_size = count > 0 ? std::min(std::size_t(count), sizeof(crash_header) - 1) : 0;
    default_action = {};
    default_action.sa_handler = SIG_DFL;
    ::sigemptyset(&default_action.sa_mask);
    stack_t stack{};
    stack.ss_sp = crash_stack;
    stack.ss_size = sizeof(crash_stack);
    installed_stack = ::sigaltstack(&stack, &previous_stack) == 0;
    struct sigaction action{};
    action.sa_sigaction = crash_handler;
    action.sa_flags = SA_SIGINFO | (installed_stack ? SA_ONSTACK : 0);
    ::sigemptyset(&action.sa_mask);
    bool complete = true;
    for (std::size_t i = 0; i < installed_actions.size(); ++i) {
        installed_actions[i] = ::sigaction(kSignals[i], &action, &previous_actions[i]) == 0;
        complete = complete && installed_actions[i];
    }
    return complete;
}
void close_crash_capture() {
    for (std::size_t i = 0; i < installed_actions.size(); ++i) if (installed_actions[i]) {
        (void)::sigaction(kSignals[i], &previous_actions[i], nullptr);
        installed_actions[i] = false;
    }
    if (installed_stack) { (void)::sigaltstack(&previous_stack, nullptr); installed_stack = false; }
    const int fd = crash_fd;
    crash_fd = -1;
    if (fd >= 0) ::close(fd);
}

void write_event(State& s, const Event& event) {
    const auto safe = diagnostics_redact(event.message);
    const auto stamp = utc_stamp(event.time);
    const auto category = safe_category(event.category);
    const std::string text = stamp + " [" + category + "] " + safe + "\n";
    const json structured = {{"utc", stamp}, {"sequence", event.sequence}, {"session", s.session}, {"category", category}, {"message", safe}};
    if (!s.text.put(text)) ++s.io_failures;
    if (!s.events.put(json_text(structured) + "\n")) ++s.io_failures;
#ifdef PLATFORM_PS5
    // Kernel output also runs on the writer and receives only redacted text.
    const std::string kernel = "[Stremio Plus] " + safe.substr(0, 1500) + "\n";
    sceKernelDebugOutText(0, kernel.c_str());
#else
    ::fwrite(text.data(), 1, text.size(), stdout);
#endif
}
void save_runtime(State& s, const json& runtime) {
    json result = build_metadata();
    result["session"] = s.session;
    result["updated_utc"] = utc_stamp(epoch_ms());
    result["runtime"] = runtime;
    result["log_dropped_events"] = s.dropped_total.load(std::memory_order_relaxed);
    result["log_io_failures"] = s.io_failures;
    if (!write_small(s.directory + "/runtime.json", json_text(result, 2) + "\n")) ++s.io_failures;
}
void writer_main() {
    State& s = state();
    // Reuse both vectors' storage between batches. An empty vector needs no
    // allocation, so even the writer's idle loop remains safe under pressure.
    std::vector<Event> batch;
    for (;;) {
        batch.clear();
        json runtime;
        bool update_runtime = false, finish = false;
        std::uint64_t flush_ticket = 0;
        {
            std::unique_lock<std::mutex> lock(s.mutex);
            s.changed.wait_for(lock, std::chrono::milliseconds(250), [&] {
                return s.stopping || s.queue.size() >= 64 || s.runtime_dirty || s.flush_requested > s.flush_done;
            });
            batch.swap(s.queue);
            s.queued_bytes = 0;
            update_runtime = s.runtime_dirty;
            if (update_runtime) {
                try { runtime = s.runtime; }
                catch (...) { ++s.io_failures; update_runtime = false; }
                s.runtime_dirty = false;
            }
            flush_ticket = s.flush_requested;
            finish = s.stopping;
        }
        try {
            for (const auto& event : batch) write_event(s, event);
            const auto dropped = s.dropped.exchange(0, std::memory_order_relaxed);
            if (dropped) write_event(s, {0, epoch_ms(), "diagnostics", "Bounded writer queue dropped " + std::to_string(dropped) + " events; playback was not blocked."});
            if (!s.text.flush()) ++s.io_failures;
            if (!s.events.flush()) ++s.io_failures;
            if (update_runtime) save_runtime(s, runtime);
        } catch (...) {
            ++s.io_failures; // A diagnostics failure must not terminate playback.
        }
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            s.flush_done = std::max(s.flush_done, flush_ticket);
            finish = finish && s.queue.empty();
        }
        s.changed.notify_all();
        if (finish) break;
    }
    try {
        s.marker["clean_shutdown"] = true;
        s.marker["ended_utc"] = utc_stamp(epoch_ms());
        s.marker["log_dropped_events"] = s.dropped_total.load(std::memory_order_relaxed);
        s.marker["log_io_failures"] = s.io_failures;
        (void)write_small(s.directory + "/session.json", json_text(s.marker, 2) + "\n");
        save_runtime(s, s.runtime);
    } catch (...) {}
    s.text.close();
    s.events.close();
}
} // namespace

extern "C" void stremio_diagnostics_abort(std::uintptr_t caller) {
    last_abort_caller.store(caller, std::memory_order_relaxed);
    CrashRecord record;
    record.header();
    record.literal("event=abort caller="); record.hex(caller);
    record.literal(" anchor="); record.hex(reinterpret_cast<std::uintptr_t>(&diagnostics_start));
    record.cached_heap();
    record.finish(true);
}

extern "C" void stremio_diagnostics_native_exit(int status, std::uintptr_t caller) {
    // Driver fail-stop paths use _Exit, which intentionally bypasses signals,
    // destructors and the asynchronous logger. Preserve its numeric origin
    // without running cleanup or changing the requested process termination.
    CrashRecord record;
    record.header();
    record.literal("event=exit status="); record.signed_number(status);
    record.literal(" caller="); record.hex(caller);
    record.literal(" anchor="); record.hex(reinterpret_cast<std::uintptr_t>(&diagnostics_start));
    record.cached_heap();
    record.finish(true);
}

extern "C" void stremio_diagnostics_assert(const char* function, const char* file, int line,
                                           const char* expression, std::uintptr_t caller) {
    last_assert_caller.store(caller, std::memory_order_relaxed);
    CrashRecord record;
    record.header();
    record.literal("event=assert caller="); record.hex(caller);
    record.literal(" source="); record.source_text(file, 96, true);
    record.literal(" line="); record.signed_number(line);
    record.literal(" function="); record.source_text(function, 96);
    record.literal(" expression="); record.source_text(expression, 256);
    record.literal(" anchor="); record.hex(reinterpret_cast<std::uintptr_t>(&diagnostics_start));
    record.finish(true);
}

extern "C" void stremio_diagnostics_allocation_failure(unsigned operation, std::size_t requested,
                                                       std::size_t alignment, std::uintptr_t caller) {
    if (crash_fd.load(std::memory_order_relaxed) < 0) return;
    unsigned count = allocation_failures.load(std::memory_order_relaxed);
    // Saturate after eight records. A contending thread may omit a diagnostic,
    // but never waits for another thread or the regular diagnostics writer.
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        if (count >= 8) return;
        if (!allocation_failures.compare_exchange_strong(count, count + 1, std::memory_order_relaxed)) continue;
        const int saved_errno = errno;
        CrashRecord record;
        record.header();
        record.literal("event=allocation_failure ordinal="); record.number(count + 1);
        record.literal(" operation="); record.number(operation);
        record.literal(" requested="); record.number(requested);
        record.literal(" alignment="); record.number(alignment);
        record.literal(" caller="); record.hex(caller);
        record.literal(" anchor="); record.hex(reinterpret_cast<std::uintptr_t>(&diagnostics_start));
        record.cached_heap();
        record.finish(false);
        errno = saved_errno;
        return;
    }
}

extern "C" void stremio_diagnostics_heap_snapshot(unsigned checkpoint, std::size_t capacity,
                                                  std::size_t used, std::size_t available,
                                                  std::size_t observed_peak, std::uintptr_t caller) {
    const int saved_errno = errno;
    heap_capacity.store(capacity, std::memory_order_relaxed);
    heap_used.store(used, std::memory_order_relaxed);
    heap_available.store(available, std::memory_order_relaxed);
    heap_observed_peak.store(observed_peak, std::memory_order_relaxed);
    CrashRecord record;
    record.header();
    record.literal("event=heap_snapshot checkpoint="); record.number(checkpoint);
    record.literal(" caller="); record.hex(caller);
    record.cached_heap();
    record.finish(false);
    errno = saved_errno;
}

std::string diagnostics_redact(const std::string& message) {
    return redact_emails_and_tokens(redact_labels(redact_urls(unescape_for_redaction(message))));
}

bool diagnostics_start(const std::string& log_directory) {
    State& s = state();
    std::lock_guard<std::mutex> lifecycle(s.lifecycle);
    if (s.running.load(std::memory_order_acquire)) return true;
    if (log_directory.empty()) return false;
    try {
        s.directory = log_directory;
        while (s.directory.size() > 1 && s.directory.back() == '/') s.directory.pop_back();
        if (!make_dirs(s.directory)) return false;
        s.queue.clear(); s.queued_bytes = 0; s.next_sequence = 0; s.io_failures = 0;
        s.flush_requested = 0; s.flush_done = 0; s.stopping = false;
        s.dropped = 0; s.dropped_total = 0;
        s.runtime = json::object(); s.runtime_dirty = true;
        const auto start_time = epoch_ms();
        char identity[64];
        std::snprintf(identity, sizeof(identity), "%llx-%lx", static_cast<unsigned long long>(start_time), static_cast<unsigned long>(::getpid()));
        s.session = identity;
        bool previous_unclean = file_exists(s.directory + "/session.json");
        json previous;
        if (load_json(s.directory + "/session.json", previous) && previous.is_object()) {
            previous_unclean = !jbool(previous, "clean_shutdown", false);
            json safe_previous = {{"clean_shutdown", !previous_unclean}};
            for (const char* key : {"session", "started_utc", "ended_utc", "build_id", "title_id"}) {
                if (previous.contains(key) && previous[key].is_string()) safe_previous[key] = diagnostics_redact(previous[key].get<std::string>());
            }
            (void)write_small(s.directory + "/previous-session.json", json_text(safe_previous, 2) + "\n");
        }
        s.marker = build_metadata();
        s.marker["session"] = s.session;
        s.marker["started_utc"] = utc_stamp(start_time);
        s.marker["clean_shutdown"] = false;
        s.marker["previous_shutdown_unclean"] = previous_unclean;
        s.text = {"log", ".txt", s.directory, nullptr, 0};
        s.events = {"events", ".jsonl", s.directory, nullptr, 0};
        if (!s.text.open() || !s.events.open()) { s.text.close(); s.events.close(); return false; }
        s.marker["crash_capture_installed"] = install_crash_capture(s.directory, s.session);
        s.marker["crash_rotation_error"] = crash_rotation_error;
        if (!write_small(s.directory + "/session.json", json_text(s.marker, 2) + "\n")) {
            close_crash_capture(); s.text.close(); s.events.close(); return false;
        }
        s.running.store(true, std::memory_order_release);
        s.writer = std::thread(writer_main);
        diagnostics_note("session", std::string("Stremio Plus session started; previous shutdown ") + (previous_unclean ? "unclean (crash, forced close, or power loss)." : "clean or first launch."));
        if (crash_rotation_error) diagnostics_note("diagnostics", "Previous native crash record could not be rotated; retaining crash-current.txt (error " + std::to_string(crash_rotation_error) + ").");
        return true;
    } catch (...) {
        s.running = false;
        if (s.writer.joinable()) {
            { std::lock_guard<std::mutex> lock(s.mutex); s.stopping = true; }
            s.changed.notify_all();
            s.writer.join();
        }
        close_crash_capture(); s.text.close(); s.events.close();
        return false;
    }
}

namespace {
void enqueue_note(const std::string& category, const std::string& message, bool critical) {
    State& s = state();
    if (!s.running.load(std::memory_order_acquire)) return;
    const auto bytes = std::min(message.size(), kMessageBytes);
    std::unique_lock<std::mutex> lock(s.mutex, std::defer_lock);
    // Ordinary messages never wait on the render/playback thread. A fatal
    // FFmpeg message is followed by abort(), so retain it even if the writer
    // happens to be swapping its queue at this instant.
    if (critical) lock.lock();
    else (void)lock.try_lock();
    if (critical) {
        while (!s.queue.empty() && (s.queue.size() >= kQueueEntries || s.queued_bytes + bytes > kQueueBytes)) {
            s.queued_bytes -= s.queue.back().message.size();
            s.queue.pop_back();
            s.dropped.fetch_add(1, std::memory_order_relaxed);
            s.dropped_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (!lock.owns_lock() || s.queue.size() >= kQueueEntries || s.queued_bytes + bytes > kQueueBytes) {
        s.dropped.fetch_add(1, std::memory_order_relaxed);
        s.dropped_total.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!s.running.load(std::memory_order_relaxed)) return;
    try {
        s.queue.push_back({++s.next_sequence, epoch_ms(), safe_category(category), message.substr(0, kMessageBytes)});
        s.queued_bytes += bytes;
    } catch (...) {
        s.dropped.fetch_add(1, std::memory_order_relaxed);
        s.dropped_total.fetch_add(1, std::memory_order_relaxed);
    }
    const bool wake = critical || s.queue.size() >= 64;
    lock.unlock();
    if (wake) s.changed.notify_one();
}
} // namespace

void diagnostics_note(const std::string& category, const std::string& message) {
    enqueue_note(category, message, false);
}

void diagnostics_set_runtime(const json& context) {
    State& s = state();
    if (!s.running.load(std::memory_order_acquire)) return;
    try {
        json safe = safe_runtime(context);
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.running.load(std::memory_order_relaxed)) return;
        s.runtime.merge_patch(safe);
        s.runtime_dirty = true;
        s.changed.notify_one();
    } catch (...) {}
}

bool diagnostics_flush(unsigned timeout_ms) {
    State& s = state();
    if (!s.running.load(std::memory_order_acquire)) return true;
    std::unique_lock<std::mutex> lock(s.mutex);
    const auto ticket = ++s.flush_requested;
    s.changed.notify_one();
    return s.changed.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return s.flush_done >= ticket || !s.running.load(std::memory_order_relaxed); });
}

void diagnostics_stop() {
    State& s = state();
    std::lock_guard<std::mutex> lifecycle(s.lifecycle);
    if (!s.running.load(std::memory_order_acquire)) return;
    diagnostics_note("session", "Clean shutdown requested; draining the diagnostics writer.");
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.running.store(false, std::memory_order_release);
        s.stopping = true;
        ++s.flush_requested;
    }
    s.changed.notify_all();
    if (s.writer.joinable()) s.writer.join();
    close_crash_capture();
}

void diagnostics_ffmpeg_log(void* context, int level, const char* format, va_list arguments) {
    (void)context;
    if (level > 32 || !format) return;
    char message[kMessageBytes + 1];
    va_list copy;
    va_copy(copy, arguments);
    std::vsnprintf(message, sizeof(message), format, copy);
    va_end(copy);
    enqueue_note("ffmpeg", message, level <= 8);
    // FFmpeg's fatal assertions may abort immediately after this callback.
    // Drain only fatal/panic messages, on their failing worker, so the useful
    // cause is persisted before the minimal signal receipt is written.
    if (level <= 8) (void)diagnostics_flush(1000); // AV_LOG_FATAL, without an FFmpeg header dependency
}

extern "C" void stremio_diagnostics_ui_log(const char* message) {
    if (message) diagnostics_note("ui", message);
}
