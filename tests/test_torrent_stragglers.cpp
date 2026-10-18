// Download-only overdue bulk rescue; real request ledger, no external swarm.
#ifndef ENGINE_IMPLEMENTATION
#define ENGINE_IMPLEMENTATION "../src/torrent/engine.cpp"
#endif
#include ENGINE_IMPLEMENTATION
#include <iostream>
#include <stdexcept>

namespace {
int checks = 0, network_attempts = 0;
void check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
struct Fixture {
    std::shared_ptr<bt::Torrent> t = std::make_shared<bt::Torrent>();
    std::string data;
    Fixture(int peers = 1, int64_t length = 1 << 20, int pieces = 32) {
        data.assign(size_t(length), 'Q');
        unsigned char digest[20]; SHA1(reinterpret_cast<const unsigned char*>(data.data()), data.size(), digest);
        t->has_meta = true; t->plen = length; t->npieces = pieces; t->total = pieces * length;
        t->files = {{"synthetic.bin", t->total, 0}}; t->file = -1;
        t->readers[1] = 0; t->reader_files[1] = 0; t->download_readers.insert(1);
        t->started = t->last_reader = 100; t->rate_at = 100;
        t->have.resize(size_t(pieces)); t->ever.resize(size_t(pieces));
        t->slot_of.assign(size_t(pieces), -1); t->nslots = 32; t->ram.resize(32);
        t->piece_in_slot.assign(32, -1); t->slot_touch.resize(32);
        for (int i = 0; i < pieces; ++i) t->hashes.append(reinterpret_cast<const char*>(digest), 20);
        for (int i = 0; i < peers; ++i) {
            auto p = std::make_unique<bt::Peer>();
            p->key = uint64_t(i + 1) << 16; p->state = bt::Peer::Active; p->peer_choking = false;
            p->since = p->last_data = p->last_send = 100; p->reqq = 1000;
            p->max_reqs = bt::speed_limits().floor;
            p->has.assign(size_t(pieces), 1); p->has_count = pieces;
            t->peers.push_back(std::move(p));
        }
        bt::refresh_wanted(t.get());
    }
    bt::Peer* peer(int n = 0) { return t->peers[size_t(n)].get(); }
    void answer(const bt::Peer::Req& r, double now) {
        bt::on_block(t.get(), peer(), r.piece, r.begin, data.data() + r.begin, r.len, now);
    }
};
void tests() {
    bt::impl().speed_profile = bt::SpeedProfile::UltraFast;
    {
        Fixture f(3, 65536, 8); auto* owner = f.peer(); auto* helper = f.peer(1); auto* other = f.peer(2);
        f.t->wanted = {0, 1, 2, 3}; f.t->urgent_wanted = 2; f.t->wanted_dirty = false;
        owner->max_reqs = 16; bt::pick(f.t.get(), owner, 100);
        helper->has[0] = helper->has[1] = other->has[0] = other->has[1] = 0;
        helper->max_reqs = other->max_reqs = 1;
        bt::pick(f.t.get(), helper, 100.5);
        check(helper->reqs.empty(), "fresh bulk blocks are never speculatively duplicated");
        f.t->download_readers.clear(); bt::pick(f.t.get(), helper, 101.1);
        check(helper->reqs.empty(), "bulk straggler rescue is restricted to background downloads");
        f.t->download_readers.insert(1); f.t->wanted.push_back(4);
        bt::pick(f.t.get(), helper, 101.1);
        check(helper->reqs.size() == 1 && helper->reqs[0].piece == 4, "available unique work wins over bulk duplicates");
        bt::release_all(f.t.get(), helper); f.t->wanted.pop_back();
        bt::pick(f.t.get(), helper, 101.2);
        check(helper->reqs.size() == 1 && helper->reqs[0].piece == 2, "idle peer rescues an overdue piece beyond urgent head");
        bt::pick(f.t.get(), other, 101.2);
        check(other->reqs.size() == 1, "another peer can use the remaining duplicate capacity");
        for (const auto& active : f.t->active) {
            for (size_t block = 0; block < active.second.asked.size(); ++block) {
                unsigned owners = 0;
                for (const auto& peer : f.t->peers)
                    for (const auto& request : peer->reqs)
                        if (request.piece == uint32_t(active.first) && request.begin == block * bt::kBlock) ++owners;
                check(owners <= 2 && owners == active.second.asked[block],
                      "bulk rescue or explicit replacement keeps the real owner ledger at at most two copies");
            }
        }
    }
    {
        Fixture f(2, 1 << 20, 8); auto* owner = f.peer(); auto* helper = f.peer(1);
        owner->max_reqs = helper->max_reqs = 512;
        bt::pick(f.t.get(), owner, 100);
        helper->has[0] = helper->has[1] = 0;
        bt::pick(f.t.get(), helper, 101.1);
        check(helper->reqs.size() == 16, "idle bulk rescue is bounded to sixteen blocks rather than a full 512-block queue");
        const auto duplicates = f.t->duplicate_requests;
        bt::pick(f.t.get(), helper, 101.2);
        check(helper->reqs.size() == 16 && f.t->duplicate_requests == duplicates,
              "an outstanding rescue batch does not trigger another bulk duplicate scan");
        const auto request = helper->reqs.front();
        bt::on_block(f.t.get(), helper, request.piece, request.begin, f.data.data() + request.begin, request.len, 101.3);
        bt::pick(f.t.get(), helper, 101.3);
        check(helper->reqs.size() == 15 && f.t->duplicate_requests == duplicates,
              "one reply does not refill an otherwise busy peer with more bulk duplicates");
        check(f.t->active.at(int(request.piece)).asked.at(request.begin / bt::kBlock) == 0,
              "a rescued reply releases both original and duplicate reservations");
    }
    check(network_attempts == 0, "no network operations attempted by offline policy regressions");
}
}
extern "C" int __wrap_socket(int, int, int) { ++network_attempts; errno = ENETUNREACH; return -1; }
extern "C" int __wrap_connect(int, const sockaddr*, socklen_t) { ++network_attempts; errno = ENETUNREACH; return -1; }
extern "C" int __wrap_getaddrinfo(const char*, const char*, const addrinfo*, addrinfo**) { ++network_attempts; return EAI_FAIL; }
int main() {
    try { tests(); std::cout << "RESULT {\"result\":\"PASS\",\"assertions\":" << checks << ",\"network_attempts\":" << network_attempts << "}\n"; }
    catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
