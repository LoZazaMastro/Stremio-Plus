#include "engine.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <openssl/rand.h>
#include <openssl/sha.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <new>
#include <set>
#include <thread>

#include "../http.h"
#include "../util.h"
#include "bencode.h"

#ifdef HAVE_DHT
#include <dht/dht.h>
#endif
#ifdef HAVE_UPNP
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#include <miniupnpc/upnperrors.h>
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#ifdef PLATFORM_PS5_NATIVE
// native/console_curl.c: the console's own non-blocking socket option.
extern "C" int console_curl_nonblocking(int socket);
#endif

namespace bt {

namespace {

const int kBlock = 16 * 1024;
const int kMaxPeers = 80;            // connected at once
// Most addresses from trackers and the DHT never answer (gone, or behind a
// router): try many at once and give up on each soon, so the few that work
// are found in seconds rather than minutes (on the PS5: 5 of 1000 answered,
// 30 attempts of 7 s each took minutes to get through the list).
const int kMaxConnecting = 80;       // connection attempts at once
const double kConnectTimeout = 4;
const double kHandshakeTimeout = 15;
const double kRequestTimeout = 15;   // useful-payload idle timeout and urgent-head handoff age
const double kStallRecoveryDelay = 30;
const double kChokedTimeout = 60;    // a peer that keeps us choked this long makes room for another
const uint32_t kMaxMessage = 2u << 20;
const size_t kPeerReceiveBudget = 256u << 10;
const int kBulkRescueBlocks = 16;
// How far ahead of the player to download: half the 6 GB cache, about a
// quarter of an hour of a 4K remux, to ride out slow patches in the swarm.
const int64_t kMaxReadahead = 3ll << 30;
const int64_t kMaxActiveBytes = 128ll << 20;  // all assembly, worker and verified buffers
const int64_t kMaxRamCache = 64ll << 20;      // included in the 128 MiB payload budget
const double kUrgentRetry = 1;      // rescue a waiting reader before fetching more bulk data
const double kPauseAfter = 45;       // no reader for this long: stop downloading
const int kTrackerThreads = 2;
const int kExtMetadata = 1, kExtPex = 2;  // our extension message ids (BEP 10)
// A full set of slow connections must still leave opportunities to find a
// better seed. Explore gradually, after measuring established peers, and never
// trade away the only available copy of a wanted piece.
const double kPeerTurnoverInterval = 30;
const double kPeerTurnoverGrace = 60;
const double kPeerTurnoverRetry = 180;
const double kPeerProductiveRate = 64 * 1024;
const double kPeerTurnoverRate = 512 * 1024;

const char* kDefaultTrackers[] = {
    "udp://tracker.opentrackr.org:1337/announce",
    "udp://open.stealth.si:80/announce",
    "udp://tracker.torrent.eu.org:451/announce",
    "udp://exodus.desync.com:6969/announce",
    "udp://open.demonii.com:1337/announce",
    "udp://explodie.org:6969/announce",
    "udp://tracker.openbittorrent.com:6969/announce",
    "udp://tracker.dler.org:6969/announce",
    "udp://p4p.arenabg.com:1337/announce",
    "http://tracker.opentrackr.org:1337/announce",
};

const char* kDhtBootstrap[][2] = {
    {"router.bittorrent.com", "6881"},
    {"dht.transmissionbt.com", "6881"},
    {"router.utorrent.com", "6881"},
    {"dht.libtorrent.org", "25401"},
};

}  // namespace

// A reservation follows the bytes from assembly to the worker and the verified
// read cache. Shared readers retain the same reservation, so stopping a torrent
// or evicting a slot cannot hide its still-live memory from the global bound.
std::atomic<int64_t> payload_memory_bytes{0};
struct PayloadReservation {
    int64_t bytes;
    explicit PayloadReservation(int64_t count) : bytes(count) { payload_memory_bytes += count; }
    ~PayloadReservation() { payload_memory_bytes -= bytes; }
};

namespace {

// ---------------------------------------------------------------------------
// Small helpers

uint32_t be32(const char* p) {
	const auto* u = reinterpret_cast<const uint8_t*>(p);
	return (uint32_t(u[0]) << 24) | (uint32_t(u[1]) << 16) | (uint32_t(u[2]) << 8) | u[3];
}
void put32(std::string& s, uint32_t v) {
	char b[4] = {char(v >> 24), char(v >> 16), char(v >> 8), char(v)};
	s.append(b, 4);
}
void put64(std::string& s, uint64_t v) {
	put32(s, uint32_t(v >> 32));
	put32(s, uint32_t(v));
}

bool hex_to_bytes(const std::string& hex, uint8_t out[20]) {
	if (hex.size() != 40) return false;
	for (int i = 0; i < 20; i++) {
		unsigned v;
		if (sscanf(hex.c_str() + i * 2, "%2x", &v) != 1) return false;
		out[i] = uint8_t(v);
	}
	return true;
}

std::string pct_encode(const std::string& raw) {
	static const char* hx = "0123456789ABCDEF";
	std::string out;
	for (unsigned char c : raw) {
		if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += char(c);
		else out += '%', out += hx[c >> 4], out += hx[c & 15];
	}
	return out;
}

uint64_t addr_key(const sockaddr_in& a) { return (uint64_t(ntohl(a.sin_addr.s_addr)) << 16) | ntohs(a.sin_port); }

std::string addr_str(const sockaddr_in& a) {
	char ip[INET_ADDRSTRLEN] = "?";
	inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
	return std::string(ip) + ":" + std::to_string(ntohs(a.sin_port));
}

bool usable(const sockaddr_in& a) {
	uint32_t ip = ntohl(a.sin_addr.s_addr);
	return ip != 0 && (ip >> 24) != 127 && (ip >> 24) != 0 && ntohs(a.sin_port) != 0 && (ip >> 28) < 14;
}

// Compact peers: 4 bytes IPv4 + 2 bytes port each.
void parse_compact(const std::string& s, std::vector<sockaddr_in>& out) {
	for (size_t i = 0; i + 6 <= s.size(); i += 6) {
		sockaddr_in a{};
		a.sin_family = AF_INET;
		memcpy(&a.sin_addr.s_addr, s.data() + i, 4);
		memcpy(&a.sin_port, s.data() + i + 4, 2);
		if (usable(a)) out.push_back(a);
	}
}

// In a PS5 title fcntl(O_NONBLOCK) doesn't make a socket non-blocking: the
// console has its own socket option for it (as curl's sockets use).
void set_nonblocking(int fd) {
#ifdef PLATFORM_PS5_NATIVE
	static bool logged = false;
	int r = console_curl_nonblocking(fd);
	if (!logged) {
		logged = true;
		dlog("torrent: non-blocking sockets: %s", r == 0 ? "ok" : "FAILED");
	}
	if (r == 0) return;
#endif
	int fl = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, (fl < 0 ? 0 : fl) | O_NONBLOCK);
}

// Whether a read would return at once. Reads in a loop check this first, so
// a socket that stayed blocking can't stall the network thread.
bool readable_now(int fd) {
	pollfd pf{fd, POLLIN, 0};
	return poll(&pf, 1, 0) > 0 && (pf.revents & (POLLIN | POLLERR | POLLHUP));
}

void random_bytes(void* buf, size_t n) {
	if (RAND_bytes(static_cast<unsigned char*>(buf), int(n)) != 1) {
		auto* p = static_cast<uint8_t*>(buf);
		for (size_t i = 0; i < n; i++) p[i] = uint8_t(rand());
	}
}

bool is_video_name(const std::string& path) {
	static const char* exts[] = {".mkv", ".mp4", ".avi", ".m4v", ".mov", ".ts", ".m2ts", ".webm", ".wmv", ".mpg", ".mpeg", ".flv"};
	std::string p = lower(path);
	for (auto* e : exts)
		if (ends_with(p, e)) return true;
	return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// State

struct Peer {
	int fd = -1;
	sockaddr_in addr{};
	uint64_t key = 0;
	enum State { Connecting, Handshaking, Active } state = Connecting;
	double since = 0, last_data = 0, last_send = 0, choked_since = 0;
	bool got_any = false;  // sent us at least one block
	bool incoming = false;  // it connected to us: our handshake goes after theirs
	std::string in;
	size_t in_off = 0;
	std::string out;
	bool ext = false;
	bool peer_choking = true, am_interested = false;
	int ut_metadata = 0, ut_pex = 0;
	int64_t metadata_size = 0;
	int reqq = 250;
	std::vector<uint8_t> has;
	int has_count = 0;
	bool has_all_early = false;
	std::string early_bitfield;
	std::vector<uint32_t> early_haves;
	struct Req {
		uint32_t piece, begin, len;
		double at;
	};
	std::vector<Req> reqs;
	int max_reqs = 16;
	int64_t bytes_window = 0;
	double rate = 0, pipeline_rate = 0, penalty_until = 0;
	int64_t useful_window = 0;
	double useful_rate = 0, payload_at = 0;
	double recent_useful_sample = 0;
	unsigned handoff_probe_requests = 0;
	bool turnover_retired = false;
	bool recovery_retired = false;
	bool dead = false;
};

struct Piece {  // being downloaded: blocks collect in memory until verified
	std::shared_ptr<PayloadReservation> memory;
	std::string data;
	std::vector<uint8_t> got;
	std::vector<uint8_t> asked;  // outstanding requests per block
	std::vector<double> asked_at;  // first outstanding copy; used only for overdue urgent blocks
	size_t next_unasked = 0;  // do not scan every earlier block for each received packet
	std::vector<uint64_t> from;  // which peer sent each block (for smart ban)
	int got_count = 0;
};

// A block of a piece that failed its check: who sent it and a hash of what
// they sent. When the piece later checks out, whoever sent a different block
// sent the bad data.
struct SuspectBlock {
	int block;
	uint64_t peer;
	uint64_t hash;
};

struct Known {
	double retry_at = 0;
	int fails = 0;
	bool connected = false;
};

struct VerifiedPiece {
    std::shared_ptr<PayloadReservation> memory;
    std::string data;
};

struct Torrent : std::enable_shared_from_this<Torrent> {
	std::string hex;
	uint8_t ih[20] = {};
	std::atomic<bool> stopped{false};
	double started = 0;

	std::mutex trk_m;  // trackers and tracker_peers (tracker threads)
	std::vector<std::string> trackers;
	std::vector<sockaddr_in> tracker_peers;
	std::atomic<bool> want_peers{true};
	std::atomic<bool> idle{false};
	std::atomic<uint64_t> discovery_generation{0};
	std::vector<std::thread> tracker_threads;

	// Metadata
	bool has_meta = false;
	std::string meta;
	int64_t meta_size = 0;
	std::vector<uint8_t> meta_got;
	std::vector<double> meta_asked;
	int metadata_waiters = 0;
	double metadata_progress_at = 0;
	int64_t plen = 0, total = 0;
	int npieces = 0;
	std::string hashes;
	std::vector<FileInfo> files;

	// Pieces
	std::vector<uint8_t> have, ever;
	std::map<int, Piece> active;
	std::set<int> disk_pending;
	int64_t disk_pending_bytes = 0;
	int file = -1;

	// Rolling cache: slots of one piece each, in a file (or memory).
	FILE* cache = nullptr;
    std::string cache_path;
	std::vector<std::shared_ptr<VerifiedPiece>> ram;
    bool cache_failed = false;  // keep its descriptor alive until pinned reads finish
    uint64_t cache_epoch = 0;
    std::vector<unsigned> slot_pins;
    std::vector<uint8_t> slot_busy;
    int hot_piece = -1;
    std::shared_ptr<VerifiedPiece> hot_verified;  // last verified piece, same budget
	int nslots = 0, slots_used = 0;
	std::vector<int> slot_of, piece_in_slot;
	std::vector<double> slot_touch;

	// Readers: position plus file identity. An auxiliary subtitle/hash
	// reader must not change the selected video or inherit its file bounds.
	std::map<int, int64_t> readers;
	std::map<int, int> reader_files;
	std::set<int> auxiliary_readers;
	std::set<int> download_readers;
	struct WaitingRead { int piece; double since; const std::atomic<bool>* cancel; };
	std::map<int, WaitingRead> waiting_readers;
	bool used_download_memory = false;
	int next_reader = 1;
	double last_reader = 0;

	// Download order, refreshed when readers move
	std::vector<int> wanted;
	size_t urgent_wanted = 0;  // video head and bounded auxiliary windows, before bulk read-ahead
	bool wanted_dirty = true;

	// Peers
	std::vector<std::unique_ptr<Peer>> peers;
	std::deque<sockaddr_in> candidates;
	std::map<uint64_t, Known> known;
	double next_refill = 0;
	double next_turnover = 0, last_payload = 0;
	double last_recovery = 0, next_recovery = 0;
	unsigned recovery_attempts = 0, stall_recoveries = 0;
	unsigned turnover_count = 0;
	uint64_t request_handoffs = 0, request_probe_handoffs = 0, timeout_cancels = 0;
	uint64_t request_head_handoffs = 0;
	uint64_t duplicate_requests = 0;
	uint64_t picker_passes = 0, received_batches = 0;
	// Smart ban: peers caught sending bad data (by IP), and the blocks of
	// failed pieces still to be judged.
	std::set<uint32_t> banned;
	std::map<int, std::vector<SuspectBlock>> suspects;

	// Stats
	int64_t bytes_window = 0;
	int64_t unique_window = 0, verified_bytes = 0, duplicate_bytes = 0;
	double rate = 0, wire_rate = 0, rate_at = 0;
	double hash_ms = 0, cache_read_ms = 0, cache_write_ms = 0;
	int64_t cache_ram_read_bytes = 0, cache_disk_read_bytes = 0;
	double next_dht = 0, next_log = 0;

	int64_t piece_size(int p) const { return std::min<int64_t>(plen, total - int64_t(p) * plen); }
	int blocks(int p) const { return int((piece_size(p) + kBlock - 1) / kBlock); }
	~Torrent() {
		if (cache) fclose(cache);
        if (!cache_path.empty()) ::unlink(cache_path.c_str());
	}
};

struct DiskJob {
    std::shared_ptr<Torrent> torrent;
    int index = -1;
    Piece piece;
    double received_at = 0;
};

// ---------------------------------------------------------------------------
// The engine

namespace {

struct Impl {
	std::mutex mu;
	std::condition_variable cv;  // pieces, metadata, stops
	std::thread net;
    std::thread disk_worker;
    std::deque<DiskJob> disk_jobs;
    bool disk_running = false, disk_quit = false;
    size_t disk_inflight = 0;
	bool running = false, quit = false;
	std::vector<std::shared_ptr<Torrent>> torrents;
	std::string data_dir;
	int64_t cache_bytes = 1ll << 30;
    uint64_t cache_sequence = 0;
	SpeedProfile speed_profile = SpeedProfile::UltraFast;
	std::string peer_id;

	// DHT
	int dht_fd = -1;
	bool dht_ok = false;
	double dht_next = 0, dht_saved = 0;
	uint8_t dht_id[20] = {};
	std::mutex boot_m;
	std::vector<sockaddr_in> boot;
	std::atomic<bool> boot_started{false};

	// Incoming connections: peers behind a router can only reach us here,
	// once the router forwards the port (UPnP, in a tracker thread).
	int listen_fd = -1;
	int listen_port = 0;
	std::mutex upnp_m;
	std::string upnp_control, upnp_service;  // to remove the mapping at exit
	std::atomic<int> mapped_port{0};
	std::atomic<bool> map_started{false};
};

Impl& impl() {
	static Impl* i = new Impl();  // never destroyed: threads may outlive statics at exit
	return *i;
}

struct SpeedLimits {
	int peers, floor, requests;
	int64_t active_bytes;
};

SpeedLimits speed_limits() {
	switch (impl().speed_profile) {
	case SpeedProfile::Balanced: return {40, 8, 128, 64ll << 20};
	case SpeedProfile::Fast: return {60, 16, 256, 96ll << 20};
	default: return {kMaxPeers, 32, 512, kMaxActiveBytes};
	}
}

void tracker_worker(std::shared_ptr<Torrent> t, int worker);
void net_loop();

std::shared_ptr<Torrent> find_locked(const std::string& hex) {
	for (auto& t : impl().torrents)
		if (t->hex == hex && !t->stopped) return t;
	return nullptr;
}

// ---------------------------------------------------------------------------
// Messages to peers

void send_msg(Peer* p, uint8_t id, const std::string& payload = "") {
	put32(p->out, uint32_t(payload.size() + 1));
	p->out += char(id);
	p->out += payload;
}

void send_ext(Peer* p, int ext_id, const std::string& payload) {
	std::string m;
	m += char(ext_id);
	m += payload;
	send_msg(p, 20, m);
}

void send_request(Peer* p, uint32_t piece, uint32_t begin, uint32_t len, double now) {
	std::string m;
	put32(m, piece);
	put32(m, begin);
	put32(m, len);
	send_msg(p, 6, m);
	p->reqs.push_back({piece, begin, len, now});
}

void send_cancel(Peer* p, uint32_t piece, uint32_t begin, uint32_t len) {
	std::string m;
	put32(m, piece);
	put32(m, begin);
	put32(m, len);
	send_msg(p, 8, m);
}

void send_handshake(Torrent* t, Peer* p) {
	std::string h;
	h += char(19);
	h += "BitTorrent protocol";
	char reserved[8] = {0, 0, 0, 0, 0, 0x10, 0, 0};  // extension protocol
	h.append(reserved, 8);
	h.append(reinterpret_cast<const char*>(t->ih), 20);
	h += impl().peer_id;
	p->out += h;
}

void send_ext_handshake(Peer* p) {
	BValue d = BValue::dict();
	BValue m = BValue::dict();
	m.d["ut_metadata"] = BValue(int64_t(kExtMetadata));
	m.d["ut_pex"] = BValue(int64_t(kExtPex));
	d.d["m"] = m;
	d.d["v"] = BValue(std::string("Stremio Plus"));
	d.d["reqq"] = BValue(int64_t(500));
	send_ext(p, 0, bencode(d));
}

// ---------------------------------------------------------------------------
// Requests

void release_request(Torrent* t, const Peer::Req& r) {
	auto it = t->active.find(int(r.piece));
	if (it == t->active.end()) return;
	size_t b = r.begin / kBlock;
	if (b < it->second.asked.size() && it->second.asked[b] > 0) {
		Piece& pc = it->second;
		if (--pc.asked[b] == 0 && !pc.got[b]) {
			pc.next_unasked = std::min(pc.next_unasked, b);
			if (b < pc.asked_at.size()) pc.asked_at[b] = 0;
		}
	}
}

void release_all(Torrent* t, Peer* p) {
	for (auto& r : p->reqs) release_request(t, r);
	p->reqs.clear();
}

void drop_peer(Torrent* t, Peer* p, const char* why) {
	if (p->dead) return;
	p->dead = true;
	release_all(t, p);
	(void)why;
}

// ---------------------------------------------------------------------------
// Download order

void start_disk_worker_locked();
void stop_disk_worker();
void wait_for_disk_idle();
bool make_payload_room_locked(Torrent* t, int64_t bytes, bool urgent = true);

bool download_memory_mode(const Torrent* t) {
    if (t->download_readers.empty()) return false;
    for (const auto& reader : t->readers)
        if (!t->download_readers.count(reader.first) && !t->auxiliary_readers.count(reader.first)) return false;
    return true;
}

int64_t readahead_bytes(Torrent* t) {
	int64_t cache = int64_t(t->nslots) * t->plen;
    const bool download_memory = download_memory_mode(t);
    if (!t->cache || t->cache_failed || download_memory) {
        cache = std::min(cache, std::min(kMaxActiveBytes, std::max(kMaxRamCache, 2 * t->plen)));
        if (download_memory) {
            // Reserve the actual bounded auxiliary windows before assigning
            // sequential read-ahead. Otherwise tail metadata would evict the
            // download's future pieces, only for the picker to fetch them again.
            int64_t auxiliary = 0;
            for (int id : t->auxiliary_readers) {
                const auto position = t->readers.find(id);
                const auto file = t->reader_files.find(id);
                if (position == t->readers.end() || file == t->reader_files.end() ||
                    file->second < 0 || size_t(file->second) >= t->files.size()) continue;
                const auto& f = t->files[size_t(file->second)];
                const int64_t first = std::max(position->second, f.offset);
                if (first >= f.offset + f.size) continue;
                const int64_t end = std::min(first + 65535, f.offset + f.size - 1);
                auxiliary += (end / t->plen - first / t->plen + 1) * t->plen;
            }
            cache = std::max(t->plen, cache - std::min(cache, auxiliary));
            cache = std::max(t->plen, cache / int64_t(t->download_readers.size()));
            cache -= cache % t->plen;
        }
        // Exactly the pieces the fallback can retain, including the current
        // piece; do not force the disk-only four-piece minimum onto tiny RAM.
        return std::max<int64_t>(0, std::min<int64_t>(cache - t->plen, kMaxReadahead));
    }
	return std::max<int64_t>(std::min<int64_t>(cache / 2, kMaxReadahead), 4 * t->plen);
}

bool in_reader_window(Torrent* t, int piece) {
    const int64_t begin = int64_t(piece) * t->plen, end = begin + t->piece_size(piece);
    const int64_t ahead = readahead_bytes(t);
    for (const auto& reader : t->readers) {
        const auto file = t->reader_files.find(reader.first);
        if (file == t->reader_files.end() || file->second < 0 || size_t(file->second) >= t->files.size()) continue;
        const auto& f = t->files[size_t(file->second)];
        const int64_t start = std::max(reader.second, f.offset);
        if (start >= f.offset + f.size) continue;
        const int64_t window = t->auxiliary_readers.count(reader.first) ? 65535 : ahead;
        const int64_t stop = std::min(start + window, f.offset + f.size - 1);
        if (begin <= stop && end > start) return true;
    }
    return false;
}

void refresh_wanted(Torrent* t) {
	t->wanted_dirty = false;
	t->wanted.clear();
	t->urgent_wanted = 0;
	if (!t->has_meta) return;
	struct ReaderStart { int64_t position; const FileInfo* file; int64_t ahead; bool auxiliary; };
	std::vector<ReaderStart> starts;
	const int64_t ahead = readahead_bytes(t);
	for (const auto& r : t->readers) {
		auto file = t->reader_files.find(r.first);
		int idx = file == t->reader_files.end() ? t->file : file->second;
		if (idx < 0 || size_t(idx) >= t->files.size()) continue;
		bool auxiliary = t->auxiliary_readers.count(r.first) != 0;
		starts.push_back({r.second, &t->files[size_t(idx)], auxiliary ? 65535 : ahead, auxiliary});
	}
	// Standalone downloads and auxiliary readers have their own file bounds;
	// they do not need (and must not invent) a selected playback file. Only
	// fall back to that selection when preparing playback without a reader.
	if (starts.empty() && t->readers.empty() && t->file >= 0 && size_t(t->file) < t->files.size()) {
		const FileInfo& f = t->files[size_t(t->file)];
		if (f.size > 0) starts.push_back({f.offset, &f, ahead, false});
	}
	std::stable_sort(starts.begin(), starts.end(), [](const auto& a, const auto& b) {
		if (a.auxiliary != b.auxiliary) return !a.auxiliary;
		return a.position < b.position;
	});
	std::set<int> seen;
	// The next two video pieces protect playback. Schedule each small
	// subtitle/hash window beside them, before any bulk video read-ahead:
	// an auxiliary file may be gigabytes later in the torrent's byte order.
	for (bool urgent : {true, false}) {
		for (const auto& reader : starts) {
			if (!urgent && reader.auxiliary) continue;
			const FileInfo& file = *reader.file;
			if (file.size <= 0) continue;
			const int64_t start = std::max(reader.position, file.offset);
			const int64_t file_end = file.offset + file.size - 1;
			if (start > file_end) continue;
			int first = int(start / t->plen);
			int end = int((start + std::min(reader.ahead, file_end - start)) / t->plen);
			if (urgent && !reader.auxiliary) end = std::min(end, first + 1);
			for (int p = first; p <= end; p++)
				if (!t->have[size_t(p)] && seen.insert(p).second) t->wanted.push_back(p);
		}
		if (urgent) t->urgent_wanted = t->wanted.size();
	}

	// After a jump the peers are still busy with blocks for the old spot:
	// call those off, so the new spot comes first, and drop the half-done
	// pieces nobody needs now (they'd hold memory and request slots).
	for (auto& up : t->peers) {
		Peer* p = up.get();
		if (p->dead) continue;
		for (size_t i = 0; i < p->reqs.size();) {
			if (seen.count(int(p->reqs[i].piece))) {
				i++;
				continue;
			}
			send_cancel(p, p->reqs[i].piece, p->reqs[i].begin, p->reqs[i].len);
			release_request(t, p->reqs[i]);
			p->reqs.erase(p->reqs.begin() + long(i));
		}
	}
	for (auto it = t->active.begin(); it != t->active.end();) {
		if (seen.count(it->first)) ++it;
		else it = t->active.erase(it);
	}
}

bool peer_has(Peer* p, int piece) { return size_t(piece) < p->has.size() && p->has[size_t(piece)]; }

void update_interest(Torrent* t, Peer* p) {
	if (!t->has_meta || p->state != Peer::Active) return;
	bool want = false;
	for (int w : t->wanted)
		if (peer_has(p, w)) {
			want = true;
			break;
		}
	// Keep interest in a peer that has anything we lack, so it may unchoke
	// us before the reader gets to its pieces.
	if (!want && p->has_count > 0) {
		for (int i = 0; i < t->npieces && !want; i++)
			if (p->has[size_t(i)] && !t->have[size_t(i)]) want = true;
	}
	if (want != p->am_interested) {
		p->am_interested = want;
		send_msg(p, want ? 2 : 3);
	}
}

// A slow peer holding a reader's first piece must not make all the fast peers
// fill gigabytes of later data first. Rescue only overdue head/auxiliary blocks,
// with at most two copies in flight; fresh blocks keep a full second to arrive.
// The eight-second EWMA remains useful for stable peer turnover. A newly
// admitted peer must also be able to prove capacity using the last complete
// one-second sample and new useful bytes in the current sampling interval.
double handoff_useful_rate(Torrent* t, Peer* p, double now) {
	if (p->payload_at <= 0 || now - p->payload_at >= 2) return p->useful_rate;
	return std::max(p->useful_rate, std::max(p->recent_useful_sample,
	                p->useful_window / std::max(1.0, now - t->rate_at)));
}

// Replace an old reservation atomically, keeping two owners as the hard limit.
// Downloads normally require a proven faster peer or a small newcomer probe;
// an overdue playback/download head also has a bounded deadline handoff.
bool make_duplicate_slot(Torrent* t, Peer* target, Piece& piece, int index, size_t block, double now,
                         int* overdue_head_room = nullptr) {
	if (piece.asked[block] == 1) return true;
	if (piece.asked[block] != 2) return false;
	const bool overdue_head = overdue_head_room && *overdue_head_room > 0 &&
	                          block < piece.asked_at.size() && now - piece.asked_at[block] > kRequestTimeout;
	if (t->download_readers.empty() && !overdue_head) return false;
	// A fresh peer with no measured capacity can otherwise be locked out by
	// two old reservations on every block. Permit at most 256 KiB of trial
	// requests, replacing only old, demonstrably slow owners. This lifetime
	// counter cannot reset as a request is answered, canceled or timed out.
	const double target_rate = handoff_useful_rate(t, target, now);
	const bool probe = target_rate < 128 * 1024 && target->handoff_probe_requests < 16 &&
	                   target->since > 0 && now >= target->since && now - target->since < 10;
	const bool measured_target = target->got_any && target_rate >= 128 * 1024 &&
	                             target->payload_at > 0 && now - target->payload_at < 15;
	const bool ordinary_handoff = !t->download_readers.empty() && (probe || measured_target);
	if (!ordinary_handoff && !overdue_head) return false;
	const uint32_t begin = uint32_t(block) * kBlock;
	Peer* slowest = nullptr;
	size_t selected = 0;
	bool selected_overdue = false;
	int owners = 0;
	for (const auto& candidate : t->peers) {
		Peer* peer = candidate.get();
		if (peer->dead || peer->state != Peer::Active) continue;
		for (size_t request = 0; request < peer->reqs.size(); ++request) {
			const auto& value = peer->reqs[request];
			if (value.piece != uint32_t(index) || value.begin != begin) continue;
			++owners;
			if (peer == target) return false;
			const double owner_rate = handoff_useful_rate(t, peer, now);
			const bool ordinary = ordinary_handoff && now - value.at >= kUrgentRetry &&
			                      (probe ? owner_rate < 64 * 1024 : target_rate >= owner_rate * 4);
			const bool expired = overdue_head && now - value.at > kRequestTimeout;
			if (!ordinary && !expired) continue;
			if (!slowest || owner_rate < handoff_useful_rate(t, slowest, now)) {
				slowest = peer; selected = request; selected_overdue = !ordinary;
			}
		}
	}
	// A stale/inconsistent reservation count is not permission to cancel work.
	if (owners != 2 || !slowest) return false;
	const Peer::Req previous = slowest->reqs[selected];
	send_cancel(slowest, previous.piece, previous.begin, previous.len);
	release_request(t, previous);
	slowest->reqs.erase(slowest->reqs.begin() + long(selected));
	++t->request_handoffs;
	if (selected_overdue) { --*overdue_head_room; ++t->request_head_handoffs; }
	else if (probe) { ++target->handoff_probe_requests; ++t->request_probe_handoffs; }
	return piece.asked[block] == 1;
}

void pick_urgent_duplicates(Torrent* t, Peer* p, double now, int& room) {
	const size_t urgent = std::min(t->wanted.size(), std::max<size_t>(2, t->urgent_wanted));
	// Tail delivery must not protect a missing head forever. Reassign at most
	// one old head reservation per pass even when the new peer is not four
	// times faster. Fresh owners and the rest of their queues stay untouched.
	int overdue_head_room = 1;
	for (size_t wi = 0; wi < urgent && room > 0; wi++) {
		int piece = t->wanted[wi];
		if (t->have[size_t(piece)] || t->disk_pending.count(piece) || !peer_has(p, piece)) continue;
		auto it = t->active.find(piece);
		if (it == t->active.end()) continue;
		Piece& pc = it->second;
		for (size_t b = 0; b < pc.got.size() && room > 0; b++) {
			if (pc.got[b] || pc.asked[b] == 0 || pc.asked[b] > 2 || b >= pc.asked_at.size() ||
			    now - pc.asked_at[b] < kUrgentRetry) continue;
			uint32_t begin = uint32_t(b) * kBlock;
			bool mine = false;
			for (const auto& request : p->reqs)
				if (request.piece == uint32_t(piece) && request.begin == begin) { mine = true; break; }
			if (mine) continue;
			if (!make_duplicate_slot(t, p, pc, piece, b, now, &overdue_head_room)) continue;
			uint32_t len = uint32_t(std::min<int64_t>(kBlock, t->piece_size(piece) - begin));
			send_request(p, uint32_t(piece), begin, len, now);
			++t->duplicate_requests;
			pc.asked[b]++;
			room--;
		}
	}
}

// If the download has no unique work available for this peer, use otherwise
// idle capacity to finish older partially assembled pieces. This is distinct
// from the unconditional head rescue above: bulk duplicates are a last resort,
// never exceed two copies, and fresh requests keep their one-second grace.
void pick_download_stragglers(Torrent* t, Peer* p, double now, int& room) {
	if (t->download_readers.empty() || room <= 0 || !p->reqs.empty()) return;
	// Endgame work uses an idle peer and a small request batch. A peer still
	// serving unique blocks must not fill its remaining queue with duplicates.
	// Urgent playback/download heads retain the separate immediate rescue.
	int rescue_room = std::min(room, kBulkRescueBlocks);
	for (int piece : t->wanted) {
		if (rescue_room <= 0) break;
		if (t->have[size_t(piece)] || !peer_has(p, piece)) continue;
		auto it = t->active.find(piece);
		if (it == t->active.end()) continue;
		Piece& pc = it->second;
		for (size_t b = 0; b < pc.got.size() && rescue_room > 0; ++b) {
			if (pc.got[b] || pc.asked[b] == 0 || pc.asked[b] > 2 || b >= pc.asked_at.size() ||
			    now - pc.asked_at[b] < kUrgentRetry) continue;
			const uint32_t begin = uint32_t(b) * kBlock;
			if (std::any_of(p->reqs.begin(), p->reqs.end(), [&](const Peer::Req& r) {
				return r.piece == uint32_t(piece) && r.begin == begin;
			})) continue;
			if (!make_duplicate_slot(t, p, pc, piece, b, now)) continue;
			const uint32_t length = uint32_t(std::min<int64_t>(kBlock, t->piece_size(piece) - begin));
			send_request(p, uint32_t(piece), begin, length, now);
			++t->duplicate_requests;
			++pc.asked[b];
			--room;
			--rescue_room;
		}
	}
}

void pick(Torrent* t, Peer* p, double now) {
	if (!t->has_meta || p->peer_choking || p->state != Peer::Active || p->dead) return;
	if (t->wanted_dirty) refresh_wanted(t);
	int room = std::min(p->max_reqs, p->reqq) - int(p->reqs.size());
	if (room <= 0) return;
	++t->picker_passes;
	
	const size_t urgent = std::min(t->wanted.size(), std::max<size_t>(2, t->urgent_wanted));
	for (size_t wi = 0; wi < t->wanted.size() && room > 0; wi++) {
		if (wi == urgent) {
			pick_urgent_duplicates(t, p, now, room);
			if (room <= 0) break;
		}
		int piece = t->wanted[wi];
		if (t->have[size_t(piece)] || t->disk_pending.count(piece) || !peer_has(p, piece)) continue;
		auto it = t->active.find(piece);
		if (it == t->active.end()) {
            // Reserve a small part of the same hard byte budget for newly
            // opened playback/auxiliary readers, instead of overallocating.

            const int64_t bytes = t->piece_size(piece);
            if (!make_payload_room_locked(t, bytes, wi < urgent)) continue;
            Piece pc;
            try {
                pc.memory = std::make_shared<PayloadReservation>(bytes);
                pc.data.resize(size_t(bytes));
                pc.got.assign(size_t(t->blocks(piece)), 0);
                pc.asked.assign(size_t(t->blocks(piece)), 0);
                pc.asked_at.assign(size_t(t->blocks(piece)), 0);
                it = t->active.emplace(piece, std::move(pc)).first;
            } catch (const std::bad_alloc&) {
                // Nothing has been requested or published for this piece.
                // The reservation rolls back and the next scheduler pass retries.
                return;
            }
		}
		Piece& pc = it->second;
		int nb = t->blocks(piece);
		for (size_t b = pc.next_unasked; b < size_t(nb) && room > 0; b++) {
			pc.next_unasked = b + 1;
			if (pc.got[b] || pc.asked[b]) continue;
			uint32_t begin = uint32_t(b) * kBlock;
			uint32_t len = uint32_t(std::min<int64_t>(kBlock, t->piece_size(piece) - begin));
			send_request(p, uint32_t(piece), begin, len, now);
			pc.asked[b]++;
			if (pc.asked_at.size() != pc.asked.size()) pc.asked_at.resize(pc.asked.size());
			pc.asked_at[b] = now;
			room--;
		}
	}
	if (urgent == t->wanted.size() && room > 0) pick_urgent_duplicates(t, p, now, room);
	if (room > 0) pick_download_stragglers(t, p, now, room);
}

// ---------------------------------------------------------------------------
// Cache

bool positional_write(int fd, int64_t offset, const std::string& data) {
    size_t done = 0;
    while (done < data.size()) {
        const ssize_t n = ::pwrite(fd, data.data() + done, data.size() - done, off_t(offset + done));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        done += size_t(n);
    }
    return true;
}

bool positional_read(int fd, int64_t offset, uint8_t* data, size_t size) {
    size_t done = 0;
    while (done < size) {
        const ssize_t n = ::pread(fd, data + done, size - done, off_t(offset + done));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        done += size_t(n);
    }
    return true;
}

void ensure_slot_state(Torrent* t) {
    if (t->slot_pins.size() != size_t(t->nslots)) t->slot_pins.resize(size_t(t->nslots));
    if (t->slot_busy.size() != size_t(t->nslots)) t->slot_busy.resize(size_t(t->nslots));
    if (t->ram.size() != size_t(t->nslots)) t->ram.resize(size_t(t->nslots));
}

bool slot_unavailable(Torrent* t, int s) {
    return t->slot_pins[size_t(s)] || t->slot_busy[size_t(s)];
}

void forget_slot_locked(Torrent* t, int slot) {
    const int previous = t->piece_in_slot[size_t(slot)];
    // A delayed failure/eviction of an old slot must not invalidate the
    // same piece after it has been verified again in a different slot.
    if (previous >= 0 && t->slot_of[size_t(previous)] == slot) {
        t->have[size_t(previous)] = 0;
        t->slot_of[size_t(previous)] = -1;
        if (t->hot_piece == previous) {
            t->hot_piece = -1;
            t->hot_verified.reset();
        }
    }
    t->piece_in_slot[size_t(slot)] = -1;
    t->ram[size_t(slot)].reset();
    t->wanted_dirty = true;
}

bool reader_at_piece(Torrent* t, int piece) {
    if (piece < 0) return false;
    for (const auto& reader : t->readers)
        if (reader.second / t->plen == piece) return true;
    return false;
}

bool evict_ram_piece_locked(Torrent* t) {
    ensure_slot_state(t);
    int64_t head = t->total;
    for (const auto& reader : t->readers) head = std::min(head, reader.second);
    int victim = -1;
    bool victim_behind = false;
    int64_t victim_distance = -1;
    const bool preserve_window = download_memory_mode(t);
    for (int slot = 0; slot < t->slots_used; ++slot) {
        const int piece = t->piece_in_slot[size_t(slot)];
        if (!t->ram[size_t(slot)] || slot_unavailable(t, slot) || reader_at_piece(t, piece)) continue;
        // A paused local writer applies backpressure instead of repeatedly
        // evicting and redownloading verified pieces ahead of its current read.
        if (preserve_window && in_reader_window(t, piece)) continue;
        const int64_t start = int64_t(piece) * t->plen;
        const bool behind = start + t->piece_size(piece) <= head;
        const int64_t distance = start - head;
        if (victim < 0 || (behind && !victim_behind) ||
            (behind == victim_behind &&
             (behind ? t->slot_touch[size_t(slot)] < t->slot_touch[size_t(victim)] : distance > victim_distance))) {
            victim = slot; victim_behind = behind; victim_distance = distance;
        }
    }
    if (victim < 0) return false;
    forget_slot_locked(t, victim);
    return true;
}

int64_t ram_cache_limit(const Torrent* t) {
    // Two maximum-sized pieces let an auxiliary tail reader coexist with a
    // downloader held on its current piece. Both remain inside the global cap.
    return std::min(kMaxActiveBytes, std::max(kMaxRamCache, 2 * t->plen));
}

bool make_payload_room_locked(Torrent* t, int64_t bytes, bool urgent) {
    int64_t limit = std::min(kMaxActiveBytes, std::max(t->plen, speed_limits().active_bytes));
    if (!urgent) limit = std::max(t->plen, limit - std::min<int64_t>(16ll << 20, 2 * t->plen));
    if (payload_memory_bytes.load() + bytes <= limit) return true;
    // A reader holding the old buffer still owns its reservation. Dropping the
    // hot-cache reference can only reclaim bytes that really are no longer used.
    t->hot_verified.reset(); t->hot_piece = -1;
    while (payload_memory_bytes.load() + bytes > limit && evict_ram_piece_locked(t)) {}
    return payload_memory_bytes.load() + bytes <= limit;
}

void disable_disk_cache_locked(Torrent* t) {
    t->cache_failed = true;
    ++t->cache_epoch;
    ensure_slot_state(t);
    for (int slot = 0; slot < t->slots_used; ++slot)
        if (!t->ram[size_t(slot)]) forget_slot_locked(t, slot);
    t->hot_verified.reset(); t->hot_piece = -1;
    t->wanted_dirty = true;
    dlog("torrent: cache write failed; bounded RAM cache max=%lld MiB, aggregate payload max=128 MiB",
         (long long)(ram_cache_limit(t) >> 20));
}

// A free slot, or the one whose piece is least likely to be read again:
// not ahead of a reader, and used longest ago.
int take_slot(Torrent* t, double now) {
    ensure_slot_state(t);
    for (int s = 0; s < t->slots_used; ++s)
        if (t->piece_in_slot[size_t(s)] < 0 && !slot_unavailable(t, s)) return s;
    if (t->slots_used < t->nslots) return t->slots_used++;
	int64_t ahead = readahead_bytes(t);
	int best = -1;
	double best_touch = 1e300;
	for (int s = 0; s < t->nslots; s++) {
        if (slot_unavailable(t, s)) continue;
		int p = t->piece_in_slot[size_t(s)];
		bool keep = false;
		int64_t ps = int64_t(p) * t->plen;
		for (const auto& r : t->readers) {
			int64_t window = t->auxiliary_readers.count(r.first) ? 65535 : ahead;
			auto file = t->reader_files.find(r.first);
			if (file != t->reader_files.end() && file->second >= 0 && size_t(file->second) < t->files.size()) {
				const auto& f = t->files[size_t(file->second)];
				window = std::min(window, f.offset + f.size - 1 - r.second);
			}
			if (window >= 0 && ps + t->plen > r.second && ps <= r.second + window) keep = true;
		}
		if (keep) continue;
		if (t->slot_touch[size_t(s)] < best_touch) best_touch = t->slot_touch[size_t(s)], best = s;
	}
    if (best < 0) {  // everything is ahead of a reader: the oldest anyway
        for (int s = 0; s < t->nslots; s++) {
            if (slot_unavailable(t, s)) continue;
            if (t->ram[size_t(s)] && reader_at_piece(t, t->piece_in_slot[size_t(s)])) continue;
            if (download_memory_mode(t) && in_reader_window(t, t->piece_in_slot[size_t(s)])) continue;
            if (t->slot_touch[size_t(s)] < best_touch) best_touch = t->slot_touch[size_t(s)], best = s;
        }
    }
    if (best < 0) return -1;
    forget_slot_locked(t, best);
	(void)now;
	return best;
}

// ---------------------------------------------------------------------------
// Metadata

bool parse_info(Torrent* t, const std::string& raw) {
	BValue info;
	if (!bdecode(raw, info) || !info.is_dict()) return false;
	int64_t plen = info.get_int("piece length");
	std::string hashes = info.get_str("pieces");
	if (plen <= 0 || plen > (64ll << 20) || hashes.empty() || hashes.size() % 20) return false;
	std::string name = info.get_str("name.utf-8");
	if (name.empty()) name = info.get_str("name");
	std::vector<FileInfo> files;
	int64_t total = 0;
	if (const BValue* fl = info.get("files")) {
		if (!fl->is_list()) return false;
		for (auto& f : fl->l) {
			FileInfo fi;
			fi.size = f.get_int("length", -1);
			if (fi.size < 0) return false;
			const BValue* path = f.get("path.utf-8");
			if (!path || !path->is_list()) path = f.get("path");
			std::string p = name;
			if (path && path->is_list())
				for (auto& part : path->l)
					if (part.is_str()) p += "/" + part.s;
			fi.path = p;
			fi.offset = total;
			total += fi.size;
			files.push_back(fi);
		}
	} else {
		FileInfo fi;
		fi.size = info.get_int("length", -1);
		if (fi.size < 0) return false;
		fi.path = name;
		total = fi.size;
		files.push_back(fi);
	}
	int64_t np = (total + plen - 1) / plen;
	if (total <= 0 || np * 20 != int64_t(hashes.size())) return false;

	t->plen = plen;
	t->total = total;
	t->npieces = int(np);
	t->hashes = hashes;
	t->files = files;
	t->have.assign(size_t(np), 0);
	t->ever.assign(size_t(np), 0);
	t->slot_of.assign(size_t(np), -1);

	// The cache: as many slots as fit in the cache size (at least 8).
	Impl& I = impl();
	int64_t slots = std::max<int64_t>(8, std::min<int64_t>(np, I.cache_bytes / plen));
	t->nslots = int(slots);
	std::string path = I.data_dir + "/torrent-cache-" + std::to_string(++I.cache_sequence) + ".bin";
	t->cache = fopen(path.c_str(), "w+b");
    if (t->cache) { t->cache_path = path; if (::unlink(path.c_str()) == 0) t->cache_path.clear(); }
	if (!t->cache) {
		dlog("torrent: can't create %s (errno %d); keeping pieces in memory", path.c_str(), errno);
		t->nslots = int(std::max<int64_t>(1, std::min<int64_t>(slots, ram_cache_limit(t) / plen)));
		t->ram.resize(size_t(t->nslots));
	}
	t->piece_in_slot.assign(size_t(t->nslots), -1);
	t->slot_touch.assign(size_t(t->nslots), 0);
    ensure_slot_state(t);
	t->has_meta = true;
	t->wanted_dirty = true;
	dlog("torrent %s: \"%s\", %zu files, %lld bytes, %d pieces of %lld KB, cache %d pieces", t->hex.c_str(),
	     name.c_str(), files.size(), (long long)total, t->npieces, (long long)(plen / 1024), t->nslots);
	return true;
}

void apply_bitfields(Torrent* t, Peer* p) {
	p->has.assign(size_t(t->npieces), 0);
	p->has_count = 0;
	if (p->has_all_early) {
		std::fill(p->has.begin(), p->has.end(), 1);
		p->has_count = t->npieces;
	} else {
		for (int i = 0; i < t->npieces && size_t(i / 8) < p->early_bitfield.size(); i++)
			if (uint8_t(p->early_bitfield[size_t(i / 8)]) & (0x80 >> (i % 8))) p->has[size_t(i)] = 1, p->has_count++;
		for (uint32_t h : p->early_haves)
			if (h < uint32_t(t->npieces) && !p->has[h]) p->has[h] = 1, p->has_count++;
	}
	p->early_bitfield.clear();
	p->early_haves.clear();
}

void metadata_done(Torrent* t) {
	unsigned char digest[20];
	SHA1(reinterpret_cast<const unsigned char*>(t->meta.data()), t->meta.size(), digest);
	if (memcmp(digest, t->ih, 20) != 0 || !parse_info(t, t->meta)) {
		dlog("torrent %s: metadata from peers didn't check out, fetching again", t->hex.c_str());
		std::fill(t->meta_got.begin(), t->meta_got.end(), 0);
		std::fill(t->meta_asked.begin(), t->meta_asked.end(), 0);
		return;
	}
	make_shared_dirs(impl().data_dir + "/torrents");
	write_file(impl().data_dir + "/torrents/" + t->hex + ".info", t->meta);
	for (auto& p : t->peers)
		if (p->state == Peer::Active) apply_bitfields(t, p.get());
	impl().cv.notify_all();
}

void request_metadata(Torrent* t, Peer* p, double now) {
	if (t->has_meta || !p->ut_metadata || p->metadata_size <= 0 || p->metadata_size > (32 << 20)) return;
	if (t->meta_size == 0) {
		t->meta_size = p->metadata_size;
		t->meta.assign(size_t(t->meta_size), '\0');
		size_t n = size_t((t->meta_size + kBlock - 1) / kBlock);
		t->meta_got.assign(n, 0);
		t->meta_asked.assign(n, 0);
	}
	if (p->metadata_size != t->meta_size) return;
	for (size_t i = 0; i < t->meta_got.size(); i++) {
		if (t->meta_got[i] || now - t->meta_asked[i] < 8) continue;
		BValue d = BValue::dict();
		d.d["msg_type"] = BValue(int64_t(0));
		d.d["piece"] = BValue(int64_t(i));
		send_ext(p, p->ut_metadata, bencode(d));
		t->meta_asked[i] = now;
	}
}

void on_metadata_msg(Torrent* t, Peer* p, const char* data, size_t n, double now) {
	BValue d;
	size_t used = 0;
	if (!bdecode(data, n, d, &used) || !d.is_dict()) return;
	int64_t type = d.get_int("msg_type", -1);
	int64_t piece = d.get_int("piece", -1);
	if (type == 1 && !t->has_meta && piece >= 0 && size_t(piece) < t->meta_got.size()) {
		size_t off = size_t(piece) * kBlock;
		size_t want = size_t(std::min<int64_t>(kBlock, t->meta_size - int64_t(off)));
		if (n - used != want || t->meta_got[size_t(piece)]) return;
		memcpy(&t->meta[off], data + used, want);
		t->meta_got[size_t(piece)] = 1;
		t->metadata_progress_at = now;
		if (std::all_of(t->meta_got.begin(), t->meta_got.end(), [](uint8_t g) { return g != 0; })) metadata_done(t);
	} else if (type == 2 && piece >= 0 && size_t(piece) < t->meta_asked.size()) {
		t->meta_asked[size_t(piece)] = 0;  // rejected: ask someone else
	} else if (type == 0) {
		// We don't serve metadata.
		BValue r = BValue::dict();
		r.d["msg_type"] = BValue(int64_t(2));
		r.d["piece"] = BValue(piece);
		if (p->ut_metadata) send_ext(p, p->ut_metadata, bencode(r));
	}
	(void)now;
}

// ---------------------------------------------------------------------------
// Peer messages

uint32_t key_ip(uint64_t key) { return uint32_t(key >> 16); }

uint64_t block_hash(const char* p, size_t n) {
	uint64_t h = 1469598103934665603ull;
	for (size_t i = 0; i < n; i++) h = (h ^ uint8_t(p[i])) * 1099511628211ull;
	return h;
}

// Never again: the peer sent data that failed the check.
void ban_peer(Torrent* t, uint64_t key, const char* why) {
	if (!key || !t->banned.insert(key_ip(key)).second) return;
	sockaddr_in a{};
	a.sin_addr.s_addr = htonl(key_ip(key));
	a.sin_port = htons(uint16_t(key & 0xffff));
	dlog("torrent %s: banned %s (%s)", t->hex.c_str(), addr_str(a).c_str(), why);
	for (auto& p : t->peers)
		if (key_ip(p->key) == key_ip(key)) drop_peer(t, p.get(), "banned");
}

void add_candidates(Torrent* t, const std::vector<sockaddr_in>& addrs) {
	for (auto& a : addrs) {
		uint64_t k = addr_key(a);
		if (t->known.count(k) || t->banned.count(key_ip(k))) continue;
		t->known[k] = Known();
		t->candidates.push_back(a);
	}
}

// Completed pieces leave the network thread immediately. The worker owns their
// existing buffers (no copy), verifies them and performs positional disk I/O.
// Only short state transitions and publication take the engine mutex.
void finish_disk_job_locked(const DiskJob& job) {
    auto& t = *job.torrent;
    t.disk_pending.erase(job.index);
    t.disk_pending_bytes -= int64_t(job.piece.data.size());
    t.wanted_dirty = true;
    impl().cv.notify_all();
}

void process_disk_job(DiskJob job) {
    Impl& I = impl();
    auto t = job.torrent;
    const int index = job.index;
    const int64_t byte_count = int64_t(job.piece.data.size());
    {
        std::lock_guard<std::mutex> lock(I.mu);
        if (t->stopped || I.disk_quit) { finish_disk_job_locked(job); return; }
    }
    unsigned char digest[20];
    const double hash_started = now_seconds();
    SHA1(reinterpret_cast<const unsigned char*>(job.piece.data.data()), job.piece.data.size(), digest);
    const double hash_ms = (now_seconds() - hash_started) * 1000;
    const bool valid = memcmp(digest, t->hashes.data() + size_t(index) * 20, 20) == 0;
    std::vector<SuspectBlock> evidence;
    bool one_sender = true;
    if (!valid) {
        evidence.reserve(job.piece.from.size());
        for (size_t b = 0; b < job.piece.from.size(); ++b) {
            const size_t off = b * kBlock, count = std::min<size_t>(kBlock, job.piece.data.size() - off);
            evidence.push_back({int(b), job.piece.from[b], block_hash(job.piece.data.data() + off, count)});
            if (job.piece.from[b] != job.piece.from[0]) one_sender = false;
        }
    } else {
        std::lock_guard<std::mutex> lock(I.mu);
        auto suspect = t->suspects.find(index);
        if (suspect != t->suspects.end()) evidence = suspect->second;
    }
    std::vector<uint64_t> bad_senders;
    if (valid) for (const auto& previous : evidence) {
        const size_t off = size_t(previous.block) * kBlock;
        if (off < job.piece.data.size() &&
            block_hash(job.piece.data.data() + off, std::min<size_t>(kBlock, job.piece.data.size() - off)) != previous.hash)
            bad_senders.push_back(previous.peer);
    }
    std::unique_lock<std::mutex> lock(I.mu);
    t->hash_ms += hash_ms;
    if (t->stopped || I.disk_quit) { finish_disk_job_locked(job); return; }
    if (!valid) {
        dlog("torrent %s: piece %d failed its check, downloading it again", t->hex.c_str(), index);
        t->suspects[index] = std::move(evidence);
        if (one_sender && !job.piece.from.empty()) ban_peer(t.get(), job.piece.from[0], "a whole piece of bad data");
        Piece& pc = job.piece;
        std::fill(pc.got.begin(), pc.got.end(), 0);
        std::fill(pc.asked.begin(), pc.asked.end(), 0);
        std::fill(pc.from.begin(), pc.from.end(), 0);
        std::fill(pc.asked_at.begin(), pc.asked_at.end(), 0);
        pc.got_count = 0; pc.next_unasked = 0;
        finish_disk_job_locked(job);
        t->active.emplace(index, std::move(pc));
        return;
    }
    for (uint64_t sender : bad_senders) ban_peer(t.get(), sender, "bad block in a failed piece");
    t->suspects.erase(index);
    const bool download_memory = download_memory_mode(t.get());
    // A seek, resume reconciliation or newly opened auxiliary reader can shrink
    // a previous request window while verification is queued. Such bytes do not
    // displace the bounded, still useful prefix or create cache writes on cancel.
    if ((download_memory && !in_reader_window(t.get(), index)) ||
        (t->used_download_memory && t->readers.empty() && t->file < 0)) { finish_disk_job_locked(job); return; }
    int slot = -1;
    for (;;) {
        if (t->stopped || I.disk_quit) { finish_disk_job_locked(job); return; }
        if ((slot = take_slot(t.get(), now_seconds())) >= 0) break;
        I.cv.wait_for(lock, std::chrono::milliseconds(50));
    }
    t->slot_busy[size_t(slot)] = 1;
    // A standalone download already writes the selected prefix to media.part.
    // Keep the verified handoff in bounded RAM so every downloaded byte is
    // written to its durable destination once, not to a second rolling file.
    bool on_disk = t->cache && !t->cache_failed && !download_memory;
    if (on_disk) {
        const int fd = fileno(t->cache);
        const double started = now_seconds();
        lock.unlock();
        const bool saved = positional_write(fd, int64_t(slot) * t->plen, job.piece.data);
        const double write_ms = (now_seconds() - started) * 1000;
        lock.lock();
        t->cache_write_ms += write_ms;
        if (!saved && !t->stopped && !I.disk_quit) {
            disable_disk_cache_locked(t.get());
            on_disk = false;
        }
    }
    auto release_slot = [&] { t->slot_busy[size_t(slot)] = 0; I.cv.notify_all(); };
    if (t->stopped || I.disk_quit) { release_slot(); finish_disk_job_locked(job); return; }
    if (!on_disk) {
        for (;;) {
            int64_t resident = 0;
            for (const auto& cached : t->ram) if (cached) resident += int64_t(cached->data.size());
            if (resident + byte_count <= ram_cache_limit(t.get())) break;
            if (evict_ram_piece_locked(t.get())) continue;
            if (t->stopped || I.disk_quit) { release_slot(); finish_disk_job_locked(job); return; }
            I.cv.wait_for(lock, std::chrono::milliseconds(50));
        }
    }
    auto verified = std::make_shared<VerifiedPiece>();
    finish_disk_job_locked(job);  // accounts the bytes before moving the buffer
    verified->data = std::move(job.piece.data);
    verified->memory = std::move(job.piece.memory);
    if (on_disk) {
        t->hot_piece = index;
        t->hot_verified = verified;
    } else {
        t->ram[size_t(slot)] = verified;
    }
    t->piece_in_slot[size_t(slot)] = index;
    t->slot_of[size_t(index)] = slot;
    t->slot_touch[size_t(slot)] = now_seconds();
    t->have[size_t(index)] = 1;
    t->ever[size_t(index)] = 1;
    t->verified_bytes += byte_count;
    release_slot();
}

void disk_loop() {
    Impl& I = impl();
    for (;;) {
        DiskJob job;
        {
            std::unique_lock<std::mutex> lock(I.mu);
            I.cv.wait(lock, [&] { return I.disk_quit || !I.disk_jobs.empty(); });
            if (I.disk_jobs.empty()) { if (I.disk_quit) break; continue; }
            job = std::move(I.disk_jobs.front()); I.disk_jobs.pop_front();
            ++I.disk_inflight;
        }
        process_disk_job(std::move(job));
        {
            std::lock_guard<std::mutex> lock(I.mu);
            --I.disk_inflight;
            I.cv.notify_all();
        }
    }
}

void start_disk_worker_locked() {
    Impl& I = impl();
    if (I.disk_running) return;
    I.disk_quit = false; I.disk_running = true;
    I.disk_worker = std::thread(disk_loop);
}

void wait_for_disk_idle() {
    Impl& I = impl();
    std::unique_lock<std::mutex> lock(I.mu);
    I.cv.wait(lock, [&] { return I.disk_jobs.empty() && I.disk_inflight == 0; });
}

void stop_disk_worker() {
    Impl& I = impl();
    {
        std::lock_guard<std::mutex> lock(I.mu);
        if (!I.disk_running) return;
        I.disk_quit = true;
        I.cv.notify_all();
    }
    if (I.disk_worker.joinable()) I.disk_worker.join();
    std::lock_guard<std::mutex> lock(I.mu);
    I.disk_running = false;
}

void on_block(Torrent* t, Peer* p, uint32_t piece, uint32_t begin, const char* data, size_t len, double now) {
	Peer::Req matched{};
	bool requested = false;
	for (size_t i = 0; i < p->reqs.size(); i++)
		if (p->reqs[i].piece == piece && p->reqs[i].begin == begin) {
			matched = p->reqs[i]; requested = true;
			p->reqs.erase(p->reqs.begin() + long(i));
			break;
		}
	// Invalid/redundant replies release their reservation for a fresh request.
	// A valid reply fills its block directly and must not rewind the picker.
	auto discard = [&] { if (requested) release_request(t, matched); };
	p->bytes_window += int64_t(len);
	t->bytes_window += int64_t(len);
	if (!t->has_meta || piece >= uint32_t(t->npieces) || begin % kBlock) { discard(); return; }
	auto it = t->active.find(int(piece));
	if (it == t->active.end()) { discard(); t->duplicate_bytes += int64_t(len); return; }
	Piece& pc = it->second;
	size_t b = begin / kBlock;
	if (b >= pc.got.size()) { discard(); return; }
	if (pc.got[b]) { discard(); t->duplicate_bytes += int64_t(len); return; }
	if (int64_t(begin) + int64_t(len) > int64_t(pc.data.size()) ||
	    int64_t(len) != std::min<int64_t>(kBlock, t->piece_size(int(piece)) - begin)) { discard(); return; }
	p->got_any = true;
	t->unique_window += int64_t(len);
	p->useful_window += int64_t(len);
	p->payload_at = t->last_payload = now;
	const bool has_other_copies = pc.asked[b] > (requested ? 1 : 0);
	memcpy(&pc.data[begin], data, len);
	pc.got[b] = 1;
	pc.got_count++;
	pc.asked[b] = 0;
	if (pc.from.size() != pc.got.size()) pc.from.assign(pc.got.size(), 0);
	pc.from[b] = p->key;
	// Others asked for the same block (see pick): call that off.
	// The common case has just one owner. Scanning every other peer's whole
	// request queue for every 16-KiB block wasted CPU even without duplicates.
	if (has_other_copies) for (auto& q : t->peers) {
		if (q.get() == p || q->dead) continue;
		for (size_t i = 0; i < q->reqs.size(); i++)
			if (q->reqs[i].piece == piece && q->reqs[i].begin == begin) {
				send_cancel(q.get(), piece, begin, q->reqs[i].len);
				q->reqs.erase(q->reqs.begin() + long(i));
				break;
			}
	}
	if (pc.got_count < int(pc.got.size())) return;

    DiskJob job;
    job.torrent = t->shared_from_this();
    job.index = int(piece);
    job.received_at = now;
    job.piece = std::move(pc);
    t->active.erase(it);
    t->disk_pending.insert(int(piece));
    t->disk_pending_bytes += int64_t(job.piece.data.size());
    impl().disk_jobs.push_back(std::move(job));
    impl().cv.notify_all();
}

void on_ext_handshake(Torrent* t, Peer* p, const char* data, size_t n, double now) {
	BValue d;
	if (!bdecode(data, n, d) || !d.is_dict()) return;
	if (const BValue* m = d.get("m")) {
		p->ut_metadata = int(m->get_int("ut_metadata", 0));
		p->ut_pex = int(m->get_int("ut_pex", 0));
	}
	p->metadata_size = d.get_int("metadata_size", 0);
	p->reqq = int(std::max<int64_t>(4, std::min<int64_t>(d.get_int("reqq", 250), 1000)));
	request_metadata(t, p, now);
}

void on_pex(Torrent* t, const char* data, size_t n) {
	BValue d;
	if (!bdecode(data, n, d) || !d.is_dict()) return;
	std::vector<sockaddr_in> v;
	parse_compact(d.get_str("added"), v);
	add_candidates(t, v);
}

void on_message(Torrent* t, Peer* p, uint8_t id, const char* pl, size_t n, double now) {
	switch (id) {
	case 0:  // choke
		if (!p->peer_choking) p->choked_since = now;
		p->peer_choking = true;
		release_all(t, p);
		break;
	case 1:  // unchoke
		p->peer_choking = false;
		pick(t, p, now);
		break;
	case 4:  // have
		if (n >= 4) {
			uint32_t idx = be32(pl);
			if (t->has_meta) {
				if (idx < uint32_t(t->npieces) && !p->has[idx]) {
					p->has[idx] = 1;
					p->has_count++;
					if (!p->am_interested) update_interest(t, p);
					if (!p->peer_choking) pick(t, p, now);
				}
			} else if (p->early_haves.size() < 1u << 20) {
				p->early_haves.push_back(idx);
			}
		}
		break;
	case 5:  // bitfield
		p->early_bitfield.assign(pl, n);
		if (t->has_meta) {
			apply_bitfields(t, p);
			update_interest(t, p);
		}
		break;
	case 7:  // piece
		if (n >= 8) on_block(t, p, be32(pl), be32(pl + 4), pl + 8, n - 8, now);
		break;
	case 14:  // have all (fast extension)
		p->has_all_early = true;
		if (t->has_meta) {
			apply_bitfields(t, p);
			update_interest(t, p);
		}
		break;
	case 20:  // extended
		if (n < 1) break;
		if (pl[0] == 0) on_ext_handshake(t, p, pl + 1, n - 1, now);
		else if (uint8_t(pl[0]) == kExtMetadata) on_metadata_msg(t, p, pl + 1, n - 1, now);
		else if (uint8_t(pl[0]) == kExtPex) on_pex(t, pl + 1, n - 1);
		break;
	default:  // interested, not interested, request, cancel, port, ...: we don't upload
		break;
	}
}

void process_input(Torrent* t, Peer* p, double now) {
	bool received_piece = false;
	if (p->state == Peer::Handshaking) {
		if (p->in.size() - p->in_off < 68) return;
		const char* h = p->in.data() + p->in_off;
		if (h[0] != 19 || memcmp(h + 1, "BitTorrent protocol", 19) != 0 || memcmp(h + 28, t->ih, 20) != 0) {
			drop_peer(t, p, "bad handshake");
			return;
		}
		p->ext = (uint8_t(h[25]) & 0x10) != 0;
		p->in_off += 68;
		p->state = Peer::Active;
		p->last_data = now;
		p->choked_since = now;
		if (p->incoming) send_handshake(t, p);
		if (p->ext) send_ext_handshake(p);
		if (t->has_meta) apply_bitfields(t, p);
	}
	while (!p->dead) {
		size_t avail = p->in.size() - p->in_off;
		if (avail < 4) break;
		uint32_t len = be32(p->in.data() + p->in_off);
		if (len > kMaxMessage) {
			drop_peer(t, p, "message too big");
			return;
		}
		if (avail < 4 + size_t(len)) break;
		const char* m = p->in.data() + p->in_off + 4;
		p->in_off += 4 + len;
		p->last_data = now;
		if (len > 0) {
			received_piece |= uint8_t(m[0]) == 7;
			on_message(t, p, uint8_t(m[0]), m + 1, len - 1, now);
		}
	}
	if (p->in_off == p->in.size()) {
		p->in.clear();
		p->in_off = 0;
	} else if (p->in_off > (256u << 10)) {
		p->in.erase(0, p->in_off);
		p->in_off = 0;
	}
	// All replies in this bounded receive batch release their request slots
	// together. Refilling once avoids rescanning pieces and duplicate owners
	// for every 16 KiB block while the verifier and readers wait for the lock.
	if (received_piece && !p->dead) {
		++t->received_batches;
		pick(t, p, now);
	}
}

// ---------------------------------------------------------------------------
// Connections

void prepare_peer_socket(int fd) {
	set_nonblocking(fd);
#ifdef SO_NOSIGPIPE
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
	// BitTorrent replenishes the pipeline with small request/cancel messages.
	// Do not wait for Nagle coalescing while a fast sender waits for work.
	int no_delay = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay));
	int rcv = 512 * 1024;
	const int buffer_result = setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
	const int buffer_error = buffer_result == 0 ? 0 : errno;
	int actual = 0;
	socklen_t actual_size = sizeof(actual);
	const int actual_result = getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &actual, &actual_size);
	static unsigned buffer_logs = 0;
	if (buffer_logs++ < 3)
		dlog("torrent socket: receive_requested=%d receive_actual=%d set_errno=%d query_ok=%d",
		     rcv, actual_result == 0 ? actual : -1, buffer_error, actual_result == 0);
}

