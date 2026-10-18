// Request refill follows a complete receive batch. The real parser, picker,
// request ledger and SHA-1 worker are exercised with deterministic wire frames.
#define main tcp_benchmark_unused_main
#include "test_torrent_network.cpp"
#undef main

namespace {
int checks = 0;
void check(bool value, const char* message) { ++checks; require(value, message); }

struct Fixture {
	std::shared_ptr<bt::Torrent> torrent = std::make_shared<bt::Torrent>();
	std::string payload = std::string(4u << 20, 'B');
	bt::Peer* peer = nullptr;
	int reader_id = -1;
	Fixture(const fs::path& directory, int requests = 32) {
		BValue info = BValue::dict();
		info.d["name"] = BValue(std::string("receive-batch.mp4"));
		info.d["length"] = BValue(int64_t(payload.size()));
		info.d["piece length"] = BValue(int64_t(1u << 20));
		std::string hashes;
		for (size_t off = 0; off < payload.size(); off += 1u << 20)
			hashes += digest(payload.substr(off, 1u << 20));
		info.d["pieces"] = BValue(hashes);
		torrent->hex = hex(digest(bencode(info)));
		auto& impl = bt::impl(); impl.data_dir = directory.string(); impl.cache_bytes = 8ll << 20;
		check(bt::parse_info(torrent.get(), bencode(info)), "parse actual torrent metadata");
		impl.torrents.push_back(torrent);
		int64_t file_size = 0; std::string error;
		auto reader = bt::Engine::get().open_reader(torrent->hex, 0, &reader_id, &file_size, &error, bt::ReaderRole::Download);
		check(reader && file_size == int64_t(payload.size()), "open actual bounded download reader");
		auto p = std::make_unique<bt::Peer>();
		p->state = bt::Peer::Active; p->peer_choking = false;
		p->has.assign(size_t(torrent->npieces), 1); p->has_count = torrent->npieces;
		p->max_reqs = p->reqq = requests;
		peer = p.get(); torrent->peers.push_back(std::move(p));
		bt::pick(torrent.get(), peer, 100);
		peer->out.clear();
	}
	std::string frames(int count) const {
		std::string wire;
		for (int i = 0; i < count; ++i) {
			const auto& request = peer->reqs.at(size_t(i));
			bt::put32(wire, request.len + 9); wire += char(7);
			bt::put32(wire, request.piece); bt::put32(wire, request.begin);
			wire.append(payload.data() + size_t(request.piece) * (1u << 20) + request.begin, request.len);
		}
		return wire;
	}
	void ledger() const {
		std::set<std::pair<uint32_t, uint32_t>> seen;
		for (const auto& request : peer->reqs) {
			check(seen.emplace(request.piece, request.begin).second, "one request for each block per peer");
			const auto& piece = torrent->active.at(int(request.piece));
			check(piece.asked.at(request.begin / bt::kBlock) == 1, "each outstanding request owns one reservation");
			check(!piece.got.at(request.begin / bt::kBlock), "verified received blocks are not requested again");
		}
		check(peer->reqs.size() <= size_t(std::min(peer->max_reqs, peer->reqq)), "remote and local queue bounds are preserved");
		check(bt::payload_memory_bytes <= bt::kMaxActiveBytes, "payload stays inside the existing memory limit");
	}
	~Fixture() {
		while (!bt::impl().disk_jobs.empty()) {
			auto job = std::move(bt::impl().disk_jobs.front()); bt::impl().disk_jobs.pop_front();
			bt::process_disk_job(std::move(job));
		}
		bt::Engine::get().close_reader(torrent, reader_id);
		bt::impl().torrents.clear();
	}
};
}

