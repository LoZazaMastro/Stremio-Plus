#include "download_manager.h"
#include "download_transfer.h"
#include "download_metadata.h"
#include "download_artwork.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <new>
#include <map>
#include <set>
#include <sys/stat.h>
#include <sys/file.h>
#if defined(PLATFORM_PS5_NATIVE)
#include <sys/mount.h>
extern "C" int relocation_fstatfs(int, struct statfs*) __asm__("_fstatfs");
#elif defined(__linux__)
#include <sys/vfs.h>
#else
#include <sys/mount.h>
#endif
#include <limits>
#include <thread>
#include <unistd.h>

#ifdef PLATFORM_PS5_NATIVE
extern "C" int sceKernelGetdents(int, char*, int);
#endif

namespace {
constexpr size_t kMaxJobs = 128, kMaxManifest = 256 * 1024;
constexpr size_t kMaxPendingBytes = 4 * 1024 * 1024;
constexpr int64_t kMaxArtwork = 32 * 1024 * 1024;
using Clock = std::chrono::steady_clock;

#ifdef PLATFORM_PS5_NATIVE
struct DownloadDIR {
	int fd = -1;
	std::array<char, 64u << 10> bytes{};
	size_t position = 0, count = 0;
	dirent entry{};
};
DownloadDIR* download_opendir(const char* path) {
	const int fd = ::open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	if (fd < 0) return nullptr;
	auto* directory = new (std::nothrow) DownloadDIR;
	if (!directory) { ::close(fd); errno = ENOMEM; return nullptr; }
	directory->fd = fd; return directory;
}
const dirent* download_readdir(DownloadDIR* directory) {
	if (directory->position == directory->count) {
		const int count = sceKernelGetdents(directory->fd, directory->bytes.data(), int(directory->bytes.size()));
		if (count <= 0 || count > int(directory->bytes.size())) {
			errno = count < 0 ? int(unsigned(count) & 0xffffu) : count == 0 ? 0 : EIO;
			return nullptr;
		}
		directory->position = 0; directory->count = size_t(count);
	}
	const auto left = directory->count - directory->position;
	constexpr auto name_offset = offsetof(dirent, d_name);
	if (left <= name_offset) { errno = EIO; return nullptr; }
	const char* bytes = directory->bytes.data() + directory->position;
	uint16_t length = 0;
	std::memcpy(&length, bytes + offsetof(dirent, d_reclen), sizeof(length));
	if (length <= name_offset || length > left) { errno = EIO; return nullptr; }
	const char* name = bytes + name_offset;
	const auto* end = static_cast<const char*>(std::memchr(name, 0, length - name_offset));
	if (!end || size_t(end - name) >= sizeof(directory->entry.d_name)) { errno = EIO; return nullptr; }
	directory->entry = {};
	std::memcpy(directory->entry.d_name, name, size_t(end - name));
	directory->position += length;
	errno = 0; return &directory->entry;
}
int download_closedir(DownloadDIR* directory) {
	const int result = ::close(directory->fd); delete directory; return result;
}
#else
using DownloadDIR = DIR;
DownloadDIR* download_opendir(const char* path) { return ::opendir(path); }
const dirent* download_readdir(DownloadDIR* directory) { return ::readdir(directory); }
int download_closedir(DownloadDIR* directory) { return ::closedir(directory); }
#endif

bool storage_failure(std::string* error, const char* operation, const std::string& path, int code) {
	if (error) *error = std::string(operation) + " failed for " + path + " (errno " +
		std::to_string(code) + ": " + std::strerror(code) + ")";
	errno = code;
	return false;
}

bool safe_id(const std::string& value) {
	return value.size() == 33 && value[0] == 'd' &&
		std::all_of(value.begin() + 1, value.end(), [](unsigned char c) {
			return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
		});
}

bool safe_extension(const std::string& value) {
	return value.size() >= 2 && value.size() <= 8 && value[0] == '.' &&
		std::all_of(value.begin() + 1, value.end(), [](unsigned char c) {
			return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z');
		});
}

std::string normalized_root(std::string path) {
	while (path.size() > 1 && path.back() == '/') path.pop_back();
	if (path.size() < 2 || path.size() > 1023 || path[0] != '/' || path.find('\0') != std::string::npos ||
		path.find('\\') != std::string::npos || std::any_of(path.begin(), path.end(), [](unsigned char c) { return c < 32 || c == 127; })) return {};
	for (size_t begin = 1; begin < path.size();) {
		const auto end = path.find('/', begin);
		const auto part = path.substr(begin, end == std::string::npos ? end : end - begin);
		if (part.empty() || part == "." || part == "..") return {};
		if (end == std::string::npos) break;
		begin = end + 1;
	}
#if defined(PLATFORM_PS5_NATIVE)
	if (path != "/data" && path.rfind("/data/", 0) != 0 && path.rfind("/mnt/", 0) != 0) return {};
#endif
	return path;
}

// Every parent is checked as well as the last component. This also rejects
// traversal in a supplied data directory and refuses a redirected download root.
bool safe_directory(const std::string& path, std::string* error = nullptr) {
	if (path.empty() || path[0] != '/') return storage_failure(error, "validate directory", path, EINVAL);
	std::string current;
	for (size_t begin = 1; begin < path.size();) {
		const size_t end = path.find('/', begin);
		const auto part = path.substr(begin, end == std::string::npos ? end : end - begin);
		if (part.empty() || part == "." || part == "..") return storage_failure(error, "validate directory", path, EINVAL);
		current += '/' + part;
		struct stat st{};
		if (::lstat(current.c_str(), &st) != 0) {
#if defined(PLATFORM_PS5_NATIVE)
			// Some mounted PS5 volumes deny lstat while permitting an opened
			// directory. O_NOFOLLOW on each component still rejects symlinks.
			const int code = errno;
			const int fd = (code == EPERM || code == EACCES) ? ::open(current.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW) : -1;
			if (fd < 0) return storage_failure(error, "lstat/open directory", current, code);
			const bool ok = ::fstat(fd, &st) == 0 && S_ISDIR(st.st_mode);
			const int failure = errno;
			::close(fd);
			if (!ok) return storage_failure(error, "fstat directory", current, failure ? failure : ENOTDIR);
#else
			return storage_failure(error, "lstat", current, errno);
#endif
		}
		if (!S_ISDIR(st.st_mode)) return storage_failure(error, "validate directory", current, ENOTDIR);
		if (end == std::string::npos) break;
		begin = end + 1;
	}
	return true;
}

bool create_private_directory(const std::string& path, std::string* error = nullptr, bool* created = nullptr) {
	if (created) *created = false;
	const size_t slash = path.rfind('/');
	if (slash == std::string::npos) return storage_failure(error, "validate directory", path, EINVAL);
	if (!safe_directory(path.substr(0, slash), error)) return false;
	struct stat existing{};
	if (::lstat(path.c_str(), &existing) == 0) return safe_directory(path, error);
	if (errno != ENOENT) return storage_failure(error, "lstat", path, errno);
	const bool fresh = ::mkdir(path.c_str(), 0700) == 0;
	if (created) *created = fresh;
	if (!fresh && errno != EEXIST) return storage_failure(error, "mkdir", path, errno);
	if (!safe_directory(path, error)) return false;
	return !fresh || ::chmod(path.c_str(), 0700) == 0 || storage_failure(error, "chmod", path, errno);
}

bool share_download_directory(const std::string& path, std::string* error = nullptr) {
	if (!safe_directory(path, error)) return false;
	const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	if (fd < 0) return storage_failure(error, "open download directory", path, errno);
	struct Close { int fd; ~Close() { ::close(fd); } } close{fd};
	struct stat before{}, after{}, current{};
	if (::fstat(fd, &before) != 0) return storage_failure(error, "fstat download directory", path, errno);
	if (!S_ISDIR(before.st_mode)) return storage_failure(error, "validate download directory", path, ENOTDIR);
	if ((before.st_mode & 07777) != 0777 && ::fchmod(fd, 0777) != 0)
		return storage_failure(error, "fchmod download directory", path, errno);
	if (::fstat(fd, &after) != 0) return storage_failure(error, "read download directory permissions", path, errno);
	if ((after.st_mode & 07777) != 0777)
		return storage_failure(error, "download directory did not retain required 0777 permissions", path, EPERM);
	const int current_fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	if (current_fd < 0) return storage_failure(error, "reopen download directory", path, errno);
	const bool inspected = ::fstat(current_fd, &current) == 0;
	const int failure = errno;
	::close(current_fd);
	if (!inspected) return storage_failure(error, "recheck download directory", path, failure);
	if (!S_ISDIR(current.st_mode) || before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
		current.st_dev != after.st_dev || current.st_ino != after.st_ino)
		return storage_failure(error, "download directory identity changed", path, EIO);
	return true;
}

bool create_download_directory(const std::string& path, std::string* error = nullptr, bool* created = nullptr) {
	// Keep the new directory private until its identity has been checked, then
	// explicitly apply and read back the loader's required mode despite umask.
	return create_private_directory(path, error, created) && share_download_directory(path, error);
}

int64_t regular_size(const std::string& path) {
	struct stat st{};
	return ::lstat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) ? st.st_size : -1;
}

bool write_all(int fd, const void* bytes, size_t length) {
	const auto* data = static_cast<const unsigned char*>(bytes);
	while (length) {
		const ssize_t written = ::write(fd, data, length);
		if (written < 0 && errno == EINTR) continue;
		if (written <= 0) { if (written == 0) errno = EIO; return false; }
		data += written;
		length -= static_cast<size_t>(written);
	}
	return true;
}

bool sync_directory(const std::string& path, std::string* error = nullptr) {
	const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	if (fd < 0) return storage_failure(error, "open directory", path, errno);
	bool ok = ::fsync(fd) == 0 || storage_failure(error, "fsync directory", path, errno);
	if (::close(fd) != 0 && ok) ok = storage_failure(error, "close directory", path, errno);
	return ok;
}

bool atomic_private_write(const std::string& path, const std::string& bytes, std::string* error = nullptr,
	bool preserve_temporary = false) {
	if (!safe_directory(path.substr(0, path.rfind('/')), error)) return false;
	const std::string temporary = path + (preserve_temporary ? ".new" : ".tmp");
	const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
	if (fd < 0) return storage_failure(error, "open", temporary, errno);
	bool ok = ::fchmod(fd, 0600) == 0 || storage_failure(error, "fchmod", temporary, errno);
	if (ok && !write_all(fd, bytes.data(), bytes.size())) ok = storage_failure(error, "write", temporary, errno);
	if (ok && ::fsync(fd) != 0) ok = storage_failure(error, "fsync", temporary, errno);
	int failure = ok ? 0 : errno;
	if (::close(fd) != 0 && ok) { failure = errno; ok = storage_failure(error, "close", temporary, failure); }
	if (ok && ::rename(temporary.c_str(), path.c_str()) != 0) ok = storage_failure(error, "rename", path, errno);
	if (ok) ok = sync_directory(path.substr(0, path.rfind('/')), error);
	if (!ok) { if (!failure) failure = errno; ::unlink(temporary.c_str()); errno = failure; }
	return ok;
}

bool probe_download_storage(const std::string& root, std::string* error) {
	// Completed downloads must remain discoverable as well as writable. Some
	// native mounts permit file writes while directory enumeration is denied.
	DownloadDIR* directory = download_opendir(root.c_str());
	if (!directory) return storage_failure(error, "opendir", root, errno);
	errno = 0;
	const bool listed = download_readdir(directory) != nullptr || errno == 0;
	const int listing_error = errno;
	const int closed = download_closedir(directory);
	if (!listed) return storage_failure(error, "readdir", root, listing_error);
	if (closed != 0) return storage_failure(error, "closedir", root, errno);
	// The selected directory can contain unrelated user files. A probe must
	// exclusively create its own name and never truncate an existing file.
	static std::atomic<uint64_t> sequence{0};
	const auto path = root + "/.stremio-storage-check-" + std::to_string(::getpid()) + '-' + std::to_string(++sequence);
	const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
	if (fd < 0) return storage_failure(error, "open probe", path, errno);
	bool ok = ::fchmod(fd, 0600) == 0 || storage_failure(error, "fchmod", path, errno);
	constexpr char message[] = "Stremio download storage check\n";
	if (ok && !write_all(fd, message, sizeof(message) - 1)) ok = storage_failure(error, "write", path, errno);
	if (ok && ::fsync(fd) != 0) ok = storage_failure(error, "fsync", path, errno);
	int failure = ok ? 0 : errno;
	if (::close(fd) != 0 && ok) { failure = errno; ok = storage_failure(error, "close", path, failure); }
	if (::unlink(path.c_str()) != 0 && ok) { failure = errno; ok = storage_failure(error, "unlink", path, failure); }
	if (!ok) errno = failure;
	return ok;
}

bool read_manifest(const std::string& path, json& out, size_t limit = kMaxManifest) {
	const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW);
	if (fd < 0) return false;
	struct stat st{};
	bool ok = ::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 &&
		st.st_size <= static_cast<int64_t>(limit);
	std::string bytes;
	if (ok) {
		bytes.resize(static_cast<size_t>(st.st_size));
		size_t offset = 0;
		while (offset < bytes.size()) {
			const ssize_t count = ::read(fd, bytes.data() + offset, bytes.size() - offset);
			if (count < 0 && errno == EINTR) continue;
			if (count <= 0) { ok = false; break; }
			offset += static_cast<size_t>(count);
		}
	}
	::close(fd);
	if (!ok) return false;
	// The inventory schema is shallow; reject hostile nesting before the JSON
	// parser can allocate an unbounded stack of arrays from a damaged backup.
	int depth = 0;
	bool quoted = false, escaped = false;
	for (const char c : bytes) {
		if (quoted) {
			if (escaped) escaped = false;
			else if (c == '\\') escaped = true;
			else if (c == '"') quoted = false;
		} else if (c == '"') quoted = true;
		else if (c == '{' || c == '[') { if (++depth > 32) return false; }
		else if (c == '}' || c == ']') --depth;
	}
	out = json::parse(bytes, nullptr, false);
	return out.is_object();
}

int64_t directory_bytes(const std::string& path) {
	if (!safe_directory(path)) return -1;
	DownloadDIR* directory = download_opendir(path.c_str());
	if (!directory) return -1;
	int64_t total = 0;
	bool complete = true;
	size_t count = 0;
	for (;;) {
		errno = 0;
		const auto* entry = download_readdir(directory);
		if (!entry) { if (errno) complete = false; break; }
		const std::string name = entry->d_name;
		if (name == "." || name == "..") continue;
		struct stat st{};
		if (++count > 512 || name.find('/') != std::string::npos ||
			::lstat((path + '/' + name).c_str(), &st) != 0 || S_ISDIR(st.st_mode)) { complete = false; break; }
		if (S_ISREG(st.st_mode)) {
			if (st.st_blocks < 0) { complete = false; break; }
			const int64_t allocated = st.st_blocks > INT64_MAX / 512 ? INT64_MAX : st.st_blocks * 512;
			total = allocated > INT64_MAX - total ? INT64_MAX : total + allocated;
		}
	}
	if (download_closedir(directory) != 0) complete = false;
	return complete ? total : -1;
}