void connect_peer(Torrent* t, const sockaddr_in& a, double now) {
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return;
	prepare_peer_socket(fd);
	auto p = std::make_unique<Peer>();
	p->max_reqs = speed_limits().floor;
	p->fd = fd;
	p->addr = a;
	p->key = addr_key(a);
	p->since = now;
	t->known[p->key].connected = true;
	int r = connect(fd, reinterpret_cast<const sockaddr*>(&a), sizeof(a));
	if (r == 0) {
		p->state = Peer::Handshaking;
		send_handshake(t, p.get());
	} else if (errno == EINPROGRESS || errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR) {
		p->state = Peer::Connecting;
	} else {
		static int logged = 0;
		if (logged++ < 3) dlog("torrent: connect to %s failed at once (errno %d)", addr_str(a).c_str(), errno);
		close(fd);
		Known& k = t->known[p->key];
		k.connected = false;
		k.fails++;
		k.retry_at = now + 60;
		return;
	}
	t->peers.push_back(std::move(p));
}

// The port peers can reach us on (told to trackers and the DHT).
int announce_port() {
	int p = impl().listen_port;
	return p > 0 ? p : 6881;
}

void open_listener() {
	Impl& I = impl();
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		dlog("torrent: no listening socket (errno %d)", errno);
		return;
	}
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	sockaddr_in a{};
	a.sin_family = AF_INET;
	int port = 0;
	for (int p = 6881; p <= 6889 && !port; p++) {
		a.sin_port = htons(uint16_t(p));
		if (bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) port = p;
	}
	if (!port || listen(fd, 32) != 0) {
		dlog("torrent: can't accept connections (errno %d)", errno);
		close(fd);
		return;
	}
	set_nonblocking(fd);
	I.listen_fd = fd;
	I.listen_port = port;
	dlog("torrent: accepting peers on port %d", port);
}

