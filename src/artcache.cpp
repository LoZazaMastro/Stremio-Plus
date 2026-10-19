#include "artcache.h"
#include "art_decode_budget.h"
#include "download_artwork.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <limits>
#include <sys/stat.h>

#include "http.h"
#include "tasks.h"
#include "util.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#include "stb_image.h"
#include <webp/decode.h>

ArtCache g_art;

namespace {
ArtDecodeBudget decode_budget;
using DecodedPixels = std::unique_ptr<unsigned char, void (*)(void*)>;
constexpr size_t kLargestResampleBytes = 1920u * 1080u * 4u;
}

// PNG/JPEG via stb_image, WebP (what metahub serves posters as) via libwebp.
static bool image_dimensions(const std::string& data, int& w, int& h, const char** why) {
	if (data.size() > 32u * 1024u * 1024u) {
		if (why) *why = "image exceeds compressed size limit";
		return false;
	}
	const auto safe_dimensions = [](int width, int height) {
		return width > 0 && height > 0 && width <= 8192 && height <= 8192 &&
		       size_t(width) * size_t(height) <= 16u * 1024u * 1024u;
	};
	const unsigned char* bytes = (const unsigned char*)data.data();
	if (data.size() >= 12 && memcmp(bytes, "RIFF", 4) == 0 && memcmp(bytes + 8, "WEBP", 4) == 0) {
		if (!WebPGetInfo(bytes, data.size(), &w, &h) || !safe_dimensions(w, h)) {
			if (why) *why = "invalid or excessive WebP dimensions";
			return false;
		}
		return true;
	}
	int comp;
	if (!stbi_info_from_memory(bytes, int(data.size()), &w, &h, &comp) || !safe_dimensions(w, h)) {
		if (why) *why = "invalid or excessive image dimensions";
		return false;
	}
	return true;
}

// Call only after image_dimensions() and weighted admission. Retaining the
// decoder's allocation avoids a second source-size RGBA vector in ArtCache.
static DecodedPixels decode_owned(const std::string& data, int& w, int& h, const char** why) {
	const auto* bytes = reinterpret_cast<const unsigned char*>(data.data());
	if (data.size() >= 12 && memcmp(bytes, "RIFF", 4) == 0 && memcmp(bytes + 8, "WEBP", 4) == 0) {
		auto* out = WebPDecodeRGBA(bytes, data.size(), &w, &h);
		if (!out && why) *why = "bad WebP";
		return DecodedPixels(out, WebPFree);
	}
	int components = 0;
	auto* out = stbi_load_from_memory(bytes, int(data.size()), &w, &h, &components, 4);
	if (!out && why) *why = stbi_failure_reason();
	return DecodedPixels(out, stbi_image_free);
}

static bool decode_image(const std::string& data, std::vector<unsigned char>& px, int& w, int& h, const char** why) {
	if (!image_dimensions(data, w, h, why)) return false;
	auto admission = decode_budget.acquire(size_t(w) * h * 8u);
	auto decoded = decode_owned(data, w, h, why);
	if (!decoded) return false;
	px.assign(decoded.get(), decoded.get() + size_t(w) * h * 4u);
	return true;
}

static const char* kind_suffix(ArtKind k) {
	switch (k) {
	case ArtKind::Poster: return "p2";  // "2": sized for kUiScale (older files were 182x277)
	case ArtKind::PosterLarge: return "pl2";
	case ArtKind::DownloadPoster: return "dp1"; // Never reuse the small catalog derivatives.
	case ArtKind::Background: return "b";
	case ArtKind::Backdrop: return "bd2"; // Sharp detail hero; never reuse the old blurred variant.
	case ArtKind::Still: return "s";
	case ArtKind::LogoBox: return "lb";
	case ArtKind::Logo: return "l";
	case ArtKind::Thumb: return "t3"; // Larger episode carousel; do not reuse old low-resolution thumbnails.
	case ArtKind::LaunchLogo: return "ll";
	case ArtKind::Qr: return "q";
	case ArtKind::Icon: return "i2";
	}
	return "x";
}

void ArtCache::start(const std::string& dir, int workers) {
	if (!threads_.empty()) stop();
	dir_ = dir;
	make_dirs(dir_);
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stopping_ = false;
		cancel_ = false;
		queue_.clear(); waiting_.clear(); known_.clear(); sources_.clear();
		source_bytes_ = 0; order_ = dispatched_ = 0;
	}
	for (int i = 0; i < std::clamp(workers, 1, 4); i++) threads_.emplace_back([this] { worker(); });
}