std::string json_bytes(const json& value) {
	return value.dump(-1, ' ', false, json::error_handler_t::replace);
}

std::string text_value(const json& value, const char* name, size_t limit = 16384) {
	const auto it = value.find(name);
	if (it == value.end() || !it->is_string()) return {};
	const auto& result = it->get_ref<const std::string&>();
	return result.size() <= limit ? result : std::string{};
}

int64_t integer_value(const json& value, const char* name, int64_t fallback = -1) {
	const auto it = value.find(name);
	if (it == value.end() || !it->is_number_integer()) return fallback;
	if (it->is_number_unsigned() && it->get<uint64_t>() > uint64_t(INT64_MAX)) return fallback;
	return it->get<int64_t>();
}

bool boolean_value(const json& value, const char* name, bool fallback = false) {
	const auto it = value.find(name);
	return it != value.end() && it->is_boolean() ? it->get<bool>() : fallback;
}

bool read_directory_registry(const std::string& path, json& result) {
	json value;
	if (!read_manifest(path, value) || integer_value(value, "version") != 1 ||
		!value.contains("roots") || !value["roots"].is_array() || value["roots"].size() > 32 ||
		!value.contains("current") || !value["current"].is_string()) return false;
	const auto current_raw = value["current"].get<std::string>();
	const auto current = normalized_root(current_raw);
	if (!current_raw.empty() && current.empty()) return false;
	std::vector<std::string> history;
	for (const auto& item : value["roots"]) {
		if (!item.is_string()) return false;
		const auto path = normalized_root(item.get<std::string>());
		if (path.empty()) return false;
		if (std::find(history.begin(), history.end(), path) == history.end()) history.push_back(path);
	}
	if (!current.empty() && std::find(history.begin(), history.end(), current) == history.end()) return false;
	result = json{{"version", 1}, {"current", current}, {"roots", history}};
	return true;
}

std::vector<std::string> string_list(const json& value, const char* name) {
	std::vector<std::string> result;
	const auto it = value.find(name);
	if (it == value.end() || !it->is_array() || it->size() > 64) return result;
	size_t total = 0;
	for (const auto& item : *it) {
		if (!item.is_string()) return {};
		const auto s = item.get<std::string>();
		if (s.size() > 16384 || (total += s.size()) > 65536) return {};
		result.push_back(s);
	}
	return result;
}

json stream_json(const Stream& s) {
	return {{"name", s.name}, {"description", s.description}, {"addon", s.addon},
		{"url", s.url}, {"info_hash", s.info_hash}, {"file_idx", s.file_idx},
		{"sources", s.sources}, {"filename", s.filename}, {"request_headers", s.request_headers},
		{"kind", static_cast<int>(s.kind)}, {"video_size", s.has_video_size ? s.video_size : 0}};
}

Stream parse_stream(const json& value) {
	Stream s;
	if (!value.is_object()) return s;
	s.name = text_value(value, "name"); s.description = text_value(value, "description");
	s.addon = text_value(value, "addon"); s.url = text_value(value, "url");
	s.info_hash = text_value(value, "info_hash", 64); s.filename = text_value(value, "filename");
	const int64_t index = integer_value(value, "file_idx");
	s.file_idx = index >= -1 && index <= INT32_MAX ? static_cast<int>(index) : -1;
	s.sources = string_list(value, "sources"); s.request_headers = string_list(value, "request_headers");
	const auto kind = integer_value(value, "kind", static_cast<int>(StreamKind::Unsupported));
	if (kind >= 0 && kind <= static_cast<int>(StreamKind::Unsupported)) s.kind = static_cast<StreamKind>(kind);
	const int64_t size = integer_value(value, "video_size", 0);
	s.video_size = size > 0 ? static_cast<uint64_t>(size) : 0; s.has_video_size = s.video_size > 0;
	return s;
}

std::string digest(const std::string& value) {
	uint64_t a = 14695981039346656037ULL, b = 1099511628211ULL;
	for (unsigned char c : value) { a = (a ^ c) * 1099511628211ULL; b = (b ^ c) * 14029467366897019727ULL; }
	char output[33];
	std::snprintf(output, sizeof(output), "%016llx%016llx", static_cast<unsigned long long>(a),
		static_cast<unsigned long long>(b));
	return output;
}

bool torrent_stream(const Stream& s) { return !s.info_hash.empty(); }

std::string download_source_identity(const DownloadRequest& request) {
	return digest(json_bytes(json{{"media_id", request.media_id}, {"type", request.type},
		{"video_id", request.video_id}, {"url", request.stream.url}, {"info_hash", request.stream.info_hash},
		{"file_idx", request.stream.file_idx}, {"season", request.season}, {"episode", request.episode},
		{"headers", request.stream.request_headers}}));
}

Stream download_stream(const Stream& source) {
	Stream result;
	result.name = source.name; result.description = source.description; result.addon = source.addon;
	result.url = source.url; result.info_hash = source.info_hash; result.file_idx = source.file_idx;
	result.sources = source.sources; result.filename = source.filename; result.request_headers = source.request_headers;
	result.kind = source.kind; result.video_size = source.video_size; result.has_video_size = source.has_video_size;
	return result;
}

bool valid_request(const DownloadRequest& r, std::string& error) {
	if (r.live) { error = "Live streams cannot be downloaded for offline viewing."; return false; }
	if (r.media_id.empty() || r.type.empty() || r.title.empty() || r.media_id.size() > 2048 || r.poster_url.size() > 16384 ||
		r.thumbnail_url.size() > 16384 || r.thumbnail_path.size() > 16384 ||
		r.poster_path.size() > 16384 || r.background_path.size() > 16384 || r.logo_path.size() > 16384 ||
		r.type.size() > 64 || r.video_id.size() > 2048 || r.title.size() > 4096 || r.subtitle.size() > 8192 || r.episode_title.size() > 4096) {
		error = "The video metadata is not valid for downloading."; return false;
	}
	const auto& s = r.stream;
	if (!s.playable() || s.url.size() > 16384 || s.info_hash.size() > 64 ||
		(!torrent_stream(s) && s.url.rfind("http://", 0) != 0 && s.url.rfind("https://", 0) != 0)) {
		error = "This source does not support local downloads."; return false;
	}
	const auto valid_list = [](const std::vector<std::string>& values) {
		if (values.size() > 64) return false;
		size_t total = 0;
		for (const auto& value : values) if (value.size() > 16384 || (total += value.size()) > 65536) return false;
		return true;
	};
	if (!valid_list(s.sources) || !valid_list(s.request_headers) || s.name.size() > 16384 ||
		s.description.size() > 16384 || s.addon.size() > 16384 || s.filename.size() > 16384 ||
		json_bytes(stream_json(s)).size() > 192 * 1024) {
		error = "The source configuration is too large."; return false;
	}
	error.clear();
	return true;
}

std::string safe_error(std::string error) {
	if (error.empty()) return "Download failed. You can retry.";
	if (error.size() > 512 || error.find("://") != std::string::npos ||
		error.find("Bearer ") != std::string::npos || error.find("magnet:") != std::string::npos)
		return "Download failed. You can retry.";
	return error;
}

void update_fraction(DownloadEntry& entry) {
	entry.done = std::max<int64_t>(0, entry.done);
	entry.progress = entry.state == DownloadState::Complete ? 1.0 :
		(entry.total > 0 ? std::clamp(double(entry.done) / double(entry.total), 0.0, 1.0) : -1.0);
}

bool artwork_dimensions(const std::string& path, int& width, int& height) {
	width = height = 0;
	if (path.empty()) return false;
	const int input = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW);
	if (input < 0) return false;
	struct stat st{};
	std::array<unsigned char, 12> header{};
	bool valid = ::fstat(input, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 12 &&
		st.st_size <= kMaxArtwork && ::read(input, header.data(), header.size()) == 12;
	::close(input);
	uint32_t w = 0, h = 0;
	if (valid) {
		std::memcpy(&w, header.data() + 4, 4); std::memcpy(&h, header.data() + 8, 4);
		valid = std::memcmp(header.data(), "RGBA", 4) == 0 && w > 0 && h > 0 &&
			w <= 4096 && h <= 4096 && 12 + int64_t(w) * h * 4 == st.st_size;
	}
	if (valid) { width = int(w); height = int(h); }
	return valid;
}

bool copy_artwork(const std::string& source, const std::string& destination) {
	if (source.empty()) return false;
	const int input = ::open(source.c_str(), O_RDONLY | O_NOFOLLOW);
	if (input < 0) return false;
	struct stat st{};
	std::array<unsigned char, 12> header{};
	bool ok = ::fstat(input, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 12 &&
		st.st_size <= kMaxArtwork && ::read(input, header.data(), header.size()) == 12;
	uint32_t width = 0, height = 0;
	if (ok) {
		std::memcpy(&width, header.data() + 4, 4); std::memcpy(&height, header.data() + 8, 4);
		ok = std::memcmp(header.data(), "RGBA", 4) == 0 && width > 0 && height > 0 &&
			width <= 4096 && height <= 4096 && 12 + int64_t(width) * height * 4 == st.st_size;
	}
	int output = -1;
	const auto temporary = destination + ".tmp";
	if (ok) output = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
	ok = ok && output >= 0;
	if (ok) ok = ::fchmod(output, 0600) == 0 && write_all(output, header.data(), header.size());
	std::array<unsigned char, 65536> buffer{};
	while (ok) {
		const ssize_t count = ::read(input, buffer.data(), buffer.size());
		if (count < 0 && errno == EINTR) continue;
		if (count == 0) break;
		if (count < 0 || !write_all(output, buffer.data(), static_cast<size_t>(count))) ok = false;
	}
	::close(input);
	if (output >= 0) { if (ok) ok = ::fsync(output) == 0; if (::close(output) != 0) ok = false; }
	if (ok) ok = ::rename(temporary.c_str(), destination.c_str()) == 0;
	if (!ok) ::unlink(temporary.c_str());
	return ok;
}

// Job storage is deliberately flat. Never descend into a directory supplied by
// a manifest or follow a symlink. A symlink itself can be unlinked safely.
bool remove_flat_directory(const std::string& root, const std::string& id) {
	if (!safe_id(id) || !safe_directory(root)) return false;
	const auto path = root + '/' + id;
	struct stat original{};
	if (::lstat(path.c_str(), &original) != 0) return errno == ENOENT;
	if (!S_ISDIR(original.st_mode)) return false;
	DownloadDIR* directory = download_opendir(path.c_str());
	if (!directory) return false;
	std::vector<std::string> files;
	bool ok = true;
	for (;;) {
		errno = 0;
		const auto* entry = download_readdir(directory);
		if (!entry) { if (errno) ok = false; break; }
		const std::string name = entry->d_name;
		if (name == "." || name == "..") continue;
		struct stat st{};
		if (name.find('/') != std::string::npos || files.size() >= 512 ||
			::lstat((path + '/' + name).c_str(), &st) != 0 || S_ISDIR(st.st_mode)) { ok = false; break; }
		files.push_back(name);
	}
	download_closedir(directory);
	if (!ok) return false;
	for (const auto& name : files) {
		struct stat current{};
		if (!safe_directory(root) || ::lstat(path.c_str(), &current) != 0 ||
			!S_ISDIR(current.st_mode) || current.st_ino != original.st_ino || current.st_dev != original.st_dev)
			return false;
		if (::unlink((path + '/' + name).c_str()) != 0 && errno != ENOENT) return false;
	}
	return ::rmdir(path.c_str()) == 0 || errno == ENOENT;
}

struct MoveFd {
 int fd = -1;
 explicit MoveFd(int value = -1) : fd(value) {}
 ~MoveFd() { if (fd >= 0) ::close(fd); }
 MoveFd(const MoveFd&) = delete;
 MoveFd& operator=(const MoveFd&) = delete;
};
struct MoveFile { std::string name; int64_t size; uint64_t device, inode; };
bool move_identity(const std::string& path, uint64_t device, uint64_t inode) {
 struct stat st{};
 return safe_directory(path) && ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode) &&
  uint64_t(st.st_dev) == device && uint64_t(st.st_ino) == inode;
}
bool move_inventory(const std::string& path, std::vector<MoveFile>& files, int64_t& bytes, std::string& error) {
 if (!safe_directory(path, &error)) return false;
 DownloadDIR* directory = download_opendir(path.c_str());
 if (!directory) return storage_failure(&error, "open relocation directory", path, errno);
 bool ok = true; bytes = 0;
 for (;;) {
  errno = 0; const auto* entry = download_readdir(directory);
  if (!entry) { if (errno) ok = storage_failure(&error, "read relocation directory", path, errno); break; }
  const std::string name = entry->d_name;
  if (name == "." || name == "..") continue;
  struct stat st{};
  if (name.empty() || name.find('/') != std::string::npos || name.find('\\') != std::string::npos ||
   files.size() >= 512 || ::lstat((path + '/' + name).c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_size < 0 ||
   st.st_size > std::numeric_limits<int64_t>::max() - bytes) {
   ok = storage_failure(&error, "validate relocation file", path + '/' + name, EINVAL); break;
  }
  files.push_back({name, st.st_size, uint64_t(st.st_dev), uint64_t(st.st_ino)}); bytes += st.st_size;
 }
 const int code = errno;
 if (download_closedir(directory) != 0 && ok) ok = storage_failure(&error, "close relocation directory", path, errno);
 if (!ok) errno = code;
 std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
 return ok;
}
bool move_read(int fd, void* bytes, size_t count) {
 auto* data = static_cast<char*>(bytes);
 while (count) {
  const auto size = ::read(fd, data, count);
  if (size < 0 && errno == EINTR) continue;
  if (size <= 0) { if (!size) errno = EIO; return false; }
  data += size; count -= size_t(size);
 }
 return true;
}
// Resume a staged file only after verifying its complete existing prefix. The
// second full comparison is intentionally independent of the write buffer.
bool move_copy_file(const std::string& source, const std::string& target, const MoveFile& file,
 const std::function<bool(int64_t, bool)>& progress, std::string& error) {
 MoveFd input(::open(source.c_str(), O_RDONLY | O_NOFOLLOW));
 if (input.fd < 0) return storage_failure(&error, "open relocation source", source, errno);
 struct stat original{};
 if (::fstat(input.fd, &original) != 0 || !S_ISREG(original.st_mode) || original.st_nlink != 1 || original.st_size != file.size ||
  uint64_t(original.st_dev) != file.device || uint64_t(original.st_ino) != file.inode)
  return storage_failure(&error, "verify relocation source", source, EIO);
 MoveFd output(::open(target.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW, 0600));
 if (output.fd < 0) return storage_failure(&error, "open relocation destination", target, errno);
 struct stat existing{};
 if (::fstat(output.fd, &existing) != 0 || !S_ISREG(existing.st_mode) || existing.st_nlink != 1 || existing.st_size < 0 || existing.st_size > file.size)
  return storage_failure(&error, "verify staged file", target, EINVAL);
 if (::fchmod(output.fd, 0600) != 0) return storage_failure(&error, "fchmod staged file", target, errno);
 std::vector<char> a(1024 * 1024), b(a.size());
 int64_t position = 0;
 while (position < existing.st_size) {
  const size_t count = size_t(std::min<int64_t>(int64_t(a.size()), existing.st_size - position));
  if (!move_read(input.fd, a.data(), count) || !move_read(output.fd, b.data(), count) || std::memcmp(a.data(), b.data(), count))
   return storage_failure(&error, "verify staged prefix", target, EIO);
  position += int64_t(count);
  if (!progress(position, true)) return storage_failure(&error, "interrupt relocation", target, ECANCELED);
 }
 while (position < file.size) {
  const size_t count = size_t(std::min<int64_t>(int64_t(a.size()), file.size - position));
  if (!move_read(input.fd, a.data(), count)) return storage_failure(&error, "read relocation source", source, errno);
  if (!write_all(output.fd, a.data(), count)) return storage_failure(&error, "write relocation destination", target, errno);
  position += int64_t(count);
  if (!progress(position, false)) return storage_failure(&error, "interrupt relocation", target, ECANCELED);
 }
 if (::fsync(output.fd) != 0) return storage_failure(&error, "fsync relocated file", target, errno);
 if (::lseek(input.fd, 0, SEEK_SET) < 0 || ::lseek(output.fd, 0, SEEK_SET) < 0)
  return storage_failure(&error, "seek relocation verification", target, errno);
 position = 0;
 while (position < file.size) {
  const size_t count = size_t(std::min<int64_t>(int64_t(a.size()), file.size - position));
  if (!move_read(input.fd, a.data(), count) || !move_read(output.fd, b.data(), count) || std::memcmp(a.data(), b.data(), count))
   return storage_failure(&error, "verify relocated bytes", target, EIO);
  position += int64_t(count);
  if (!progress(file.size, true)) return storage_failure(&error, "interrupt relocation", target, ECANCELED);
 }
 struct stat after{};
 if (::fstat(input.fd, &after) != 0 || after.st_size != original.st_size || after.st_mtime != original.st_mtime || after.st_ctime != original.st_ctime)
  return storage_failure(&error, "source changed during relocation", source, EBUSY);
 return true;
}

} // namespace