// Peers that connected to us join the torrent being played (one at a time).
void accept_peers(double now) {
	Impl& I = impl();
	for (int i = 0; i < 16; i++) {
		if (i > 0 && !readable_now(I.listen_fd)) break;
		sockaddr_in a{};
		socklen_t len = sizeof(a);
		int fd = accept(I.listen_fd, reinterpret_cast<sockaddr*>(&a), &len);
		if (fd < 0) break;
		Torrent* t = nullptr;
		for (auto& x : I.torrents)
			if (!x->stopped && !x->idle) t = x.get();
		uint64_t key = addr_key(a);
		if (!t || int(t->peers.size()) >= speed_limits().peers + 20 || t->known[key].connected) {
			close(fd);
			continue;
		}
		prepare_peer_socket(fd);
		auto p = std::make_unique<Peer>();
		p->max_reqs = speed_limits().floor;
		p->fd = fd;
		p->addr = a;
		p->key = key;
		p->since = now;
		p->incoming = true;
		p->state = Peer::Handshaking;
		t->known[key].connected = true;
		static int logged = 0;
		if (logged++ < 3) dlog("torrent: %s connected to us", addr_str(a).c_str());
		t->peers.push_back(std::move(p));
	}
}

void on_writable(Torrent* t, Peer* p, double now) {
	if (p->state == Peer::Connecting) {
		int err = 0;
		socklen_t len = sizeof(err);
		getsockopt(p->fd, SOL_SOCKET, SO_ERROR, &err, &len);
		if (err) {
			static int logged = 0;
			if (logged++ < 3) dlog("torrent: connect to %s failed (error %d)", addr_str(p->addr).c_str(), err);
			drop_peer(t, p, "connect failed");
			return;
		}
		p->state = Peer::Handshaking;
		p->since = now;
		send_handshake(t, p);
	}
	for (int i = 0; !p->out.empty(); i++) {
		if (i > 0) {  // as readable_now: never wait for room
			pollfd pf{p->fd, POLLOUT, 0};
			if (poll(&pf, 1, 0) <= 0 || !(pf.revents & POLLOUT)) break;
		}
		ssize_t n = send(p->fd, p->out.data(), p->out.size(), MSG_NOSIGNAL);
		if (n > 0) {
			p->out.erase(0, size_t(n));
			p->last_send = now;
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) break;
		drop_peer(t, p, "send failed");
		return;
	}
}

