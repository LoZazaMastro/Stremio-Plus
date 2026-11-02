// White-box test of the production scheduler, without starting its network.
// Including this implementation makes its internal Torrent/refresh_wanted
// available in one test translation unit; the algorithm is not reimplemented.
#include "../src/torrent/engine.cpp"

#include <iostream>
#include <future>
#include <stdexcept>

namespace {
int checks = 0;
void expect(bool condition, const char* message) {
	++checks;
	if (!condition) throw std::runtime_error(message);
}
bool wanted(const bt::Torrent& torrent, int piece) {
	return std::find(torrent.wanted.begin(), torrent.wanted.end(), piece) != torrent.wanted.end();
}
std::shared_ptr<bt::Torrent> fixture(bool subtitle_first = false, int64_t video_size = 4 * 1024 * 1024,
                                     int64_t piece_length = 16384) {
	auto torrent = std::make_shared<bt::Torrent>();
	torrent->hex = subtitle_first ? "1123456789abcdef0123456789abcdef01234567" : "0123456789abcdef0123456789abcdef01234567";
	torrent->has_meta = true;
	torrent->plen = piece_length;
	if (subtitle_first)
		torrent->files = {{"ita.srt", 96 * 1024, 0}, {"video.mkv", video_size, 96 * 1024},
		                  {"other.bin", 1024 * 1024, video_size + 96 * 1024}};
	else
		torrent->files = {{"video.mkv", video_size, 0}, {"ita.srt", 96 * 1024, video_size},
		                  {"other.bin", 1024 * 1024, video_size + 96 * 1024}};
	torrent->total = torrent->files.back().offset + torrent->files.back().size;
	torrent->npieces = int((torrent->total + torrent->plen - 1) / torrent->plen);
	torrent->have.resize(size_t(torrent->npieces));
	torrent->ever.resize(size_t(torrent->npieces));
	torrent->slot_of.assign(size_t(torrent->npieces), -1);
	torrent->nslots = 64;
	torrent->file = subtitle_first ? 1 : 0;
	bt::impl().torrents.push_back(torrent);
	return torrent;
}
}  // namespace

