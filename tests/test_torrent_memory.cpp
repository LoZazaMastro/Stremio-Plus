// Exercise the existing payload budget and real C++ allocation-failure path.
// SPDX-License-Identifier: GPL-3.0-or-later
#define PLATFORM_PS5_NATIVE 1
#include "../src/torrent/engine.cpp"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>

namespace {
std::atomic<bool> fail_piece_allocation{false};
unsigned checks = 0, injected_failures = 0;
constexpr size_t piece_bytes = 512u << 10;

void check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
}

__attribute__((noinline)) void* operator new(size_t size) {
    if (size >= piece_bytes && size <= piece_bytes + 64 && fail_piece_allocation.exchange(false)) {
        ++injected_failures;
        throw std::bad_alloc();
    }
    if (void* pointer = std::malloc(size ? size : 1)) return pointer;
    throw std::bad_alloc();
}
__attribute__((noinline)) void operator delete(void* pointer) noexcept { std::free(pointer); }
__attribute__((noinline)) void operator delete(void* pointer, size_t) noexcept { std::free(pointer); }
void* operator new[](size_t size) { return ::operator new(size); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer, size_t) noexcept { std::free(pointer); }

extern "C" int console_curl_nonblocking(int socket) {
    return fcntl(socket, F_SETFL, fcntl(socket, F_GETFL, 0) | O_NONBLOCK);
}

int main() {
    try {
        auto torrent = std::make_shared<bt::Torrent>();
        torrent->has_meta = true;
        torrent->plen = piece_bytes;
        torrent->npieces = 4;
        torrent->total = int64_t(piece_bytes) * torrent->npieces;
        torrent->files = {{"video.mkv", torrent->total, 0}};
        torrent->file = 0;
        torrent->have.assign(4, 0);
        torrent->ever.assign(4, 0);
        torrent->wanted = {0};
        torrent->urgent_wanted = 1;
        torrent->wanted_dirty = false;
        bt::Peer peer;
        peer.state = bt::Peer::Active;
        peer.peer_choking = false;
        peer.has.assign(4, 1);
        peer.max_reqs = peer.reqq = 32;
        check(bt::payload_memory_bytes.load() == 0, "Fixture started with a live payload reservation");

        {
            bt::PayloadReservation backlog(128ll << 20);
            bt::pick(torrent.get(), &peer, 10);
            check(torrent->active.empty() && peer.reqs.empty() && peer.out.empty(),
                  "Existing payload backpressure published a piece or sent requests");
            check(bt::payload_memory_bytes.load() == (128ll << 20), "Rejected work retained another reservation");
        }
        check(bt::payload_memory_bytes.load() == 0, "Released backlog kept the payload budget occupied");
        fail_piece_allocation = true;
        bt::pick(torrent.get(), &peer, 11);
        check(injected_failures == 1, "The exact piece-buffer allocation was not forced to fail");
        check(torrent->active.empty() && peer.reqs.empty() && peer.out.empty(),
              "Allocation failure published incomplete scheduler state");
        check(bt::payload_memory_bytes.load() == 0, "Allocation failure leaked the provisional reservation");

        bt::pick(torrent.get(), &peer, 12);
        check(torrent->active.size() == 1 && peer.reqs.size() == 32,
              "The unchanged torrent source did not resume on its next scheduler pass");
        check(bt::payload_memory_bytes.load() == int64_t(piece_bytes),
              "A successful retry reserved the piece more than once");
        check(torrent->active.at(0).data.size() == piece_bytes && !peer.dead,
              "Retry did not retain the requested data size and peer connection");

        // Already-reserved work remains eligible while new allocations pause.
        peer.reqs.clear(); peer.out.clear();
        auto& piece = torrent->active.at(0);
        std::fill(piece.asked.begin(), piece.asked.end(), 0);
        piece.next_unasked = 0;
        {
            bt::PayloadReservation backlog((128ll << 20) - piece_bytes);
            bt::pick(torrent.get(), &peer, 13);
            check(peer.reqs.size() == 32, "A full payload budget prevented already-reserved work from completing");
        }
        check(bt::speed_limits().peers == 80 && bt::speed_limits().active_bytes == (128ll << 20),
              "The memory fix changed the existing ultra-fast limits");
        torrent->active.clear();
        check(bt::payload_memory_bytes.load() == 0, "Final cleanup left a payload reservation");
        std::cout << "TORRENT_MEMORY_TESTS_OK checks=" << checks << " injected_failures=" << injected_failures << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
