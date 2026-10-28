// Diagnostics HTTP server: GET /log (the whole log, or ?tail=KB), GET /log?follow=1
// (the last 16 KB, then every new line as it is written, as a chunked response),
// GET /events (the event log), GET /status (JSON). One thread accepts, each
// client gets a short-lived thread; at most a few at a time.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "diag_server.h"

#include "diagnostics.h"
#include "util.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <string>
#include <thread>

namespace diag_server {
namespace {
constexpr int kMaxClients = 4;
constexpr size_t kFollowTailBytes = 16 * 1024;
constexpr size_t kDefaultTailKb = 256;

struct State {
    std::mutex mutex;
    std::thread thread;
    std::atomic<bool> stop{false};
    std::atomic<int> clients{0};
    std::atomic<bool> running{false};
    int listen_fd = -1;
    int port = kDefaultPort;
    std::string directory;
    std::string status = "{}";
} g;

bool send_all(int fd, const char* data, size_t size) {
    while (size > 0) {
        const ssize_t n = ::send(fd, data, size, 0);
        if (n < 0) { if (errno == EINTR) continue; return false; }
        if (n == 0) return false;
        data += size_t(n);
        size -= size_t(n);
    }
    return true;
}

bool send_text(int fd, const std::string& text) { return send_all(fd, text.data(), text.size()); }

bool send_chunk(int fd, const char* data, size_t size) {
    char head[32];
    const int n = std::snprintf(head, sizeof(head), "%zx\r\n", size);
    return send_all(fd, head, size_t(n)) && send_all(fd, data, size) && send_all(fd, "\r\n", 2);
}

std::string header(int code, const char* reason, const char* type, long length, bool chunked) {
    std::string h = "HTTP/1.1 " + std::to_string(code) + " " + reason + "\r\n";
    h += "Content-Type: "; h += type; h += "; charset=utf-8\r\n";
    h += "Access-Control-Allow-Origin: *\r\nCache-Control: no-store\r\nConnection: close\r\n";
    if (chunked) h += "Transfer-Encoding: chunked\r\n";
    else if (length >= 0) h += "Content-Length: " + std::to_string(length) + "\r\n";
    return h + "\r\n";
}

bool file_size(const std::string& path, size_t* size) {
    struct stat info{};
    if (::stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) return false;
    *size = size_t(info.st_size);
    return true;
}

// Sends `path` from `from`; false when the file could not be read.
bool send_file_from(int fd, const std::string& path, size_t from, bool chunked) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, long(from), SEEK_SET);
    char buffer[16 * 1024];
    bool ok = true;
    while (ok) {
        const size_t n = std::fread(buffer, 1, sizeof(buffer), f);
        if (!n) break;
        ok = chunked ? send_chunk(fd, buffer, n) : send_all(fd, buffer, n);
    }
    std::fclose(f);
    return ok;
}

std::string query_value(const std::string& query, const std::string& key) {
    size_t at = 0;
    while (at < query.size()) {
        size_t end = query.find('&', at);
        if (end == std::string::npos) end = query.size();
        const std::string part = query.substr(at, end - at);
        if (part.compare(0, key.size() + 1, key + "=") == 0) return part.substr(key.size() + 1);
        at = end + 1;
    }
    return "";
}

void serve_log(int fd, const std::string& path, const std::string& query, const char* type) {
    size_t size = 0;
    if (!file_size(path, &size)) {
        const std::string body = "No log at " + path + "\n";
        send_text(fd, header(404, "Not Found", "text/plain", long(body.size()), false) + body);
        return;
    }
    if (query_value(query, "follow") == "1") {
        send_text(fd, header(200, "OK", type, -1, true));
        size_t at = size > kFollowTailBytes ? size - kFollowTailBytes : 0;
        if (!send_file_from(fd, path, at, true)) return;
        at = size;
        // The log writer buffers; flush it now and then so lines arrive within a second.
        while (!g.stop) {
            diagnostics_flush(400);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            size_t now = 0;
            if (!file_size(path, &now)) break;
            if (now < at) at = 0;  // rotated: start over
            if (now > at) {
                if (!send_file_from(fd, path, at, true)) return;
                at = now;
            } else {
                // Nothing new: notice a client that went away (its socket reads as closed).
                pollfd p{fd, POLLIN, 0};
                if (::poll(&p, 1, 0) > 0) {
                    char c;
                    if (::recv(fd, &c, 1, MSG_PEEK) <= 0) break;
                }
            }
        }
        send_all(fd, "0\r\n\r\n", 5);
        return;
    }
    size_t tail_kb = kDefaultTailKb;
    const std::string tail = query_value(query, "tail");
    if (tail == "all") tail_kb = 0;
    else if (!tail.empty()) tail_kb = size_t(std::strtoul(tail.c_str(), nullptr, 10));
    const size_t from = tail_kb && size > tail_kb * 1024 ? size - tail_kb * 1024 : 0;
    send_text(fd, header(200, "OK", type, long(size - from), false));
    send_file_from(fd, path, from, false);
}