int main() {
	try {
		auto& engine = bt::Engine::get();
		expect(!bt::impl().running, "scheduler fixtures must not start the torrent network");
		{
			auto blocked = fixture();
			blocked->hex = "f123456789abcdef0123456789abcdef01234567";
			int old_reader = -1; int64_t bytes_total = 0; std::string failure;
			expect(bool(engine.open_reader(blocked->hex, 0, &old_reader, &bytes_total, &failure)),
			       "seek regression opens the original playback reader");
			std::atomic<bool> abort_old{false}; uint8_t result[8]{};
			auto reading = std::async(std::launch::async, [&] {
				return engine.read(blocked, old_reader, 0, 0, result, 8, &abort_old);
			});
			expect(reading.wait_for(std::chrono::milliseconds(70)) == std::future_status::timeout,
			       "old seek location genuinely blocks without its torrent piece");
			bool registered_wait = false;
			{
				std::lock_guard<std::mutex> lock(bt::impl().mu);
				registered_wait = blocked->waiting_readers.count(old_reader) == 1;
			}
			const auto cancelled = std::chrono::steady_clock::now();
			abort_old = true;
			expect(registered_wait, "blocked operation entered the real production missing-piece wait");
			expect(reading.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready && reading.get() == -1,
			       "seek cancellation interrupts the real blocked reader within 500 ms");
			expect(std::chrono::steady_clock::now() - cancelled < std::chrono::milliseconds(500),
			       "cancellation does not wait for peers or the old requested piece");
			engine.close_reader(blocked, old_reader);
			expect(blocked->readers.empty() && blocked->waiting_readers.empty(),
			       "retiring a seek removes the obsolete demand and reader");
			blocked->nslots = 1; blocked->slots_used = 1;
			blocked->piece_in_slot = {3}; blocked->slot_touch = {0};
			blocked->slot_of[3] = 0; blocked->have[3] = 1;
			blocked->ram = {std::make_shared<bt::VerifiedPiece>()};
			blocked->ram[0]->data.assign(size_t(blocked->plen), 'N');
			int latest_reader = -1;
			expect(bool(engine.open_reader(blocked->hex, 0, &latest_reader, &bytes_total, &failure)) && latest_reader != old_reader,
			       "the latest seek keeps the same torrent and selected file with a fresh reader");
			std::atomic<bool> abort_new{false};
			expect(engine.read(blocked, latest_reader, 0, 3 * blocked->plen, result, 8, &abort_new) == 8 && result[0] == 'N',
			       "latest seek reads the existing cache immediately without the old demand");
			engine.close_reader(blocked, latest_reader);
			bt::impl().torrents.clear();
		}
		// A new download can be the only reader: no player has selected a
		// file yet. This is the cold-start case that previously left wanted
		// and peer requests empty forever although metadata/peers existed.
		auto standalone = fixture();
		standalone->file = -1;
		int download_reader = -1;
		int64_t download_size = 0;
		std::string download_error;
		expect(bool(engine.open_reader(standalone->hex, 0, &download_reader, &download_size, &download_error,
		                               bt::ReaderRole::Download)) && standalone->file == -1,
		       "standalone download opens without a selected playback file");
		bt::refresh_wanted(standalone.get());
		expect(wanted(*standalone, 0) && wanted(*standalone, 8) && standalone->wanted.size() > 4 &&
		       standalone->download_readers.count(download_reader) == 1 &&
		       standalone->auxiliary_readers.count(download_reader) == 0,
		       "standalone download schedules full sequential read-ahead beyond the auxiliary 64-KiB window");
		auto standalone_peer = std::make_unique<bt::Peer>();
		standalone_peer->state = bt::Peer::Active;
		standalone_peer->peer_choking = false;
		standalone_peer->has.assign(size_t(standalone->npieces), 1);
		standalone_peer->max_reqs = 12;
		standalone->peers.push_back(std::move(standalone_peer));
		bt::pick(standalone.get(), standalone->peers[0].get(), 1);
		expect(standalone->peers[0]->reqs.size() == 12 && !standalone->active.empty(),
		       "real picker requests download blocks before any movie has been played");
		std::atomic<bool> already_cancelled{true};
		uint8_t resume_byte = 0;
		expect(engine.read(standalone, download_reader, 0, 2 * 1024 * 1024, &resume_byte, 1, &already_cancelled) == -1,
		       "standalone download reader moves to its resumed byte offset");
		bt::refresh_wanted(standalone.get());
		expect(wanted(*standalone, 128) && !wanted(*standalone, 0) && standalone->file == -1,
		       "resumed download schedules its current piece instead of the file start");
		bt::pick(standalone.get(), standalone->peers[0].get(), 2);
		engine.close_reader(standalone, download_reader);
		bt::refresh_wanted(standalone.get());
		expect(standalone->wanted.empty() && standalone->active.empty() && standalone->peers[0]->reqs.empty() &&
		       standalone->download_readers.empty() && standalone->reader_files.empty(),
		       "closing the last download removes its requests without inventing a playback selection");
		int auxiliary_only_reader = -1;
		expect(bool(engine.open_reader(standalone->hex, 0, &auxiliary_only_reader, &download_size, &download_error,
		                               bt::ReaderRole::Auxiliary)), "standalone auxiliary reader also opens independently");
		bt::refresh_wanted(standalone.get());
		expect(standalone->wanted.size() == 4 && wanted(*standalone, 0) && !wanted(*standalone, 4) && standalone->file == -1,
		       "auxiliary-only work keeps its bounded window and does not require video selection");
		engine.close_reader(standalone, auxiliary_only_reader);
		bt::impl().torrents.clear();

		auto torrent = fixture();
		int video_reader = -1, subtitle_reader = -1;
		int64_t size = 0;
		std::string error;
		expect(bool(engine.open_reader(torrent->hex, 0, &video_reader, &size, &error)) && size == 4 * 1024 * 1024,
		       "normal video reader opens and reports its file size");
		expect(bool(engine.open_reader(torrent->hex, 1, &subtitle_reader, &size, &error, bt::ReaderRole::Auxiliary)) && size == 96 * 1024 && torrent->file == 0,
		       "auxiliary subtitle reader reports its own size without selecting a new video");
		int concurrent_download_reader = -1;
		expect(bool(engine.open_reader(torrent->hex, 2, &concurrent_download_reader, &size, &error,
		                               bt::ReaderRole::Download)) && torrent->file == 0,
		       "background download preserves an existing playback file selection");
		bt::refresh_wanted(torrent.get());
		const int concurrent_piece = int(torrent->files[2].offset / torrent->plen);
		expect(wanted(*torrent, 0) && wanted(*torrent, concurrent_piece) && wanted(*torrent, concurrent_piece + 8),
		       "playback and background download receive their own full read-ahead windows");
		engine.close_reader(torrent, concurrent_download_reader);
		bt::refresh_wanted(torrent.get());
		const int first_subtitle_piece = int(torrent->files[1].offset / torrent->plen);
		expect(wanted(*torrent, 0) && wanted(*torrent, first_subtitle_piece),
		       "video and subtitle pieces beyond the video file bounds are scheduled concurrently");
		expect(wanted(*torrent, first_subtitle_piece + 3) && !wanted(*torrent, first_subtitle_piece + 4),
		       "auxiliary reader initially asks only for its bounded 64-KiB window");
		expect(torrent->wanted.size() > 6 && torrent->wanted[0] == 0 && torrent->wanted[1] == 1 &&
		       torrent->wanted[2] == first_subtitle_piece && torrent->wanted[5] == first_subtitle_piece + 3 &&
		       torrent->wanted[6] == 2,
		       "urgent video pieces and the complete auxiliary window precede bulk video read-ahead");
		bt::Peer first_peer;
		first_peer.state = bt::Peer::Active;
		first_peer.peer_choking = false;
		first_peer.has.assign(size_t(torrent->npieces), 1);
		first_peer.max_reqs = 7;
		bt::pick(torrent.get(), &first_peer, 1);
		expect(first_peer.reqs.size() == 7 && first_peer.reqs[0].piece == 0 && first_peer.reqs[1].piece == 1 &&
		       first_peer.reqs[2].piece == uint32_t(first_subtitle_piece) &&
		       first_peer.reqs[5].piece == uint32_t(first_subtitle_piece + 3) && first_peer.reqs[6].piece == 2,
		       "real peer picker requests urgent video and subtitle blocks before filling the bulk pipeline");
		bt::release_all(torrent.get(), &first_peer);
		torrent->active.clear();

		// A subtitle/hash reader must not pin the main player's large read-ahead
		// over neighbouring files and evict the actual video's next pieces.
		torrent->nslots = torrent->slots_used = 2;
		torrent->piece_in_slot = {first_subtitle_piece + 4, 1};
		torrent->slot_touch = {100, 0};
		torrent->slot_of[size_t(first_subtitle_piece + 4)] = 0;
		torrent->slot_of[1] = 1;
		torrent->have[size_t(first_subtitle_piece + 4)] = torrent->have[1] = 1;
		expect(bt::take_slot(torrent.get(), 200) == 0, "cache eviction honors auxiliary window and retains upcoming video data");

		// Put a synthetic piece into the in-memory cache. The real read() moves
		// the subtitle reader, then the real scheduler follows its own file.
		torrent->ram = {std::make_shared<bt::VerifiedPiece>(), std::make_shared<bt::VerifiedPiece>()};
		torrent->ram[0]->data = std::string(size_t(torrent->plen), 'Q');
		torrent->ram[1]->data = std::string(size_t(torrent->plen), 'V');
		torrent->have[size_t(first_subtitle_piece + 4)] = 1;
		torrent->slot_of[size_t(first_subtitle_piece + 4)] = 0;
		uint8_t bytes[8] = {};
		std::atomic<bool> cancel{false};
		expect(engine.read(torrent, subtitle_reader, 1, 65536, bytes, 8, &cancel) == 8 && bytes[0] == 'Q' && torrent->file == 0,
		       "auxiliary read uses the subtitle file and leaves playback selection intact");
		bt::refresh_wanted(torrent.get());
		expect(wanted(*torrent, first_subtitle_piece + 5) && !wanted(*torrent, first_subtitle_piece + 6),
		       "advanced subtitle reader requests its tail but does not run into the following file");
		cancel = true;
		expect(engine.read(torrent, subtitle_reader, 1, 0, bytes, 8, &cancel) == -1,
		       "waiting auxiliary read obeys cancellation without needing peers");
		engine.close_reader(torrent, subtitle_reader);
		bt::refresh_wanted(torrent.get());
		expect(torrent->reader_files.count(subtitle_reader) == 0 && torrent->auxiliary_readers.count(subtitle_reader) == 0 &&
		       !wanted(*torrent, first_subtitle_piece + 5), "closing auxiliary reader removes its file/window and requests");
		int other_reader = -1;
		expect(bool(engine.open_reader(torrent->hex, 2, &other_reader, &size, &error)) && torrent->file == 2,
		       "existing open_reader callers retain default playback-selection behavior");
		engine.close_reader(torrent, other_reader);
		engine.close_reader(torrent, video_reader);

		auto before = fixture(true);
		expect(bool(engine.open_reader(before->hex, 1, &video_reader, &size, &error)) &&
		       bool(engine.open_reader(before->hex, 0, &subtitle_reader, &size, &error, bt::ReaderRole::Auxiliary)),
		       "subtitle-before-video file ordering opens independent readers");
		bt::refresh_wanted(before.get());
		expect(wanted(*before, 0) && wanted(*before, 6) && !wanted(*before, 4) && before->file == 1,
		       "subtitle located before the selected video is not clipped to the video's starting offset");
		engine.close_reader(before, subtitle_reader);
		engine.close_reader(before, video_reader);
		bt::impl().torrents.clear();

		// A late hash/subtitle reader must bypass gigabytes of wanted video,
		// including a full active-piece budget left by bulk read-ahead.
		auto large = fixture(false, 4ll << 30, 1ll << 20);
		large->nslots = 4096;  // 2 GiB of read-ahead; sparse/empty cache file, no payload allocation.
		large->cache = tmpfile();
		expect(large->cache != nullptr, "large disk-window scenario uses the actual file-cache backend");
		int hash_reader = -1;
		expect(bool(engine.open_reader(large->hex, 0, &video_reader, &size, &error)) &&
		       bool(engine.open_reader(large->hex, 1, &subtitle_reader, &size, &error, bt::ReaderRole::Auxiliary)) &&
		       bool(engine.open_reader(large->hex, 0, &hash_reader, &size, &error, bt::ReaderRole::Auxiliary)),
		       "large-video fixture opens simultaneous playback, subtitle and hash readers");
		cancel = true;
		expect(engine.read(large, hash_reader, 0, large->files[0].size - 65536, bytes, 8, &cancel) == -1,
		       "real hash reader moves to the video tail without waiting for a peer");
		bt::refresh_wanted(large.get());
		const int tail_piece = int((large->files[0].size - 1) / large->plen);
		const int subtitle_piece = int(large->files[1].offset / large->plen);
		expect(large->wanted.size() > 2000 && large->wanted[0] == 0 && large->wanted[1] == 1 &&
		       large->wanted[2] == tail_piece && large->wanted[3] == subtitle_piece && large->wanted[4] == 2,
		       "hash tail and subtitle window retain urgent priority with 2 GiB of bulk read-ahead");
		const int64_t already_reserved = bt::payload_memory_bytes.load();
		const size_t active_limit = size_t((bt::kMaxActiveBytes - already_reserved - 2 * large->plen) / large->plen);
		for (size_t i = 0; i < active_limit; ++i) {
			int piece = int(i) + 2;
			auto& pending = large->active[piece];
			// Reserve the same bytes as production; no network payload is required.
            pending.memory = std::make_shared<bt::PayloadReservation>(large->plen);
			pending.got.assign(size_t(large->blocks(piece)), 0);
			pending.asked.assign(size_t(large->blocks(piece)), 0);
		}
		bt::Peer hash_peer;
		hash_peer.state = bt::Peer::Active;
		hash_peer.peer_choking = false;
		hash_peer.has.assign(size_t(large->npieces), 0);
		hash_peer.has[size_t(tail_piece)] = hash_peer.has[2] = 1;
		hash_peer.max_reqs = 1;
		bt::pick(large.get(), &hash_peer, 2);
		expect(hash_peer.reqs.size() == 1 && hash_peer.reqs[0].piece == uint32_t(tail_piece) &&
		       large->active.size() == active_limit + 1,
		       "real picker uses reserved urgent capacity without exceeding the aggregate payload budget");
		bt::Peer subtitle_peer;
		subtitle_peer.state = bt::Peer::Active;
		subtitle_peer.peer_choking = false;
		subtitle_peer.has.assign(size_t(large->npieces), 0);
		subtitle_peer.has[size_t(subtitle_piece)] = subtitle_peer.has[2] = 1;
		subtitle_peer.max_reqs = 1;
		bt::pick(large.get(), &subtitle_peer, 2);
		expect(subtitle_peer.reqs.size() == 1 && subtitle_peer.reqs[0].piece == uint32_t(subtitle_piece) &&
		       large->active.size() == active_limit + 2,
		       "a second urgent auxiliary reader is also scheduled ahead of already-active bulk video");
		bt::Peer bulk_peer;
		bulk_peer.state = bt::Peer::Active;
		bulk_peer.peer_choking = false;
		bulk_peer.has.assign(size_t(large->npieces), 0);
		bulk_peer.has[active_limit + 2] = 1;
		bulk_peer.max_reqs = 1;
		bt::pick(large.get(), &bulk_peer, 2);
		expect(bulk_peer.reqs.empty() && large->active.size() == active_limit + 2,
		       "reserved urgent capacity cannot be consumed by additional bulk pieces");
		bt::release_all(large.get(), &hash_peer);
		bt::release_all(large.get(), &subtitle_peer);
		engine.close_reader(large, hash_reader);
		engine.close_reader(large, subtitle_reader);
		bt::refresh_wanted(large.get());
		expect(!wanted(*large, tail_piece) && !wanted(*large, subtitle_piece) &&
		       large->wanted[0] == 0 && large->wanted[1] == 1 && large->wanted[2] == 2 &&
		       large->active.count(tail_piece) == 0 && large->active.count(subtitle_piece) == 0,
		       "closing auxiliary readers restores sequential video priority and releases their active pieces");
		engine.close_reader(large, video_reader);
		bt::impl().torrents.clear();
		expect(!bt::impl().running, "all reader and scheduler checks remained offline");
		std::cout << "PASS: " << checks << " native torrent auxiliary-reader assertions\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "FAIL after " << checks << " assertions: " << error.what() << '\n';
		return 1;
	}
}