void ArtCache::stop() {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stopping_ = true;
		cancel_ = true;
		queue_.clear();
	}
	cv_.notify_all();
	for (auto& t : threads_) t.join();
	threads_.clear();
	std::lock_guard<std::mutex> lock(mutex_);
	waiting_.clear(); sources_.clear(); source_bytes_ = 0;
}

std::string ArtCache::path_for(const std::string& url, ArtKind kind) const {
	char name[64];
	snprintf(name, sizeof(name), "/%016llx%s.rgba", (unsigned long long)fnv1a(url), kind_suffix(kind));
	return dir_ + name;
}

static int art_priority(ArtKind kind, ArtPriority priority) {
	if (priority == ArtPriority::Prefetch) return 0;
	const int base = priority == ArtPriority::Immediate ? 200 : 100;
	switch (kind) {
	case ArtKind::Qr: case ArtKind::Logo: case ArtKind::LogoBox: case ArtKind::LaunchLogo: return base + 40;
	case ArtKind::Background: case ArtKind::Backdrop: case ArtKind::Still: return base + 30;
	default: return base + 10;
	}
}

std::string ArtCache::get(const std::string& url, ArtKind kind, std::function<void(const std::string&)> ready,
                          ArtPriority priority, const void* observer) {
	return get_impl(url, kind, std::move(ready), priority, observer, true);
}

std::string ArtCache::get_async(const std::string& url, ArtKind kind, std::function<void(const std::string&)> ready,
                              ArtPriority priority, const void* observer) {
	return get_impl(url, kind, std::move(ready), priority, observer, false);
}

std::string ArtCache::get_impl(const std::string& url, ArtKind kind, std::function<void(const std::string&)> ready,
                             ArtPriority priority, const void* observer, bool probe_disk) {
	if (url.empty() || (!starts_with(url, "http://") && !starts_with(url, "https://"))) return "";
	std::string path = path_for(url, kind);
	std::lock_guard<std::mutex> lock(mutex_);
	if (stopping_) return "";
	const auto now = std::chrono::steady_clock::now();
	auto k = known_.find(path);
	if (k != known_.end()) {
		if (k->second.exists) return path;
		// Missing disk entries are eligible immediately. Actual network/decode
		// failures cool down, then may retry without restarting the app.
		if (k->second.failures && now < k->second.retry_at) return "";
	}
	auto pending = waiting_.find(path);
	if (pending == waiting_.end()) {
		if (probe_disk && file_exists(path)) {
			known_[path] = Known{true, 0, {}};
			return path;
		}
		// Bound speculative work when rapidly scrolling through many catalogs.
		if (queue_.size() >= 192) {
			auto drop = queue_.end();
			for (auto it = queue_.begin(); it != queue_.end(); ++it)
				if (drop == queue_.end() || art_priority(it->kind, it->priority) < art_priority(drop->kind, drop->priority)) drop = it;
			if (drop == queue_.end() || art_priority(kind, priority) < art_priority(drop->kind, drop->priority)) return "";
			auto found = waiting_.find(drop->path);
			if (found != waiting_.end()) {
				auto abandoned = std::move(found->second);
				waiting_.erase(found);
				if (!abandoned.empty()) g_tasks.post([abandoned = std::move(abandoned)] {
					for (const auto& subscriber : abandoned) subscriber.ready("");
				});
			}
			queue_.erase(drop);
		}
		pending = waiting_.emplace(path, std::vector<Subscriber>{}).first;
		const auto order = ++order_;
		queue_.push_back(Job{url, path, kind, priority, order, order});
		cv_.notify_one();
	} else {
		for (auto& job : queue_) if (job.path == path) {
			if (int(priority) > int(job.priority)) job.priority = priority;
			job.touched = ++order_;
			break;
		}
	}
	if (ready) {
		auto& waiters = pending->second;
		auto same = observer ? std::find_if(waiters.begin(), waiters.end(), [observer](const Subscriber& sub) {
			return sub.observer == observer;
		}) : waiters.end();
		if (same != waiters.end()) same->ready = std::move(ready);
		else waiters.push_back(Subscriber{observer, std::move(ready)});
	}
	return "";
}