void on_readable(Torrent* t, Peer* p, double now) {
	char buf[64 * 1024];
	for (size_t i = 0; i < kPeerReceiveBudget / sizeof(buf) && !p->dead; i++) {
		if (i > 0 && !readable_now(p->fd)) break;
		ssize_t n = recv(p->fd, buf, sizeof(buf), 0);
		if (n > 0) {
			p->in.append(buf, size_t(n));
			if (n < ssize_t(sizeof(buf))) break;
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) break;
		// A peer can send complete blocks and FIN in the same readiness
		// batch. Admit the bytes already received before closing, otherwise
		// every full-buffer recv preceding EOF is silently discarded.
		process_input(t, p, now);
		if (!p->dead) drop_peer(t, p, n == 0 ? "closed" : "recv failed");
		return;
	}
	process_input(t, p, now);
}

void close_peer(Torrent* t, Peer* p, double now) {
	if (p->fd >= 0) close(p->fd);
	p->fd = -1;
	Known& k = t->known[p->key];
	k.connected = false;
	if (p->recovery_retired) {
		k.retry_at = now + 1;
	} else if (p->state == Peer::Active && p->got_any) {
		k.fails = 0;
		k.retry_at = now + (p->turnover_retired ? kPeerTurnoverRetry : 30);
	} else {
		k.fails++;
		k.retry_at = now + std::min(600.0, 30.0 * k.fails);
		if (p->turnover_retired) k.retry_at = std::max(k.retry_at, now + kPeerTurnoverRetry);
	}
}