int main(int argc, char** argv) {
	try {
		require(argc == 2, "usage: receive-batch cache_dir");
		const fs::path directory = argv[1]; fs::create_directories(directory);
		{
			Fixture f(directory);
			const uint64_t before = f.torrent->picker_passes;
			f.peer->in = f.frames(16); bt::process_input(f.torrent.get(), f.peer, 100.1);
			check(f.torrent->picker_passes == before + 1, "sixteen replies refill with one picker pass");
			check(f.torrent->received_batches == 1, "one batch is accounted");
			check(f.torrent->unique_window == 16ll * bt::kBlock, "all sixteen reply payloads are accepted");
			check(f.peer->reqs.size() == 32, "the peer leaves the batch with a full request queue");
			f.ledger();
		}
		{
			Fixture f(directory);
			const auto wire = f.frames(2);
			const size_t split = wire.size() - 19;
			f.peer->in = wire.substr(0, split); bt::process_input(f.torrent.get(), f.peer, 100.1);
			check(f.torrent->unique_window == bt::kBlock, "a split message admits only its complete predecessor");
			check(f.peer->in.size() > f.peer->in_off, "incomplete message remains buffered");
			f.peer->in += wire.substr(split); bt::process_input(f.torrent.get(), f.peer, 100.2);
			check(f.torrent->unique_window == 2ll * bt::kBlock, "the next batch completes the exact pending message");
			check(f.peer->in.empty(), "complete stream leaves no retained input bytes");
			f.ledger();
		}
		{
			Fixture f(directory);
			const uint64_t before = f.torrent->picker_passes;
			f.peer->in = f.frames(8); bt::put32(f.peer->in, 1); f.peer->in += char(0);
			bt::process_input(f.torrent.get(), f.peer, 100.1);
			check(f.peer->peer_choking && f.peer->reqs.empty(), "choke at the batch end releases all request reservations");
			check(f.torrent->picker_passes == before, "choked peer is not refilled");
			for (const auto& piece : f.torrent->active)
				check(std::all_of(piece.second.asked.begin(), piece.second.asked.end(), [](uint8_t n) { return n == 0; }),
				      "choke does not leave blocks reserved");
		}
		{
			Fixture f(directory, 1);
			f.peer->in = f.frames(1); bt::process_input(f.torrent.get(), f.peer, 100.1);
			check(f.peer->reqs.size() == 1, "a one-request peer receives replacement work immediately");
			f.ledger();
		}
		{
			Fixture f(directory, 64);
			f.peer->in = f.frames(64); bt::process_input(f.torrent.get(), f.peer, 100.1);
			check(bt::impl().disk_jobs.size() == 1, "a completed piece enters the real verifier queue once");
			auto job = std::move(bt::impl().disk_jobs.front()); bt::impl().disk_jobs.pop_front();
			bt::process_disk_job(std::move(job));
			check(f.torrent->have[0] && f.torrent->verified_bytes == 1ll << 20, "SHA-1 verification publishes the exact complete piece");
			std::string actual(1u << 20, '\0');
			check(bt::Engine::get().read(f.torrent, f.reader_id, 0, 0, reinterpret_cast<uint8_t*>(actual.data()), int(actual.size()), nullptr) == int(actual.size()),
			      "verified batch is readable through the production download reader");
			check(actual == f.payload.substr(0, actual.size()), "download reader returns every expected byte");
			f.ledger();
		}
		{
			Fixture f(directory);
			const uint64_t before = f.torrent->picker_passes;
			f.peer->in.assign(256, '\0'); bt::process_input(f.torrent.get(), f.peer, 100.1);
			check(f.torrent->picker_passes == before && f.torrent->received_batches == 0, "keep-alives do not invoke the payload picker");
		}
		{
			Fixture f(directory);
			bt::Peer::Req answered{};
			for (int step = 1; step <= 20; ++step) {
				const double now = 100 + step * 5;
				answered = f.peer->reqs.front();
				bt::on_block(f.torrent.get(), f.peer, answered.piece, answered.begin,
				             f.payload.data() + size_t(answered.piece) * (1u << 20) + answered.begin, answered.len, now);
				f.peer->last_data = now;
				bt::upkeep(f.torrent.get(), now);
				check(f.torrent->timeout_cancels == 0 && f.peer->penalty_until == 0,
				      "ongoing useful delivery protects older queued requests from repeated timeout penalties");
			}
			check(std::any_of(f.peer->reqs.begin(), f.peer->reqs.end(), [](const auto& request) { return request.at == 100; }),
			      "tail requests may remain older than fifteen seconds while useful delivery continues");
			const auto outstanding = f.peer->reqs.size();
			// Old duplicate payload and transport keep-alives cannot claim that
			// this peer is still delivering useful data after its final block.
			bt::on_block(f.torrent.get(), f.peer, answered.piece, answered.begin,
			             f.payload.data() + size_t(answered.piece) * (1u << 20) + answered.begin, answered.len, 215);
			f.peer->in.assign(4, '\0'); bt::process_input(f.torrent.get(), f.peer, 215);
			check(f.peer->payload_at == 200, "late duplicate and keep-alive do not reset the useful payload deadline");
			bt::upkeep(f.torrent.get(), 216);
			check(f.torrent->timeout_cancels == outstanding && f.peer->reqs.empty(),
			      "sixteen seconds without useful data cancels stale requests exactly once");
			check(f.peer->penalty_until > 216, "a real stall still reduces the peer's request window");
			bt::upkeep(f.torrent.get(), 216.25);
			check(!f.peer->reqs.empty(), "the stalled peer can retry after other peers had a scheduling pass");
			f.ledger();
		}
		{
			Fixture f(directory);
			const auto request = f.peer->reqs.front();
			bt::on_block(f.torrent.get(), f.peer, request.piece, request.begin, f.payload.data(), 1, 114);
			check(f.peer->payload_at == 0, "malformed payload cannot postpone a request timeout");
			bt::upkeep(f.torrent.get(), 116);
			check(f.torrent->timeout_cancels == 31 && f.peer->reqs.empty(),
			      "never-productive peer releases its remaining stale reservations");
		}
		{
			Fixture f(directory, 128);
			const auto tail = f.peer->reqs.back();
			bt::on_block(f.torrent.get(), f.peer, tail.piece, tail.begin, f.payload.data() + tail.begin, tail.len, 116);
			f.peer->last_data = 116; bt::upkeep(f.torrent.get(), 116);
			check(f.torrent->timeout_cancels == 0, "tail delivery preserves the active peer's remaining requests");
			auto helper = std::make_unique<bt::Peer>();
			helper->state = bt::Peer::Active; helper->peer_choking = false; helper->max_reqs = helper->reqq = 1;
			helper->has.assign(size_t(f.torrent->npieces), 1); helper->has_count = f.torrent->npieces;
			auto* rescuer = helper.get(); f.torrent->peers.push_back(std::move(helper));
			bt::pick(f.torrent.get(), rescuer, 116.1);
			check(rescuer->reqs.size() == 1 && rescuer->reqs.front().piece == 0 && rescuer->reqs.front().begin == 0,
			      "recent tail delivery does not prevent another peer rescuing the missing head");
			check(f.torrent->active.at(0).asked[0] == 2, "head rescue preserves the two-owner reservation bound");
		}
		{
			Fixture f(directory, 128);
			const auto original_requests = f.peer->reqs;
			for (const auto& request : original_requests)
				if (request.piece == 0 && request.begin != 0)
					bt::on_block(f.torrent.get(), f.peer, request.piece, request.begin,
					             f.payload.data() + request.begin, request.len, 116);
			f.peer->last_data = 116; f.peer->useful_rate = 128 * 1024;
			f.peer->recent_useful_sample = 0; f.peer->useful_window = 0;
			auto second = std::make_unique<bt::Peer>();
			second->state = bt::Peer::Active; second->peer_choking = false; second->key = 2;
			second->has.assign(size_t(f.torrent->npieces), 1); second->has_count = f.torrent->npieces;
			second->max_reqs = second->reqq = 32; second->got_any = true;
			second->payload_at = second->last_data = 116; second->useful_rate = 128 * 1024;
			second->reqs.push_back(original_requests.front());
			++f.torrent->active.at(0).asked[0];
			auto* other_owner = second.get(); f.torrent->peers.push_back(std::move(second));
			f.torrent->rate_at = 116;
			bt::upkeep(f.torrent.get(), 116);
			check(f.torrent->timeout_cancels == 0, "both head owners keep their queues while their tails are productive");
			auto third = std::make_unique<bt::Peer>();
			third->state = bt::Peer::Active; third->peer_choking = false; third->key = 3;
			third->max_reqs = third->reqq = 1; third->got_any = true;
			third->payload_at = 116; third->useful_rate = 128 * 1024;
			third->has.assign(size_t(f.torrent->npieces), 0); third->has[0] = 1; third->has_count = 1;
			auto* rescuer = third.get(); f.torrent->peers.push_back(std::move(third));
			const auto primary_before = f.peer->reqs.size(), secondary_before = other_owner->reqs.size();
			bt::pick(f.torrent.get(), rescuer, 116.1);
			check(rescuer->reqs.size() == 1 && rescuer->reqs.front().piece == 0 && rescuer->reqs.front().begin == 0,
			      "a similarly fast third peer can rescue a head held by two productive owners beyond fifteen seconds");
			check(f.peer->reqs.size() + other_owner->reqs.size() + 1 == primary_before + secondary_before,
			      "atomic deadline handoff replaces exactly one owner without disturbing other queued blocks");
			check(f.torrent->active.at(0).asked[0] == 2 && f.torrent->request_head_handoffs == 1,
			      "deadline handoff retains two reservations and records its cause");
			bt::on_block(f.torrent.get(), rescuer, 0, 0, f.payload.data(), bt::kBlock, 116.2);
			check(bt::impl().disk_jobs.size() == 1, "the rescued head completes its piece");
			auto job = std::move(bt::impl().disk_jobs.front()); bt::impl().disk_jobs.pop_front();
			bt::process_disk_job(std::move(job));
			std::string actual(1u << 20, '\0');
			check(f.torrent->have[0] && bt::Engine::get().read(f.torrent, f.reader_id, 0, 0,
			      reinterpret_cast<uint8_t*>(actual.data()), int(actual.size()), nullptr) == int(actual.size()) &&
			      actual == f.payload.substr(0, actual.size()), "the rescued head is SHA-1 verified and readable without waiting for tail exhaustion");
		}
		check(bt::payload_memory_bytes == 0, "all payload reservations are released");
		std::cout << json({{"result", "PASS"}, {"checks", checks}, {"external_network_attempts", external_attempts.load()}}).dump() << '\n';
		return external_attempts ? 1 : 0;
	} catch (const std::exception& error) {
		std::cerr << "FAIL: " << error.what() << '\n'; return 1;
	}
}
