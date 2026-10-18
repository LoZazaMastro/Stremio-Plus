// Exercise request ownership and actual encoded CANCEL/REQUEST messages.
#define main tcp_benchmark_unused_main
#include "test_torrent_network.cpp"
#undef main

namespace {
int assertions = 0;
void expect(bool condition, const char* message) { ++assertions; require(condition, message); }

struct Requests {
	bt::Torrent torrent;
	bt::Peer *a, *b, *fast;
	static constexpr double now = 20;
	Requests() {
		torrent.has_meta = true; torrent.npieces = 1; torrent.plen = 16384; torrent.total = 16384;
		torrent.have.assign(1, 0); torrent.wanted = {0}; torrent.urgent_wanted = 1; torrent.wanted_dirty = false;
		torrent.readers[1] = 0; torrent.download_readers.insert(1); torrent.rate_at = now;
		bt::Piece piece; piece.data.resize(16384); piece.got = {0}; piece.asked = {2}; piece.asked_at = {now - 5};
		torrent.active.emplace(0, std::move(piece));
		for (int i = 0; i < 3; ++i) {
			auto peer = std::make_unique<bt::Peer>();
			peer->state = bt::Peer::Active; peer->peer_choking = false; peer->am_interested = true;
			peer->has.assign(1, 1); peer->has_count = 1; peer->max_reqs = 32; peer->reqq = 250;
			peer->key = uint64_t(i + 1); peer->got_any = true; peer->payload_at = now - 0.05;
			peer->last_data = now; peer->last_send = now;
			peer->useful_rate = i == 2 ? 4 * 1024 * 1024 : (i + 1) * 64 * 1024;
			if (i < 2) peer->reqs.push_back({0, 0, 16384, now - 5});
			torrent.peers.push_back(std::move(peer));
		}
		a = torrent.peers[0].get(); b = torrent.peers[1].get(); fast = torrent.peers[2].get();
	}
	bt::Piece& piece() { return torrent.active.at(0); }
	void ledger() {
		int owners = 0;
		for (const auto& peer : torrent.peers)
			for (const auto& request : peer->reqs) owners += request.piece == 0 && request.begin == 0;
		expect(owners == piece().asked[0], "request ownership equals reservation count");
		expect(owners <= 2, "never more than two active reservations");
	}
	void untouched() {
		expect(a->out.empty() && b->out.empty() && fast->out.empty(), "protected requests produce no cancel or extra request");
		expect(a->reqs.size() == 1 && b->reqs.size() == 1 && fast->reqs.empty(), "protected owners remain unchanged");
		ledger();
	}
	void resize_blocks(size_t blocks) {
		torrent.plen = torrent.total = int64_t(blocks * 16384);
		piece().data.assign(blocks * 16384, '\0'); piece().got.assign(blocks, 0);
		piece().asked.assign(blocks, 2); piece().asked_at.assign(blocks, now - 5);
		piece().next_unasked = blocks;
		a->reqs.clear(); b->reqs.clear();
		for (size_t i = 0; i < blocks; ++i) {
			a->reqs.push_back({0, uint32_t(i * 16384), 16384, now - 5});
			b->reqs.push_back({0, uint32_t(i * 16384), 16384, now - 5});
		}
	}
	void every_block_ledger() {
		for (size_t i = 0; i < piece().got.size(); ++i) {
			int owners = 0;
			for (const auto& peer : torrent.peers)
				for (const auto& request : peer->reqs) owners += request.piece == 0 && request.begin == i * 16384;
			expect(owners == piece().asked[i] && owners <= 2, "every block keeps exact ownership and maximum two copies");
		}
	}
};

bool one_wire_message(const std::string& wire, uint8_t id) {
	return wire.size() == 17 && bt::be32(wire.data()) == 13 && uint8_t(wire[4]) == id &&
	       bt::be32(wire.data() + 5) == 0 && bt::be32(wire.data() + 9) == 0 && bt::be32(wire.data() + 13) == 16384;
}

void run() {
	#ifdef BT_HEAD_HANDOFF
	{
		Requests f; f.resize_blocks(4);
		f.a->useful_rate = f.b->useful_rate = f.fast->useful_rate = 128 * 1024;
		for (auto* owner : {f.a, f.b}) for (auto& request : owner->reqs) request.at = f.now - 16;
		std::fill(f.piece().asked_at.begin(), f.piece().asked_at.end(), f.now - 16);
		const auto window_a = f.a->max_reqs, window_b = f.b->max_reqs;
		int room = 8; bt::pick_urgent_duplicates(&f.torrent, f.fast, f.now, room);
		expect(f.fast->reqs.size() == 1 && room == 7, "only one aged head owner is replaced per urgent picker pass");
		expect(f.torrent.request_head_handoffs == 1 && f.torrent.timeout_cancels == 0,
		       "deadline handoff is separate from a whole-peer stall timeout");
		expect(f.a->reqs.size() + f.b->reqs.size() == 7, "all other productive-owner requests remain in place");
		expect(f.a->max_reqs == window_a && f.b->max_reqs == window_b && f.a->penalty_until == 0 && f.b->penalty_until == 0,
		       "late head does not penalize productive tail delivery or reduce learned request windows");
		f.every_block_ledger();
	}
	{
		Requests f; f.torrent.download_readers.clear();
		f.a->useful_rate = f.b->useful_rate = f.fast->useful_rate = 128 * 1024;
		f.a->reqs[0].at = f.b->reqs[0].at = f.piece().asked_at[0] = f.now - 16;
		int room = 1; bt::pick_urgent_duplicates(&f.torrent, f.fast, f.now, room);
		expect(room == 0 && f.fast->reqs.size() == 1 && f.torrent.request_head_handoffs == 1,
		       "playback-only readers retain the deadline rescue for a head held by two productive peers");
		f.ledger();
	}
	{
		Requests f;
		f.a->useful_rate = f.b->useful_rate = f.fast->useful_rate = 128 * 1024;
		f.a->reqs[0].at = f.b->reqs[0].at = f.now - 14;
		int room = 1; bt::pick_urgent_duplicates(&f.torrent, f.fast, f.now, room);
		expect(room == 1, "moderate-speed replacement waits for the urgent request deadline");
		f.untouched();
		f.piece().asked_at[0] = f.now - 60;
		f.a->reqs[0].at = f.b->reqs[0].at = f.now - 1;
		bt::pick_urgent_duplicates(&f.torrent, f.fast, f.now, room);
		expect(room == 1, "an old aggregate block timestamp never expires fresh individual owners");
		f.untouched();
	}
	#endif
	{
		Requests f; int room = 1;
		bt::pick_urgent_duplicates(&f.torrent, f.fast, f.now, room);
		expect(room == 0 && f.fast->reqs.size() == 1, "proven fast peer receives overdue urgent block");
		expect(f.a->reqs.empty() && f.b->reqs.size() == 1, "only the slowest old owner relinquishes its reservation");
		expect(one_wire_message(f.a->out, 8), "slow owner receives exact protocol CANCEL");
		expect(one_wire_message(f.fast->out, 6), "fast peer receives exact protocol REQUEST");
		f.ledger();
	}
	{
		Requests f; int room = 1;
		bt::pick_download_stragglers(&f.torrent, f.fast, f.now, room);
		expect(room == 0 && one_wire_message(f.a->out, 8), "idle download capacity can transfer a bulk straggler");
		f.ledger();
	}
	for (int protection = 0; protection < 6; ++protection) {
		Requests f; int room = 1;
		switch (protection) {
		case 0: f.a->reqs[0].at = f.b->reqs[0].at = f.now - 0.5; break;
		case 1: f.fast->useful_rate = 127 * 1024; break;
		case 2: f.fast->useful_rate = 255 * 1024; break; // less than 4x even slowest
		case 3: f.fast->got_any = false; break;
		case 4: f.fast->payload_at = f.now - 16; break;
		case 5: f.torrent.download_readers.clear(); break;
		}
		bt::pick_urgent_duplicates(&f.torrent, f.fast, f.now, room);
		expect(room == 1, "fresh, unproven, insufficiently faster or playback-only peer is protected");
		f.untouched();
	}
	{
		Requests f; int room = 1;
		f.b->reqs[0].at = f.now - 0.2;
		bt::pick_urgent_duplicates(&f.torrent, f.fast, f.now, room);
		expect(f.b->reqs.size() == 1 && f.b->out.empty() && f.a->reqs.empty(), "fresh secondary owner is preserved while old slower owner is transferred");
		f.ledger();
	}
	{
		Requests f; int room = 1;
		f.fast->useful_rate = 256 * 1024;
		bt::pick_urgent_duplicates(&f.torrent, f.fast, f.now, room);
		expect(room == 0, "exactly four times the old owner's useful rate is sufficient");
		f.ledger();
	}
	{
		Requests f; int room = 1;
		f.fast->reqs = std::move(f.b->reqs);
		bt::pick_urgent_duplicates(&f.torrent, f.fast, f.now, room);
		expect(room == 1 && f.a->out.empty() && f.fast->out.empty(), "a peer that already owns the block never requests it again");
		f.ledger();
	}
	{
		Requests f; int room = 1;
		f.b->reqs.clear(); f.piece().asked[0] = 1; f.fast->got_any = false; f.fast->useful_rate = 0;
		bt::pick_urgent_duplicates(&f.torrent, f.fast, f.now, room);
		expect(room == 0 && f.a->reqs.size() == 1 && f.a->out.empty(), "existing single-owner urgent rescue still permits a newly discovered peer");
		f.ledger();
	}
	{
		Requests f; int room = 1;
		f.b->reqs.clear(); // deliberate corrupted ledger must not authorize cancellation
		bt::pick_urgent_duplicates(&f.torrent, f.fast, f.now, room);
		expect(room == 1 && f.a->out.empty() && f.fast->out.empty(), "inconsistent reservation accounting is not permission for handoff");
	}
	{
		Requests f;
		f.a->reqs[0].at = 1; f.b->reqs[0].at = f.now - 0.2;
		for (const auto& peer : f.torrent.peers) peer->peer_choking = true;
		bt::upkeep(&f.torrent, f.now);
		expect(f.a->reqs.empty() && f.b->reqs.size() == 1, "timeout releases only the expired request");
		#ifdef BT_TIMEOUT_CANCEL
		expect(one_wire_message(f.a->out, 8), "timeout sends exact protocol CANCEL before dropping local ownership");
		expect(f.torrent.timeout_cancels == 1, "timeout cancellation counter records one retired wire request");
		#endif
		expect(f.b->out.empty(), "timeout does not cancel the fresh owner's request");
		f.ledger();
	}
	#ifdef BT_HANDOFF_BOOTSTRAP
	for (int protection = 0; protection < 4; ++protection) {
		Requests f; int room = 1;
		f.fast->since = f.now - 1; f.fast->got_any = false; f.fast->payload_at = 0; f.fast->useful_rate = 0;
		f.a->useful_rate = f.b->useful_rate = 16 * 1024;
		switch (protection) {
		case 0: f.fast->handoff_probe_requests = 16; break;
		case 1: f.fast->since = f.now - 10; break;
		case 2: f.a->reqs[0].at = f.b->reqs[0].at = f.now - 0.5; break;
		case 3: f.a->useful_rate = f.b->useful_rate = 64 * 1024; break;
		}
		bt::pick_urgent_duplicates(&f.torrent, f.fast, f.now, room);
		expect(room == 1, "bootstrap preserves its lifetime cap, grace window, fresh owners and productive owners");
		f.untouched();
	}
	{
		Requests f; f.resize_blocks(20);
		f.fast->since = f.now - 1; f.fast->got_any = false; f.fast->payload_at = 0; f.fast->useful_rate = 0;
		f.a->useful_rate = f.b->useful_rate = 16 * 1024;
		bt::pick(&f.torrent, f.fast, f.now);
		expect(f.fast->reqs.size() == 16 && f.fast->handoff_probe_requests == 16, "unmeasured newcomer can trial only sixteen 16-KiB blocks");
		expect(f.torrent.request_probe_handoffs == 16 && f.torrent.request_handoffs == 16, "diagnostics count the bounded bootstrap handoffs");
		f.every_block_ledger();
		bt::pick(&f.torrent, f.fast, f.now + 0.01);
		expect(f.fast->reqs.size() == 16 && f.fast->handoff_probe_requests == 16, "another scheduling pass cannot reset or expand bootstrap budget");
	}
	{
		Requests f; f.resize_blocks(20);
		f.fast->since = f.now - 1; f.fast->got_any = false; f.fast->payload_at = 0; f.fast->useful_rate = 0;
		f.fast->max_reqs = 4; f.fast->reqq = 4;
		f.a->useful_rate = f.b->useful_rate = 16 * 1024;
		bt::pick(&f.torrent, f.fast, f.now);
		expect(f.fast->reqs.size() == 4, "bootstrap honors remote request-queue limit of four");
		std::string bytes(16384, 'x');
		for (int i = 0; i < 8; ++i) {
			expect(!f.fast->reqs.empty(), "partly proven peer with reqq four keeps a bounded opportunity to reach 128 KiB");
			const auto request = f.fast->reqs.front();
			const double when = f.now + 0.01 * (i + 1);
			bt::on_block(&f.torrent, f.fast, request.piece, request.begin, bytes.data(), bytes.size(), when);
			bt::pick(&f.torrent, f.fast, when);
			expect(f.fast->reqs.size() <= 4, "each real block response replenishes at most the remote queue capacity");
			f.every_block_ledger();
		}
		expect(f.fast->useful_window == 128 * 1024 && f.fast->useful_rate == 0, "128 KiB of structurally valid new payload is credited before the eight-second EMA");
		expect(bt::handoff_useful_rate(&f.torrent, f.fast, f.now + 0.1) >= 128 * 1024, "recent actual payload qualifies the fresh faster peer");
		expect(f.fast->handoff_probe_requests <= 16, "successful bootstrap still never exceeds its lifetime reservation cap");
		const int64_t credited = f.fast->useful_window;
		bt::on_block(&f.torrent, f.fast, 0, 0, bytes.data(), bytes.size(), f.now + 0.11);
		expect(f.fast->useful_window == credited, "a duplicate block cannot inflate useful evidence");
		const auto request = f.fast->reqs.front();
		bt::on_block(&f.torrent, f.fast, request.piece, request.begin, bytes.data(), 8, f.now + 0.12);
		expect(f.fast->useful_window == credited, "a wrong-length block cannot inflate useful evidence");
		f.every_block_ledger();
	}
	{
		Requests f; int room = 1;
		f.fast->useful_rate = 0; f.fast->recent_useful_sample = 512 * 1024;
		bt::pick_urgent_duplicates(&f.torrent, f.fast, f.now, room);
		expect(room == 0, "a completed one-second useful sample can qualify the peer before EWMA catches up");
		f.ledger();
	}
	#endif
}
}

int main() {
	try { run(); std::cout << "PASS: " << assertions << " bounded request handoff/CANCEL assertions\n"; return 0; }
	catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
