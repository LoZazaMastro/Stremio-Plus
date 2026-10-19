#pragma once

#include "stremio.h"
#include "growing_file.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

enum class DownloadState { Queued, Downloading, Paused, Complete, Failed, Waiting };

struct DownloadRequest {
	std::string media_id, type, video_id, title, subtitle;
	std::string episode_title;
	// Cached local RGBA artwork only. Copies are deferred while media is being
	// written and completed before this video's transfer starts. Completed
	// offline videos therefore do not depend on the shared artwork cache.
	std::string poster_path, background_path, logo_path, thumbnail_path;
	// Retain the poster address so an uncached cover can be fetched after
	// enqueue or restart. Account/provider credentials are never logged.
	std::string poster_url, thumbnail_url;
	Stream stream;
	int season = -1, episode = -1;
	int release_year = 0; // Catalog metadata only; zero means unavailable.
	bool live = false;
};

struct DownloadEntry {
	std::string id, media_id, type, video_id, title, subtitle;
	std::string episode_title;
	// Stable digest of the selected source and episode, retained during moves.
	std::string source_identity;
	std::string poster_path, background_path, logo_path, thumbnail_path, local_path, error;
	std::string poster_url, thumbnail_url;
	DownloadState state = DownloadState::Queued;
	// Verified RGBA dimensions, published by the artwork worker. The gallery
	// can refresh legacy small covers without opening files on a UI event.
	int poster_width = 0, poster_height = 0;
	int season = -1, episode = -1;
	int release_year = 0;
	// Epoch milliseconds from queue admission and successful media promotion.
	// Legacy entries retain zero completion time and use created_ms in the UI.
	int64_t created_ms = 0, completed_ms = 0;
	int64_t done = 0, total = -1;
	double progress = -1, bytes_per_second = 0;
	// Live worker telemetry only; -1 means not yet known / not applicable.
	// Seeders are connected peers which advertise every torrent piece, never
	// a tracker estimate or the addon's advertised swarm count.
	int64_t remaining_seconds = -1;
	int connected_peers = -1, connected_seeders = -1;
	bool playable_while_downloading = false;
	// Recovery-only entries expose unrecognized owned storage for explicit
	// removal. No source, title identity or playable media is inferred.
	bool recovery_only = false;
	// Allocated bytes of regular files in the job directory; -1 means it could
	// not be measured completely. This includes saved metadata and artwork.
	int64_t disk_bytes = -1;
};

struct DownloadRelocationStatus {
 bool active = false, pending = false;
 int files_done = 0, files_total = 0;
 int64_t bytes_done = 0, bytes_total = 0;
 std::string title, phase, error, destination;
};

// A bounded, persistent, sequential download queue. Network transfers happen on
// its worker only. Playback of a foreground torrent takes priority over the
// background torrent; HTTP transfers are independent of the torrent engine.
class DownloadManager {
public:
	DownloadManager();
	~DownloadManager();
	DownloadManager(const DownloadManager&) = delete;
	DownloadManager& operator=(const DownloadManager&) = delete;

	// Networking is disabled initially and remains so until set_enabled(true).
	// Incomplete jobs interrupted by shutdown/crash are restored as Paused.
	// Final files require matching completion metadata before they are playable.
	// A failed startup is retried on the next explicit enqueue, never in a loop.
	// init's error and storage_error() contain local operation/path/errno details
	// for diagnostics; enqueue keeps a separate user-facing error message.
	// init returns write readiness. A readable inventory is still published when
	// it returns false because storage is full or read-only.
	bool init(const std::string& data_dir, std::string* error = nullptr);
	// Registry selection wins over the preference fallback. Interrupted moves
	// are reconciled using bounded metadata; retry resumes them on a worker.
	bool init(const std::string& data_parent, const std::string& registry_appdata,
		const std::string& preferred_directory, std::string* error = nullptr);
	// Worker-only operation: quiesce and move every saved download to this exact
	// directory. A durable journal preserves interrupted migrations for retry.
	bool set_download_directory(const std::string& directory, std::string& error);
	DownloadRelocationStatus relocation_status() const;
	std::string download_directory() const;
	std::vector<std::string> download_directories() const;
	bool storage_available() const;
	// Existing downloads remain readable/deletable when new writes are denied.
	bool storage_readable() const;
	std::string storage_error() const;
	void shutdown();
	std::string enqueue(const DownloadRequest& request, std::string& error);
	// Submission does no filesystem work. A dedicated bounded worker commits
	// the selected request before delivering its id. Completion runs off the UI
	// thread; an empty id reports failure or shutdown before persistence started.
	using EnqueueCallback = std::function<void(std::string id, std::string error)>;
	bool enqueue_async(DownloadRequest request, EnqueueCallback done);
	// Copies a published inventory without waiting for storage or network I/O.
	std::vector<DownloadEntry> snapshot() const;
	std::optional<DownloadEntry> find(const std::string& id) const;
	// Exact selected source/headers/file and episode identity. This never opens
	// media, starts a second transfer or waits on filesystem operations.
	std::optional<DownloadEntry> find_matching(const DownloadRequest& request) const;
	// Queue a cached cover/source without filesystem work or waiting for the
	// storage mutex. The artwork worker validates and publishes cached paths;
	// private copies and persistence wait until the media writer is idle.
	// true means the update was accepted;
	// a vanished or invalid cached image is rejected by the worker.
	bool update_poster(const std::string& id, const std::string& cached_path);
	bool update_poster_source(const std::string& id, const std::string& url);
	bool update_thumbnail(const std::string& id, const std::string& cached_path);
	bool update_thumbnail_source(const std::string& id, const std::string& url);
	bool pause(const std::string& id);
	bool resume(const std::string& id);
	bool remove(const std::string& id, std::string& error);
	// Only a reconciled active transfer with a known total and >=5% prefix.
	// The held descriptor survives promotion; no torrent reservation is taken.
	std::shared_ptr<GrowingFilePlayback> open_progressive(const std::string& id, std::string& error);
	uint64_t revision() const;
	void set_enabled(bool enabled);

	// Call true before preparing a foreground torrent, then wait on that
	// foreground worker before Engine::start(). false releases the reservation.
	// Setter is nonblocking; wait finishes only after background transfer returns.
	void set_torrent_playback_active(bool active);
	bool wait_for_torrent_idle(const std::atomic<bool>& cancel);
	// Acquire before submitting a foreground resolver and capture the token in
	// its worker closure. Even if the UI cancels/release()s the reservation, the
	// background engine stays excluded until this old resolver has really exited.
	// Dropping an unstarted queued task also releases its token automatically.
	std::shared_ptr<void> acquire_torrent_resolution();

private:
	std::string enqueue_at_generation(const DownloadRequest& request, std::string& error, uint64_t generation);
	struct Impl;
	std::unique_ptr<Impl> impl_;
};