struct DownloadManager::Impl {
	struct Job {
		DownloadRequest request;
		DownloadEntry entry;
		std::string identity, directory, storage_root, folder_id, media_name, manifest_source;
		bool removing = false, artwork_pending = true, artwork_busy = false;
		bool artwork_manifest_dirty = false, metadata_dirty = false;
		uint64_t artwork_generation = 0;
		int64_t created_ms = 0;
		Clock::time_point last_progress{};
		Clock::time_point artwork_retry_at{};
		std::shared_ptr<GrowingFileState> playback = std::make_shared<GrowingFileState>();
		uint64_t playback_generation = 0;
		bool progress_ready = false;
		int torrent_file_idx = -1;
	};
	mutable std::mutex mutex;
	// New-job storage has its own lifecycle gate. Committing a waiting job must
	// not hold the live progress mutex or stall the active transfer's callback.
	std::mutex lifecycle_mutex;
	// Storage still uses mutex to serialize durable state transitions. UI reads
	// only this separately published inventory: a slow fsync must never become
	// a frame-long wait in snapshot() or find(). Nothing holding this mutex does
	// filesystem or torrent work.
	mutable std::mutex published_mutex;
	std::vector<DownloadEntry> published;
	bool published_readable = false, published_writable = false;
	std::string published_storage_error, published_directory;
	std::vector<std::string> published_directories;
	DownloadRelocationStatus published_relocation;
	std::atomic<bool> relocating{false}, relocation_pending{false};
	json relocation;
	bool worker_deleting = false, artwork_updating = false;
	struct ArtworkUpdate { std::string id, path, url; bool thumbnail = false; };
	std::mutex artwork_updates_mutex;
	std::vector<ArtworkUpdate> artwork_updates;
	std::atomic<bool> artwork_updates_pending{false}, accept_artwork{false};
	std::condition_variable changed;
	std::thread worker, artwork_worker;
	struct EnqueueWork { DownloadRequest request; EnqueueCallback done; size_t bytes; uint64_t generation; };
	std::mutex enqueue_mutex;
	std::condition_variable enqueue_changed;
	std::deque<EnqueueWork> enqueue_work;
	std::thread enqueue_worker;
	size_t pending_bytes = 0;
	bool enqueue_closed = true;
	std::atomic<uint64_t> suspend_generation{0};
	std::vector<std::shared_ptr<Job>> jobs;
	std::string root, storage_parent, storage_error, active_id, registry_directory;
	std::vector<std::string> roots;
	std::map<std::string, bool> root_writable;
	std::set<std::string> loaded_roots;
	std::atomic<bool> selection_open{false};
	bool registry_valid = true;
	std::atomic<bool> cancel{false};
	std::atomic<uint64_t> revision{1};
	bool ready = false, writable = false, stopping = false, enabled = false;
	bool foreground_torrent = false, active_torrent = false;
	bool torrent_preempted = false;
	std::shared_ptr<std::atomic<unsigned>> foreground_resolvers = std::make_shared<std::atomic<unsigned>>(0);
	uint64_t sequence = 0;
	bool torrent_reserved() const { return foreground_torrent || foreground_resolvers->load() != 0; }
	bool artwork_running() const {
		return std::any_of(jobs.begin(), jobs.end(), [](const auto& job) { return job->artwork_busy; });
	}
	bool artwork_due(const Job& job) const {
		return can_write(job.storage_root) && !job.entry.recovery_only &&
			(job.artwork_pending || job.artwork_manifest_dirty) && Clock::now() >= job.artwork_retry_at;
	}
	bool runnable_media() const {
		if (!enabled || relocating || relocation_pending) return false;
		return std::any_of(jobs.begin(), jobs.end(), [&](const auto& job) {
			return can_write(job->storage_root) && !job->removing && !job->entry.recovery_only &&
				(job->entry.state == DownloadState::Queued || job->entry.state == DownloadState::Waiting) &&
				(!torrent_stream(job->request.stream) || !torrent_reserved());
		});
	}

	void touch() {
		for (const auto& job : jobs) {
			auto& e = job->entry;
			if (e.state != DownloadState::Downloading) {
				e.bytes_per_second = 0; e.remaining_seconds = e.state == DownloadState::Complete ? 0 : -1;
				e.connected_peers = e.connected_seeders = -1;
			}
			e.playable_while_downloading = !job->removing && job->progress_ready &&
				e.state == DownloadState::Downloading && progressive_download_threshold(e.done, e.total);
			GrowingFilePhase phase = GrowingFilePhase::Preparing;
			if (job->removing || stopping) phase = GrowingFilePhase::Removed;
			else if (e.state == DownloadState::Complete) phase = GrowingFilePhase::Complete;
			else if (e.state == DownloadState::Failed) phase = GrowingFilePhase::Failed;
			else if (e.state == DownloadState::Paused) phase = GrowingFilePhase::Paused;
			else if (e.state == DownloadState::Waiting) phase = GrowingFilePhase::Waiting;
			else if (e.state == DownloadState::Downloading && job->progress_ready) phase = GrowingFilePhase::Downloading;
			job->playback->publish({e.done, e.total, job->playback_generation, phase});
		}
		std::vector<DownloadEntry> next; next.reserve(jobs.size());
		for (const auto& job : jobs) if (!job->removing) next.push_back(job->entry);
		{
			std::lock_guard lock(published_mutex);
			published.swap(next);
			published_readable = ready; published_writable = ready && writable;
			published_storage_error = storage_error;
			published_directory = root; published_directories = roots;
		}
		revision.fetch_add(1, std::memory_order_relaxed);
	}
	bool can_write(const std::string& path) const {
		const auto it = root_writable.find(path);
		return it != root_writable.end() && it->second;
	}
	void refresh_access() {
		writable = can_write(root);
		accept_artwork = std::any_of(root_writable.begin(), root_writable.end(), [](const auto& item) { return item.second; });
	}
	void block_storage(const char* operation, const std::string& path, int code) {
		std::string failed;
		for (const auto& candidate : roots)
			if (path == candidate || path.rfind(candidate + '/', 0) == 0)
				if (candidate.size() > failed.size()) failed = candidate;
		if (failed.empty()) failed = root;
		root_writable[failed] = false;
		if (failed == root) storage_failure(&storage_error, operation, path, code > 0 ? code : EIO);
		refresh_access();
		for (const auto& job : jobs) if (job->storage_root == failed && !job->removing &&
			(job->entry.state == DownloadState::Queued || job->entry.state == DownloadState::Waiting)) {
			job->entry.state = DownloadState::Paused;
			job->entry.error = "This download directory is unavailable. Reconnect its storage and retry.";
		}
		touch(); changed.notify_all();
	}
	bool check_write_access(const std::string& directory = {}) {
		const auto& target = directory.empty() ? root : directory;
		std::string failure;
		const bool result = share_download_directory(target, &failure) && probe_download_storage(target, &failure);
		root_writable[target] = result;
		if (target == root) storage_error = failure;
		refresh_access(); touch(); changed.notify_all();
		return result;
	}
	void merge_jobs(std::vector<std::shared_ptr<Job>> incoming) {
		for (auto& job : incoming) {
			if (std::any_of(jobs.begin(), jobs.end(), [&](const auto& old) { return old->directory == job->directory; })) continue;
			// Physical IDs can repeat on two disks, especially after backups.
			// Never rename either directory or overwrite its manifest identity.
			for (unsigned collision = 0; lookup(job->entry.id); ++collision)
				job->entry.id = 'd' + digest(job->storage_root + ':' + job->folder_id + ':' + std::to_string(collision));
			jobs.push_back(std::move(job));
		}
		std::stable_sort(jobs.begin(), jobs.end(), [](const auto& a, const auto& b) { return a->created_ms < b->created_ms; });
	}
	bool save_registry(const std::string& selected, const std::vector<std::string>& history, std::string* error) const {
		if (registry_directory.empty()) return true;
		const auto path = registry_directory + "/download-directories.json";
		// Before replacing a main or a recovered temporary, retain a separately
		// committed valid backup. Never overwrite that backup with corrupt JSON.
		json previous;
		for (const char* suffix : {"", ".bak", ".tmp", ".new"}) {
			if (!read_directory_registry(path + suffix, previous)) continue;
			if (!atomic_private_write(path + ".bak", json_bytes(previous), error)) return false;
			break;
		}
		return atomic_private_write(path,
			json_bytes(json{{"version", 1}, {"current", selected}, {"roots", history}}), error);
	}
 std::string relocation_path() const { return registry_directory + "/download-relocation.json"; }
 void move_status(const std::string& phase, const std::string& title = {}, int64_t bytes = -1, int done = -1) {
  std::lock_guard lock(published_mutex);
  published_relocation.active = relocating;
  published_relocation.pending = relocation_pending;
  published_relocation.phase = phase;
  if (!title.empty()) published_relocation.title = title;
  if (bytes >= 0) published_relocation.bytes_done = bytes;
  if (done >= 0) published_relocation.files_done = done;
  revision.fetch_add(1, std::memory_order_relaxed);
 }
 bool persist_relocation(std::string& error) {
  relocation_pending = true;
  return atomic_private_write(relocation_path(), json_bytes(relocation), &error);
 }
 bool read_relocation(std::string& error) {
  json value;
  bool exists = false;
  for (const char* suffix : {"", ".tmp", ".new"}) {
   const auto path = relocation_path() + suffix; struct stat st{};
   if (::lstat(path.c_str(), &st) != 0 && errno == ENOENT) continue;
   exists = true;
   if (read_manifest(path, value, 2u << 20)) break;
  }
  if (!exists) return true;
  relocation_pending = true;
  if (integer_value(value, "version") != 1 || !value.contains("items") || !value["items"].is_array() ||
   value["items"].size() > 1024 || !value.contains("roots") || !value["roots"].is_array() || value["roots"].size() > 32)
   return storage_failure(&error, "read relocation journal", relocation_path(), EINVAL);
  const auto target = text_value(value, "target", 1023), transaction = text_value(value, "transaction", 33);
  if (target.empty() || normalized_root(target) != target || !safe_id(transaction))
   return storage_failure(&error, "validate relocation journal", relocation_path(), EINVAL);
  std::set<std::string> sources, destinations;
  for (const auto& root_value : value["roots"]) {
   if (!root_value.is_string() || normalized_root(root_value.get<std::string>()) != root_value.get<std::string>())
    return storage_failure(&error, "validate relocation roots", relocation_path(), EINVAL);
  }
  for (const auto& item : value["items"]) {
   const auto source = text_value(item, "source", 1100), destination = text_value(item, "destination", 1100);
   const auto folder = destination.substr(destination.rfind('/') + 1), old_id = source.substr(source.rfind('/') + 1);
   const auto parent = source.substr(0, source.rfind('/'));
   const auto phase = text_value(item, "phase"), mode = text_value(item, "mode");
   if (!safe_id(old_id) || !safe_id(folder) || !safe_id(text_value(item, "id")) ||
    normalized_root(parent) != parent || destination != target + '/' + folder ||
    text_value(item, "staging", 1200) != target + "/.stremio-move-" + transaction + '-' + folder ||
    std::find(value["roots"].begin(), value["roots"].end(), json(parent)) == value["roots"].end() ||
    !sources.insert(source).second || !destinations.insert(destination).second ||
    (mode != "rename" && mode != "copy") ||
    (phase != "planned" && phase != "copied" && phase != "published" && phase != "retired"))
    return storage_failure(&error, "validate relocation item", relocation_path(), EINVAL);
  }
  relocation = std::move(value);
  { std::lock_guard lock(published_mutex);
   published_relocation = {}; published_relocation.pending = true;
   published_relocation.destination = target; published_relocation.phase = "pending";
   published_relocation.error = "A download relocation is pending. Retry the same destination to finish moving the saved files.";
   published_relocation.files_total = int(relocation["items"].size());
  }
  return true;
 }
 std::string relocation_location(const json& item) const {
  const auto destination = text_value(item, "destination", 1100), staging = text_value(item, "staging", 1200);
  const auto source = text_value(item, "source", 1100);
  const bool renamed = text_value(item, "mode") == "rename";
  const auto device = uint64_t(integer_value(item, renamed ? "source_device" : "stage_device", 0));
  const auto inode = uint64_t(integer_value(item, renamed ? "source_inode" : "stage_inode", 0));
  if (inode && move_identity(destination, device, inode)) return destination;
  if (renamed && inode && move_identity(staging, device, inode)) return staging;
  return source;
 }
 const json* relocation_item(const std::string& path) const {
  if (!relocation.is_object() || !relocation.contains("items")) return nullptr;
  for (const auto& item : relocation["items"])
   if (text_value(item, "source", 1100) == path || text_value(item, "destination", 1100) == path || text_value(item, "staging", 1200) == path) return &item;
  return nullptr;
 }
 static void rebase_job(Job& job, const std::string& old_path, const std::string& directory,
  const std::string& final_path, const std::string& target, const std::string& folder) {
  const auto rebase = [&](std::string& path) {
   if (path.rfind(old_path + '/', 0) == 0) path = final_path + path.substr(old_path.size());
   else if (path.rfind(job.directory + '/', 0) == 0) path = final_path + path.substr(job.directory.size());
  };
  rebase(job.entry.local_path); rebase(job.entry.poster_path); rebase(job.entry.background_path); rebase(job.entry.logo_path);
  rebase(job.entry.thumbnail_path);
  rebase(job.request.poster_path); rebase(job.request.background_path); rebase(job.request.logo_path);
  rebase(job.request.thumbnail_path);
  job.directory = directory; job.storage_root = target; job.folder_id = folder; job.manifest_source = "manifest.json";
  job.progress_ready = false; ++job.playback_generation;
 }