// ---------------------------------------------------------------------------
// Upkeep, a few times a second

bool ready_candidate(Torrent* t, double now) {
	for (const auto& address : t->candidates) {
		const uint64_t key = addr_key(address);
		if (t->banned.count(key_ip(key))) continue;
		const auto known = t->known.find(key);
		if (known == t->known.end() || (!known->second.connected && known->second.retry_at <= now)) return true;
	}
	return false;
}

bool uniquely_useful_peer(Torrent* t, Peer* candidate) {
	// Most useful swarms contain several complete seeds. This shortcut avoids
	// scanning a long wanted window when any other established seed covers it.
	for (const auto& peer : t->peers)
		if (peer.get() != candidate && !peer->dead && peer->state == Peer::Active &&
		    peer->has_count == t->npieces) return false;
	for (int piece : t->wanted) {
		if (t->have[size_t(piece)] || !peer_has(candidate, piece)) continue;
		bool another = false;
		for (const auto& peer : t->peers)
			if (peer.get() != candidate && !peer->dead && peer->state == Peer::Active && peer_has(peer.get(), piece)) {
				another = true; break;
			}
		if (!another) return true;
	}
	return false;
}

void turnover_peer(Torrent* t, double now) {
	if (t->download_readers.empty() || !t->has_meta || t->wanted.empty() || t->idle ||
	    now < t->next_turnover) return;
	const auto limits = speed_limits();
	std::vector<Peer*> established;
	double best_rate = 0;
	for (auto& peer : t->peers) {
		if (peer->dead || peer->state != Peer::Active) continue;
		established.push_back(peer.get());
		best_rate = std::max(best_rate, peer->useful_rate);
	}
	// Connecting/handshaking peers get their full discovery grace. We make a
	// slot only when established connections themselves fill the budget.
	if (established.size() < size_t(limits.peers) || !ready_candidate(t, now)) return;
	t->next_turnover = now + kPeerTurnoverInterval;
	const double threshold = std::max(kPeerTurnoverRate, best_rate * 0.2);
	// Filter protections before ranking. Otherwise a slowest quintile made
	// entirely of idle/recent/rare peers can hide every eligible slow sender.
	established.erase(std::remove_if(established.begin(), established.end(), [&](Peer* peer) {
		if (now - peer->since < kPeerTurnoverGrace || peer->useful_rate >= threshold ||
		    uniquely_useful_peer(t, peer)) return true;
		// Keep peers starved by the picker while some useful capacity exists.
		// If the whole established pool is unproductive, an old idle peer may
		// yield one slot so a new candidate can be measured instead.
		return !peer->peer_choking && peer->reqs.empty() && best_rate >= kPeerProductiveRate &&
		       std::any_of(t->wanted.begin(), t->wanted.end(), [&](int piece) { return peer_has(peer, piece); });
	}), established.end());
	if (established.empty()) return;
	std::stable_sort(established.begin(), established.end(), [](const Peer* a, const Peer* b) {
		return a->useful_rate < b->useful_rate;
	});
	const size_t lowest = std::max<size_t>(1, established.size() / 5);
	for (size_t index = 0; index < lowest; ++index) {
		Peer* peer = established[index];
		peer->turnover_retired = true;
		drop_peer(t, peer, "low useful payload turnover");
		++t->turnover_count;
		dlog("torrent peers: replacing one slow connection, useful_KiBs=%.1f payload_idle_s=%.1f candidates=%zu total_replaced=%u",
		     peer->useful_rate / 1024, now - (peer->payload_at > 0 ? peer->payload_at : peer->since),
		     t->candidates.size(), t->turnover_count);
		break;  // one connection per interval; the network loop closes it first
	}
}

struct PeerHealth { int unchoked = 0, productive = 0; };

