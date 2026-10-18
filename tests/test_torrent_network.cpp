// Real TCP peer-wire benchmark. Engine::net_loop, connect, poll, recv, send,
// message parsing, picker, SHA-1, bounded verified cache and Engine::read are
// production code. The only fixture boundary is local torrent metadata and
// the list of loopback-only seeder addresses. No public swarm is contacted.
#ifdef BT_ENGINE_SOURCE
#include BT_ENGINE_SOURCE
#else
#include "../src/torrent/engine.cpp"
#endif

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <sys/resource.h>

namespace fs = std::filesystem;
namespace {
std::atomic<int> external_attempts{0};
std::atomic<FILE*> delayed_cache{nullptr};
std::atomic<int> delayed_cache_fd{-1};
std::atomic<int> cache_delay_ms{0};
std::atomic<int64_t> delayed_writes{0};

void require(bool yes, const char* message) {
	if (!yes) throw std::runtime_error(message);
}

std::string digest(const std::string& data) {
	unsigned char bytes[20];
	SHA1(reinterpret_cast<const unsigned char*>(data.data()), data.size(), bytes);
	return {reinterpret_cast<const char*>(bytes), sizeof(bytes)};
}

std::string hex(const std::string& bytes) {
	const char* digits = "0123456789abcdef";
	std::string result;
	for (unsigned char ch : bytes) { result += digits[ch >> 4]; result += digits[ch & 15]; }
	return result;
}

struct Seeder {
	const std::string& payload;
	const std::string info_hash;
	const int64_t piece_bytes;
	const double response_delay, rate;
	int listener = -1, fd = -1;
	sockaddr_in address{};
	std::thread thread;
	std::atomic<bool> stopping{false}, accepted{false}, handshaken{false};
	std::atomic<uint64_t> requested_bytes{0}, sent_bytes{0}, requests{0}, cancels{0};
	std::atomic<double> first_request_at{0};
	std::exception_ptr failure;
	struct Request { uint32_t piece, offset, length; double due, ready; };
	std::deque<Request> pending;
	std::string input, output;
	size_t input_offset = 0, output_offset = 0;
	bool handshake_done = false;
	double next_data_time = 0, committed_data_time = 0;

	Seeder(const std::string& bytes, std::string hash, int64_t piece, double latency_ms, double mib_per_second)
		: payload(bytes), info_hash(std::move(hash)), piece_bytes(piece),
		  response_delay(latency_ms / 1000), rate(mib_per_second * 1024 * 1024) {
		listener = socket(AF_INET, SOCK_STREAM, 0);
		require(listener >= 0, "fixture socket");
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		require(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "fixture bind");
		socklen_t length = sizeof(address);
		require(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) == 0, "fixture getsockname");
		require(listen(listener, 2) == 0, "fixture listen");
		bt::set_nonblocking(listener);
		thread = std::thread([this] {
			try { serve(); } catch (...) { failure = std::current_exception(); }
		});
	}

	void queue_message(uint8_t id, const std::string& data = {}) {
		bt::put32(output, uint32_t(data.size() + 1)); output += char(id); output += data;
	}