	bool start_storage() {
		if (ready) return writable || check_write_access();
		storage_error.clear();
		if (storage_parent.empty()) {
			storage_error = "Download storage has not been initialized.";
			return false;
		}
		const auto candidate = storage_parent + "/downloads";
		if (!safe_directory(storage_parent, &storage_error) ||
			!create_download_directory(candidate, &storage_error)) return false;
		root = candidate; roots = {root}; stopping = false; cancel.store(false);
		std::vector<std::shared_ptr<Job>> found;
		if (!load(root, found, &storage_error)) { root.clear(); return false; }
		merge_jobs(std::move(found)); loaded_roots.insert(root);
		// Reading an existing inventory must not depend on allocating another
		// file. The deletion worker remains available even on a full filesystem.
		ready = true;
		check_write_access();
		worker = std::thread([this] { run(); });
		artwork_worker = std::thread([this] { run_artwork(); }); touch();
		return writable;
	}
	std::shared_ptr<Job> lookup(const std::string& id) const {
		for (const auto& job : jobs) if (job->entry.id == id) return job;
		return {};
	}
	bool completed_torrent_file(const Job& job, std::string& name, int64_t& size) const {
		const auto& stream = job.request.stream;
		if (stream.info_hash.size() != 40 || stream.file_idx < 0) return false;
		std::string hash = stream.info_hash;
		for (char& c : hash) {
			if (c >= 'A' && c <= 'F') c = char(c - 'A' + 'a');
			if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
		}
		json state;
		if (!read_manifest(job.directory + "/transfer.json", state) || integer_value(state, "version") != 1 ||
			text_value(state, "kind") != "torrent" ||
			text_value(state, "source") != hash + ':' + std::to_string(stream.file_idx)) return false;
		size = integer_value(state, "total");
		const auto extension = text_value(state, "extension", 8);
		if (size <= 0 || integer_value(state, "bytes") != size || !safe_extension(extension) || extension == ".part" ||
			(job.entry.total > 0 && job.entry.total != size)) return false;
		name = "media" + extension;
		if (!job.media_name.empty() && job.media_name != name) return false;
		struct stat part{}, final{};
		if (::lstat((job.directory + "/media.part").c_str(), &part) == 0 || errno != ENOENT) return false;
		return ::lstat((job.directory + '/' + name).c_str(), &final) == 0 && S_ISREG(final.st_mode) &&
			final.st_nlink == 1 && final.st_size == size;
	}
	bool save(Job& job) const {
		const auto& e = job.entry;
		if (e.recovery_only) { errno = EINVAL; return false; }
		json manifest{{"version", 1}, {"id", job.folder_id}, {"identity", job.identity}, {"created_ms", job.created_ms},
			{"completed_ms", e.completed_ms}, {"release_year", e.release_year},
			{"media_id", e.media_id}, {"type", e.type}, {"video_id", e.video_id},
			{"title", e.title}, {"subtitle", e.subtitle}, {"episode_title", e.episode_title}, {"season", e.season}, {"episode", e.episode},
			{"state", static_cast<int>(e.state)}, {"done", e.done}, {"total", e.total},
			{"error", e.error}, {"media_name", job.media_name}, {"removing", job.removing},
			{"artwork_pending", job.artwork_pending},
			{"poster_url", job.request.poster_url},
			{"thumbnail_url", job.request.thumbnail_url}, {"thumbnail_source", job.request.thumbnail_path},
			{"poster_source", job.request.poster_path}, {"background_source", job.request.background_path},
			{"logo_source", job.request.logo_path}, {"stream", stream_json(job.request.stream)}};
		if (e.id != job.folder_id) manifest["public_id"] = e.id;
		const auto bytes = json_bytes(manifest);
		if (bytes.size() > kMaxManifest) { errno = EFBIG; return false; }
		const bool saved = safe_directory(job.storage_root) && atomic_private_write(job.directory + "/manifest.json", bytes,
			nullptr, job.manifest_source == "manifest.json.tmp");
		job.metadata_dirty = !saved;
		return saved;
	}
	bool load(const std::string& root, std::vector<std::shared_ptr<Job>>& found, std::string* error) {
		auto& jobs = found;
		DownloadDIR* dir = download_opendir(root.c_str());
		if (!dir) return storage_failure(error, "opendir", root, errno);
		std::vector<std::string> names;
		int enumeration_error = 0;
		for (;;) {
			errno = 0;
			const auto* ent = download_readdir(dir);
			if (!ent) { enumeration_error = errno; break; }
			if (safe_id(ent->d_name)) {
				if (names.size() >= 1024) break;
				names.emplace_back(ent->d_name);
			}
		}
		if (download_closedir(dir) != 0 && !enumeration_error) enumeration_error = errno;
		if (enumeration_error) return storage_failure(error, "read directory", root, enumeration_error);
		if (relocation.is_object() && relocation.contains("items")) for (const auto& item : relocation["items"]) {
            const auto location = relocation_location(item);
            if (location.rfind(root + "/.stremio-move-", 0) == 0) names.push_back(location.substr(root.size() + 1));
        }
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());
		for (const auto& id : names) {
			const auto path = root + '/' + id;
            const auto* move = relocation_item(path);
            if (move && relocation_location(*move) != path) continue;
            const auto original_id = move ? text_value(*move, "source", 1100).substr(text_value(*move, "source", 1100).rfind('/') + 1) : id;
            const auto final_id = move ? text_value(*move, "destination", 1100).substr(text_value(*move, "destination", 1100).rfind('/') + 1) : id;
			struct stat directory{};
			const bool identified = ::lstat(path.c_str(), &directory) == 0;
			if ((identified && !S_ISDIR(directory.st_mode)) ||
				(!identified && errno != EACCES && errno != EPERM)) continue;
			const bool readable_path = identified && safe_directory(path);
			if (readable_path && !share_download_directory(path))
				dlog("download directory: could not restore 0777 for %s (errno %d)", path.c_str(), errno);
			json value;
			std::string source;
			if (readable_path) for (const char* name : {"manifest.json", "manifest.json.bak", "manifest.json.tmp", "manifest.json.new"}) {
				json candidate;
				if (!read_manifest(path + '/' + name, candidate) || integer_value(candidate, "version") != 1 ||
					(text_value(candidate, "id") != original_id && text_value(candidate, "id") != final_id) || text_value(candidate, "media_id", 2048).empty() ||
					text_value(candidate, "type", 64).empty() || text_value(candidate, "title", 4096).empty()) continue;
				value = std::move(candidate); source = name; break;
			}
			auto job = std::make_shared<Job>();
			job->directory = path; job->storage_root = root; job->folder_id = id; job->manifest_source = source;
			auto& e = job->entry; auto& r = job->request;
			e.id = move ? text_value(*move, "id") : id; e.disk_bytes = readable_path ? directory_bytes(path) : -1;
			if (source.empty()) {
				e.recovery_only = true; e.state = DownloadState::Failed;
				e.title = "Recovered download";
				e.error = "Download metadata is missing or damaged. You can delete the saved files.";
				job->artwork_pending = false; jobs.push_back(job);
				continue;
			}
			e.id = move ? text_value(*move, "id") : safe_id(text_value(value, "public_id")) ? text_value(value, "public_id") : id; e.media_id = r.media_id = text_value(value, "media_id", 2048);
			e.type = r.type = text_value(value, "type", 64); e.video_id = r.video_id = text_value(value, "video_id", 2048);
			e.title = r.title = text_value(value, "title", 4096); e.subtitle = r.subtitle = text_value(value, "subtitle", 8192);
			const auto year = integer_value(value, "release_year", 0);
			e.release_year = r.release_year = year >= 1000 && year <= 9999 ? int(year) :
				e.type == "movie" ? download_release_year(e.subtitle) : 0;
			e.poster_url = r.poster_url = text_value(value, "poster_url");
			e.thumbnail_url = r.thumbnail_url = text_value(value, "thumbnail_url");
			if (e.media_id.empty() || e.type.empty() || e.title.empty()) continue;
			e.season = r.season = static_cast<int>(std::clamp<int64_t>(integer_value(value, "season"), -1, 99999));
			e.episode = r.episode = static_cast<int>(std::clamp<int64_t>(integer_value(value, "episode"), -1, 999999));
			e.episode_title = r.episode_title = text_value(value, "episode_title", 4096);
			if (e.episode_title.empty())
				e.episode_title = r.episode_title = download_legacy_episode_title(e.season, e.episode, e.subtitle);
			if (e.season >= 0 && e.episode >= 0)
				e.subtitle = r.subtitle = download_episode_label(e.season, e.episode, e.episode_title);
			e.done = std::max<int64_t>(0, integer_value(value, "done", 0)); e.total = integer_value(value, "total");
			e.error = safe_error(text_value(value, "error", 512));
			if (value.contains("stream")) r.stream = parse_stream(value["stream"]);
			job->identity = text_value(value, "identity", 32);
			e.source_identity = job->identity;
			job->created_ms = e.created_ms = std::max<int64_t>(0, integer_value(value, "created_ms", 0));
			e.completed_ms = std::max<int64_t>(0, integer_value(value, "completed_ms", 0));
			job->media_name = text_value(value, "media_name", 16);
			// Only an atomically committed main manifest can retain a previous
			// explicit deletion request. Backup/temp recovery never auto-deletes.
			job->removing = source == "manifest.json" && boolean_value(value, "removing");
			job->artwork_pending = boolean_value(value, "artwork_pending");
			if (job->artwork_pending) {
				r.poster_path = text_value(value, "poster_source"); r.background_path = text_value(value, "background_source");
				r.logo_path = text_value(value, "logo_source");
				r.thumbnail_path = text_value(value, "thumbnail_source");
			}
			if (artwork_dimensions(path + "/poster-hd.rgba", e.poster_width, e.poster_height) &&
				download_poster_full_size(e.poster_width, e.poster_height)) e.poster_path = path + "/poster-hd.rgba";
			else if (artwork_dimensions(path + "/poster.rgba", e.poster_width, e.poster_height)) e.poster_path = path + "/poster.rgba";
			else if (artwork_dimensions(r.poster_path, e.poster_width, e.poster_height)) e.poster_path = r.poster_path;
			if (regular_size(path + "/background.rgba") >= 12) e.background_path = path + "/background.rgba";
			else if (regular_size(r.background_path) >= 12) e.background_path = r.background_path;
			if (regular_size(path + "/logo.rgba") >= 12) e.logo_path = path + "/logo.rgba";
			else if (regular_size(r.logo_path) >= 12) e.logo_path = r.logo_path;
			if (regular_size(path + "/thumbnail.rgba") >= 12) e.thumbnail_path = path + "/thumbnail.rgba";
			else if (regular_size(r.thumbnail_path) >= 12) e.thumbnail_path = r.thumbnail_path;
			const int64_t state = integer_value(value, "state");
			if (state == static_cast<int>(DownloadState::Complete)) {
				const bool name_ok = job->media_name.rfind("media.", 0) == 0 &&
					safe_extension(job->media_name.substr(5)) && job->media_name != "media.part";
				const int64_t size = name_ok ? regular_size(path + '/' + job->media_name) : -1;
				if (size > 0 && size == e.done) {
					e.state = DownloadState::Complete; e.local_path = path + '/' + job->media_name; e.total = size; e.error.clear();
				} else { e.state = DownloadState::Failed; e.error = "The downloaded file is missing or incomplete. Download it again."; }
			} else {
				e.state = state == static_cast<int>(DownloadState::Failed) ? DownloadState::Failed : DownloadState::Paused;
				if (e.state == DownloadState::Paused && state != static_cast<int>(DownloadState::Paused)) e.error.clear();
				const int64_t size = regular_size(path + "/media.part");
				if (size >= 0) e.done = size;
				std::string filename; int64_t completed_size = 0;
				if (completed_torrent_file(*job, filename, completed_size)) {
					job->media_name = filename; e.local_path = path + '/' + filename;
					e.done = e.total = completed_size; e.state = DownloadState::Complete; e.error.clear();
					job->metadata_dirty = true;
				}
			}
			update_fraction(e); jobs.push_back(job);
		}
		std::stable_sort(jobs.begin(), jobs.end(), [](const auto& a, const auto& b) {
			return a->created_ms < b->created_ms;
		});
		return true;
	}
	void prepare_artwork(const std::shared_ptr<Job>& job) {
		const auto started = Clock::now();
		DownloadRequest request;
		uint64_t generation = 0;
		bool pending = false;
		{ std::lock_guard lock(mutex); request = job->request; generation = job->artwork_generation; pending = job->artwork_pending; }
		int poster_width = 0, poster_height = 0;
		const bool valid_poster = pending && artwork_dimensions(request.poster_path, poster_width, poster_height);
		// A new path also invalidates the GPU texture naturally. Keep the old
		// private file usable until the full-sized replacement is committed.
		const auto poster_destination = job->directory + (download_poster_full_size(poster_width, poster_height)
			? "/poster-hd.rgba" : "/poster.rgba");
		const bool poster = valid_poster && safe_directory(job->directory) &&
			copy_artwork(request.poster_path, poster_destination);
		const bool background = pending && !request.background_path.empty() && safe_directory(job->directory) &&
			copy_artwork(request.background_path, job->directory + "/background.rgba");
		const bool logo = pending && !request.logo_path.empty() && safe_directory(job->directory) &&
			copy_artwork(request.logo_path, job->directory + "/logo.rgba");
		const bool thumbnail = pending && !request.thumbnail_path.empty() && safe_directory(job->directory) &&
			copy_artwork(request.thumbnail_path, job->directory + "/thumbnail.rgba");
		std::lock_guard lock(mutex);
		job->artwork_busy = false;
		if (!job->removing && generation == job->artwork_generation) {
			if (poster) {
				job->entry.poster_path = poster_destination;
				job->entry.poster_width = poster_width; job->entry.poster_height = poster_height;
			}
			if (background) job->entry.background_path = job->directory + "/background.rgba";
			if (logo) job->entry.logo_path = job->directory + "/logo.rgba";
			if (thumbnail) job->entry.thumbnail_path = job->directory + "/thumbnail.rgba";
			job->request.poster_path.clear(); job->request.background_path.clear(); job->request.logo_path.clear();
			job->request.thumbnail_path.clear();
			job->artwork_pending = false;
			job->artwork_manifest_dirty = !save(*job);
			if (job->artwork_manifest_dirty) block_storage("save artwork metadata", job->directory, errno);
			job->artwork_retry_at = job->artwork_manifest_dirty ? Clock::now() + std::chrono::seconds(5) : Clock::time_point{};
		}
		dlog("download queue: artwork_ms=%.1f copied=%d media_active=%d",
			std::chrono::duration<double, std::milli>(Clock::now() - started).count(),
			int(poster) + int(background) + int(logo) + int(thumbnail), !active_id.empty());
		touch(); changed.notify_all();
	}
	bool queue_artwork(const std::string& id, const std::string& path, const std::string& url, bool thumbnail = false) {
		if (relocating || relocation_pending || !accept_artwork.load() || !safe_id(id)) return false;
		{
			std::lock_guard lock(published_mutex);
			if (std::none_of(published.begin(), published.end(), [&](const auto& e) { return e.id == id && !e.recovery_only; })) return false;
		}
		{
			std::lock_guard lock(artwork_updates_mutex);
			if (!accept_artwork.load()) return false;
			auto update = std::find_if(artwork_updates.begin(), artwork_updates.end(),
				[&](const auto& item) { return item.id == id && item.thumbnail == thumbnail; });
			if (update == artwork_updates.end()) {
				if (artwork_updates.size() >= 2 * kMaxJobs) return false;
				artwork_updates.push_back({id, path, url, thumbnail});
			} else {
				if (!path.empty()) update->path = path;
				if (!url.empty()) update->url = url;
			}
			artwork_updates_pending = true;
		}
		changed.notify_all(); return true;
	}
	void apply_artwork_updates() {
		std::vector<ArtworkUpdate> pending;
		{
			std::lock_guard lock(artwork_updates_mutex);
			pending.swap(artwork_updates); artwork_updates_pending = false;
		}
		for (const auto& update : pending) {
			// All file validation and persistence happen on this worker. Callback
			// callers can continue drawing even while a previous fsync is blocked.
			int poster_width = 0, poster_height = 0;
			const bool image_valid = update.thumbnail ? !update.path.empty() && regular_size(update.path) >= 12
				: artwork_dimensions(update.path, poster_width, poster_height);
			std::lock_guard lock(mutex);
			const auto job = lookup(update.id);
			if (!job || job->removing || job->entry.recovery_only) continue;
			bool modified = false;
			auto& request_url = update.thumbnail ? job->request.thumbnail_url : job->request.poster_url;
			auto& entry_url = update.thumbnail ? job->entry.thumbnail_url : job->entry.poster_url;
			auto& request_path = update.thumbnail ? job->request.thumbnail_path : job->request.poster_path;
			auto& entry_path = update.thumbnail ? job->entry.thumbnail_path : job->entry.poster_path;
			if (!update.url.empty() && request_url != update.url) {
				entry_url = request_url = update.url; modified = true;
			}
			const bool owned_image = entry_path == job->directory + (update.thumbnail ? "/thumbnail.rgba" : "/poster.rgba") ||
				(!update.thumbnail && entry_path == job->directory + "/poster-hd.rgba");
			const bool poster_upgrade = !update.thumbnail && download_poster_full_size(poster_width, poster_height) &&
				!download_poster_full_size(job->entry.poster_width, job->entry.poster_height);
			const bool no_downgrade = update.thumbnail ||
				(poster_width >= job->entry.poster_width && poster_height >= job->entry.poster_height);
			if (image_valid && no_downgrade && (!owned_image || poster_upgrade) &&
				!(request_path == update.path && job->artwork_pending)) {
				request_path = update.path;
				if (!owned_image) {
					entry_path = update.path;
					if (!update.thumbnail) {
						job->entry.poster_width = poster_width; job->entry.poster_height = poster_height;
					}
				}
				++job->artwork_generation; job->artwork_pending = true; modified = true;
			}
			// Cache paths are immediately usable by the UI. The source URL and
			// private artwork are persisted when the media writer is idle, so a
			// late cover cannot flush another job's filesystem under its writer.
			if (modified) { job->artwork_manifest_dirty = true; touch(); }
		}
		changed.notify_all();
	}
	void run_artwork() {
		for (;;) {
			std::shared_ptr<Job> selected;
			{
				std::unique_lock lock(mutex);
				// Callback producers intentionally never take the storage mutex.
				// A bounded wait also covers a notify racing the predicate-to-wait
				// transition of that independent, coalesced queue.
				changed.wait_for(lock, std::chrono::milliseconds(100), [&] {
					if (stopping) return true;
					if (relocating || relocation_pending) return false;
					if (artwork_updates_pending.load()) return true;
					if (!active_id.empty() || torrent_reserved() || artwork_running() || runnable_media()) return false;
					for (const auto& job : jobs)
						if (artwork_due(*job) && !job->removing) return true;
					return false;
				});
				if (stopping) break;
				if (relocating || relocation_pending) continue;
				if (artwork_updates_pending.load()) {
					artwork_updating = true;
					lock.unlock(); apply_artwork_updates(); lock.lock();
					artwork_updating = false; changed.notify_all(); continue;
				}
				if (!active_id.empty() || torrent_reserved() || artwork_running() || runnable_media()) continue;
				for (const auto& job : jobs) if (artwork_due(*job) && !job->removing) {
					selected = job; job->artwork_busy = true; break;
				}
			}
			if (selected) prepare_artwork(selected);
		}
	}
	void run() {
		for (;;) {
			std::shared_ptr<Job> selected;
			bool deleting = false, preparing_artwork = false;
			{
				std::unique_lock lock(mutex);
				changed.wait_for(lock, std::chrono::milliseconds(100), [&] {
					if (stopping) return true;
					if (relocating || relocation_pending) return false;
					for (const auto& j : jobs) if ((j->removing && !j->artwork_busy) ||
						(enabled && can_write(j->storage_root) && !artwork_running() && !j->removing &&
						!j->entry.recovery_only && j->entry.state == DownloadState::Queued)) return true;
					return false;
				});
				if (stopping) break;
				if (relocating || relocation_pending) continue;
				for (const auto& j : jobs) if (j->removing && !j->artwork_busy) { selected = j; deleting = true; worker_deleting = true; break; }
				if (!selected && enabled && !artwork_running()) for (const auto& j : jobs) {
					if (!can_write(j->storage_root)) continue;
					if (j->removing || j->entry.recovery_only) continue;
					if (j->entry.state != DownloadState::Queued && j->entry.state != DownloadState::Waiting) continue;
					if (torrent_stream(j->request.stream) && torrent_reserved()) {
						if (j->entry.state != DownloadState::Waiting) { j->entry.state = DownloadState::Waiting; touch(); }
						continue;
					}
					if (artwork_due(*j)) {
						selected = j; j->artwork_busy = true; preparing_artwork = true; break;
					}
					selected = j; active_id = j->entry.id; active_torrent = torrent_stream(j->request.stream);
					torrent_preempted = false;
					cancel.store(false); j->entry.state = DownloadState::Downloading; j->entry.error.clear();
					j->entry.bytes_per_second = 0; j->entry.remaining_seconds = -1;
					j->entry.connected_peers = j->entry.connected_seeders = -1;
					++j->playback_generation; j->progress_ready = false; j->torrent_file_idx = -1;
					j->last_progress = Clock::now();
					if (!save(*j)) {
						block_storage("save download state", j->directory, errno);
						j->entry.state = DownloadState::Failed; j->entry.error = "The download state could not be saved.";
						active_id.clear(); active_torrent = false; selected.reset(); changed.notify_all();
					}
					touch(); break;
				}
			}
			if (!selected) continue;
			if (preparing_artwork) { prepare_artwork(selected); continue; }
			if (deleting) {
				const bool removed = remove_flat_directory(selected->storage_root, selected->folder_id);
				std::lock_guard lock(mutex);
				worker_deleting = false; changed.notify_all();
				if (removed) {
					jobs.erase(std::remove(jobs.begin(), jobs.end(), selected), jobs.end());
					if (!can_write(selected->storage_root) && check_write_access(selected->storage_root)) {
						for (const auto& job : jobs) if (job->storage_root == selected->storage_root && job->metadata_dirty && !job->entry.recovery_only && !save(*job)) {
							block_storage("save recovered download state", job->directory, errno); break;
						}
					}
				}
				else {
					selected->removing = false; selected->entry.state = DownloadState::Failed;
					selected->entry.error = "The download folder could not be deleted.";
					selected->entry.disk_bytes = directory_bytes(selected->directory);
					if (!selected->entry.recovery_only) save(*selected);
				}
				touch(); continue;
			}
			DownloadTransferRequest transfer;
			{
				std::lock_guard lock(mutex);
				transfer.stream = selected->request.stream; transfer.work_dir = selected->directory;
				transfer.partial_path = selected->directory + "/media.part";
				transfer.checkpoint_path = selected->directory + "/transfer.json";
				transfer.season = selected->request.season; transfer.episode = selected->request.episode;
			}
			DownloadTransferResult result;
			bool progress_metadata_failed = false;
			if (safe_directory(selected->storage_root) && share_download_directory(selected->directory)) {
				try {
					result = download_transfer(transfer, [this, selected, &progress_metadata_failed](const DownloadTransferProgress& p) {
						std::lock_guard lock(mutex);
						if (selected->removing || selected->entry.state != DownloadState::Downloading) return;
						const auto now = Clock::now();
						if (selected->progress_ready && now - selected->last_progress < std::chrono::milliseconds(200) &&
							p.done != p.total && p.done >= selected->entry.done && p.total == selected->entry.total) return;
						if (selected->progress_ready && (p.done < selected->entry.done ||
							(p.total > 0 && p.total != selected->entry.total))) ++selected->playback_generation;
						const int64_t total = p.total > 0 ? p.total : -1;
						const bool metadata_changed = total != selected->entry.total;
						selected->progress_ready = true; selected->torrent_file_idx = p.torrent_file_idx;
						selected->last_progress = now; selected->entry.done = std::max<int64_t>(0, p.done);
						selected->entry.total = total;
						selected->entry.bytes_per_second = std::isfinite(p.bytes_per_second) ? std::max(0.0, p.bytes_per_second) : 0;
						selected->entry.remaining_seconds = p.remaining_seconds >= 0 ? p.remaining_seconds : -1;
						selected->entry.connected_peers = std::max(-1, p.connected_peers);
						selected->entry.connected_seeders = p.connected_seeders >= 0 && p.connected_peers >= 0 ?
							std::min(p.connected_seeders, p.connected_peers) : -1;
						update_fraction(selected->entry); touch();
						// The transfer owns durable byte checkpoints. Persist a newly
						// resolved total once; load() recovers current bytes from the
						// partial file without flushing the manifest on every update.
						if (metadata_changed && !save(*selected)) {
							block_storage("save download metadata", selected->directory, errno);
							progress_metadata_failed = true; cancel.store(true);
						}
					}, cancel);
				} catch (...) { result.error = "An internal error interrupted the download. You can retry."; }
			} else result.error = "The download folder is not available.";
			{
				std::lock_guard lock(mutex);
				active_id.clear(); active_torrent = false; changed.notify_all();
				if (result.storage_error > 0) block_storage("download transfer", selected->directory, result.storage_error);
				auto& e = selected->entry; e.bytes_per_second = 0; e.remaining_seconds = -1;
				e.connected_peers = e.connected_seeders = -1;
				if (selected->removing) { touch(); continue; }
				if (progress_metadata_failed) {
					e.state = DownloadState::Failed; e.error = "The download metadata could not be saved.";
				} else if (result.status == DownloadTransferStatus::Complete) {
					const int64_t size = regular_size(transfer.partial_path);
					std::string extension = safe_extension(result.extension) && result.extension != ".part" ? result.extension : ".media";
					const std::string filename = "media" + extension;
					if (safe_directory(selected->storage_root) && safe_directory(selected->directory) && size > 0 &&
						(result.done <= 0 || result.done == size) && (result.total <= 0 || result.total == size) &&
						::rename(transfer.partial_path.c_str(), (selected->directory + '/' + filename).c_str()) == 0) {
						selected->media_name = filename; e.local_path = selected->directory + '/' + filename;
						e.done = e.total = size; e.state = DownloadState::Complete; e.error.clear();
						e.completed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
							std::chrono::system_clock::now().time_since_epoch()).count();
						e.remaining_seconds = 0;
					} else { e.state = DownloadState::Failed; e.error = "The downloaded file is incomplete or cannot be saved."; }
				} else if (result.status == DownloadTransferStatus::Cancelled || cancel.load()) {
					const int64_t size = regular_size(transfer.partial_path);
					if (torrent_stream(transfer.stream)) {
						// A disconnected writer can leave bytes beyond its last reply.
						// File length alone must not expose that tail to playback.
						const int64_t acknowledged = std::max<int64_t>(0, result.done);
						const int64_t prefix = std::min(acknowledged, std::max<int64_t>(0, size));
						if (prefix < e.done) ++selected->playback_generation;
						e.done = e.total > 0 ? std::min(prefix, e.total) : prefix;
					} else if (size >= 0) e.done = size;
					if (!stopping && enabled && torrent_preempted && e.state == DownloadState::Downloading)
						e.state = torrent_reserved() ? DownloadState::Waiting : DownloadState::Queued;
					else if (e.state == DownloadState::Downloading) e.state = DownloadState::Paused;
					if (can_write(selected->storage_root)) e.error.clear();
					else if (e.error.empty()) e.error = "This download directory is unavailable. Reconnect its storage and retry.";
				} else { e.state = DownloadState::Failed; e.error = safe_error(result.error); }
				update_fraction(e);
				e.disk_bytes = directory_bytes(selected->directory);
				if (!save(*selected)) {
					block_storage("save download state", selected->directory, errno);
					// The transfer and final rename already proved completion. Keep
					// the usable local file visible; retry its metadata after storage
					// recovers instead of telling the user to download it again.
					if (e.state == DownloadState::Complete)
						e.error = "The video is saved, but its download metadata could not be updated.";
				}
				touch();
			}
		}
	}
};