// Recovery is driven by a reader waiting for missing bytes, not a displayed
// zero rate. A paused player, full read-ahead cache or busy disk has no reason
// to discard its connections. Readers and verified/partial pieces survive.
void recover_stalled_torrent(Torrent* t, double now) {
	if (t->stopped || t->idle) return;
	double waiting_since = now;
	bool waiting = !t->has_meta && t->metadata_waiters > 0;
	if (waiting) waiting_since = t->started;
	if (t->has_meta) for (const auto& [reader, demand] : t->waiting_readers) {
		if (!t->readers.count(reader) || (demand.cancel && demand.cancel->load()) ||
		    demand.piece < 0 || demand.piece >= t->npieces || t->have[size_t(demand.piece)] ||
		    t->disk_pending.count(demand.piece)) continue;
		// A piece already being assembled has reserved its memory. Otherwise
		// wait for local capacity instead of treating backpressure as a stall.
		const int64_t memory_limit = std::min(kMaxActiveBytes, std::max(t->plen, speed_limits().active_bytes));
		if (!t->active.count(demand.piece) &&
		    payload_memory_bytes.load() + t->piece_size(demand.piece) > memory_limit) continue;
		waiting = true;
		waiting_since = std::min(waiting_since, demand.since);
	}
	if (!waiting) return;
	const double progress_at = t->has_meta ? t->last_payload : t->metadata_progress_at;
	if (progress_at > t->last_recovery) t->recovery_attempts = 0;
	const double quiet_since = std::max({waiting_since, progress_at, t->last_recovery});
	if (now < t->next_recovery || now - quiet_since < kStallRecoveryDelay) return;

	unsigned retired = 0;
	for (auto& peer : t->peers) {
		if (peer->dead || peer->state != Peer::Active ||
		    now - std::max(peer->since, peer->payload_at) < kStallRecoveryDelay) continue;
		peer->recovery_retired = true;
		drop_peer(t, peer.get(), "no useful data; automatic reconnect");
		++retired;
	}
	// Keep all known and banned identities. Eligible alternatives and the
	// retired peer's fresh handshake are tried through the ordinary limits.
	t->next_refill = 0;
	t->next_dht = 0;
	t->want_peers = true;
	++t->discovery_generation;
	t->wanted_dirty = true;
	if (!t->has_meta) std::fill(t->meta_asked.begin(), t->meta_asked.end(), 0);
	t->last_recovery = now;
	const double delay = std::min(120.0, kStallRecoveryDelay * double(1u << std::min(t->recovery_attempts, 2u)));
	t->next_recovery = now + delay;
	++t->recovery_attempts;
	++t->stall_recoveries;
	dlog("torrent recovery: stalled_s=%.1f reconnected=%u attempt=%u next_retry_s=%.0f metadata=%d waiting_readers=%zu verified_bytes=%lld",
	     now - quiet_since, retired, t->stall_recoveries, delay, int(t->has_meta),
	     t->waiting_readers.size(), (long long)t->verified_bytes);
}

PeerHealth peer_health(Torrent* t, double now) {
	PeerHealth health;
	for (const auto& peer : t->peers) {
		if (peer->dead || peer->state != Peer::Active) continue;
		if (!peer->peer_choking && peer->am_interested) ++health.unchoked;
		if (peer->payload_at > 0 && now - peer->payload_at < 15 && peer->useful_rate >= kPeerProductiveRate)
			++health.productive;
	}
	return health;
}

void upkeep(Torrent* t, double now) {
	const SpeedLimits limits = speed_limits();
	// Peers from the trackers
	{
		std::lock_guard<std::mutex> lock(t->trk_m);
		if (!t->tracker_peers.empty()) {
			add_candidates(t, t->tracker_peers);
			t->tracker_peers.clear();
		}
	}

	if (t->wanted_dirty) refresh_wanted(t);
	bool reading = !t->readers.empty();
	if (reading) t->last_reader = now;
	bool idle = t->has_meta && !reading && now - std::max(t->last_reader, t->started) > kPauseAfter;
	if (idle != t->idle) {
		t->idle = idle;
		dlog("torrent %s: %s", t->hex.c_str(), idle ? "nobody is watching, pausing" : "resuming");
	}
	recover_stalled_torrent(t, now);

	int active = 0, connecting = 0;
	for (auto& up : t->peers) {
		Peer* p = up.get();
		if (p->dead) continue;
		if (idle) {
			drop_peer(t, p, "paused");
			continue;
		}
		if (p->state == Peer::Connecting) {
			connecting++;
			if (now - p->since > kConnectTimeout) drop_peer(t, p, "connect timeout");
			continue;
		}
		if (p->state == Peer::Handshaking) {
			connecting++;  // a handshake still owns a socket/connection-attempt slot
			if (now - p->since > kHandshakeTimeout) drop_peer(t, p, "handshake timeout");
			continue;
		}
		active++;
		// A request near the back of a queue can be older than the timeout
		// while the peer keeps delivering. Only a lack of useful payload is a
		// stalled transfer; keep-alives and duplicate replies do not reset it.
		const bool delivery_stalled = p->peer_choking || p->payload_at <= 0 || now - p->payload_at > kRequestTimeout;
		bool late = false;
		for (size_t i = 0; i < p->reqs.size();) {
			if (delivery_stalled && now - p->reqs[i].at > kRequestTimeout) {
				send_cancel(p, p->reqs[i].piece, p->reqs[i].begin, p->reqs[i].len);
				++t->timeout_cancels;
				release_request(t, p->reqs[i]);
				p->reqs.erase(p->reqs.begin() + long(i));
				late = true;
			} else {
				i++;
			}
		}
		if (late) {
			p->max_reqs = std::max(4, p->max_reqs / 2);
			p->penalty_until = now + kRequestTimeout;
		}
		// Peers that keep us waiting make room for others, when there are others.
		bool others = !t->candidates.empty();
		if (others && p->peer_choking && p->am_interested && now - p->choked_since > kChokedTimeout) {
			drop_peer(t, p, "kept us choked");
			continue;
		}
		if (others && !t->has_meta && !p->ut_metadata && now - p->since > 20) {
			drop_peer(t, p, "can't send metadata");
			continue;
		}
		if (now - p->last_data > 180) {
			drop_peer(t, p, "silent");
			continue;
		}
		if (!t->has_meta) request_metadata(t, p, now);
		else {
			update_interest(t, p);
			// Give the other peers this pass to take released blocks before
			// the stalled sender can immediately reserve its old work again.
			if (!late) pick(t, p, now);
		}
		if (p->out.empty() && now - p->last_send > 90) put32(p->out, 0);  // keep-alive
	}

	// More peers
	if (!idle) {
		if (t->candidates.empty() && now >= t->next_refill) {
			t->next_refill = now + 5;
			for (auto& k : t->known) {
				if (k.second.connected || k.second.retry_at > now) continue;
				sockaddr_in a{};
				a.sin_family = AF_INET;
				a.sin_addr.s_addr = htonl(uint32_t(k.first >> 16));
				a.sin_port = htons(uint16_t(k.first & 0xffff));
				t->candidates.push_back(a);
				if (t->candidates.size() >= 200) break;
			}
		}
		while (active + connecting < limits.peers && connecting < std::min(limits.peers, kMaxConnecting) &&
		       !t->candidates.empty()) {
			sockaddr_in a = t->candidates.front();
			t->candidates.pop_front();
			Known& k = t->known[addr_key(a)];
			if (k.connected || k.retry_at > now || t->banned.count(key_ip(addr_key(a)))) continue;
			connect_peer(t, a, now);
			connecting++;
		}
	}
	// Do not let protocol-only connections suppress discovery. Once enough
	// alternatives are queued, wait for the normal tracker interval instead
	// of repeatedly asking for the same addresses.
	turnover_peer(t, now);
	const auto health = peer_health(t, now);
	const bool needs_data = !t->has_meta || !t->wanted.empty();
	t->want_peers = !idle && needs_data &&
	                (active < limits.peers / 2 || health.unchoked < limits.peers / 3 ||
	                 (health.productive < std::max(4, limits.peers / 4) && t->candidates.size() < size_t(limits.peers)));

	// Speeds, once a second
	if (now - t->rate_at >= 1) {
		double dt = now - t->rate_at;
		t->rate = t->rate_at > 0 ? t->unique_window / dt : 0;
		t->wire_rate = t->rate_at > 0 ? t->bytes_window / dt : 0;
		t->bytes_window = 0;
		t->unique_window = 0;
		t->rate_at = now;
		for (auto& up : t->peers) {
			Peer* p = up.get();
			const bool measured_requests = p->bytes_window > 0 || !p->reqs.empty();
			p->rate = p->bytes_window / dt;
			p->bytes_window = 0;
			// Credit only new, structurally valid payload accepted by on_block;
			// duplicate blocks and keepalives cannot make a stalled peer look fast.
			const double useful_sample = p->useful_window / dt;
			p->recent_useful_sample = useful_sample;
			p->useful_window = 0;
			const double keep = std::exp(-dt / 8.0);
			p->useful_rate = p->useful_rate * keep + useful_sample * (1 - keep);
			// Grow promptly; reduce gradually across short gaps. A quiet second
			// used to collapse a fast peer to 4 requests, then its artificially
			// small throughput kept it under-fed. Respect both the remote queue
			// limit and the selected profile, including a real timeout penalty.
			// A full local write buffer stops issuing requests. Idle capacity is
			// not evidence that an established peer became slower.
			if (measured_requests)
				p->pipeline_rate = p->rate >= p->pipeline_rate ? p->rate : p->pipeline_rate * 0.75 + p->rate * 0.25;
			int want = std::max(limits.floor, int(std::min(double(limits.requests), p->pipeline_rate * 2 / kBlock + 4)));
			if (now < p->penalty_until) want = std::min(want, p->max_reqs);
			p->max_reqs = std::max(4, std::min(want, p->reqq));
		}
		if (now >= t->next_log) {
			t->next_log = now + 10;
			int have = 0;
			for (uint8_t h : t->have) have += h;
			size_t requests = 0;
			for (const auto& p : t->peers)
				if (!p->dead) requests += p->reqs.size();
			dlog("torrent %s: %d peers (%d connecting, %zu known), %.2f MiB/s useful, %.2f MiB/s wire, %d pieces cached, %zu active, %zu readers, %zu download readers, %zu wanted, %zu requests",
			     t->hex.c_str(), active, connecting, t->known.size(), t->rate / (1024 * 1024), t->wire_rate / (1024 * 1024), have, t->active.size(),
			     t->readers.size(), t->download_readers.size(), t->wanted.size(), requests);
            dlog("torrent cost: verified=%lld duplicate=%lld bytes, hash=%.2fms cache_read=%.2fms cache_write=%.2fms payload_MiB=%.2f disk_pending=%zu ram_read=%lld disk_read=%lld",
                 (long long)t->verified_bytes, (long long)t->duplicate_bytes, t->hash_ms, t->cache_read_ms, t->cache_write_ms,
                 double(payload_memory_bytes.load()) / (1 << 20), t->disk_pending.size(),
                 (long long)t->cache_ram_read_bytes, (long long)t->cache_disk_read_bytes);
			const auto sampled = peer_health(t, now);
			dlog("torrent peers: unchoked=%d productive=%d candidates=%zu payload_idle_s=%.1f turnover=%u",
			     sampled.unchoked, sampled.productive, t->candidates.size(),
			     now - (t->last_payload > 0 ? t->last_payload : t->started), t->turnover_count);
			dlog("torrent requests: handoffs=%llu probes=%llu head_handoffs=%llu timeout_cancels=%llu duplicate_requests=%llu",
			     (unsigned long long)t->request_handoffs, (unsigned long long)t->request_probe_handoffs,
			     (unsigned long long)t->request_head_handoffs,
			     (unsigned long long)t->timeout_cancels, (unsigned long long)t->duplicate_requests);
			dlog("torrent scheduling: picker_passes=%llu receive_batches=%llu",
			     (unsigned long long)t->picker_passes, (unsigned long long)t->received_batches);
			t->hash_ms = t->cache_read_ms = t->cache_write_ms = 0;
		}
	}
}

// ---------------------------------------------------------------------------
// DHT

#ifdef HAVE_DHT
void dht_event(void* closure, int event, const unsigned char* info_hash, const void* data, size_t len) {
	(void)closure;
	if (event != DHT_EVENT_VALUES) return;
	std::vector<sockaddr_in> v;
	parse_compact(std::string(static_cast<const char*>(data), len), v);
	for (auto& t : impl().torrents)
		if (!t->stopped && memcmp(t->ih, info_hash, 20) == 0) add_candidates(t.get(), v);
}

void dht_start() {
	Impl& I = impl();
	I.dht_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (I.dht_fd < 0) {
		dlog("dht: no UDP socket (errno %d)", errno);
		return;
	}
	sockaddr_in any{};
	any.sin_family = AF_INET;
	// The same port number as the peer listener, so one router mapping
	// covers both; any port if that's taken.
	any.sin_port = htons(uint16_t(I.listen_port));
	bool bound = I.listen_port && bind(I.dht_fd, reinterpret_cast<sockaddr*>(&any), sizeof(any)) == 0;
	if (!bound) {
		any.sin_port = 0;
		bound = bind(I.dht_fd, reinterpret_cast<sockaddr*>(&any), sizeof(any)) == 0;
	}
	if (!bound) {
		dlog("dht: bind failed (errno %d)", errno);
		close(I.dht_fd);
		I.dht_fd = -1;
		return;
	}
	set_nonblocking(I.dht_fd);
	std::string saved;
	std::string path = I.data_dir + "/dht.dat";
	if (read_file(path, saved) && saved.size() >= 20) memcpy(I.dht_id, saved.data(), 20);
	else random_bytes(I.dht_id, 20);
	const unsigned char v[4] = {'S', 'P', 0, 1};
	if (dht_init(I.dht_fd, -1, I.dht_id, v) < 0) {
		dlog("dht: init failed");
		close(I.dht_fd);
		I.dht_fd = -1;
		return;
	}
	I.dht_ok = true;
	std::vector<sockaddr_in> nodes;
	if (saved.size() > 20) parse_compact(saved.substr(20), nodes);
	for (auto& a : nodes) dht_ping_node(reinterpret_cast<sockaddr*>(&a), sizeof(a));
	dlog("dht: started, %zu saved nodes", nodes.size());
	I.dht_saved = now_seconds();
}

void dht_save() {
	Impl& I = impl();
	if (!I.dht_ok) return;
	sockaddr_in sins[300];
	int num = 300, num6 = 0;
	dht_get_nodes(sins, &num, nullptr, &num6);
	std::string out(reinterpret_cast<const char*>(I.dht_id), 20);
	for (int i = 0; i < num; i++) {
		out.append(reinterpret_cast<const char*>(&sins[i].sin_addr.s_addr), 4);
		out.append(reinterpret_cast<const char*>(&sins[i].sin_port), 2);
	}
	write_file(I.data_dir + "/dht.dat", out);
}

void dht_poll(bool readable, double now) {
	Impl& I = impl();
	if (!I.dht_ok) return;
	{
		std::lock_guard<std::mutex> lock(I.boot_m);
		for (auto& a : I.boot) dht_ping_node(reinterpret_cast<sockaddr*>(&a), sizeof(a));
		I.boot.clear();
	}
	time_t tosleep = 1;
	if (readable) {
		char buf[4096];
		for (int i = 0; i < 64; i++) {
			if (i > 0 && !readable_now(I.dht_fd)) break;
			sockaddr_storage from{};
			socklen_t fl = sizeof(from);
			ssize_t n = recvfrom(I.dht_fd, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&from), &fl);
			if (n <= 0) break;
			buf[n] = 0;  // dht_periodic wants it terminated
			dht_periodic(buf, size_t(n), reinterpret_cast<sockaddr*>(&from), int(fl), &tosleep, dht_event, nullptr);
		}
		I.dht_next = now + double(tosleep);
	}
	if (now >= I.dht_next) {
		dht_periodic(nullptr, 0, nullptr, 0, &tosleep, dht_event, nullptr);
		I.dht_next = now + double(tosleep);
	}
	for (auto& t : I.torrents) {
		if (t->stopped || t->idle || now < t->next_dht) continue;
		int good = 0, dubious = 0, cached = 0, incoming = 0;
		dht_nodes(AF_INET, &good, &dubious, &cached, &incoming);
		if (good + dubious < 4) {
			t->next_dht = now + 3;  // still bootstrapping
			continue;
		}
		// With the port open on the router, also tell the DHT we have it,
		// so peers that can't be reached come to us.
		if (dht_search(t->ih, I.mapped_port.load(), AF_INET, dht_event, nullptr) >= 0) {
			t->next_dht = now + (t->want_peers ? 45 : 300);
		} else {
			t->next_dht = now + 10;
		}
	}
	if (now - I.dht_saved > 600) {
		I.dht_saved = now;
		dht_save();
	}
}
#endif