// Small files sent whole, one after the other, each under its name. A missing
// file is reported as such; the receipts and boot files are a few KB at most.
void serve_files(int fd, const std::string& directory, std::initializer_list<const char*> names) {
    std::string body;
    for (const char* name : names) {
        body += "==== "; body += name; body += " ====\n";
        std::FILE* file = std::fopen((directory + "/" + name).c_str(), "rb");
        if (!file) { body += "(not present)\n\n"; continue; }
        char chunk[4096];
        size_t total = 0, n;
        while ((n = std::fread(chunk, 1, sizeof(chunk), file)) > 0 && total < (1u << 20)) { body.append(chunk, n); total += n; }
        std::fclose(file);
        if (body.empty() || body.back() != '\n') body += '\n';
        body += "\n";
    }
    send_text(fd, header(200, "OK", "text/plain", long(body.size()), false) + body);
}

void serve(int fd) {
    // One request line is enough: GET <path>[?query] HTTP/1.x
    char request[2048];
    size_t have = 0;
    pollfd p{fd, POLLIN, 0};
    while (have < sizeof(request) - 1) {
        if (::poll(&p, 1, 3000) <= 0) { ::close(fd); return; }
        const ssize_t n = ::recv(fd, request + have, sizeof(request) - 1 - have, 0);
        if (n <= 0) { ::close(fd); return; }
        have += size_t(n);
        request[have] = 0;
        if (std::strstr(request, "\r\n\r\n") || std::strstr(request, "\n\n")) break;
    }
    std::string line(request);
    line = line.substr(0, line.find('\n'));
    std::string target;
    if (line.compare(0, 4, "GET ") == 0) {
        target = line.substr(4);
        target = target.substr(0, target.find(' '));
    }
    std::string path = target, query;
    const size_t q = target.find('?');
    if (q != std::string::npos) { path = target.substr(0, q); query = target.substr(q + 1); }
    std::string directory;
    { std::lock_guard<std::mutex> lock(g.mutex); directory = g.directory; }
    if (path == "/log" || path == "/log.txt") {
        serve_log(fd, directory + "/log.txt", query, "text/plain");
    } else if (path == "/events" || path == "/events.jsonl") {
        serve_log(fd, directory + "/events.jsonl", query, "application/x-ndjson");
    } else if (path == "/status" || path == "/status.json") {
        std::string body;
        { std::lock_guard<std::mutex> lock(g.mutex); body = g.status; }
        body += "\n";
        send_text(fd, header(200, "OK", "application/json", long(body.size()), false) + body);
    } else if (path == "/crash" || path == "/crash.txt") {
        serve_files(fd, directory, {"crash-last.txt", "crash-current.txt"});
    } else if (path == "/boot" || path == "/boot.txt") {
        serve_files(fd, directory, {"boot-last.txt", "boot-current.txt"});
    } else if (path == "/" || path.empty()) {
        const std::string body =
            "Stremio Plus diagnostics\n"
            "  /log            the last 256 KB of the log (?tail=KB, ?tail=all)\n"
            "  /log?follow=1   the last 16 KB, then new lines as they are written\n"
            "  /events         the event log (same options)\n"
            "  /status         playback and output status as JSON\n"
            "  /crash          the crash receipts (crash-last.txt, then crash-current.txt)\n"
            "  /boot           the boot step files (boot-last.txt, then boot-current.txt)\n";
        send_text(fd, header(200, "OK", "text/plain", long(body.size()), false) + body);
    } else {
        const std::string body = "Not found\n";
        send_text(fd, header(404, "Not Found", "text/plain", long(body.size()), false) + body);
    }
    ::shutdown(fd, SHUT_RDWR);
    ::close(fd);
}

void run() {
    while (!g.stop) {
        pollfd p{g.listen_fd, POLLIN, 0};
        if (::poll(&p, 1, 250) <= 0) continue;
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        const int fd = ::accept(g.listen_fd, reinterpret_cast<sockaddr*>(&addr), &len);
        if (fd < 0) continue;
        if (g.clients.load() >= kMaxClients) {
            const std::string body = "Busy\n";
            send_text(fd, header(503, "Busy", "text/plain", long(body.size()), false) + body);
            ::close(fd);
            continue;
        }
        ++g.clients;
        std::thread([fd] { serve(fd); --g.clients; }).detach();
    }
}
}  // namespace

void configure(bool enabled, const std::string& log_directory, int port_number) {
    if (!enabled) { stop(); return; }
    if (g.running) return;
    std::lock_guard<std::mutex> lock(g.mutex);
    if (g.running) return;
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(uint16_t(port_number));
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(fd, 8) != 0) {
        dlog("diag: server could not listen on port %d (errno %d)", port_number, errno);
        ::close(fd);
        return;
    }
    g.listen_fd = fd;
    g.port = port_number;
    g.directory = log_directory;
    g.stop = false;
    g.running = true;
    g.thread = std::thread(run);
    dlog("diag: HTTP server on port %d: /log /log?follow=1 /events /status", port_number);
}

void stop() {
    if (!g.running) return;
    g.stop = true;
    if (g.thread.joinable()) g.thread.join();
    ::close(g.listen_fd);
    g.listen_fd = -1;
    g.running = false;
    dlog("diag: HTTP server stopped");
}

bool running() { return g.running; }
int port() { return g.port; }

void set_status(std::string json) {
    std::lock_guard<std::mutex> lock(g.mutex);
    g.status = std::move(json);
}

}  // namespace diag_server