DownloadManager::DownloadManager() : impl_(std::make_unique<Impl>()) {}
DownloadManager::~DownloadManager() { shutdown(); }

bool DownloadManager::init(const std::string& data_dir, std::string* error) {
	shutdown();
	auto& p = *impl_;
	std::lock_guard lifecycle_lock(p.lifecycle_mutex);
	{
		std::lock_guard lock(p.enqueue_mutex);
		p.enqueue_closed = false;
	}
	std::lock_guard lock(p.mutex);
	p.selection_open = true;
	p.storage_parent = data_dir;
	while (p.storage_parent.size() > 1 && p.storage_parent.back() == '/') p.storage_parent.pop_back();
	p.foreground_torrent = p.active_torrent = false; p.active_id.clear();
	const bool ready = p.start_storage();
	p.touch();
	if (error) *error = p.storage_error;
	return ready;
}

bool DownloadManager::init(const std::string& data_parent, const std::string& registry_appdata,
	const std::string& preferred_directory, std::string* error) {
	shutdown();
	auto& p = *impl_;
	std::lock_guard lifecycle_lock(p.lifecycle_mutex);
	p.registry_directory = normalized_root(registry_appdata);
	p.storage_parent = normalized_root(data_parent);
	if (p.registry_directory.empty() || p.storage_parent.empty()) {
		if (error) *error = "Invalid download registry or data directory.";
		return false;
	}
	p.selection_open = true;
	{ std::lock_guard queue_lock(p.enqueue_mutex); p.enqueue_closed = false; }
	p.stopping = false; p.cancel = false;
	const auto legacy = p.storage_parent + "/downloads";
	p.roots.clear();
	p.root = normalized_root(preferred_directory); // Empty means no explicit choice.
	const auto registry_path = p.registry_directory + "/download-directories.json";
	json registry;
	bool any_registry = false, recovered_registry = false;
	for (const char* suffix : {"", ".bak", ".tmp", ".new"}) {
		const auto candidate = registry_path + suffix;
		struct stat existing{};
		if (::lstat(candidate.c_str(), &existing) != 0 && errno == ENOENT) continue;
		any_registry = true;
		if (!read_directory_registry(candidate, registry)) continue;
		recovered_registry = true; break;
	}
	p.registry_valid = !any_registry || recovered_registry;
	if (recovered_registry) {
		p.root = text_value(registry, "current", 1023);
		for (const auto& value : registry["roots"]) {
			const auto path = value.get<std::string>();
			if (std::find(p.roots.begin(), p.roots.end(), path) == p.roots.end()) p.roots.push_back(path);
		}
	} else if (!p.registry_valid) {
		p.storage_error = "Download directory registry and recovery copies are unreadable or damaged; they have been preserved.";
	}

	std::string relocation_error;
    if (!p.read_relocation(relocation_error)) { p.registry_valid = false; p.storage_error = relocation_error; }
    if (p.relocation.is_object()) {
        for (const auto& value : p.relocation["roots"]) {
            const auto path = value.get<std::string>();
            if (std::find(p.roots.begin(), p.roots.end(), path) == p.roots.end()) p.roots.push_back(path);
        }
        const auto target = text_value(p.relocation, "target", 1023);
        if (std::find(p.roots.begin(), p.roots.end(), target) == p.roots.end()) p.roots.push_back(target);
    }
    if (std::find(p.roots.begin(), p.roots.end(), legacy) == p.roots.end() && safe_directory(legacy)) {
        std::vector<std::shared_ptr<Impl::Job>> legacy_jobs;
        if (p.load(legacy, legacy_jobs, nullptr) && !legacy_jobs.empty()) {
            p.roots.push_back(legacy); p.merge_jobs(std::move(legacy_jobs)); p.loaded_roots.insert(legacy);
        }
    }
	if (!p.root.empty() && std::find(p.roots.begin(), p.roots.end(), p.root) == p.roots.end()) p.roots.push_back(p.root);
	// Discover old locations even when the chosen removable drive is absent.
	// Never create a missing mount path or migrate its media into another disk.
	for (const auto& path : p.roots) {
		std::string failure;
		std::vector<std::shared_ptr<Impl::Job>> found;
		const bool readable = safe_directory(path, &failure) && p.load(path, found, &failure);
		if (readable) { p.merge_jobs(std::move(found)); p.loaded_roots.insert(path); }
		p.root_writable[path] = readable && share_download_directory(path, &failure) && probe_download_storage(path, &failure);
		if (path == p.root && !failure.empty()) p.storage_error = failure;
	}
	if (p.registry_valid) {
		std::string failure;
		if (!create_private_directory(p.registry_directory, &failure) || !p.save_registry(p.root, p.roots, &failure))
			p.storage_error = failure;
	}
	p.ready = true; p.refresh_access();
    if (p.relocation_pending) { p.writable = false; p.accept_artwork = false;
        if (p.storage_error.empty()) p.storage_error = "A download relocation is pending. Retry the same destination to finish moving the saved files.";
    }
	if (p.root.empty() && p.storage_error.empty()) p.storage_error = "Choose a download directory before adding a video.";
	p.touch();
	p.worker = std::thread([&p] { p.run(); });
	p.artwork_worker = std::thread([&p] { p.run_artwork(); });
	if (error) *error = p.storage_error;
	return p.writable;
}

