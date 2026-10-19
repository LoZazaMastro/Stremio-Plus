#pragma once

#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Downloads artwork, scales it to the size the UI draws it at and stores it
// as raw RGBA (".rgba": "RGBA", u32 width, u32 height, pixels) so the
// renderer can load it without decoding. Kinds decide the size:
//
//   poster      182x277 dp, cropped to fill, rounded corners (grid cards)
//   poster_l    278x422 dp, the same (board and search rows)
//   download_p  720x1080 pixels, sharp; covers the Download gallery at 4K
//   background  1280x720, cropped to fill (detail page, launch screen)
//   logo        height 150, aspect kept (detail page, discover preview)
//   thumb       640x360, cropped to fill (episode carousel)
//   qr          400x400, nearest neighbour (sign-in)
//   icon        96x96 dp, fitted, transparent around (addon tiles)
//   backdrop    1920x1080, cropped to fill, sharp (detail page)
//   still       652x367 dp, cropped to fill, rounded corners (discover preview)
//   logo_box    560x120 dp, fitted, left-aligned on transparency (discover preview)
//   launch_logo up to 1536x864, alpha-cropped, original aspect (stream opening)
enum class ArtKind { Poster, PosterLarge, Background, Backdrop, Still, Logo, LogoBox, Thumb, Qr, Icon, LaunchLogo, DownloadPoster };
enum class ArtPriority { Prefetch, Visible, Immediate };

class ArtCache {
public:
	void start(const std::string& dir, int workers);
	void stop();

	// Returns the local path when cached; otherwise queues a download and
	// returns "", calling `ready(path)` on the UI thread when done (path is
	// "" on failure). Visible hero/logo art takes precedence over posters;
	// posters are FIFO so an off-screen prefetch cannot jump ahead of the row.
	// A non-null observer replaces that observer's previous pending callback,
	// avoiding one callback per frame while a selected hero is downloading.
	std::string get(const std::string& url, ArtKind kind, std::function<void(const std::string&)> ready,
	                ArtPriority priority = ArtPriority::Visible, const void* observer = nullptr);

	// Returns the local path when cached, without downloading.
	std::string peek(const std::string& url, ArtKind kind);
	// Memory-only lookups never probe storage on a controller event. Unknown
	// disk entries can be resolved by get_async() on the artwork worker.
	std::string peek_cached(const std::string& url, ArtKind kind);
	std::string get_async(const std::string& url, ArtKind kind, std::function<void(const std::string&)> ready,
	                      ArtPriority priority = ArtPriority::Visible, const void* observer = nullptr);

	// Drops queued (not yet started) downloads, e.g. when leaving a page.
	void clear_queue();

private:
	std::string get_impl(const std::string& url, ArtKind kind, std::function<void(const std::string&)> ready,
	                     ArtPriority priority, const void* observer, bool probe_disk);
	struct Job {
		std::string url, path;
		ArtKind kind;
		ArtPriority priority = ArtPriority::Visible;
		uint64_t order = 0, touched = 0;
	};
	struct Subscriber {
		const void* observer = nullptr;
		std::function<void(const std::string&)> ready;
	};
	struct Known {
		bool exists = false;
		unsigned failures = 0;
		std::chrono::steady_clock::time_point retry_at;
	};
	struct Source {
		std::string bytes;
		bool loading = true, ok = false;
		uint64_t used = 0;
	};
	std::string path_for(const std::string& url, ArtKind kind) const;
	std::shared_ptr<Source> source(const std::string& url);
	void worker();
	bool fetch(const Job& job);

	std::string dir_;
	std::mutex mutex_;
	std::condition_variable cv_;
	std::deque<Job> queue_;
	std::map<std::string, std::vector<Subscriber>> waiting_;  // by path
	std::map<std::string, Known> known_;
	std::map<std::string, std::shared_ptr<Source>> sources_; // bounded compressed-image reuse across sizes
	size_t source_bytes_ = 0;
	uint64_t order_ = 0, dispatched_ = 0;
	std::vector<std::thread> threads_;
	bool stopping_ = false;
	std::atomic<bool> cancel_{false};
};

extern ArtCache g_art;

// Loads an .rgba file (or any image stb_image can decode). Pixels are RGBA,
// straight alpha.
bool load_image_rgba(const std::string& path, std::vector<unsigned char>& pixels, int& w, int& h);