std::string ArtCache::peek(const std::string& url, ArtKind kind) {
	if (url.empty()) return "";
	std::string path = path_for(url, kind);
	std::lock_guard<std::mutex> lock(mutex_);
	auto k = known_.find(path);
	if (k != known_.end()) return k->second.exists ? path : "";
	if (waiting_.find(path) != waiting_.end()) return "";
	if (file_exists(path)) {
		known_[path] = Known{true, 0, {}};
		return path;
	}
	// Remember a disk miss so rebuilding an off-screen row does not perform
	// the same stat() for every uncached cover. get() still queues it on demand.
	known_[path] = Known{};
	return "";
}

std::string ArtCache::peek_cached(const std::string& url, ArtKind kind) {
	if (url.empty()) return {};
	const auto path = path_for(url, kind);
	std::lock_guard lock(mutex_);
	const auto found = known_.find(path);
	return found != known_.end() && found->second.exists ? path : std::string{};
}

void ArtCache::clear_queue() {
	std::lock_guard<std::mutex> lock(mutex_);
	for (auto& j : queue_) waiting_.erase(j.path);
	queue_.clear();
}

void ArtCache::worker() {
	while (true) {
		Job job;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
			if (stopping_) return;
			auto chosen = queue_.begin();
			// One in five dispatches serves the oldest request. A selected hero
			// therefore responds first without starving the posters indefinitely.
			if (++dispatched_ % 5 != 0) {
				for (auto it = queue_.begin(); it != queue_.end(); ++it) {
					const int a = art_priority(it->kind, it->priority), b = art_priority(chosen->kind, chosen->priority);
					if (a > b || (a == b && a % 100 >= 30 && it->touched > chosen->touched)) chosen = it;
				}
			}
			job = std::move(*chosen);
			queue_.erase(chosen);
		}
		bool ok = false;
		try { ok = fetch(job); }
		catch (const std::exception& error) { dlog("Artwork request failed: %s", error.what()); }
		std::vector<Subscriber> waiters;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			auto& known = known_[job.path];
			known.exists = ok;
			if (ok) known.failures = 0;
			else {
				known.failures = std::min(known.failures + 1, 7u);
				known.retry_at = std::chrono::steady_clock::now() + std::chrono::seconds(std::min(300u, 5u << (known.failures - 1)));
			}
			auto it = waiting_.find(job.path);
			if (it != waiting_.end()) {
				waiters.swap(it->second);
				waiting_.erase(it);
			}
		}
		std::string path = ok ? job.path : "";
		if (!waiters.empty())
			g_tasks.post([waiters = std::move(waiters), path]() {
				for (auto& w : waiters) w.ready(path);
			});
	}
}

std::shared_ptr<ArtCache::Source> ArtCache::source(const std::string& url) {
	std::shared_ptr<Source> selected;
	{
		std::unique_lock<std::mutex> lock(mutex_);
		auto found = sources_.find(url);
		if (found != sources_.end()) {
			selected = found->second;
			selected->used = ++order_;
			cv_.wait(lock, [&] { return stopping_ || !selected->loading; });
			return stopping_ || !selected->ok ? nullptr : selected;
		}
		selected = std::make_shared<Source>();
		selected->used = ++order_;
		sources_[url] = selected;
	}
	HttpResponse result;
	try { result = http_get(url, 20, &cancel_, {}, 16u * 1024u * 1024u); }
	catch (const std::exception& error) { result.error = error.what(); }
	catch (...) { result.error = "image request failed"; }
	if (!result.ok()) dlog("art: %s: %s", http_log_target(url).c_str(), result.describe().c_str());
	{
		std::lock_guard<std::mutex> lock(mutex_);
		selected->loading = false;
		selected->ok = result.ok() && !stopping_;
		if (selected->ok) {
			selected->bytes = std::move(result.body);
			source_bytes_ += selected->bytes.size();
		} else sources_.erase(url);
		// Only compressed bytes are reused; finished decodes do not linger in
		// CPU memory. Shared owners keep in-flight variants valid during eviction.
		while (source_bytes_ > (16u << 20) || sources_.size() > 64) {
			auto oldest = sources_.end();
			for (auto it = sources_.begin(); it != sources_.end(); ++it)
				if (!it->second->loading && (oldest == sources_.end() || it->second->used < oldest->second->used)) oldest = it;
			if (oldest == sources_.end()) break;
			source_bytes_ -= oldest->second->bytes.size();
			sources_.erase(oldest);
		}
	}
	cv_.notify_all();
	return selected->ok ? selected : nullptr;
}