bool DownloadManager::set_download_directory(const std::string& directory, std::string& error) {
 error.clear();
 const auto target = normalized_root(directory);
 if (target.empty()) { error = "Choose a valid absolute directory under allowed storage."; return false; }
 auto& p = *impl_;
 if (p.relocating.load()) { error = "Download relocation is in progress."; return false; }
 std::lock_guard lifecycle_lock(p.lifecycle_mutex);
 std::map<std::string, DownloadState> previous_states;
 {
  std::lock_guard lock(p.mutex);
  if (!p.selection_open || !p.ready || p.registry_directory.empty()) {
   error = "Download storage is shutting down or has not been initialized."; return false;
  }
  if (!p.registry_valid) { error = "Download directory registry is damaged; restore it before changing directories."; return false; }
  if (p.relocation_pending && (!p.relocation.is_object() || text_value(p.relocation, "target", 1023) != target)) {
   error = "A download relocation is pending. Retry the same destination to finish moving the saved files."; return false;
  }
  for (const auto& job : p.jobs) {
   if (job->playback.use_count() > 1) { error = "Close local playback before moving downloads."; return false; }
   previous_states[job->entry.id] = job->entry.state;
  }
  p.relocating = true; p.cancel = true; p.accept_artwork = false; p.changed.notify_all();
 }
 bool succeeded = false;
 struct Finish { std::function<void()> call; ~Finish() { call(); } } finish{[&] {
  std::lock_guard lock(p.mutex);
  if (p.relocation_pending && p.relocation.is_object()) for (const auto& item : p.relocation["items"]) {
   const auto job = p.lookup(text_value(item, "id"));
   if (!job) continue;
   const auto location = p.relocation_location(item), source = text_value(item, "source", 1100);
   if (location != job->directory) Impl::rebase_job(*job, source, location, location,
    location.substr(0, location.rfind('/')), location.substr(location.rfind('/') + 1));
  }
  if (!p.relocation_pending) {
   if (!succeeded) p.relocation = json();
   if (p.enabled && p.selection_open) for (const auto& job : p.jobs) {
    const auto old = previous_states.find(job->entry.id);
    if (old != previous_states.end() && job->entry.state == DownloadState::Paused &&
     (old->second == DownloadState::Downloading || old->second == DownloadState::Queued || old->second == DownloadState::Waiting))
     job->entry.state = torrent_stream(job->request.stream) && p.torrent_reserved() ? DownloadState::Waiting : DownloadState::Queued;
   }
   p.refresh_access();
  } else { p.writable = false; p.accept_artwork = false; }
  p.relocating = false;
  { std::lock_guard published_lock(p.published_mutex);
   p.published_relocation.active = false; p.published_relocation.pending = p.relocation_pending;
   p.published_relocation.phase = p.relocation_pending ? "pending" : succeeded ? "complete" : "error";
   p.published_relocation.error = error;
  }
  p.touch(); p.changed.notify_all();
 }};
 {
  std::lock_guard lock(p.published_mutex);
  p.published_relocation = {}; p.published_relocation.active = true;
  p.published_relocation.pending = p.relocation_pending;
  p.published_relocation.destination = target; p.published_relocation.phase = "preparing";
 }
 {
  std::unique_lock lock(p.mutex);
  const auto deadline = Clock::now() + std::chrono::seconds(35);
  while (!p.active_id.empty() || p.artwork_running() || p.worker_deleting || p.artwork_updating) {
   if (!p.selection_open || Clock::now() >= deadline) return storage_failure(&error, "wait for download writer", target, EBUSY);
   p.changed.wait_for(lock, std::chrono::milliseconds(50));
  }
  for (const auto& job : p.jobs) if (job->entry.state == DownloadState::Queued || job->entry.state == DownloadState::Waiting || job->entry.state == DownloadState::Downloading)
   job->entry.state = DownloadState::Paused;
  p.touch();
 }
 // Drain already accepted cover updates before the transaction captures paths.
 if (!p.relocation_pending) p.apply_artwork_updates();
 if (!safe_directory(target, &error)) return false;
 MoveFd selected(::open(target.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW));
 if (selected.fd < 0) return storage_failure(&error, "open selected directory", target, errno);
 struct stat target_stat{};
 if (::fstat(selected.fd, &target_stat) != 0 || !S_ISDIR(target_stat.st_mode) ||
  !move_identity(target, uint64_t(target_stat.st_dev), uint64_t(target_stat.st_ino)))
  return storage_failure(&error, "verify selected directory identity", target, EIO);
 if (::fchmod(selected.fd, 0777) != 0) return storage_failure(&error, "fchmod selected directory", target, errno);
 struct stat permissions{};
 if (::fstat(selected.fd, &permissions) != 0)
  return storage_failure(&error, "read selected directory permissions", target, errno);
 if (!S_ISDIR(permissions.st_mode) || permissions.st_dev != target_stat.st_dev || permissions.st_ino != target_stat.st_ino)
  return storage_failure(&error, "selected directory identity changed after chmod", target, EIO);
 if ((permissions.st_mode & 0777) != 0777)
  return storage_failure(&error, "selected directory did not retain required 0777 permissions", target, EPERM);
 if (!probe_download_storage(target, &error)) return false;
 std::vector<std::string> history = p.roots;
 if (std::find(history.begin(), history.end(), target) == history.end()) {
  if (history.size() >= 32) { error = "The saved download directory limit has been reached."; return false; }
  history.push_back(target);
 }
 // A remembered but absent external drive may contain jobs not loaded at boot.
 // The implicit, never-created legacy folder is the only optional missing root.
 for (const auto& root : history) {
  if (!safe_directory(root, &error)) {
   const bool optional_legacy = false; // Never-created legacy roots are not registered.
   bool retired = p.relocation.is_object();
   if (retired) for (const auto& item : p.relocation["items"])
    if (text_value(item, "source", 1100).rfind(root + '/', 0) == 0 && text_value(item, "phase") != "retired") retired = false;
   if (optional_legacy || retired) { error.clear(); continue; }
   return false;
  }
  std::vector<std::shared_ptr<Impl::Job>> found;
  if (!p.load(root, found, &error)) return false;
  { std::lock_guard lock(p.mutex); p.merge_jobs(std::move(found)); p.loaded_roots.insert(root); p.touch(); }
 }
 for (const auto& job : p.jobs) {
  if (job->removing) return storage_failure(&error, "wait for pending download removal", job->directory, EBUSY);
  if (target == job->directory || target.rfind(job->directory + '/', 0) == 0)
   return storage_failure(&error, "destination is inside a download", target, EINVAL);
 }
 if (!p.relocation_pending) {
  for (const auto& job : p.jobs) if (!job->entry.recovery_only && !p.save(*job))
   return storage_failure(&error, "checkpoint before relocation", job->directory, errno);
  const auto transaction = 'd' + digest(target + ':' + std::to_string(::getpid()) + ':' + std::to_string(Clock::now().time_since_epoch().count()));
  json items = json::array();
  std::set<std::string> destinations;
  for (const auto& job : p.jobs) {
   if (job->storage_root == target) continue;
   struct stat st{};
   if (!safe_directory(job->directory, &error) || ::stat(job->directory.c_str(), &st) != 0)
    return storage_failure(&error, "stat relocation source", job->directory, errno);
   std::string folder = job->folder_id;
   for (unsigned attempt = 0;; ++attempt) {
    const auto path = target + '/' + folder;
    struct stat existing{};
    if (!destinations.count(path) && ::lstat(path.c_str(), &existing) != 0 && errno == ENOENT) { destinations.insert(path); break; }
    if (attempt == 32) return storage_failure(&error, "reserve relocation destination", path, EEXIST);
    folder = 'd' + digest(transaction + ':' + job->entry.id + ':' + std::to_string(attempt));
   }
   std::vector<MoveFile> files; int64_t bytes = 0;
   if (!move_inventory(job->directory, files, bytes, error)) return false;
   items.push_back(json{{"id", job->entry.id}, {"source", job->directory}, {"destination", target + '/' + folder},
    {"staging", target + "/.stremio-move-" + transaction + '-' + folder},
    {"source_device", uint64_t(st.st_dev)}, {"source_inode", uint64_t(st.st_ino)},
    {"stage_device", 0}, {"stage_inode", 0}, {"bytes", bytes},
    {"phase", "planned"}, {"mode", st.st_dev == target_stat.st_dev ? "rename" : "copy"}});
  }
  p.relocation = json{{"version", 1}, {"transaction", transaction}, {"target", target}, {"previous", p.root},
   {"roots", history}, {"target_device", uint64_t(target_stat.st_dev)}, {"target_inode", uint64_t(target_stat.st_ino)}, {"items", items}};
 }
 if (!move_identity(target, uint64_t(integer_value(p.relocation, "target_device", 0)), uint64_t(integer_value(p.relocation, "target_inode", 0))))
  return storage_failure(&error, "selected storage changed since relocation began", target, EIO);
 // Take the same advisory directory lock as the native helper. A timed-out
 // helper may still be flushing after the network transfer returned.
 std::vector<std::unique_ptr<MoveFd>> source_locks;
 for (const auto& item : p.relocation["items"]) {
  const auto source = text_value(item, "source", 1100), location = p.relocation_location(item);
  for (const auto& path : source == location ? std::vector<std::string>{source} : std::vector<std::string>{source, location}) {
   struct stat st{};
   if (::lstat(path.c_str(), &st) != 0 && errno == ENOENT && path == source && location != source) continue;
   auto fd = std::make_unique<MoveFd>(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW));
   if (fd->fd < 0 || ::flock(fd->fd, LOCK_EX | LOCK_NB) != 0)
    return storage_failure(&error, "lock download writer for relocation", path, EBUSY);
   source_locks.push_back(std::move(fd));
  }
 }
 // Reject an obviously undersized destination before creating a transaction.
 // Same-filesystem renames need metadata only, never a second media allocation.
 if (!p.relocation_pending && !p.relocation["items"].empty()) {
  uint64_t required = 2u << 20;
  for (const auto& item : p.relocation["items"]) if (text_value(item, "mode") == "copy") {
   const auto bytes = uint64_t(std::max<int64_t>(0, integer_value(item, "bytes", 0)));
   if (bytes > std::numeric_limits<uint64_t>::max() - required) return storage_failure(&error, "relocation size overflow", target, EOVERFLOW);
   required += bytes;
  }
  struct statfs filesystem{};
#ifdef PLATFORM_PS5_NATIVE
  const int measured = relocation_fstatfs(selected.fd, &filesystem);
#else
  const int measured = ::fstatfs(selected.fd, &filesystem);
#endif
  if (measured == 0 && filesystem.f_bsize > 0 && filesystem.f_bavail >= 0) {
   const auto blocks = uint64_t(filesystem.f_bavail), block_size = uint64_t(filesystem.f_bsize);
   const auto available = blocks > std::numeric_limits<uint64_t>::max() / block_size ? std::numeric_limits<uint64_t>::max() : blocks * block_size;
   if (available < required) return storage_failure(&error, "insufficient relocation space", target, ENOSPC);
  }
 }
 if (!p.persist_relocation(error)) return false;
 if (!p.save_registry(p.root, history, &error)) return false;
 { std::lock_guard lock(p.mutex); p.roots = history; p.root_writable[target] = true; p.writable = false; p.touch(); }
 int64_t total = 0;
 for (const auto& item : p.relocation["items"]) {
  const auto bytes = std::max<int64_t>(0, integer_value(item, "bytes", 0));
  if (bytes > std::numeric_limits<int64_t>::max() - total) return storage_failure(&error, "relocation size overflow", target, EOVERFLOW);
  total += bytes;
 }
 { std::lock_guard lock(p.published_mutex);
  p.published_relocation.pending = true; p.published_relocation.files_total = int(p.relocation["items"].size());
  p.published_relocation.bytes_total = total;
 }
 int done = 0; int64_t completed_bytes = 0;
 for (auto& item : p.relocation["items"]) {
  if (!p.selection_open) return storage_failure(&error, "interrupt relocation", target, ECANCELED);
  const auto source = text_value(item, "source", 1100), destination = text_value(item, "destination", 1100), staging = text_value(item, "staging", 1200);
  const auto source_root = source.substr(0, source.rfind('/')), source_id = source.substr(source.rfind('/') + 1);
  const auto folder = destination.substr(destination.rfind('/') + 1), id = text_value(item, "id");
  const bool rename_mode = text_value(item, "mode") == "rename";
  auto job = p.lookup(id);
  if (!job) return storage_failure(&error, "find relocating download", source, ENOENT);
  p.move_status("moving", job->entry.title, completed_bytes, done);
  auto location = p.relocation_location(item);
  bool published = location == destination;
  if (!published) {
   const auto source_device = uint64_t(integer_value(item, "source_device", 0)), source_inode = uint64_t(integer_value(item, "source_inode", 0));
   if (location != staging && !move_identity(source, source_device, source_inode))
    return storage_failure(&error, "source changed since relocation began", source, EIO);
   if (!move_identity(target, uint64_t(target_stat.st_dev), uint64_t(target_stat.st_ino)))
    return storage_failure(&error, "destination storage changed", target, EIO);
   struct stat stage_stat{};
   const bool stage_exists = ::lstat(staging.c_str(), &stage_stat) == 0;
   if (!stage_exists && errno != ENOENT) return storage_failure(&error, "stat relocation staging", staging, errno);
   if (rename_mode) {
    if (location != staging) {
     if (stage_exists) return storage_failure(&error, "relocation staging already exists", staging, EEXIST);
     if (::rename(source.c_str(), staging.c_str()) != 0) return storage_failure(&error, "rename download into staging", source, errno);
     if (!sync_directory(source_root, &error) || !sync_directory(target, &error)) return false;
    }
    if (::stat(staging.c_str(), &stage_stat) != 0 || !move_identity(staging, source_device, source_inode))
     return storage_failure(&error, "verify renamed staging", staging, EIO);
   } else {
    if (!stage_exists) {
     if (::mkdir(staging.c_str(), 0700) != 0) return storage_failure(&error, "create relocation staging", staging, errno);
     if (::stat(staging.c_str(), &stage_stat) != 0 || !sync_directory(target, &error)) return false;
    } else if (!S_ISDIR(stage_stat.st_mode)) return storage_failure(&error, "validate relocation staging", staging, EINVAL);
    const auto expected_inode = uint64_t(integer_value(item, "stage_inode", 0));
    if (expected_inode && !move_identity(staging, uint64_t(integer_value(item, "stage_device", 0)), expected_inode))
     return storage_failure(&error, "relocation staging changed", staging, EIO);
    if (!expected_inode && stage_exists) {
     std::vector<MoveFile> existing; int64_t bytes = 0;
     if (!move_inventory(staging, existing, bytes, error) || !existing.empty())
      return storage_failure(&error, "unclaimed relocation staging is not empty", staging, EEXIST);
    }
   }
   item["stage_device"] = uint64_t(stage_stat.st_dev); item["stage_inode"] = uint64_t(stage_stat.st_ino);
   if (!p.persist_relocation(error)) return false;
   if (!rename_mode && text_value(item, "phase") != "copied") {
    std::vector<MoveFile> files, staged; int64_t bytes = 0, staged_bytes = 0;
    if (!move_inventory(source, files, bytes, error) || !move_inventory(staging, staged, staged_bytes, error)) return false;
    for (const auto& file : staged) if (std::none_of(files.begin(), files.end(), [&](const auto& original) { return original.name == file.name; }))
     return storage_failure(&error, "unexpected staged content", staging + '/' + file.name, EEXIST);
    int64_t before = 0;
    for (const auto& file : files) {
     if (!move_copy_file(source + '/' + file.name, staging + '/' + file.name, file, [&](int64_t bytes_done, bool verifying) {
      p.move_status(verifying ? "verifying" : "moving", {}, completed_bytes + before + bytes_done, done);
      return p.selection_open.load();
     }, error)) return false;
     before += file.size;
    }
   }
   if (!share_download_directory(staging, &error) || !sync_directory(staging, &error)) return false;
   item["phase"] = "copied";
   if (!p.persist_relocation(error)) return false;
   // A private copy carries the new public paths before being published. Do not
   // modify the live source job until the destination directory is durable.
   Impl::Job relocated;
   { std::lock_guard lock(p.mutex); relocated = *job; }
   Impl::rebase_job(relocated, source, staging, destination, target, folder);
   if (!relocated.entry.recovery_only) {
    if (!p.save(relocated)) return storage_failure(&error, "save relocated download metadata", staging, errno);
    json saved;
    if (!read_manifest(staging + "/manifest.json", saved) ||
     !atomic_private_write(staging + "/manifest.json.bak", json_bytes(saved), &error))
     return storage_failure(&error, "save relocated metadata backup", staging, errno ? errno : EIO);
   }
   if (!sync_directory(staging, &error)) return false;
   struct stat exists{};
   if (::lstat(destination.c_str(), &exists) == 0 || errno != ENOENT)
    return storage_failure(&error, "relocation destination already exists", destination, EEXIST);
   if (!move_identity(staging, uint64_t(stage_stat.st_dev), uint64_t(stage_stat.st_ino)) ||
    !move_identity(target, uint64_t(target_stat.st_dev), uint64_t(target_stat.st_ino)))
    return storage_failure(&error, "relocation path identity changed", staging, EIO);
   if (::rename(staging.c_str(), destination.c_str()) != 0) return storage_failure(&error, "publish relocated download", destination, errno);
   if (!sync_directory(target, &error)) return false;
   published = true;
  }
  if (published) {
   item["phase"] = "published";
   if (!p.persist_relocation(error)) return false;
   { std::lock_guard lock(p.mutex);
    Impl::rebase_job(*job, source, destination, destination, target, folder);
    p.touch();
   }
   // The durable journal and union registry name the destination before any
   // original bytes are retired. A failed unlink remains an explicit retry.
   struct stat old{};
   if (::lstat(source.c_str(), &old) == 0) {
    if (!move_identity(source, uint64_t(integer_value(item, "source_device", 0)), uint64_t(integer_value(item, "source_inode", 0))))
     return storage_failure(&error, "source identity changed before retirement", source, EIO);
    if (!remove_flat_directory(source_root, source_id)) return storage_failure(&error, "retire original download", source, errno ? errno : EIO);
    if (!sync_directory(source_root, &error)) return false;
   } else if (errno != ENOENT) return storage_failure(&error, "inspect original download", source, errno);
   item["phase"] = "retired";
   if (!p.persist_relocation(error)) return false;
  }
  completed_bytes += std::max<int64_t>(0, integer_value(item, "bytes", 0)); ++done;
  p.move_status("moving", {}, completed_bytes, done);
 }
 p.move_status("saving", {}, completed_bytes, done);
 if (!p.selection_open) return storage_failure(&error, "interrupt relocation", target, ECANCELED);
 if (!p.save_registry(target, {target}, &error)) return false;
 // A corrupt primary must not restore an obsolete pre-move directory list.
 const auto final_registry = json_bytes(json{{"version", 1}, {"current", target}, {"roots", std::vector<std::string>{target}}});
 if (!atomic_private_write(p.registry_directory + "/download-directories.json.bak", final_registry, &error)) return false;
 for (const char* suffix : {".tmp", ".new"}) {
  const auto path = p.relocation_path() + suffix;
  if (::unlink(path.c_str()) != 0 && errno != ENOENT) return storage_failure(&error, "remove relocation temporary", path, errno);
 }
 if (::unlink(p.relocation_path().c_str()) != 0 && errno != ENOENT) return storage_failure(&error, "complete relocation journal", p.relocation_path(), errno);
 if (!sync_directory(p.registry_directory, &error)) return false;
 {
  std::lock_guard lock(p.mutex);
  p.root = target; p.roots = {target}; p.root_writable.clear(); p.root_writable[target] = true;
  p.loaded_roots = {target}; p.storage_error.clear(); p.relocation_pending = false; p.relocation = json();
  p.refresh_access(); p.touch();
 }
 succeeded = true; return true;
}

