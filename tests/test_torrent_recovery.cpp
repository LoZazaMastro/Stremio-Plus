// Automatic recovery must preserve bytes and active readers while refreshing
// a stalled connection. No public peer or tracker is contacted by this test.
#define main tcp_benchmark_unused_main
#include "test_torrent_network.cpp"
#undef main
#include <future>
#include <sys/stat.h>

namespace {
int checks = 0;
void check(bool value, const char* message) { ++checks; require(value, message); }

struct Fixture {
    std::shared_ptr<bt::Torrent> torrent = std::make_shared<bt::Torrent>();
    std::string payload = std::string(3 * 32768, 'R');
    int reader = -1;
    explicit Fixture(const fs::path& directory) {
        for (size_t i = 0; i < payload.size(); ++i) payload[i] = char(i * 17 + i / 53);
        BValue info = BValue::dict();
        info.d["name"] = BValue(std::string("recovery.mp4"));
        info.d["length"] = BValue(int64_t(payload.size()));
        info.d["piece length"] = BValue(int64_t(32768));
        std::string hashes;
        for (size_t i = 0; i < payload.size(); i += 32768) hashes += digest(payload.substr(i, 32768));
        info.d["pieces"] = BValue(hashes);
        torrent->hex = hex(digest(bencode(info)));
        bt::impl().data_dir = directory.string(); bt::impl().cache_bytes = 8ll << 20;
        check(bt::parse_info(torrent.get(), bencode(info)), "parse exact metadata");
        torrent->started = now_seconds() - 60;
        bt::impl().torrents.push_back(torrent);
        int64_t size = 0; std::string error;
        check(bool(bt::Engine::get().open_reader(torrent->hex, 0, &reader, &size, &error,
                   bt::ReaderRole::Download)), "open real download reader");
        check(size == int64_t(payload.size()), "reader identity retains exact size");
    }
    bt::Peer* peer(double since) {
        auto p = std::make_unique<bt::Peer>();
        p->state = bt::Peer::Active; p->peer_choking = false; p->since = since;
        p->key = uint64_t(torrent->peers.size() + 1);
        p->has.assign(size_t(torrent->npieces), 1); p->has_count = torrent->npieces;
        p->reqq = p->max_reqs = 4;
        auto* result = p.get(); torrent->peers.push_back(std::move(p));
        return result;
    }
    void deliver(bt::Peer* p, size_t count, double at) {
        std::string wire;
        check(count <= p->reqs.size(), "send only requested blocks");
        for (size_t i = 0; i < count; ++i) {
            const auto& r = p->reqs[i];
            bt::put32(wire, r.len + 9); wire += char(7);
            bt::put32(wire, r.piece); bt::put32(wire, r.begin);
            wire.append(payload.data() + size_t(r.piece) * 32768 + r.begin, r.len);
        }
        p->in += wire; bt::process_input(torrent.get(), p, at);
    }
    void verify() {
        while (!bt::impl().disk_jobs.empty()) {
            auto job = std::move(bt::impl().disk_jobs.front()); bt::impl().disk_jobs.pop_front();
            bt::process_disk_job(std::move(job));
        }
    }
    ~Fixture() {
        verify(); bt::Engine::get().close_reader(torrent, reader);
        bt::impl().torrents.clear();
    }
};

struct ReadTask {
    std::atomic<bool> cancel{false};
    std::future<int> result;
    std::string bytes = std::string(32768, '\0');
    explicit ReadTask(Fixture& f) {
        result = std::async(std::launch::async, [this, &f] {
            return bt::Engine::get().read(f.torrent, f.reader, 0, 32768,
                reinterpret_cast<uint8_t*>(bytes.data()), int(bytes.size()), &cancel);
        });
    }
    ~ReadTask() {
        cancel = true; bt::impl().cv.notify_all();
        if (result.valid()) result.wait();
    }
};

double wait_for_reader(Fixture& f) {
    const double deadline = now_seconds() + 2;
    while (now_seconds() < deadline) {
        {
            std::lock_guard<std::mutex> lock(bt::impl().mu);
            const auto it = f.torrent->waiting_readers.find(f.reader);
            if (it != f.torrent->waiting_readers.end()) return it->second.since;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    throw std::runtime_error("reader did not enter wait");
}

void test_recovery(const fs::path& directory) {
    Fixture f(directory);
    auto* old = f.peer(now_seconds() - 60);
    bt::pick(f.torrent.get(), old, now_seconds() - 40);
    f.deliver(old, 3, now_seconds() - 40); f.verify();
    check(f.torrent->have[0] && !f.torrent->have[1], "first piece verified while next piece is partial");
    check(f.torrent->active.at(1).got[0], "first block of waiting piece is retained");
    ReadTask read(f);
    const double since = wait_for_reader(f);
    {
        std::lock_guard<std::mutex> lock(bt::impl().mu);
        bt::recover_stalled_torrent(f.torrent.get(), since + 29);
        check(f.torrent->stall_recoveries == 0 && !old->dead, "initial thirty-second grace");
        bt::recover_stalled_torrent(f.torrent.get(), since + 31);
        check(old->dead && old->recovery_retired && old->reqs.empty(), "stalled peer releases reservations");
        check(f.torrent->stall_recoveries == 1 && f.torrent->discovery_generation == 1,
              "one recovery wakes discovery");
        check(!f.torrent->stopped && f.torrent->readers.count(f.reader), "existing reader and torrent remain alive");
        check(f.torrent->have[0] && f.torrent->active.at(1).got[0], "verified and partial bytes survive recovery");
        check(std::all_of(f.torrent->active.at(1).asked.begin(), f.torrent->active.at(1).asked.end(),
              [](uint8_t owners) { return owners == 0; }), "cancelled peer leaves no stale reservations");
        bt::close_peer(f.torrent.get(), old, since + 31);
        check(f.torrent->known.at(old->key).retry_at == since + 32, "fresh handshake is eligible after one second");
        auto* fresh = f.peer(since + 31);
        bt::pick(f.torrent.get(), fresh, since + 32);
        check(!fresh->reqs.empty(), "new connection receives remaining work");
        check(std::none_of(fresh->reqs.begin(), fresh->reqs.end(), [](const auto& r) {
            return r.piece == 0 || (r.piece == 1 && r.begin == 0);
        }), "recovery requests exclude previously received blocks");
        f.deliver(fresh, fresh->reqs.size(), since + 32);
    }
    f.verify();
    check(read.result.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
          "original blocked reader resumes without reopening");
    check(read.result.get() == 32768 && read.bytes == f.payload.substr(32768, 32768),
          "resumed reader returns the exact SHA-1-verified bytes");
    check(f.torrent->waiting_readers.empty(), "completed reads clear recovery demand");
}

void test_guards(const fs::path& directory) {
    Fixture f(directory); auto* p = f.peer(1);
    const double now = 100;
    auto recover = [&] { bt::recover_stalled_torrent(f.torrent.get(), now); };
    recover(); check(!p->dead, "zero rate with no blocked reader does not restart");
    f.torrent->waiting_readers[f.reader] = {1, 10, nullptr};
    f.torrent->last_payload = 95;
    recover(); check(!p->dead, "recent useful data protects a productive connection");
    f.torrent->last_payload = 0;
    f.torrent->disk_pending.insert(1);
    recover(); check(!p->dead, "pending disk verification is not a network stall");
    f.torrent->disk_pending.clear();
    {
        bt::PayloadReservation occupied(bt::kMaxActiveBytes);
        recover(); check(!p->dead, "full memory budget does not trigger recovery");
    }
    std::atomic<bool> cancelled{true}; f.torrent->waiting_readers[f.reader].cancel = &cancelled;
    recover(); check(!p->dead, "cancelled playback never reconnects");
    f.torrent->waiting_readers[f.reader].cancel = nullptr;
    f.torrent->idle = true; recover(); check(!p->dead, "idle torrent never reconnects");
    f.torrent->idle = false; f.torrent->stopped = true;
    recover(); check(!p->dead, "stopped torrent never reconnects");
    f.torrent->stopped = false; f.torrent->have[1] = 1;
    recover(); check(!p->dead, "available bytes never reconnect");
    f.torrent->have[1] = 0;
    p->since = 90; recover(); check(!p->dead, "fresh connection retains discovery grace");
}

void test_backoff() {
    bt::Torrent t; t.started = 10; t.metadata_waiters = 1; t.metadata_progress_at = 12;
    bt::recover_stalled_torrent(&t, 41); check(t.stall_recoveries == 0, "metadata progress extends grace");
    bt::recover_stalled_torrent(&t, 43); check(t.stall_recoveries == 1 && t.next_recovery == 73, "metadata recovery starts after no progress");
    bt::recover_stalled_torrent(&t, 72); check(t.stall_recoveries == 1, "no rapid retries");
    bt::recover_stalled_torrent(&t, 74); check(t.stall_recoveries == 2 && t.next_recovery == 134, "second recovery backs off");
    bt::recover_stalled_torrent(&t, 135); check(t.stall_recoveries == 3 && t.next_recovery == 255, "repeated retries have a two-minute ceiling");
    t.metadata_waiters = 0; bt::recover_stalled_torrent(&t, 400);
    check(t.stall_recoveries == 3, "metadata cancellation removes retry demand");
    t.metadata_waiters = 1; t.metadata_progress_at = 399;
    bt::recover_stalled_torrent(&t, 400); check(t.stall_recoveries == 3, "renewed progress cancels an expired retry");
    bt::recover_stalled_torrent(&t, 430); check(t.stall_recoveries == 4 && t.next_recovery == 460, "successful progress resets backoff");
}

void test_directories(const fs::path& base) {
    fs::create_directories(base);
    ::chmod(base.c_str(), 0700);
    const auto path = base / "appdata/torrents";
    const mode_t mask = ::umask(0077);
    const bool created = make_shared_dirs(path.string());
    ::umask(mask);
    check(created, "shared cache created with restrictive umask");
    struct stat st{};
    for (const auto& p : {base / "appdata", path}) {
        check(::stat(p.c_str(), &st) == 0 && (st.st_mode & 0777) == 0777, "new cache folders actually have0777");
    }
    check(::stat(base.c_str(), &st) == 0 && (st.st_mode & 0777) == 0700, "unrelated parent permissions are preserved");
    ::chmod(path.c_str(), 0700);
    check(make_shared_dirs(path.string()) && ::stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0777,
          "older cache leaf permissions are repaired");
    fs::create_directory_symlink(path, base / "redirect");
    check(!make_shared_dirs((base / "redirect").string()), "shared leaf refuses redirected directories");
}
}

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: torrent-recovery cache_dir");
        const fs::path directory = argv[1]; fs::create_directories(directory);
        test_recovery(directory); test_guards(directory); test_backoff(); test_directories(directory / "permissions");
        check(external_attempts == 0, "no external network traffic");
        std::cout << "{\"result\":\"PASS\",\"checks\":" << checks << "}\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