// ---------------------------------------------------------------------------
// Scaling

// Area-averaging resample of the source rectangle (sx, sy, sw, sh) into dw x dh.
static void resample(const unsigned char* src, int src_w, float sx, float sy, float sw, float sh,
                     unsigned char* dst, int dw, int dh, bool nearest) {
	float fx = sw / dw, fy = sh / dh;
	for (int y = 0; y < dh; y++) {
		for (int x = 0; x < dw; x++) {
			unsigned char* d = dst + (size_t(y) * dw + x) * 4;
			if (nearest || (fx <= 1.0f && fy <= 1.0f)) {
				// Upscaling (or QR codes): bilinear would blur, nearest is fine at these sizes.
				int ix = int(sx + (x + 0.5f) * fx), iy = int(sy + (y + 0.5f) * fy);
				const unsigned char* s = src + (size_t(iy) * src_w + ix) * 4;
				memcpy(d, s, 4);
				continue;
			}
			int x0 = int(sx + x * fx), x1 = std::max(x0 + 1, int(sx + (x + 1) * fx));
			int y0 = int(sy + y * fy), y1 = std::max(y0 + 1, int(sy + (y + 1) * fy));
			uint32_t acc[4] = {0, 0, 0, 0}, n = 0;
			for (int yy = y0; yy < y1; yy++) {
				const unsigned char* s = src + (size_t(yy) * src_w + x0) * 4;
				for (int xx = x0; xx < x1; xx++, s += 4) {
					// Weight colour by alpha so transparent pixels don't darken edges (logos).
					uint32_t a = s[3];
					acc[0] += s[0] * a;
					acc[1] += s[1] * a;
					acc[2] += s[2] * a;
					acc[3] += a;
					n++;
				}
			}
			if (acc[3]) {
				d[0] = (unsigned char)(acc[0] / acc[3]);
				d[1] = (unsigned char)(acc[1] / acc[3]);
				d[2] = (unsigned char)(acc[2] / acc[3]);
			} else {
				d[0] = d[1] = d[2] = 0;
			}
			d[3] = (unsigned char)(acc[3] / n);
		}
	}
}

static void round_corners(unsigned char* px, int w, int h, float r) {
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			float cx = -1, cy = -1;
			if (x < r && y < r) cx = r, cy = r;
			else if (x >= w - r && y < r) cx = w - r, cy = r;
			else if (x < r && y >= h - r) cx = r, cy = h - r;
			else if (x >= w - r && y >= h - r) cx = w - r, cy = h - r;
			if (cx < 0) continue;
			float dx = (x + 0.5f) - cx, dy = (y + 0.5f) - cy;
			float dist = std::sqrt(dx * dx + dy * dy);
			float cover = r - dist + 0.5f;  // 1px anti-aliased edge
			if (cover >= 1) continue;
			unsigned char* a = px + (size_t(y) * w + x) * 4 + 3;
			*a = cover <= 0 ? 0 : (unsigned char)(*a * cover);
		}
	}
}

static bool write_rgba(const std::string& path, const std::vector<unsigned char>& px, int w, int h) {
	// Keep atomic writes without assembling a second full image in a string.
	const std::string temporary = path + ".tmp";
	FILE* file = fopen(temporary.c_str(), "wb");
	if (!file) return false;
	uint32_t dims[2] = {uint32_t(w), uint32_t(h)};
	bool ok = fwrite("RGBA", 1, 4, file) == 4 && fwrite(dims, 1, sizeof(dims), file) == sizeof(dims) &&
	          fwrite(px.data(), 1, px.size(), file) == px.size();
	ok = (fclose(file) == 0) && ok;
	if (!ok) { remove(temporary.c_str()); return false; }
	if (rename(temporary.c_str(), path.c_str()) != 0) {
		remove(path.c_str());
		if (rename(temporary.c_str(), path.c_str()) != 0) { remove(temporary.c_str()); return false; }
	}
	return true;
}