void resolve_bootstrap() {
	std::vector<sockaddr_in> found;
	for (auto& h : kDhtBootstrap) {
		addrinfo hints{}, *res = nullptr;
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_DGRAM;
		if (getaddrinfo(h[0], h[1], &hints, &res) != 0 || !res) continue;
		for (addrinfo* r = res; r; r = r->ai_next)
			if (r->ai_family == AF_INET) found.push_back(*reinterpret_cast<sockaddr_in*>(r->ai_addr));
		freeaddrinfo(res);
	}
	std::lock_guard<std::mutex> lock(impl().boot_m);
	impl().boot.insert(impl().boot.end(), found.begin(), found.end());
}

// ---------------------------------------------------------------------------
// Network thread

void net_loop() {
	Impl& I = impl();
	{
		std::lock_guard<std::mutex> lock(I.mu);
#ifdef HAVE_DHT
		dht_start();
#endif
	}
	std::vector<pollfd> fds;
	std::vector<std::pair<Torrent*, Peer*>> who;
	double next_upkeep = 0;
	for (;;) {
		fds.clear();
		who.clear();
		{
			std::lock_guard<std::mutex> lock(I.mu);
			if (I.quit) break;
			if (I.dht_fd >= 0) fds.push_back({I.dht_fd, POLLIN, 0});
			if (I.listen_fd >= 0) fds.push_back({I.listen_fd, POLLIN, 0});
			for (auto& t : I.torrents)
				for (auto& p : t->peers) {
					if (p->dead || p->fd < 0) continue;
					short ev = 0;
					if (p->state == Peer::Connecting) ev = POLLOUT;
					else ev = short(POLLIN | (p->out.empty() ? 0 : POLLOUT));
					fds.push_back({p->fd, ev, 0});
					who.push_back({t.get(), p.get()});
				}
		}
		int n = poll(fds.data(), nfds_t(fds.size()), 50);
		if (n < 0 && errno != EINTR) usleep(20000);

		std::unique_lock<std::mutex> lock(I.mu);
		if (I.quit) break;
		double now = now_seconds();
		size_t base = 0;
		bool dht_in = false, listen_in = false;
		if (I.dht_fd >= 0) dht_in = n > 0 && (fds[base++].revents & POLLIN);
		if (I.listen_fd >= 0) listen_in = n > 0 && (fds[base++].revents & POLLIN);
#ifdef HAVE_DHT
		dht_poll(dht_in, now);
#else
		(void)dht_in;
#endif
		if (listen_in) accept_peers(now);
		for (size_t i = 0; i < who.size(); i++) {
			short re = fds[base + i].revents;
			Torrent* t = who[i].first;
			Peer* p = who[i].second;
			if (!re || p->dead || t->stopped) continue;
			if (re & POLLOUT) on_writable(t, p, now);
			if (!p->dead && (re & POLLIN)) on_readable(t, p, now);
			if (!p->dead && (re & (POLLERR | POLLHUP | POLLNVAL)) && !(re & POLLIN)) drop_peer(t, p, "socket error");
			if (!p->dead && !p->out.empty()) on_writable(t, p, now);
			// Only this thread removes peers/torrents. Public calls and the
			// verifier may mark them stopped/dead while the lock is released.
			// Let cache publication, download reads and stats proceed between
			// peers instead of keeping the mutex for an entire busy swarm.
			lock.unlock();
			lock.lock();
			if (I.quit) break;
			now = now_seconds();
		}
		if (I.quit) break;
		if (now >= next_upkeep) {
			next_upkeep = now + 0.25;
			for (auto& t : I.torrents)
				if (!t->stopped) upkeep(t.get(), now);
		}
		// Flush what upkeep and the handlers queued
		for (auto& t : I.torrents)
			for (auto& p : t->peers)
				if (!p->dead && p->state != Peer::Connecting && !p->out.empty()) on_writable(t.get(), p.get(), now);
		// Remove closed peers and stopped torrents
		for (auto& t : I.torrents) {
			auto& v = t->peers;
			for (auto& p : v)
				if ((p->dead || t->stopped) && p->fd >= 0) {
					if (!p->dead) drop_peer(t.get(), p.get(), "stopped");
					close_peer(t.get(), p.get(), now);
				}
			v.erase(std::remove_if(v.begin(), v.end(), [](const std::unique_ptr<Peer>& p) { return p->fd < 0; }),
			        v.end());
		}
		for (auto it = I.torrents.begin(); it != I.torrents.end();) {
			if ((*it)->stopped) {
				for (auto& th : (*it)->tracker_threads)
					if (th.joinable()) th.join();
				dlog("torrent %s: stopped", (*it)->hex.c_str());
				it = I.torrents.erase(it);
			} else {
				++it;
			}
		}
	}
	// Quitting
	std::lock_guard<std::mutex> lock(I.mu);
	for (auto& t : I.torrents) {
		t->stopped = true;
		for (auto& p : t->peers)
			if (p->fd >= 0) close(p->fd), p->fd = -1;
		for (auto& th : t->tracker_threads)
			if (th.joinable()) th.join();
	}
	I.torrents.clear();
#ifdef HAVE_DHT
	if (I.dht_ok) {
		dht_save();
		dht_uninit();
		I.dht_ok = false;
	}
#endif
	if (I.dht_fd >= 0) close(I.dht_fd), I.dht_fd = -1;
	I.cv.notify_all();
}

// ---------------------------------------------------------------------------
// Trackers (their own threads: HTTP and DNS block)

bool wait_stoppable(Torrent* t, double seconds) {
	double until = now_seconds() + seconds;
	while (now_seconds() < until) {
		if (t->stopped) return false;
		usleep(250 * 1000);
	}
	return !t->stopped;
}

int64_t bytes_left(Torrent* t) { return t->has_meta ? t->total : 1ll << 30; }

bool announce_http(Torrent* t, const std::string& url, bool first, std::vector<sockaddr_in>& peers, int* interval) {
	std::string u = url + (url.find('?') == std::string::npos ? "?" : "&");
	u += "info_hash=" + pct_encode(std::string(reinterpret_cast<const char*>(t->ih), 20));
	u += "&peer_id=" + pct_encode(impl().peer_id);
	u += "&port=" + std::to_string(announce_port()) + "&uploaded=0&downloaded=0&left=" + std::to_string(bytes_left(t));
	u += "&compact=1&numwant=200";
	if (first) u += "&event=started";
	HttpResponse r = http_get(u, 15, &t->stopped);
	if (!r.ok()) return false;
	BValue d;
	if (!bdecode(r.body, d) || !d.is_dict() || d.get("failure reason")) return false;
	if (const BValue* p = d.get("peers")) {
		if (p->is_str()) {
			parse_compact(p->s, peers);
		} else if (p->is_list()) {
			for (auto& e : p->l) {
				sockaddr_in a{};
				a.sin_family = AF_INET;
				if (inet_pton(AF_INET, e.get_str("ip").c_str(), &a.sin_addr) != 1) continue;
				a.sin_port = htons(uint16_t(e.get_int("port")));
				if (usable(a)) peers.push_back(a);
			}
		}
	}
	*interval = int(d.get_int("interval", 0));
	return true;
}

bool udp_exchange(Torrent* t, int fd, const std::string& req, std::string& resp, uint32_t tid) {
	for (int attempt = 0; attempt < 2; attempt++) {
		if (send(fd, req.data(), req.size(), 0) < 0) return false;
		double until = now_seconds() + 4 + attempt * 4;
		while (now_seconds() < until) {
			if (t->stopped) return false;
			pollfd pf{fd, POLLIN, 0};
			if (poll(&pf, 1, 250) <= 0) continue;
			char buf[4096];
			ssize_t n = recv(fd, buf, sizeof(buf), 0);
			if (n < 8) continue;
			if (be32(buf + 4) != tid) continue;
			resp.assign(buf, size_t(n));
			return true;
		}
	}
	return false;
}

bool announce_udp(Torrent* t, const std::string& url, bool first, std::vector<sockaddr_in>& peers, int* interval) {
	// udp://host:port[/announce]
	std::string rest = url.substr(6);
	size_t slash = rest.find('/');
	if (slash != std::string::npos) rest = rest.substr(0, slash);
	size_t colon = rest.rfind(':');
	if (colon == std::string::npos) return false;
	std::string host = rest.substr(0, colon), port = rest.substr(colon + 1);
	addrinfo hints{}, *res = nullptr;
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) return false;
	sockaddr_in to = *reinterpret_cast<sockaddr_in*>(res->ai_addr);
	freeaddrinfo(res);
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) return false;
	bool ok = false;
	if (connect(fd, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == 0) {
		uint32_t tid;
		random_bytes(&tid, 4);
		std::string req;
		put64(req, 0x41727101980ull);
		put32(req, 0);  // connect
		put32(req, tid);
		std::string resp;
		if (udp_exchange(t, fd, req, resp, tid) && resp.size() >= 16 && be32(resp.data()) == 0) {
			std::string conn = resp.substr(8, 8);
			random_bytes(&tid, 4);
			uint32_t key;
			random_bytes(&key, 4);
			req.clear();
			req += conn;
			put32(req, 1);  // announce
			put32(req, tid);
			req.append(reinterpret_cast<const char*>(t->ih), 20);
			req += impl().peer_id;
			put64(req, 0);                       // downloaded
			put64(req, uint64_t(bytes_left(t)));  // left
			put64(req, 0);                       // uploaded
			put32(req, first ? 2 : 0);           // event: started / none
			put32(req, 0);                       // ip
			put32(req, key);
			put32(req, 200);  // num want
			int port = announce_port();
			req += char(port >> 8), req += char(port & 0xff);
			if (udp_exchange(t, fd, req, resp, tid) && resp.size() >= 20 && be32(resp.data()) == 1) {
				*interval = int(be32(resp.data() + 8));
				parse_compact(resp.substr(20), peers);
				ok = true;
			}
		}
	}
	close(fd);
	return ok;
}

// Asks the router (UPnP) to forward our port to the PS5, once per run, so
// peers behind their own routers can connect to us.
void map_port() {
#ifdef HAVE_UPNP
	Impl& I = impl();
	int port = I.listen_port;
	if (!port) return;
	int err = 0;
	UPNPDev* devs = upnpDiscover(2000, nullptr, nullptr, 0, 0, 2, &err);
	if (!devs) {
		dlog("upnp: no router answered (error %d); only outgoing connections", err);
		return;
	}
	UPNPUrls urls;
	IGDdatas data;
	char lan[64] = "";
	int r = UPNP_GetValidIGD(devs, &urls, &data, lan, sizeof(lan));
	freeUPNPDevlist(devs);
	if (r != 1) {
		dlog("upnp: no usable router (%d); only outgoing connections", r);
		if (r) FreeUPNPUrls(&urls);
		return;
	}
	std::string ps = std::to_string(port);
	int tcp = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype, ps.c_str(), ps.c_str(), lan,
	                              "Stremio Plus", "TCP", nullptr, "0");
	int udp = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype, ps.c_str(), ps.c_str(), lan,
	                              "Stremio Plus", "UDP", nullptr, "0");
	dlog("upnp: port %d forwarded to %s: TCP %s, UDP %s", port, lan, tcp == 0 ? "ok" : strupnperror(tcp),
	     udp == 0 ? "ok" : strupnperror(udp));
	if (tcp == 0) {
		std::lock_guard<std::mutex> lock(I.upnp_m);
		I.upnp_control = urls.controlURL;
		I.upnp_service = data.first.servicetype;
		I.mapped_port = port;
	}
	FreeUPNPUrls(&urls);
#endif
}

void unmap_port() {
#ifdef HAVE_UPNP
	Impl& I = impl();
	std::lock_guard<std::mutex> lock(I.upnp_m);
	int port = I.mapped_port.exchange(0);
	if (!port) return;
	std::string ps = std::to_string(port);
	UPNP_DeletePortMapping(I.upnp_control.c_str(), I.upnp_service.c_str(), ps.c_str(), "TCP", nullptr);
	UPNP_DeletePortMapping(I.upnp_control.c_str(), I.upnp_service.c_str(), ps.c_str(), "UDP", nullptr);
#endif
}

void tracker_worker(std::shared_ptr<Torrent> t, int worker) {
	Torrent* tp = t.get();
	// One-time set-up, beside the first announces: the router mapping first
	// (so the trackers get a port that works), the DHT's first nodes.
	if (worker == 0 && !impl().map_started.exchange(true)) map_port();
	if (worker == kTrackerThreads - 1 && !impl().boot_started.exchange(true)) resolve_bootstrap();
	int round = 0;
	while (!tp->stopped) {
		const uint64_t discovery = tp->discovery_generation.load();
		std::vector<std::string> list;
		{
			std::lock_guard<std::mutex> lock(tp->trk_m);
			list = tp->trackers;
		}
		int min_interval = 0;
		if (!tp->idle) {
			for (size_t i = size_t(worker); i < list.size() && !tp->stopped; i += kTrackerThreads) {
				std::vector<sockaddr_in> peers;
				int interval = 0;
				bool ok = starts_with(list[i], "udp://") ? announce_udp(tp, list[i], round == 0, peers, &interval)
				                                         : announce_http(tp, list[i], round == 0, peers, &interval);
				if (ok) {
					std::lock_guard<std::mutex> lock(tp->trk_m);
					tp->tracker_peers.insert(tp->tracker_peers.end(), peers.begin(), peers.end());
					if (interval > 0 && (min_interval == 0 || interval < min_interval)) min_interval = interval;
				}
				if (round == 0)
					dlog("tracker %s: %s, %zu peers", list[i].c_str(), ok ? "ok" : "no answer", peers.size());
			}
			round++;
		}
		// Next round: in a minute while peers are short, else as the tracker
		// asks (5 to 30 minutes).
		double start = now_seconds();
		while (!tp->stopped) {
			double waited = now_seconds() - start;
			double wait = tp->want_peers ? 60 : std::max(300, std::min(min_interval, 1800));
			if (waited >= wait) break;
			if (waited >= kStallRecoveryDelay && tp->discovery_generation.load() != discovery) break;
			if (!wait_stoppable(tp, 1)) break;
		}
	}
}