DownloadRelocationStatus DownloadManager::relocation_status() const {
 auto& p = *impl_; std::lock_guard lock(p.published_mutex); return p.published_relocation;
}

std::string DownloadManager::download_directory() const {
	auto& p = *impl_; std::lock_guard lock(p.published_mutex); return p.published_directory;
}
std::vector<std::string> DownloadManager::download_directories() const {
	auto& p = *impl_; std::lock_guard lock(p.published_mutex); return p.published_directories;
}

void DownloadManager::shutdown() {
	auto& p = *impl_;
	p.selection_open = false;
	{
		std::lock_guard lock(p.enqueue_mutex);
		p.enqueue_closed = true;
	}
	p.enqueue_changed.notify_all();
	if (p.enqueue_worker.joinable()) p.enqueue_worker.join();
	std::lock_guard lifecycle_lock(p.lifecycle_mutex);
	p.accept_artwork = false;
	{
		std::lock_guard lock(p.mutex);
		p.stopping = true; p.enabled = false; p.cancel.store(true); p.changed.notify_all();
		for (const auto& j : p.jobs) if (j->entry.state == DownloadState::Queued ||
			j->entry.state == DownloadState::Downloading || j->entry.state == DownloadState::Waiting) {
			j->entry.state = DownloadState::Paused; j->entry.bytes_per_second = 0;
		}
		p.touch();
	}
	if (p.worker.joinable()) p.worker.join();
	if (p.artwork_worker.joinable()) p.artwork_worker.join();
	// Preserve late source/path updates without copying queued images during
	// shutdown. They remain visible from the persistent cache on the next boot
	// and are made private before that video's transfer starts.
	p.apply_artwork_updates();
	{
		std::lock_guard lock(p.artwork_updates_mutex);
		p.artwork_updates.clear(); p.artwork_updates_pending = false;
	}
	std::lock_guard lock(p.mutex);
	if (!p.relocation_pending) for (const auto& j : p.jobs) if (!j->entry.recovery_only) p.save(*j);
	p.ready = p.writable = false; p.active_torrent = false; p.active_id.clear(); p.jobs.clear();
	p.storage_parent.clear(); p.root.clear(); p.storage_error.clear(); p.registry_directory.clear();
	p.roots.clear(); p.root_writable.clear(); p.loaded_roots.clear(); p.registry_valid = true;
	p.relocation = json(); p.relocating = p.relocation_pending = false;
	{ std::lock_guard published_lock(p.published_mutex); p.published_relocation = {}; } p.touch(); p.changed.notify_all();
}

std::string DownloadManager::enqueue(const DownloadRequest& request, std::string& error) {
	return enqueue_at_generation(request, error, impl_->suspend_generation.load());
}