static std::string download_poster_source(const std::string& url) {
	// Upgrade only known public poster-size routes. Addon artwork, overlays,
	// query strings and provider credentials otherwise keep their original URL.
	for (const char* scheme : {"https://", "http://"}) {
		const std::string metahub = std::string(scheme) + "images.metahub.space/poster/";
		for (const char* size : {"small/", "medium/"}) {
			const auto prefix = metahub + size;
			if (starts_with(url, prefix)) return metahub + "large/" + url.substr(prefix.size());
		}
		const std::string tmdb = std::string(scheme) + "image.tmdb.org/t/p/";
		for (const char* size : {"w92/", "w154/", "w185/", "w342/", "w500/", "original/"}) {
			const auto prefix = tmdb + size;
			if (starts_with(url, prefix)) return tmdb + "w780/" + url.substr(prefix.size());
		}
	}
	return url;
}

bool ArtCache::fetch(const Job& job) {
	// get_async intentionally leaves discovery of existing files to this worker.
	if (file_exists(job.path)) return true;
	const auto source_url = job.kind == ArtKind::DownloadPoster ? download_poster_source(job.url) : job.url;
	const auto compressed = source(source_url);
	if (!compressed) return false;
	int w, h;
	const char* why = "";
	const bool dimensions_ok = image_dimensions(compressed->bytes, w, h, &why);
	// Reserve source RGBA + one source-size scratch allowance + the largest
	// output before decoding. Ordinary concurrent estimates total <=64MiB;
	// a larger permitted source runs alone. HTTP and UI threads never wait here.
	auto admission = dimensions_ok ? decode_budget.acquire(size_t(w) * h * 8u + kLargestResampleBytes, &cancel_)
	                               : ArtDecodeBudget::Lease{};
	if (dimensions_ok && !admission) return false;
	auto decoded = dimensions_ok ? decode_owned(compressed->bytes, w, h, &why) : DecodedPixels(nullptr, stbi_image_free);
	if (!decoded) {
		dlog("art: can't decode %s (%s)", http_log_target(job.url).c_str(), why);
		// A CDN can temporarily return an HTML/error body with HTTP 200. Do
		// not let the compressed reuse cache make that failure permanent.
		std::lock_guard<std::mutex> lock(mutex_);
		auto cached = sources_.find(source_url);
		if (cached != sources_.end() && cached->second == compressed) {
			source_bytes_ -= cached->second->bytes.size();
			sources_.erase(cached);
		}
		return false;
	}
	const unsigned char* src = decoded.get();

	int dw, dh;
	bool crop = true, nearest = false;
	float radius = 0;
	switch (job.kind) {
	// Posters and thumbnails at exactly their on-screen size (dp * kUiScale),
	// so they're drawn unscaled.
	case ArtKind::Poster:
		dw = int(std::lround(182 * kUiScale)), dh = int(std::lround(277 * kUiScale));
		radius = 12 * kUiScale;
		break;
	case ArtKind::PosterLarge:  // .card .poster-img in board.rcss
		dw = int(std::lround(278 * kUiScale)), dh = int(std::lround(422 * kUiScale));
		radius = 14 * kUiScale;
		break;
	case ArtKind::DownloadPoster:
		// The gallery rounds corners when drawing. Store the sharp full cover,
		// independent of the legacy CSS scale and the current display mode.
		dw = kDownloadPosterWidth; dh = kDownloadPosterHeight;
		break;
	case ArtKind::Background: dw = 1280, dh = 720; break;
	case ArtKind::Backdrop: dw = 1920, dh = 1080; break;
	case ArtKind::Thumb: dw = 640; dh = 360; break;
	case ArtKind::Qr: dw = 400, dh = 400, nearest = true; break;
	case ArtKind::Icon:  // .addon-logo in browse.rcss
		dw = dh = int(std::lround(96 * kUiScale));
		radius = 18 * kUiScale;
		break;
	case ArtKind::Still:  // .preview-still in browse.rcss
		dw = int(std::lround(652 * kUiScale)), dh = int(std::lround(367 * kUiScale));
		radius = 16 * kUiScale;
		break;
	case ArtKind::LogoBox:  // .preview-logo in browse.rcss
		dw = int(std::lround(560 * kUiScale)), dh = int(std::lround(120 * kUiScale));
		break;
	case ArtKind::LaunchLogo: {
		// Remove only transparent padding. Preserve all visible logo pixels;
		// the renderer fits these actual dimensions in the available space.
		int left = w, top = h, right = -1, bottom = -1;
		for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x)
			if (src[(size_t(y) * w + x) * 4 + 3] > 8) {
				left = std::min(left, x); right = std::max(right, x);
				top = std::min(top, y); bottom = std::max(bottom, y);
			}
		if (right < left || bottom < top) return false;
		const int cw = right - left + 1, ch = bottom - top + 1;
		const double scale = std::min(1536.0 / cw, 864.0 / ch);
		dw = std::max(1, int(std::lround(cw * scale)));
		dh = std::max(1, int(std::lround(ch * scale)));
		std::vector<unsigned char> pixels(size_t(dw) * dh * 4);
		resample(src, w, float(left), float(top), float(cw), float(ch), pixels.data(), dw, dh, false);
		return write_rgba(job.path, pixels, dw, dh);
	}
	case ArtKind::Logo:
	default:
		crop = false;
		dh = std::min(150, h);
		dw = std::max(1, int(float(w) * dh / h));
		if (dw > 900) {
			dw = 900;
			dh = std::max(1, int(float(h) * dw / w));
		}
		break;
	}

	// Logos are often wide wordmarks: fit the whole picture inside the box on
	// transparency (addon icons centred, title logos left-aligned); only
	// square addon icons get rounded corners.
	if (job.kind == ArtKind::Icon || job.kind == ArtKind::LogoBox) {
		float aspect = float(w) / h;
		int fw = dw, fh = dh;
		if (aspect > float(dw) / dh) fh = std::max(1, int(std::lround(dw / aspect)));
		else fw = std::max(1, int(std::lround(dh * aspect)));
		std::vector<unsigned char> fit(size_t(fw) * fh * 4);
		resample(src, w, 0, 0, float(w), float(h), fit.data(), fw, fh, false);
		if (job.kind == ArtKind::Icon && aspect > 0.9f && aspect < 1.1f) round_corners(fit.data(), fw, fh, radius);
		std::vector<unsigned char> px(size_t(dw) * dh * 4, 0);
		int ox = job.kind == ArtKind::LogoBox ? 0 : (dw - fw) / 2, oy = (dh - fh) / 2;
		for (int y = 0; y < fh; y++)
			memcpy(&px[(size_t(y + oy) * dw + ox) * 4], &fit[size_t(y) * fw * 4], size_t(fw) * 4);
		return write_rgba(job.path, px, dw, dh);
	}

	float sx = 0, sy = 0, sw = float(w), sh = float(h);
	if (crop) {
		float target = float(dw) / dh, aspect = float(w) / h;
		if (aspect > target) {
			sw = h * target;
			sx = (w - sw) / 2;
		} else {
			sh = w / target;
			sy = (h - sh) / 2;
		}
	}
	std::vector<unsigned char> px(size_t(dw) * dh * 4);
	resample(src, w, sx, sy, sw, sh, px.data(), dw, dh, nearest);
	if (radius > 0) round_corners(px.data(), dw, dh, radius);
	return write_rgba(job.path, px, dw, dh);
}