void ensure_running() {
	Impl& I = impl();
	if (I.running) return;
	I.running = true;
	I.quit = false;
    start_disk_worker_locked();
	char id[21];
	snprintf(id, sizeof(id), "-SP0100-");
	static const char* al = "0123456789abcdefghijklmnopqrstuvwxyz";
	uint8_t r[12];
	random_bytes(r, 12);
	std::string pid(id);
	for (int i = 0; i < 12; i++) pid += al[r[i] % 36];
	I.peer_id = pid;
	if (I.listen_fd < 0) open_listener();
	I.net = std::thread(net_loop);
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API

Engine& Engine::get() {
	static Engine e;
	return e;
}

void Engine::configure(const std::string& data_dir, int64_t cache_bytes) {
	if (!make_shared_dirs(data_dir + "/torrents"))
		dlog("torrent: cannot prepare metadata directory (errno %d)", errno);
	std::lock_guard<std::mutex> lock(impl().mu);
	impl().data_dir = data_dir;
	impl().cache_bytes = cache_bytes;
}

void Engine::set_speed_profile(SpeedProfile profile) {
	std::lock_guard<std::mutex> lock(impl().mu);
	if (profile != SpeedProfile::Balanced && profile != SpeedProfile::Fast && profile != SpeedProfile::UltraFast)
		profile = SpeedProfile::UltraFast;
	if (impl().speed_profile == profile) return;
	impl().speed_profile = profile;
	const auto limits = speed_limits();
	const double now = now_seconds();
	for (auto& torrent : impl().torrents)
		for (auto& peer : torrent->peers) {
			int wanted = std::min(peer->max_reqs, limits.requests);
			if (now >= peer->penalty_until) wanted = std::max(wanted, limits.floor);
			peer->max_reqs = std::min(wanted, peer->reqq);
		}
	dlog("torrent: profile=%d peers=%d request_floor=%d request_cap=%d active_MiB=%lld",
	     int(profile), limits.peers, limits.floor, limits.requests, (long long)(limits.active_bytes >> 20));
}

void Engine::shutdown() {
	Impl& I = impl();
	{
		std::lock_guard<std::mutex> lock(I.mu);
        if (!I.running && !I.disk_running) return;
		I.quit = true;
		for (auto& t : I.torrents) t->stopped = true;
	}
	I.cv.notify_all();
	if (I.net.joinable()) I.net.join();
    stop_disk_worker();
	unmap_port();
	std::lock_guard<std::mutex> lock(I.mu);
	I.running = false;
}

void Engine::start(const std::string& hex_in, const std::vector<std::string>& trackers_in) {
	Impl& I = impl();
	std::string hex = lower(hex_in);
	std::vector<std::string> trackers;
	for (auto x : trackers_in) {
		if (starts_with(x, "tracker:")) x = x.substr(8);
		if (starts_with(x, "dht:")) continue;
		if ((starts_with(x, "udp://") || starts_with(x, "http://") || starts_with(x, "https://")) &&
		    std::find(trackers.begin(), trackers.end(), x) == trackers.end())
			trackers.push_back(x);
	}
	for (auto* d : kDefaultTrackers)
		if (std::find(trackers.begin(), trackers.end(), d) == trackers.end()) trackers.push_back(d);

	std::lock_guard<std::mutex> lock(I.mu);
	ensure_running();
	if (auto t = find_locked(hex)) {
		std::lock_guard<std::mutex> tl(t->trk_m);
		for (auto& x : trackers)
			if (std::find(t->trackers.begin(), t->trackers.end(), x) == t->trackers.end()) t->trackers.push_back(x);
		t->started = now_seconds();  // fresh grace period before pausing
		return;
	}
	for (auto& t : I.torrents) t->stopped = true;  // one torrent at a time
	I.cv.notify_all();

	auto t = std::make_shared<Torrent>();
	t->hex = hex;
	if (!hex_to_bytes(hex, t->ih)) {
		dlog("torrent: bad info hash %s", hex.c_str());
		t->stopped = true;
		return;
	}
	t->started = now_seconds();
	t->trackers = trackers;
	// Metadata saved from an earlier time: no need to ask the peers.
	std::string saved;
	if (read_file(I.data_dir + "/torrents/" + hex + ".info", saved)) {
		unsigned char digest[20];
		SHA1(reinterpret_cast<const unsigned char*>(saved.data()), saved.size(), digest);
		if (memcmp(digest, t->ih, 20) == 0 && parse_info(t.get(), saved)) dlog("torrent %s: metadata from the cache", hex.c_str());
	}
	dlog("torrent %s: starting with %zu trackers", hex.c_str(), trackers.size());
	const auto limits = speed_limits();
	dlog("torrent: profile=%s peers=%d request_floor=%d request_cap=%d active_MiB=%lld",
	     I.speed_profile == SpeedProfile::UltraFast ? "ultra-fast" : I.speed_profile == SpeedProfile::Fast ? "fast" : "balanced",
	     limits.peers, limits.floor, limits.requests, (long long)(limits.active_bytes >> 20));
	I.torrents.push_back(t);
	for (int w = 0; w < kTrackerThreads; w++) t->tracker_threads.emplace_back(tracker_worker, t, w);
}

bool Engine::wait_metadata(const std::string& hex, std::vector<FileInfo>& files, const std::atomic<bool>* cancel,
                           double timeout_s, std::string* error) {
	Impl& I = impl();
	double until = now_seconds() + timeout_s;
	std::unique_lock<std::mutex> lock(I.mu);
	auto t = find_locked(lower(hex));
	if (!t) {
		if (error) *error = "the torrent was stopped";
		return false;
	}
	++t->metadata_waiters;
	struct MetadataWait {
		Torrent* torrent;
		~MetadataWait() { --torrent->metadata_waiters; }
	} waiting{t.get()};
	for (;;) {
		if (t->stopped) {
			if (error) *error = "the torrent was stopped";
			return false;
		}
		if (t->has_meta) {
			files = t->files;
			return true;
		}
		if (cancel && cancel->load()) {
			if (error) *error = "cancelled";
			return false;
		}
		if (now_seconds() >= until) {
			if (error) *error = "timed out";
			return false;
		}
		I.cv.wait_for(lock, std::chrono::milliseconds(200));
	}
}

void Engine::select_file(const std::string& hex, int file_idx) {
	std::lock_guard<std::mutex> lock(impl().mu);
	if (auto t = find_locked(lower(hex))) {
		if (file_idx >= 0 && size_t(file_idx) < t->files.size()) t->file = file_idx;
		t->wanted_dirty = true;
	}
}

Stats Engine::stats(const std::string& hex) {
	Stats s;
	std::lock_guard<std::mutex> lock(impl().mu);
	auto t = find_locked(lower(hex));
	if (!t) return s;
	s.found = true;
	s.has_metadata = t->has_meta;
	if (t->has_meta) s.seeders = 0;
	for (auto& p : t->peers)
		if (!p->dead && p->state == Peer::Active) {
			s.peers++;
			if (t->has_meta && t->npieces > 0 && p->has_count == t->npieces) ++s.seeders;
		}
	s.known_peers = int(t->known.size());
	s.incoming = impl().mapped_port.load() > 0;
	s.download_rate = t->rate;
	if (t->has_meta && t->file >= 0) {
		const FileInfo& f = t->files[size_t(t->file)];
		if (f.size > 0) {
			int first = int(f.offset / t->plen), last = int((f.offset + f.size - 1) / t->plen);
			int got = 0;
			for (int p = first; p <= last; p++) got += t->ever[size_t(p)];
			s.file_progress = double(got) / double(last - first + 1);
		}
	}
	return s;
}

std::shared_ptr<Torrent> Engine::open_reader(const std::string& hex, int file_idx, int* reader_id, int64_t* file_size,
                                             std::string* error, ReaderRole role) {
	std::lock_guard<std::mutex> lock(impl().mu);
	auto t = find_locked(lower(hex));
	if (!t || !t->has_meta) {
		if (error) *error = t ? "no metadata yet" : "the torrent isn't running";
		return nullptr;
	}
	if (file_idx < 0 || size_t(file_idx) >= t->files.size()) {
		if (error) *error = "no such file in the torrent";
		return nullptr;
	}
	if (role == ReaderRole::Playback) t->file = file_idx;
	int id = t->next_reader++;
	t->readers[id] = t->files[size_t(file_idx)].offset;
	t->reader_files[id] = file_idx;
	if (role == ReaderRole::Auxiliary) t->auxiliary_readers.insert(id);
	if (role == ReaderRole::Download) { t->download_readers.insert(id); t->used_download_memory = true; }
	t->wanted_dirty = true;
	t->idle = false;
	*reader_id = id;
	*file_size = t->files[size_t(file_idx)].size;
	const char* role_name = role == ReaderRole::Playback ? "playback" : role == ReaderRole::Auxiliary ? "auxiliary" : "download";
	dlog("torrent: reader %d opened, role=%s, file=%d, size=%lld, selected=%d",
	     id, role_name, file_idx, (long long)*file_size, t->file);
	return t;
}

void Engine::close_reader(const std::shared_ptr<Torrent>& t, int reader_id) {
	std::lock_guard<std::mutex> lock(impl().mu);
	t->readers.erase(reader_id);
	t->waiting_readers.erase(reader_id);
	t->reader_files.erase(reader_id);
	t->auxiliary_readers.erase(reader_id);
	t->download_readers.erase(reader_id);
	t->last_reader = now_seconds();
	t->wanted_dirty = true;
    impl().cv.notify_all();
}

int Engine::read(const std::shared_ptr<Torrent>& t, int reader_id, int file_idx, int64_t pos, uint8_t* buf, int n,
                 const std::atomic<bool>* abort) {
    Impl& I = impl();
    std::unique_lock<std::mutex> lock(I.mu);
    if (!t || file_idx < 0 || size_t(file_idx) >= t->files.size() || pos < 0) return -1;
    const FileInfo f = t->files[size_t(file_idx)];
    if (pos >= f.size || n <= 0) return 0;
    const int64_t absolute = f.offset + pos;
    auto reader = t->readers.find(reader_id);
    if (reader == t->readers.end()) return -1;
    if (reader->second != absolute) {
        if (absolute / t->plen != reader->second / t->plen) t->wanted_dirty = true;
        reader->second = absolute;
        I.cv.notify_all();
    }
    const int piece = int(absolute / t->plen);
    struct ReadWait {
        Torrent* torrent;
        int reader;
        ~ReadWait() { torrent->waiting_readers.erase(reader); }
    } waiting{t.get(), reader_id};
    if (!t->have[size_t(piece)])
        t->waiting_readers[reader_id] = {piece, now_seconds(), abort};
    while (!t->have[size_t(piece)]) {
        if (t->stopped) return -2;
        if (abort && abort->load()) return -1;
        if (!t->readers.count(reader_id)) return -1;
        I.cv.wait_for(lock, std::chrono::milliseconds(100));
    }
    t->waiting_readers.erase(reader_id);
    if (t->stopped) return -2;
    if (abort && abort->load()) return -1;
    ensure_slot_state(t.get());
    const int64_t in_piece = absolute - int64_t(piece) * t->plen;
    const int count = int(std::min<int64_t>({int64_t(n), t->piece_size(piece) - in_piece, f.size - pos}));
    const int slot = t->slot_of[size_t(piece)];
    if (slot < 0 || size_t(slot) >= t->slot_pins.size() || t->slot_busy[size_t(slot)]) return -1;
    ++t->slot_pins[size_t(slot)];
    t->slot_touch[size_t(slot)] = now_seconds();
    const uint64_t epoch = t->cache_epoch;
    std::shared_ptr<VerifiedPiece> cached = t->hot_piece == piece ? t->hot_verified : nullptr;
    if (!cached) cached = t->ram[size_t(slot)];
    const int fd = t->cache && !t->cache_failed ? fileno(t->cache) : -1;
    const double started = now_seconds();
    lock.unlock();
    bool ok = false;
    if (cached && in_piece + count <= int64_t(cached->data.size())) {
        memcpy(buf, cached->data.data() + in_piece, size_t(count));
        ok = true;
    } else if (fd >= 0) {
        ok = positional_read(fd, int64_t(slot) * t->plen + in_piece, buf, size_t(count));
    }
    const double read_ms = (now_seconds() - started) * 1000;
    lock.lock();
    --t->slot_pins[size_t(slot)];
    I.cv.notify_all();
    t->cache_read_ms += read_ms;
    if (ok) {
        if (cached) t->cache_ram_read_bytes += count;
        else t->cache_disk_read_bytes += count;
    }
    if (t->stopped) return -2;
    if (abort && abort->load()) return -1;
    if (!t->readers.count(reader_id)) return -1;
    if (epoch != t->cache_epoch && !cached) return -1;
    if (!ok) {
        dlog("torrent %s: cache read failed for piece %d", t->hex.c_str(), piece);
        if (t->slot_of[size_t(piece)] == slot && t->piece_in_slot[size_t(slot)] == piece)
            forget_slot_locked(t.get(), slot);
        return -1;
    }
    return count;
}

int Engine::guess_file(const std::vector<FileInfo>& files, int season, int episode) {
	int best = -1;
	if (season >= 0 && episode >= 0) {
		char pats[4][32];
		snprintf(pats[0], 32, "s%02de%02d", season, episode);
		snprintf(pats[1], 32, "s%de%02d", season, episode);
		snprintf(pats[2], 32, "%dx%02d", season, episode);
		snprintf(pats[3], 32, "s%02d.e%02d", season, episode);
		for (size_t i = 0; i < files.size(); i++) {
			if (!is_video_name(files[i].path)) continue;
			std::string name = lower(files[i].path);
			size_t sl = name.rfind('/');
			if (sl != std::string::npos) name = name.substr(sl + 1);
			for (auto& p : pats) {
				size_t at = name.find(p);
				// "s01e01" must not match "s01e010"
				if (at != std::string::npos && !isdigit(uint8_t(name[std::min(name.size() - 1, at + strlen(p))]))) {
					if (best < 0 || files[i].size > files[size_t(best)].size) best = int(i);
					break;
				}
			}
		}
		if (best >= 0) return best;
	}
	for (size_t i = 0; i < files.size(); i++)
		if (is_video_name(files[i].path) && (best < 0 || files[i].size > files[size_t(best)].size)) best = int(i);
	if (best >= 0) return best;
	for (size_t i = 0; i < files.size(); i++)
		if (best < 0 || files[i].size > files[size_t(best)].size) best = int(i);
	return best;
}

}  // namespace bt

#ifdef HAVE_DHT
// What the dht library asks of its user.
extern "C" {
int dht_sendto(int sockfd, const void* buf, int len, int flags, const struct sockaddr* to, int tolen) {
	return int(sendto(sockfd, buf, size_t(len), flags, to, socklen_t(tolen)));
}
int dht_blacklisted(const struct sockaddr* sa, int salen) {
	(void)sa;
	(void)salen;
	return 0;
}
void dht_hash(void* hash_return, int hash_size, const void* v1, int len1, const void* v2, int len2, const void* v3,
              int len3) {
	std::string all;
	if (v1) all.append(static_cast<const char*>(v1), size_t(len1));
	if (v2) all.append(static_cast<const char*>(v2), size_t(len2));
	if (v3) all.append(static_cast<const char*>(v3), size_t(len3));
	unsigned char d[20];
	SHA1(reinterpret_cast<const unsigned char*>(all.data()), all.size(), d);
	memset(hash_return, 0, size_t(hash_size));
	memcpy(hash_return, d, size_t(std::min(hash_size, 20)));
}
int dht_random_bytes(void* buf, size_t size) {
	bt::random_bytes(buf, size);
	return int(size);
}
}
#endif