	void parse() {
		if (!handshake_done) {
			if (input.size() < 68) return;
			require(input[0] == 19 && input.substr(1, 19) == "BitTorrent protocol", "real peer handshake protocol");
			require(input.substr(28, 20) == info_hash, "real peer handshake info hash");
			output.assign(input.data(), 68);
			// We do not negotiate optional extensions in this transport benchmark.
			output[25] = 0;
			output.replace(48, 20, std::string("-TS0001-") + std::string(12, 'S'));
			const size_t pieces = (payload.size() + size_t(piece_bytes) - 1) / size_t(piece_bytes);
			queue_message(5, std::string((pieces + 7) / 8, char(0xff)));
			queue_message(1); // unchoke
			input_offset = 68;
			handshake_done = true;
			handshaken = true;
		}
		while (input.size() - input_offset >= 4) {
			const uint32_t length = bt::be32(input.data() + input_offset);
			require(length < (2u << 20), "bounded client message");
			if (input.size() - input_offset < size_t(length) + 4) break;
			const char* message = input.data() + input_offset + 4;
			if (length == 13 && (message[0] == 6 || message[0] == 8)) {
				Request r{bt::be32(message + 1), bt::be32(message + 5), bt::be32(message + 9), 0, 0};
				const uint64_t offset = uint64_t(r.piece) * uint64_t(piece_bytes) + r.offset;
				require(r.length <= 16384 && r.length > 0 && offset + r.length <= payload.size(), "bounded piece request");
				if (message[0] == 6) {
					if (!first_request_at.load()) first_request_at = now_seconds();
					r.ready = now_seconds() + response_delay;
					r.due = std::max(r.ready, next_data_time);
					next_data_time = r.due + (rate > 0 ? double(r.length) / rate : 0);
					pending.push_back(r); requested_bytes += r.length; requests++;
				} else {
					cancels++;
					pending.erase(std::remove_if(pending.begin(), pending.end(), [&](const Request& q) {
						return q.piece == r.piece && q.offset == r.offset && q.length == r.length;
					}), pending.end());
					// Canceled queued blocks consume no future seeder bandwidth.
					// Frames already passed to output retain their reserved time.
					next_data_time = committed_data_time;
					for (auto& q : pending) {
						q.due = std::max(q.ready, next_data_time);
						next_data_time = q.due + (rate > 0 ? double(q.length) / rate : 0);
					}
				}
			}
			input_offset += length + 4;
		}
		if (input_offset == input.size()) { input.clear(); input_offset = 0; }
		else if (input_offset > 65536) { input.erase(0, input_offset); input_offset = 0; }
	}

	void serve() {
		while (!stopping) {
			if (fd < 0) {
				pollfd event{listener, POLLIN, 0};
				if (poll(&event, 1, 10) <= 0) continue;
				fd = accept(listener, nullptr, nullptr);
				if (fd < 0) continue;
				accepted = true;
				bt::set_nonblocking(fd);
				int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
			}
			while (!pending.empty() && pending.front().due <= now_seconds() && output.size() - output_offset < (128u << 10)) {
				const auto r = pending.front(); pending.pop_front();
				committed_data_time = r.due + (rate > 0 ? double(r.length) / rate : 0);
				std::string message;
				bt::put32(message, r.piece); bt::put32(message, r.offset);
				message.append(payload.data() + uint64_t(r.piece) * uint64_t(piece_bytes) + r.offset, r.length);
				queue_message(7, message); sent_bytes += r.length;
			}
			pollfd event{fd, short(POLLIN | (output.empty() ? 0 : POLLOUT)), 0};
			if (poll(&event, 1, 1) <= 0) continue;
			if (event.revents & POLLIN) {
				char bytes[32768];
				const ssize_t n = recv(fd, bytes, sizeof(bytes), 0);
				if (!n) break;
				if (n > 0) { input.append(bytes, size_t(n)); parse(); }
				else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) break;
			}
			if (event.revents & POLLOUT) {
				const ssize_t n = send(fd, output.data() + output_offset, output.size() - output_offset, MSG_NOSIGNAL);
				if (n > 0) output_offset += size_t(n);
				else if (n < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) break;
				if (output_offset == output.size()) { output.clear(); output_offset = 0; }
			}
			if (event.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
		}
	}
	void stop() {
		stopping = true;
		if (thread.joinable()) thread.join();
		if (fd >= 0) close(fd), fd = -1;
		if (listener >= 0) close(listener), listener = -1;
	}
	~Seeder() { stop(); }
};

struct StopEngine {
	~StopEngine() { bt::Engine::get().shutdown(); }
};
}

extern "C" int __real_connect(int, const sockaddr*, socklen_t);
extern "C" int __wrap_connect(int fd, const sockaddr* address, socklen_t length) {
	if (!address || address->sa_family != AF_INET ||
	    ntohl(reinterpret_cast<const sockaddr_in*>(address)->sin_addr.s_addr) != INADDR_LOOPBACK) {
		external_attempts++; errno = EACCES; return -1;
	}
	return __real_connect(fd, address, length);
}
extern "C" int __wrap_getaddrinfo(const char*, const char*, const addrinfo*, addrinfo**) {
	external_attempts++; return EAI_FAIL;
}
extern "C" size_t __real_fwrite(const void*, size_t, size_t, FILE*);
extern "C" size_t __wrap_fwrite(const void* data, size_t size, size_t count, FILE* file) {
	if (file == delayed_cache.load() && cache_delay_ms > 0) {
		delayed_writes++;
		std::this_thread::sleep_for(std::chrono::milliseconds(cache_delay_ms.load()));
	}
	return __real_fwrite(data, size, count, file);
}
extern "C" ssize_t __real_pwrite(int, const void*, size_t, off_t);
extern "C" ssize_t __wrap_pwrite(int fd, const void* data, size_t count, off_t offset) {
	if (fd == delayed_cache_fd.load() && cache_delay_ms > 0) {
		delayed_writes++;
		std::this_thread::sleep_for(std::chrono::milliseconds(cache_delay_ms.load()));
	}
	return __real_pwrite(fd, data, count, offset);
}