bool load_image_rgba(const std::string& path, std::vector<unsigned char>& pixels, int& w, int& h) {
	w = h = 0;
	pixels.clear();
	struct stat info{};
	if (stat(path.c_str(), &info) != 0 || info.st_size < 0 ||
	    uint64_t(info.st_size) > 64u * 1024u * 1024u + 12u) return false;
	// Native artwork is already decoded. Read it straight into the worker's
	// upload buffer, avoiding a second full-size string/copy for every hero.
	FILE* file = fopen(path.c_str(), "rb");
	if (!file) return false;
	unsigned char header[12]{};
	const size_t header_size = fread(header, 1, sizeof(header), file);
	if (header_size == sizeof(header) && memcmp(header, "RGBA", 4) == 0) {
		uint32_t dims[2];
		memcpy(dims, header + 4, 8);
		if (!dims[0] || !dims[1] || dims[0] > 4096 || dims[1] > 4096) { fclose(file); return false; }
		const size_t need = size_t(dims[0]) * dims[1] * 4;
		if (uint64_t(info.st_size) != 12u + need) { fclose(file); return false; }
		try { pixels.resize(need); }
		catch (...) { fclose(file); throw; }
		const bool read = fread(pixels.data(), 1, need, file) == need;
		fclose(file);
		if (!read) { pixels.clear(); return false; }
		w = int(dims[0]); h = int(dims[1]);
		return true;
	}
	fclose(file);
	std::string data;
	if (!read_file(path, data)) return false;
	return decode_image(data, pixels, w, h, nullptr);
}
