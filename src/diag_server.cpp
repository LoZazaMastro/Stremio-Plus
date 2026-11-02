// Bounded read-only diagnostics HTTP server. Network work stays on its manager
// and at most four owned client threads; configure only publishes desired state.
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
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <string>
#include <thread>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace diag_server {
namespace {
using Clock = std::chrono::steady_clock;
constexpr int kMaxClients = 4;
constexpr size_t kFollowTailBytes = 16 * 1024;
constexpr size_t kDefaultTailKb = 256;
constexpr int kPollMs = 50;
constexpr int kRequestMs = 3000;
constexpr int kWriteMs = 2000;
constexpr int kResponseMs = 15000;
struct Session { std::atomic<bool> stop{false}; };
struct Worker { std::thread thread; std::atomic<bool> done{true}; };
struct Config { bool enabled = false; std::string directory; int port = kDefaultPort; };
struct State {
    std::mutex lifecycle, mutex;
    std::condition_variable changed;
    std::thread thread;
    std::atomic<bool> quit{false}, running{false};
    std::atomic<uint64_t> generation{0};
    std::atomic<int> port{kDefaultPort};
    Config desired;
    std::string status = "{}";
    ~State() { quit = true; changed.notify_all(); if (thread.joinable()) thread.join(); }
} g;
struct Client {
    int fd;
    Session& session;
    Clock::time_point response_until = Clock::now() + std::chrono::milliseconds(kResponseMs);
    ~Client() { ::shutdown(fd, SHUT_RDWR); ::close(fd); }
};
bool nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}
bool wait_socket(Client& c, short events, Clock::time_point until) {
    while (!c.session.stop && !g.quit) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now()).count();
        if (left <= 0) return false;
        pollfd p{c.fd, events, 0};
        const int rc = ::poll(&p, 1, int(std::min<int64_t>(left, kPollMs)));
        if (rc < 0 && errno != EINTR) return false;
        if (rc > 0) {
            if (p.revents & (POLLERR | POLLNVAL)) return false;
            if (p.revents & events) return true;
            if (p.revents & POLLHUP) return false;
        }
    }
    return false;
}
bool send_all(Client& c, const char* data, size_t size) {
    const auto until = std::min(c.response_until, Clock::now() + std::chrono::milliseconds(kWriteMs));
    while (size && !c.session.stop && !g.quit) {
        if (!wait_socket(c, POLLOUT, until)) return false;
        const ssize_t n = ::send(c.fd, data, size, MSG_NOSIGNAL);
        if (n < 0) { if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue; return false; }
        if (!n) return false;
        data += size_t(n); size -= size_t(n);
    }
    return size == 0;
}
bool send_text(Client& c, const std::string& text) { return send_all(c, text.data(), text.size()); }
bool send_chunk(Client& c, const char* data, size_t size) {
    char head[32];
    const int n = std::snprintf(head, sizeof(head), "%zx\r\n", size);
    return send_all(c, head, size_t(n)) && send_all(c, data, size) && send_all(c, "\r\n", 2);
}
std::string header(int code, const char* reason, const char* type, size_t length, bool chunked = false) {
    std::string h = "HTTP/1.1 " + std::to_string(code) + " " + reason + "\r\nContent-Type: " + type +
        "; charset=utf-8\r\nCache-Control: no-store\r\nConnection: close\r\n";
    h += chunked ? "Transfer-Encoding: chunked\r\n" : "Content-Length: " + std::to_string(length) + "\r\n";
    return h + "\r\n";
}
bool file_size(const std::string& path, size_t* size) {
    struct stat info{};
    if (::stat(path.c_str(), &info) || !S_ISREG(info.st_mode) || info.st_size < 0) return false;
    *size = size_t(info.st_size); return true;
}
bool send_file(Client& c, const std::string& path, size_t from, size_t end, bool chunked) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return false;
    bool ok = ::fseeko(file, off_t(from), SEEK_SET) == 0;
    char buffer[16 * 1024];
    while (ok && from < end && !c.session.stop && !g.quit) {
        const size_t n = std::fread(buffer, 1, std::min(sizeof(buffer), end - from), file);
        if (!n) { ok = false; break; }
        ok = chunked ? send_chunk(c, buffer, n) : send_all(c, buffer, n);
        from += n;
    }
    std::fclose(file); return ok && from == end;
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
void serve_log(Client& c, const std::string& path, const std::string& query, const char* type) {
    size_t size = 0;
    if (!file_size(path, &size)) {
        const std::string body = "No log present\n";
        send_text(c, header(404, "Not Found", "text/plain", body.size()) + body); return;
    }
    if (query_value(query, "follow") == "1") {
        c.response_until = Clock::time_point::max(); // each chunk still has a write deadline
        if (!send_text(c, header(200, "OK", type, 0, true))) return;
        size_t at = size > kFollowTailBytes ? size - kFollowTailBytes : 0;
        if (!send_file(c, path, at, size, true)) return;
        at = size;
        auto next = Clock::now();
        while (!c.session.stop && !g.quit) {
            pollfd p{c.fd, POLLIN, 0};
            const int rc = ::poll(&p, 1, kPollMs);
            if (rc < 0 && errno != EINTR) break;
            // No second request is accepted; EOF, errors and extra bytes end follow.
            if (rc > 0 && p.revents) break;
            if (Clock::now() < next) continue;
            diagnostics_flush(100);
            next = Clock::now() + std::chrono::milliseconds(500);
            size_t now = 0;
            if (!file_size(path, &now)) break;
            if (now < at) at = 0;
            if (now > at && !send_file(c, path, at, now, true)) return;
            at = now;
        }
        if (!c.session.stop && !g.quit) send_all(c, "0\r\n\r\n", 5);
        return;
    }
    size_t tail_kb = kDefaultTailKb;
    const std::string tail = query_value(query, "tail");
    if (tail == "all") tail_kb = 0;
    else if (!tail.empty()) {
        char* end = nullptr; errno = 0;
        const unsigned long long value = std::strtoull(tail.c_str(), &end, 10);
        if (!errno && end != tail.c_str() && !*end && tail.front() != '-' && value <= (1u << 20))
            tail_kb = size_t(value);
    }
    const size_t from = tail_kb && size > tail_kb * 1024 ? size - tail_kb * 1024 : 0;
    if (send_text(c, header(200, "OK", type, size - from))) send_file(c, path, from, size, false);
}
void serve_files(Client& c, const std::string& directory, std::initializer_list<const char*> names) {
    std::string body;
    for (const char* name : names) {
        body += std::string("==== ") + name + " ====\n";
        FILE* file = std::fopen((directory + "/" + name).c_str(), "rb");
        if (!file) { body += "(not present)\n\n"; continue; }
        char chunk[4096]; size_t total = 0;
        while (total < (1u << 20)) {
            const size_t n = std::fread(chunk, 1, std::min(sizeof(chunk), (1u << 20) - total), file);
            if (!n) break;
            body.append(chunk, n); total += n;
        }
        std::fclose(file); body += "\n\n";
    }
    send_text(c, header(200, "OK", "text/plain", body.size()) + body);
}
void serve(Client& c, const std::string& directory) {
    char request[2048]{}; size_t have = 0; bool complete = false;
    const auto until = Clock::now() + std::chrono::milliseconds(kRequestMs);
    while (have < sizeof(request) - 1) {
        if (!wait_socket(c, POLLIN, until)) return;
        const ssize_t n = ::recv(c.fd, request + have, sizeof(request) - 1 - have, 0);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (n <= 0) return;
        have += size_t(n); request[have] = 0;
        if (std::strstr(request, "\r\n\r\n")) { complete = true; break; }
    }
    const std::string line(request, have);
    const size_t end = line.find("\r\n"), space = line.find(' ', 4);
    if (!complete || line.compare(0, 4, "GET ") || space == std::string::npos || space >= end ||
        (line.compare(space + 1, end - space - 1, "HTTP/1.1") && line.compare(space + 1, end - space - 1, "HTTP/1.0"))) {
        send_text(c, header(400, "Bad Request", "text/plain", 0)); return;
    }
    const std::string target = line.substr(4, space - 4);
    const size_t q = target.find('?');
    const std::string path = target.substr(0, q), query = q == std::string::npos ? "" : target.substr(q + 1);
    if (path == "/log" || path == "/log.txt") serve_log(c, directory + "/log.txt", query, "text/plain");
    else if (path == "/events" || path == "/events.jsonl") serve_log(c, directory + "/events.jsonl", query, "application/x-ndjson");
    else if (path == "/status" || path == "/status.json") {
        std::string body;
        { std::lock_guard<std::mutex> lock(g.mutex); body = g.status; }
        body += '\n'; send_text(c, header(200, "OK", "application/json", body.size()) + body);
    } else if (path == "/crash" || path == "/crash.txt") serve_files(c, directory, {"crash-last.txt", "crash-current.txt"});
    else if (path == "/boot" || path == "/boot.txt") serve_files(c, directory, {"boot-last.txt", "boot-current.txt"});
    else if (path == "/") {
        const std::string body = "Stremio Plus diagnostics\n/log (?tail=KB, ?tail=all, ?follow=1)\n/events\n/status\n/crash\n/boot\n";
        send_text(c, header(200, "OK", "text/plain", body.size()) + body);
    } else send_text(c, header(404, "Not Found", "text/plain", 0));
}
void run_session(const Config& config, uint64_t generation) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;
    int one = 1; ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in address{};
    address.sin_family = AF_INET; address.sin_port = htons(uint16_t(config.port));
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (!nonblocking(fd) || ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) || ::listen(fd, 8)) {
        dlog("diag: could not listen on port %d (errno %d)", config.port, errno); ::close(fd); return;
    }
    socklen_t length = sizeof(address);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length)) { ::close(fd); return; }
    g.port = ntohs(address.sin_port); g.running = true;
    dlog("diag: HTTP server on port %d", g.port.load());
    Session session; std::array<Worker, kMaxClients> workers;
    while (!g.quit && g.generation == generation) {
        for (auto& w : workers) if (w.thread.joinable() && w.done) w.thread.join();
        pollfd p{fd, POLLIN, 0};
        const int rc = ::poll(&p, 1, kPollMs);
        if (rc < 0 && errno != EINTR) break;
        if (rc > 0 && (p.revents & (POLLERR | POLLHUP | POLLNVAL))) break;
        if (rc <= 0 || !(p.revents & POLLIN)) continue;
        const int client = ::accept(fd, nullptr, nullptr);
        if (client < 0) continue;
        if (!nonblocking(client)) { ::close(client); continue; }
#ifdef SO_NOSIGPIPE
        ::setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        auto worker = std::find_if(workers.begin(), workers.end(), [](const Worker& w) { return !w.thread.joinable(); });
        if (worker == workers.end()) {
            static const char busy[] = "HTTP/1.1 503 Busy\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
            ::send(client, busy, sizeof(busy) - 1, MSG_NOSIGNAL); ::close(client); continue;
        }
        worker->done = false;
        try {
            worker->thread = std::thread([client, &session, &w = *worker, directory = config.directory] {
                { Client c{client, session}; try { serve(c, directory); } catch (...) {} }
                w.done = true;
            });
        } catch (...) { ::close(client); worker->done = true; }
    }
    session.stop = true; ::close(fd);
    for (auto& w : workers) if (w.thread.joinable()) w.thread.join();
    g.running = false;
}
void manage() {
    while (!g.quit) {
        Config config; uint64_t generation;
        {
            std::unique_lock<std::mutex> lock(g.mutex);
            g.changed.wait(lock, [] { return g.quit || g.desired.enabled; });
            if (g.quit) break;
            config = g.desired; generation = g.generation;
        }
        run_session(config, generation);
        // A failed bind waits for a changed configuration instead of retrying
        // and logging on each render frame. Toggle the setting to retry.
        std::unique_lock<std::mutex> lock(g.mutex);
        g.changed.wait(lock, [generation] { return g.quit || g.generation != generation; });
    }
}
} // namespace
void configure(bool enabled, const std::string& directory, int requested_port) {
    if (requested_port < 0 || requested_port > 65535) return;
    std::lock_guard<std::mutex> lifecycle(g.lifecycle);
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        if (g.desired.enabled == enabled && (!enabled ||
            (g.desired.directory == directory && g.desired.port == requested_port))) return;
        g.desired = Config{enabled, directory, requested_port}; ++g.generation;
    }
    if (enabled && !g.thread.joinable()) {
        g.quit = false;
        try { g.thread = std::thread(manage); } catch (...) {
            std::lock_guard<std::mutex> lock(g.mutex); g.desired.enabled = false; return;
        }
    }
    g.changed.notify_all();
}
void stop() {
    std::lock_guard<std::mutex> lifecycle(g.lifecycle);
    g.quit = true; g.changed.notify_all();
    if (g.thread.joinable()) g.thread.join();
    { std::lock_guard<std::mutex> lock(g.mutex); g.desired.enabled = false; ++g.generation; }
    g.running = false;
}
bool running() { return g.running; }
int port() { return g.port; }
void set_status(std::string status) {
    if (status.size() > 64 * 1024) return;
    std::lock_guard<std::mutex> lock(g.mutex); g.status = std::move(status);
}
} // namespace diag_server
