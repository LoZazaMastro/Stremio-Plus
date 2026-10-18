# Torrent engine

Stremio Plus uses a native BitTorrent implementation for streaming and offline
downloads. It discovers peers through trackers, DHT and peer exchange, obtains
magnet metadata from peers, and verifies each completed piece before making
its data available to a reader.

## Scheduling

Peer request windows adapt to measured useful transfer rates and apply a
bounded limit derived from each peer's advertised queue capacity. The UltraFast
profile is the default.
It increases bounded concurrency; it does not set or guarantee a line rate.

The network thread processes a bounded receive batch before refilling that
peer's request window. Several piece replies can therefore release their
request slots together, avoiding a complete scheduling pass after every
16 KiB block. The engine attempts to flush queued requests before returning
to readiness polling.

The network mutex is released between ready peers so that piece verification,
download readers and status requests can make progress during a busy transfer.
Receive work is limited to 256 KiB per peer per readiness turn. This is a
scheduling budget, not a bandwidth cap: readable sockets can be serviced again
on the next poll without waiting for the housekeeping interval.

Bulk recovery requests use idle peers and at most 16 blocks per batch. A peer
that is already delivering unique blocks does not fill its remaining queue
with bulk duplicates. The separate urgent-head recovery path remains active
when a missing piece blocks playback or the contiguous download prefix.

The 15-second request timeout accounts for useful payload inactivity. An old
request near the back of a productive peer's queue is not sufficient to label
the connection stalled. Keepalives, malformed blocks and duplicate replies do
not extend that timer. A sender skips refill in the housekeeping iteration
that cancels its stale requests, giving other peers a chance to take the
released work.

A missing urgent head has an independent recovery deadline. After 15 seconds,
one old reservation can be reassigned atomically to another available peer,
even if both previous owners are delivering later blocks. This keeps at most
two active request reservations per block and preserves the productive owners'
remaining queues. Cancelled bytes may already be in flight on the network.

## Storage and memory

Standalone downloads consume SHA-1-verified pieces through a bounded memory
handoff. The selected file is written as a contiguous prefix in 4 MiB batches
by the PS5 download writer. Progress advances only after the writer confirms
the bytes. A checkpoint becomes durable after the media is flushed and the
saved state is replaced successfully.

The engine retains its 128 MiB aggregate payload budget. More peers do not
create an unbounded set of active piece buffers. When a reader or the writer
falls behind, the existing bounded windows apply backpressure.

Queued titles do not start additional torrent sessions. One media worker
selects the active download; enqueueing persists the new job separately from
the live progress mutex. Artwork copying is deferred while media is active.

## Diagnostics

Logs remain in `/data/Stremio`. The existing transfer timing record separates
time waiting for verified data from writes and checkpoint synchronization.
Request records include timeouts, handoffs and duplicate requests. Scheduling
records add cumulative picker passes and received batches.

When investigating a slowdown, compare these records over the same time
window. Connected peers, advertised seed counts, useful bytes, physical
writes and durable checkpoint bytes describe different stages of a transfer.
An increase in one does not establish that the others advanced.

## Reproducing a comparison

The TCP harness starts controlled local seeders and exercises the production
network loop, parser, request scheduler, SHA-1 worker, verified cache and
download reader. It compares every delivered byte with the fixture and
rejects attempts to contact external peers or trackers. The status sampler
also measures how long `Engine::stats()` waits during the transfer.

With the host dependencies from [BUILDING.md](../BUILDING.md) installed:

```sh
mkdir -p build/baseline/src/torrent
git show 64d1bfb5b78b3c6a37e013973402f3239bee5df0:src/torrent/engine.cpp > build/baseline/src/torrent/engine.cpp
git show 64d1bfb5b78b3c6a37e013973402f3239bee5df0:src/torrent/engine.h > build/baseline/src/torrent/engine.h
python3 tests/run_torrent_network_tests.py \
  --baseline-engine build/baseline/src/torrent/engine.cpp \
  --candidate-engine src/torrent/engine.cpp \
  --cases sustained_local_fast sustained_mixed_seeders peer_delay_300ms peer_delay_1500ms \
  --repeat 3 --output build/tests/torrent-network.json
```

The report records source hashes, exact-byte verification, duplicate traffic,
transfer timing and status-call latency. Paired runs alternate their order to
reduce a consistent order bias. Stop other heavy workloads when comparing
results. Peer response delays in this harness are application turnaround
delays; they do not reproduce Internet TCP congestion.

Host measurements demonstrate behavior under these controlled conditions.
PS5 throughput and responsiveness still require validation on the console,
with its network stack, storage and the chosen live swarm.

## References

[qBittorrent](https://github.com/qbittorrent/qBittorrent) and
[libtorrent](https://github.com/arvidn/libtorrent) were studied for request
queue sizing, receive scheduling and recovery from slow peers. Useful
background includes libtorrent's [settings reference](https://www.libtorrent.org/reference-Settings.html)
and its explanation of [block request timeouts](https://blog.libtorrent.org/2011/11/block-request-time-outs/).