int main(int argc, char** argv) {
	try {
		require(argc >= 8, "usage: benchmark cache_dir peers response_ms rate_MiB_s total_MiB piece_MiB cache_delay_ms [slow_count slow_rate]");
		const fs::path directory = argv[1]; fs::create_directories(directory);
		const int peers = std::stoi(argv[2]);
		const double response_ms = std::stod(argv[3]), per_peer_rate = std::stod(argv[4]);
		const int64_t total = int64_t(std::stoi(argv[5])) << 20, piece_bytes = int64_t(std::stoi(argv[6])) << 20;
		cache_delay_ms = std::stoi(argv[7]);
		const int slow_count = argc > 8 ? std::stoi(argv[8]) : 0;
		const double slow_rate = argc > 9 ? std::stod(argv[9]) : 0;
		const char* watchdog_setting = std::getenv("STREMIO_TCP_WATCHDOG_SECONDS");
		const double watchdog_seconds = watchdog_setting ? std::stod(watchdog_setting) : 55;
		require(watchdog_seconds >= 5 && watchdog_seconds <= 240, "bounded watchdog");
		require(peers > 0 && peers <= 256 && total > 0 && total <= (1ll << 30) && piece_bytes > 0, "valid bounded arguments");
		std::string payload(size_t(total), '\0');
		for (size_t i = 0; i < payload.size(); ++i) payload[i] = char((i * 17 + (i >> 12) * 29) & 255);
		std::string hashes;
		for (size_t offset = 0; offset < payload.size(); offset += size_t(piece_bytes))
			hashes += digest(payload.substr(offset, size_t(piece_bytes)));
		BValue info = BValue::dict();
		info.d["name"] = BValue(std::string("local-tcp-fixture.mp4"));
		info.d["length"] = BValue(total);
		info.d["piece length"] = BValue(piece_bytes);
		info.d["pieces"] = BValue(hashes);
		const std::string metadata = bencode(info), info_hash = digest(metadata);
		auto torrent = std::make_shared<bt::Torrent>();
		torrent->hex = hex(info_hash); memcpy(torrent->ih, info_hash.data(), 20);
		torrent->started = now_seconds();
		auto& impl = bt::impl();
		impl.data_dir = directory.string();
		impl.cache_bytes = std::max<int64_t>(64ll << 20, std::min<int64_t>(total, 256ll << 20));
		impl.speed_profile = bt::SpeedProfile::UltraFast;
		impl.peer_id = std::string("-TC0001-") + std::string(12, 'C');
		require(bt::parse_info(torrent.get(), metadata) && torrent->cache, "production metadata and on-disk cache");
		delayed_cache = torrent->cache;
		delayed_cache_fd = fileno(torrent->cache);
		impl.torrents.push_back(torrent);
		std::vector<std::unique_ptr<Seeder>> seeders;
		std::vector<sockaddr_in> candidates;
		for (int i = 0; i < peers; ++i) {
			seeders.push_back(std::make_unique<Seeder>(payload, info_hash, piece_bytes, response_ms,
			                                         i < slow_count ? slow_rate : per_peer_rate));
			candidates.push_back(seeders.back()->address);
		}
		bt::add_candidates(torrent.get(), candidates);
		int reader_id = -1; int64_t size = 0; std::string error;
		auto reader = bt::Engine::get().open_reader(torrent->hex, 0, &reader_id, &size, &error, bt::ReaderRole::Download);
		require(reader && size == total, "production download reader");
		double network_cpu_seconds = 0;
		StopEngine shutdown;
		std::atomic<bool> abort{false}, finished{false};
		json peer_trace = json::array();
		std::vector<double> stats_wait_ms;
		double stats_next_sample = 0;
		const double start = now_seconds();
		std::thread watchdog([&] {
			const double deadline = now_seconds() + watchdog_seconds;
			double next_trace = start + 10;
			while (!finished && now_seconds() < deadline) {
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
				const double before_stats = now_seconds();
				if (before_stats >= stats_next_sample) {
					stats_next_sample = before_stats + 0.05;
					bt::Engine::get().stats(torrent->hex);
					stats_wait_ms.push_back((now_seconds() - before_stats) * 1000);
				}
				if (peers <= 80 || now_seconds() < next_trace) continue;
				next_trace += 10;
				std::lock_guard<std::mutex> lock(impl.mu);
				int active = 0, idle_unchoked = 0, requests = 0, fast_handshaken = 0, fast_requested = 0;
				std::vector<bt::Peer*> ordered;
				for (const auto& peer : torrent->peers) {
					if (peer->dead || peer->state != bt::Peer::Active) continue;
					++active; requests += int(peer->reqs.size());
					idle_unchoked += !peer->peer_choking && peer->reqs.empty();
					ordered.push_back(peer.get());
				}
				for (size_t i = size_t(slow_count); i < seeders.size(); ++i) {
					fast_handshaken += seeders[i]->handshaken.load();
					fast_requested += seeders[i]->first_request_at > 0;
				}
				json sample = {{"at_s", now_seconds() - start}, {"active", active},
				               {"unchoked_without_requests", idle_unchoked}, {"requests", requests},
				               {"candidates", torrent->candidates.size()}, {"verified_bytes", torrent->verified_bytes},
				               {"fast_handshaken", fast_handshaken}, {"fast_requested", fast_requested}};
				#ifdef BT_PEER_TURNOVER
				std::stable_sort(ordered.begin(), ordered.end(), [](const bt::Peer* a, const bt::Peer* b) { return a->useful_rate < b->useful_rate; });
				int lowest_idle = 0;
				for (size_t i = 0; i < ordered.size() / 5; ++i) lowest_idle += !ordered[i]->peer_choking && ordered[i]->reqs.empty();
				sample["slowest_quintile_idle"] = lowest_idle;
				sample["turnover_count"] = torrent->turnover_count;
				#endif
				#ifdef BT_REQUEST_HANDOFF
				sample["request_handoffs"] = torrent->request_handoffs;
				sample["request_probe_handoffs"] = torrent->request_probe_handoffs;
				sample["timeout_cancels"] = torrent->timeout_cancels;
				#endif
				peer_trace.push_back(std::move(sample));
			}
			if (!finished) { abort = true; impl.cv.notify_all(); }
		});
		#ifdef BT_ASYNC_DISK_WORKER
		{ std::lock_guard<std::mutex> lock(impl.mu); bt::start_disk_worker_locked(); }
		#endif
		rusage process_before{}; getrusage(RUSAGE_SELF, &process_before);
		impl.quit = false; impl.running = true; impl.net = std::thread([&] {
			timespec before{}, after{};
			clock_gettime(CLOCK_THREAD_CPUTIME_ID, &before);
			bt::net_loop();
			clock_gettime(CLOCK_THREAD_CPUTIME_ID, &after);
			network_cpu_seconds = double(after.tv_sec - before.tv_sec) + double(after.tv_nsec - before.tv_nsec) / 1e9;
		});
		std::string block(1u << 20, '\0');
		int64_t received = 0;
		double first_piece = 0;
		bool exact = true;
		while (received < total && !abort) {
			const int n = bt::Engine::get().read(reader, reader_id, 0, received,
			                                 reinterpret_cast<uint8_t*>(block.data()), int(block.size()), &abort);
			if (n <= 0) { abort = true; break; }
			if (received == 0) first_piece = now_seconds() - start;
			exact &= memcmp(block.data(), payload.data() + received, size_t(n)) == 0;
			received += n;
		}
		const double elapsed = now_seconds() - start;
		finished = true; watchdog.join();
		bt::Engine::get().close_reader(reader, reader_id);
		json report;
		{
			std::lock_guard<std::mutex> lock(impl.mu);
			int active = 0, choked = 0; for (const auto& peer : torrent->peers) { active += !peer->dead && peer->state == bt::Peer::Active; choked += peer->peer_choking; }
			report = {{"result", !abort && exact && received == total ? "PASS" : "FAIL"},
			          {"bytes", received}, {"elapsed_s", elapsed}, {"MiB_s", double(received) / elapsed / (1024 * 1024)},
			          {"first_piece_s", first_piece}, {"verified_bytes", torrent->verified_bytes},
			          {"duplicate_bytes", torrent->duplicate_bytes}, {"peers", peers}, {"active_peers", active},
			          {"choked_peers", choked}, {"response_ms", response_ms}, {"per_peer_MiB_s", per_peer_rate},
			          {"piece_bytes", piece_bytes}, {"cache_delay_ms", cache_delay_ms.load()}, {"delayed_cache_writes", delayed_writes.load()},
			          {"external_network_attempts", external_attempts.load()}, {"exact_bytes", exact},
			          {"slow_peers", slow_count}, {"slow_peer_MiB_s", slow_rate},
			          {"scope", "host real TCP, production engine network loop and verified download handoff; controlled local seeders, no physical PS5"}};
			#ifdef BT_REQUEST_HANDOFF
			report["request_handoffs"] = torrent->request_handoffs;
			report["request_probe_handoffs"] = torrent->request_probe_handoffs;
			report["timeout_cancels"] = torrent->timeout_cancels;
			#endif
			#ifdef BT_BATCHED_PICKER
			report["picker_passes"] = torrent->picker_passes;
			report["received_batches"] = torrent->received_batches;
			#endif
		}
		bt::Engine::get().shutdown();
		rusage process_after{}; getrusage(RUSAGE_SELF, &process_after);
		auto cpu_seconds = [](const rusage& value) {
			return double(value.ru_utime.tv_sec + value.ru_stime.tv_sec) +
			       double(value.ru_utime.tv_usec + value.ru_stime.tv_usec) / 1e6;
		};
		report["network_thread_cpu_s"] = network_cpu_seconds;
		report["fixture_total_cpu_s"] = cpu_seconds(process_after) - cpu_seconds(process_before);
		std::sort(stats_wait_ms.begin(), stats_wait_ms.end());
		report["stats_samples"] = stats_wait_ms.size();
		report["stats_wait_max_ms"] = stats_wait_ms.empty() ? 0 : stats_wait_ms.back();
		report["stats_wait_p99_ms"] = stats_wait_ms.empty() ? 0 : stats_wait_ms[std::min(stats_wait_ms.size() - 1, stats_wait_ms.size() * 99 / 100)];
		report["peer_trace"] = peer_trace;
		uint64_t requested = 0, sent = 0, cancels = 0, fast_prepared_bytes = 0;
		int fast_admitted = 0, fast_accepted = 0, fast_handshaken = 0;
		double first_fast_request = 0;
		for (size_t i = 0; i < seeders.size(); ++i) {
			auto& seeder = seeders[i];
			seeder->stop(); if (seeder->failure) std::rethrow_exception(seeder->failure);
			requested += seeder->requested_bytes; sent += seeder->sent_bytes; cancels += seeder->cancels;
			if (i >= size_t(slow_count)) { fast_accepted += seeder->accepted.load(); fast_handshaken += seeder->handshaken.load(); }
			if (i >= size_t(slow_count) && seeder->first_request_at > 0) {
				++fast_admitted; fast_prepared_bytes += seeder->sent_bytes;
				const double when = seeder->first_request_at - start;
				if (!first_fast_request || when < first_fast_request) first_fast_request = when;
			}
		}
		report["requested_bytes"] = requested; report["sent_bytes"] = sent; report["cancels"] = cancels;
		report["fast_admitted"] = fast_admitted; report["first_fast_request_s"] = first_fast_request;
		report["fast_requested"] = fast_admitted;
		report["fast_prepared_payload_bytes"] = fast_prepared_bytes;
		report["fast_tcp_accepted"] = fast_accepted; report["fast_handshaken"] = fast_handshaken;
		std::cout << report.dump() << '\n';
		return !abort && exact && received == total && !external_attempts ? 0 : 1;
	} catch (const std::exception& error) {
		std::cerr << "FAIL: " << error.what() << '\n'; return 1;
	}
}