std::string DownloadManager::enqueue_at_generation(const DownloadRequest& request, std::string& error, uint64_t generation) {
	const auto started = Clock::now();
	error.clear();
	if (!valid_request(request, error)) return {};
	const std::string identity = download_source_identity(request);
	auto& p = *impl_;
	if (p.relocating || p.relocation_pending) { error = "Download relocation is in progress or requires retry."; return {}; }
	std::lock_guard lifecycle_lock(p.lifecycle_mutex);
	std::unique_lock lock(p.mutex);
	if (p.relocating || p.relocation_pending || !p.selection_open) { error = "Download storage is unavailable during relocation or shutdown."; return {}; }
	// Retry a failed startup only on an explicit download action. A filesystem
	// grant or temporarily unavailable mount must not disable downloads until exit.
	if ((!p.ready || !p.writable) && !p.start_storage()) {
		error = "Download storage is not available."; p.touch(); return {};
	}
	for (const auto& j : p.jobs) if (!j->removing && j->identity == identity) return j->entry.id;
	if (p.jobs.size() >= kMaxJobs) { error = "The limit of 128 downloads has been reached. Delete a video to add another."; return {}; }
	auto job = std::make_shared<Impl::Job>();
	job->request.media_id = request.media_id; job->request.type = request.type; job->request.video_id = request.video_id;
	job->request.title = request.title; job->request.subtitle = request.subtitle;
	job->request.episode_title = request.episode_title;
	job->request.release_year = request.release_year >= 1000 && request.release_year <= 9999 ? request.release_year : 0;
	job->request.poster_path = request.poster_path; job->request.background_path = request.background_path; job->request.logo_path = request.logo_path;
	job->request.thumbnail_path = request.thumbnail_path; job->request.thumbnail_url = request.thumbnail_url;
	job->request.poster_url = request.poster_url; job->request.season = request.season; job->request.episode = request.episode;
	job->request.stream = download_stream(request.stream); job->identity = identity;
	job->artwork_pending = !request.poster_path.empty() || !request.background_path.empty() || !request.logo_path.empty() || !request.thumbnail_path.empty();
	auto& e = job->entry;
	e.media_id = request.media_id; e.type = request.type; e.video_id = request.video_id;
	e.title = request.title; e.subtitle = request.subtitle; e.season = request.season; e.episode = request.episode;
	e.episode_title = request.episode_title; e.source_identity = identity;
	e.release_year = job->request.release_year;
	if (e.season >= 0 && e.episode >= 0 && !e.episode_title.empty())
		e.subtitle = job->request.subtitle = download_episode_label(e.season, e.episode, e.episode_title);
	e.poster_url = request.poster_url;
	e.thumbnail_url = request.thumbnail_url;
	const std::string root = p.root;
	job->storage_root = root;
	const auto sequence = ++p.sequence;
	// This job is still private and cannot be selected, removed or observed.
	// The lifecycle gate serializes duplicate enqueues and excludes shutdown,
	// while active progress and UI snapshots continue without a storage wait.
	lock.unlock();
	if (artwork_dimensions(request.poster_path, e.poster_width, e.poster_height)) e.poster_path = request.poster_path;
	if (regular_size(request.background_path) >= 12) e.background_path = request.background_path;
	if (regular_size(request.logo_path) >= 12) e.logo_path = request.logo_path;
	if (regular_size(request.thumbnail_path) >= 12) e.thumbnail_path = request.thumbnail_path;
	const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
	job->created_ms = e.created_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
	for (int attempt = 0; attempt < 8; ++attempt) {
		e.id = 'd' + digest(identity + std::to_string(stamp) + ':' + std::to_string(sequence) + ':' + std::to_string(attempt));
		job->folder_id = e.id; job->directory = root + '/' + e.id;
		struct stat st{};
		if (::lstat(job->directory.c_str(), &st) != 0 && errno == ENOENT) break;
		e.id.clear();
	}
	bool directory_created = false;
	if (e.id.empty() || !create_download_directory(job->directory, nullptr, &directory_created) ||
		!directory_created || !p.save(*job) || !sync_directory(root)) {
		const int code = errno;
		error = "The download could not be created. Check the available storage space.";
		if (directory_created) remove_flat_directory(root, e.id);
		lock.lock();
		p.block_storage("create download", job->directory, code);
		p.touch();
		dlog("download queue: enqueue_ms=%.1f committed=0", std::chrono::duration<double, std::milli>(Clock::now() - started).count());
		return {};
	}
	lock.lock();
	if (generation != p.suspend_generation.load()) {
		// A submission may be private while logout pauses the published list.
		// Persist its suspended state before exposing it, even if another login
		// has already enabled new work in the meantime.
		e.state = DownloadState::Paused;
		lock.unlock();
		if (!p.save(*job)) {
			error = "The download could not be created. Check the available storage space.";
			remove_flat_directory(root, e.id); return {};
		}
		lock.lock();
	}
	p.jobs.push_back(job); p.touch(); p.changed.notify_all();
	const auto queued = p.jobs.size();
	const bool active = !p.active_id.empty();
	const bool artwork_deferred = active && job->artwork_pending;
	lock.unlock();
	dlog("download queue: enqueue_ms=%.1f committed=1 inventory=%zu media_active=%d artwork_deferred=%d",
		std::chrono::duration<double, std::milli>(Clock::now() - started).count(), queued, active, artwork_deferred);
	return e.id;
}

bool DownloadManager::enqueue_async(DownloadRequest request, EnqueueCallback done) {
	auto& p = *impl_;
	const auto generation = p.suspend_generation.load();
	std::string error;
	if (!valid_request(request, error)) return false;
	request.stream = download_stream(request.stream);
	const size_t bytes = json_bytes(stream_json(request.stream)).size() + request.media_id.size() + request.type.size() +
		request.video_id.size() + request.title.size() + request.subtitle.size() + request.episode_title.size() + request.poster_url.size() +
		request.poster_path.size() + request.background_path.size() + request.logo_path.size() +
		request.thumbnail_path.size() + request.thumbnail_url.size() + 1024;
	std::lock_guard lock(p.enqueue_mutex);
	if (p.relocating || p.relocation_pending || p.enqueue_closed || p.enqueue_work.size() >= kMaxJobs || bytes > kMaxPendingBytes - p.pending_bytes) return false;
	p.enqueue_work.push_back({std::move(request), std::move(done), bytes, generation}); p.pending_bytes += bytes;
	try {
	if (!p.enqueue_worker.joinable()) p.enqueue_worker = std::thread([this] {
		auto& state = *impl_;
		for (;;) {
			Impl::EnqueueWork work;
			bool cancelled = false;
			{
				std::unique_lock lock(state.enqueue_mutex);
				state.enqueue_changed.wait(lock, [&] { return state.enqueue_closed || !state.enqueue_work.empty(); });
				if (state.enqueue_closed && state.enqueue_work.empty()) break;
				cancelled = state.enqueue_closed;
				work = std::move(state.enqueue_work.front()); state.enqueue_work.pop_front();
				state.pending_bytes -= work.bytes;
			}
			std::string id, error;
			try {
				if (cancelled) error = "Download storage is not available.";
				else id = enqueue_at_generation(work.request, error, work.generation);
			}
			catch (...) { error = "The download could not be created. Check the available storage space."; }
			try { if (work.done) work.done(std::move(id), std::move(error)); }
			catch (...) { dlog("download queue: completion callback failed"); }
		}
	});
	} catch (...) { p.pending_bytes -= p.enqueue_work.back().bytes; p.enqueue_work.pop_back(); return false; }
	p.enqueue_changed.notify_one();
	return true;
}

std::vector<DownloadEntry> DownloadManager::snapshot() const {
	auto& p = *impl_; std::lock_guard lock(p.published_mutex);
	return p.published;
}

std::optional<DownloadEntry> DownloadManager::find(const std::string& id) const {
	auto& p = *impl_; std::lock_guard lock(p.published_mutex);
	for (const auto& entry : p.published) if (entry.id == id) return entry;
	return std::nullopt;
}

std::optional<DownloadEntry> DownloadManager::find_matching(const DownloadRequest& request) const {
	const auto identity = download_source_identity(request);
	auto& p = *impl_;
	std::lock_guard lock(p.published_mutex);
	for (const auto& entry : p.published)
		if (!entry.recovery_only && entry.source_identity == identity) return entry;
	return {};
}

bool DownloadManager::update_poster(const std::string& id, const std::string& cached_path) {
	if (cached_path.empty() || cached_path.size() > 16384) return false;
	return impl_->queue_artwork(id, cached_path, {});
}

bool DownloadManager::update_poster_source(const std::string& id, const std::string& url) {
	if (url.empty() || url.size() > 16384) return false;
	return impl_->queue_artwork(id, {}, url);
}

bool DownloadManager::update_thumbnail(const std::string& id, const std::string& cached_path) {
	if (cached_path.empty() || cached_path.size() > 16384) return false;
	return impl_->queue_artwork(id, cached_path, {}, true);
}

bool DownloadManager::update_thumbnail_source(const std::string& id, const std::string& url) {
	if (url.empty() || url.size() > 16384) return false;
	return impl_->queue_artwork(id, {}, url, true);
}

std::shared_ptr<GrowingFilePlayback> DownloadManager::open_progressive(const std::string& id, std::string& error) {
	error.clear();
	auto& p = *impl_; std::lock_guard lock(p.mutex);
	const auto job = p.lookup(id);
	if (p.relocating || p.relocation_pending || !job || job->removing || !job->entry.playable_while_downloading) {
		error = "Play while downloading is available from 5% of an active download.";
		return {};
	}
	if (!safe_directory(job->storage_root) || !safe_directory(job->directory)) {
		error = "The downloaded file is unavailable.";
		return {};
	}
	auto source = std::make_shared<GrowingFilePlayback>();
	source->path = job->directory + "/media.part";
	source->descriptor = ::open(source->path.c_str(), O_RDONLY | O_NOFOLLOW);
	struct stat st{};
	if (source->descriptor < 0 || ::fstat(source->descriptor, &st) || !S_ISREG(st.st_mode) ||
		!progressive_download_threshold(std::min<int64_t>(st.st_size, job->entry.done), job->entry.total)) {
		error = "The downloaded file is not ready for playback yet.";
		return {};
	}
	source->state = job->playback; source->generation = job->playback_generation;
	source->total = job->entry.total;
	if (job->torrent_file_idx >= 0) {
		source->torrent_hash = job->request.stream.info_hash;
		source->torrent_file_idx = job->torrent_file_idx;
	}
	return source;
}

bool DownloadManager::pause(const std::string& id) {
	auto& p = *impl_; std::lock_guard lock(p.mutex); const auto j = p.lookup(id);
	if (p.relocating || p.relocation_pending || !j || j->removing || j->entry.recovery_only || j->entry.state == DownloadState::Complete) return false;
	j->entry.state = DownloadState::Paused; j->entry.bytes_per_second = 0;
	if (p.active_id == id) p.cancel.store(true);
	p.save(*j); p.touch(); p.changed.notify_all(); return true;
}

bool DownloadManager::resume(const std::string& id) {
	auto& p = *impl_; std::lock_guard lock(p.mutex); const auto j = p.lookup(id);
	if (p.relocating || p.relocation_pending || !j || j->removing || j->entry.recovery_only || j->entry.state == DownloadState::Complete) return false;
	// A pause is published before the transfer finishes cancellation. Persist
	// the resume request now; the single worker retires that writer before it
	// can select the queued job again. Never reset its in-flight cancellation.
	if (p.active_id == id && (j->entry.state != DownloadState::Paused || !p.cancel.load())) return false;
	std::string error;
	if (!valid_request(j->request, error)) { j->entry.state = DownloadState::Failed; j->entry.error = error; p.touch(); return false; }
	if (!p.can_write(j->storage_root) && !p.check_write_access(j->storage_root)) {
		j->entry.state = DownloadState::Paused;
		j->entry.error = "Download storage is not available."; p.touch(); return false;
	}
	j->entry.state = torrent_stream(j->request.stream) && p.torrent_reserved() ? DownloadState::Waiting : DownloadState::Queued;
	j->entry.error.clear();
	if (!p.save(*j)) {
		p.block_storage("resume download", j->directory, errno);
		j->entry.state = DownloadState::Paused; j->entry.error = "The download state could not be saved.";
		p.touch(); return false;
	}
	p.touch(); p.changed.notify_all(); return true;
}

bool DownloadManager::remove(const std::string& id, std::string& error) {
	error.clear(); auto& p = *impl_; std::lock_guard lock(p.mutex); const auto j = p.lookup(id);
	if (p.relocating || p.relocation_pending || !safe_id(id) || !j || j->removing || !safe_directory(j->storage_root) || !safe_directory(j->directory)) {
		error = "This download cannot be deleted."; return false;
	}
	j->removing = true;
	if (!j->entry.recovery_only && !p.save(*j)) {
		const int code = errno;
		p.block_storage("save delete request", j->directory, code);
		dlog("download delete: intent_persisted=0 errno=%d; continuing explicit removal", code);
	}
	if (p.active_id == id) p.cancel.store(true);
	p.touch(); p.changed.notify_all(); return true;
}

uint64_t DownloadManager::revision() const { return impl_->revision.load(std::memory_order_relaxed); }

bool DownloadManager::storage_available() const {
	auto& p = *impl_; std::lock_guard lock(p.published_mutex); return p.published_writable;
}

bool DownloadManager::storage_readable() const {
	auto& p = *impl_; std::lock_guard lock(p.published_mutex); return p.published_readable;
}

std::string DownloadManager::storage_error() const {
	auto& p = *impl_; std::lock_guard lock(p.published_mutex); return p.published_storage_error;
}

void DownloadManager::set_enabled(bool enabled) {
	auto& p = *impl_; std::lock_guard lock(p.mutex);
	p.enabled = enabled;
	if (!enabled) p.suspend_generation.fetch_add(1);
	if (p.relocating || p.relocation_pending) { p.changed.notify_all(); return; }
	if (!enabled && !p.active_id.empty()) {
		p.cancel.store(true); const auto j = p.lookup(p.active_id);
		if (j) { j->entry.state = DownloadState::Paused; j->entry.bytes_per_second = 0; if (!p.relocating && !p.relocation_pending) p.save(*j); }
	}
	p.touch(); p.changed.notify_all();
}

void DownloadManager::set_torrent_playback_active(bool active) {
	auto& p = *impl_; std::lock_guard lock(p.mutex);
	p.foreground_torrent = active;
	if (p.relocating || p.relocation_pending) { p.changed.notify_all(); return; }
	if (active && p.active_torrent) { p.torrent_preempted = true; p.cancel.store(true); }
	for (const auto& j : p.jobs) if (torrent_stream(j->request.stream) && !j->removing) {
		if (active && j->entry.state == DownloadState::Queued) j->entry.state = DownloadState::Waiting;
		else if (!p.torrent_reserved() && j->entry.state == DownloadState::Waiting) j->entry.state = DownloadState::Queued;
	}
	p.touch(); p.changed.notify_all();
}

bool DownloadManager::wait_for_torrent_idle(const std::atomic<bool>& cancel) {
	auto& p = *impl_; std::unique_lock lock(p.mutex);
	while (p.active_torrent && !cancel.load()) p.changed.wait_for(lock, std::chrono::milliseconds(50));
	return !p.active_torrent && !cancel.load();
}

std::shared_ptr<void> DownloadManager::acquire_torrent_resolution() {
	auto& p = *impl_;
	std::lock_guard lock(p.mutex);
	const auto counter = p.foreground_resolvers;
	auto* token = new unsigned char(0);
	counter->fetch_add(1);
	if (p.active_torrent) { p.torrent_preempted = true; p.cancel.store(true); }
	p.changed.notify_all();
	// The counter has independent shared lifetime: a cancelled task may be
	// destroyed after the manager. Its release must never dereference the App.
	return std::shared_ptr<void>(token, [counter](void* token) {
		delete static_cast<unsigned char*>(token);
		counter->fetch_sub(1);
	});
}
