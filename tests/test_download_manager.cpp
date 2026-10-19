#include "download_manager.h"
#include "download_transfer.h"
#include "download_metadata.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

void dlog(const char*, ...) {}

namespace {
struct Fake {
	int starts = 0, progress_calls = 0;
	bool release = false, release_cancel = true, cancel_seen = false, returned = false;
	bool failure = false, malformed = false, throws = false;
	bool hold_progress = false, progress_release = false;
	bool periodic_progress = false;
	int storage_error = 0;
	bool fail_completion_save = false;
	int64_t offset = 0, progress_done = 4096;
	int64_t cancelled_prefix = -1, cancelled_file_size = -1;
	DownloadTransferRequest last;
};
std::mutex fake_mutex;
std::condition_variable fake_changed;
std::map<std::string, std::shared_ptr<Fake>> fakes;
int checks = 0, failures = 0;
std::atomic<int> fail_lstat{0}, fail_opendir{0}, fail_mkdir{0}, fail_chmod{0}, fail_fchmod{0}, fail_write{0}, fail_sync{0};
std::string denied_path;
std::atomic<bool> hold_manifest_sync{false}, manifest_sync_entered{false}, release_manifest_sync{true};
std::atomic<unsigned> manifest_sync_count{0};
std::atomic<unsigned> injected_write_failures{0};
std::atomic<bool> hold_registry_sync{false}, registry_sync_entered{false}, release_registry_sync{true}, fail_registry_sync{false};
std::atomic<unsigned> injected_mkdir_failures{0};
std::atomic<bool> ignore_successful_fchmod{false};
thread_local bool ui_polling = false;
std::atomic<unsigned> ui_storage_calls{0};

void check(bool ok, const char* explanation) {
	++checks;
	if (!ok) { ++failures; std::cerr << "FAIL: " << explanation << '\n'; }
}

template<class Test> bool until(Test test, int milliseconds = 3000) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
	do { if (test()) return true; std::this_thread::sleep_for(5ms); } while (std::chrono::steady_clock::now() < deadline);
	return test();
}

std::shared_ptr<Fake> plan(const std::string& name, bool released = false) {
	std::lock_guard lock(fake_mutex);
	auto fake = std::make_shared<Fake>(); fake->release = released; fakes[name] = fake; return fake;
}

template<class Action> void change(const std::shared_ptr<Fake>& fake, Action action) {
	std::lock_guard lock(fake_mutex); action(*fake); fake_changed.notify_all();
}

template<class Test> auto inspect(const std::shared_ptr<Fake>& fake, Test test) {
	std::lock_guard lock(fake_mutex); return test(*fake);
}

DownloadRequest request(const std::string& name, bool torrent = false) {
	DownloadRequest result;
	result.media_id = "tt1234567"; result.type = "movie"; result.video_id = "tt1234567";
	result.title = "Offline film"; result.subtitle = "Provider · 1080p";
	result.stream.filename = name; result.stream.name = "Selected source";
	result.stream.kind = torrent ? StreamKind::Torrent : StreamKind::Direct;
	if (torrent) result.stream.info_hash = std::string(40, name.empty() ? 'a' : 'b');
	else result.stream.url = "https://provider.invalid/private-token/" + name + ".mp4";
	result.stream.request_headers = {"Authorization: Bearer PRIVATE-DOWNLOAD-TOKEN"};
	return result;
}

std::string bytes(const fs::path& path) {
	std::ifstream input(path, std::ios::binary); return std::string(std::istreambuf_iterator<char>(input), {});
}

void write(const fs::path& path, const std::string& contents) {
	std::ofstream output(path, std::ios::binary | std::ios::trunc); output.write(contents.data(), contents.size());
}

json manifest(const fs::path& path) { return json::parse(bytes(path)); }

void artwork(const fs::path& path) {
	std::ofstream output(path, std::ios::binary);
	const uint32_t width = 2, height = 2;
	output.write("RGBA", 4); output.write(reinterpret_cast<const char*>(&width), 4);
	output.write(reinterpret_cast<const char*>(&height), 4); output.write("0123456789abcdef", 16);
}

bool state(DownloadManager& manager, const std::string& id, DownloadState expected) {
	const auto value = manager.find(id); return value && value->state == expected;
}

void storage_permissions_and_retry(const fs::path& data) {
	fs::create_directories(data);
	DownloadManager manager; std::string error;
	const auto selected = plan("permission-retry", true);
	struct stat st{};
	check(::stat(data.c_str(), &st) == 0 && S_ISDIR(st.st_mode), "pre-grant fixture permits stat like the native loader");
	denied_path = data.string(); fail_lstat = EPERM;
	check(!manager.init(data.string(), &error) && !manager.storage_available(), "pre-grant lstat EPERM cannot pass the storage safety check");
	check(error.find("lstat") != std::string::npos && error.find(data.string()) != std::string::npos &&
		error.find("errno " + std::to_string(EPERM)) != std::string::npos,
		"startup diagnostics identify lstat, the denied local path and EPERM");
	check(manager.storage_error() == error, "startup storage failure remains available to application diagnostics");
	manager.set_enabled(true);
	check(manager.enqueue(request("permission-retry"), error).empty() &&
		error == "Download storage is not available." && manager.snapshot().empty(),
		"retry while filesystem permission is denied keeps a localized UI error and creates no job");
	check(inspect(selected, [](const Fake& f) { return f.starts == 0; }), "failed storage initialization never starts a network transfer");
	fail_lstat = 0;
	const auto id = manager.enqueue(request("permission-retry"), error);
	check(!id.empty() && error.empty() && manager.storage_available() && manager.storage_error().empty(),
		"next explicit download recovers after the filesystem grant without restarting the app");
	check(until([&] { return state(manager, id, DownloadState::Complete); }), "storage recovery preserves the enabled state and completes the selected download");
	check(manager.enqueue(request("permission-retry"), error) == id && manager.snapshot().size() == 1 &&
		inspect(selected, [](const Fake& f) { return f.starts == 1; }),
		"recovery starts one worker and repeated enqueue cannot duplicate the selected transfer");
	check(!fs::exists(data / "downloads" / ".storage-check") && !fs::exists(data / "downloads" / ".storage-check.tmp"),
		"successful storage verification leaves no probe files");
	manager.shutdown();
	check(manager.enqueue(request("permission-retry"), error).empty() && !manager.storage_available(),
		"explicit shutdown cannot be undone by a later enqueue");

	::chmod(data.c_str(), 0777); ::chmod((data / "downloads").c_str(), 0777);
	denied_path = (data / "downloads").string(); fail_chmod = EPERM;
	check(manager.init(data.string(), &error) && error.empty(),
		"an existing user-configured directory does not require or attempt chmod");
	::stat(data.c_str(), &st);
	check((st.st_mode & 0777) == 0777, "the existing application data directory keeps its user-set mode");
	::stat((data / "downloads").c_str(), &st);
	check((st.st_mode & 0777) == 0777, "initialization and restart preserve the existing 0777 download directory");
	manager.shutdown();
	fail_chmod = 0; fail_mkdir = EROFS; fail_write = EROFS;
	const auto mkdir_failures = injected_mkdir_failures.load();
	check(!manager.init(data.string(), &error) && manager.storage_readable() &&
		state(manager, id, DownloadState::Complete) && injected_mkdir_failures.load() == mkdir_failures,
		"a readable existing download root never calls mkdir even when the backend rejects mutations with EROFS");
	manager.shutdown(); fail_mkdir = 0; fail_write = 0; fail_chmod = EPERM;
	const auto fresh = data / "fresh"; fs::create_directory(fresh);
	denied_path = (fresh / "downloads").string();
	{
		DownloadManager private_manager;
		check(!private_manager.init(fresh.string(), &error) && error.find("chmod") != std::string::npos,
			"chmod denial is still reported for a newly created private directory");
	}
	denied_path = (data / "downloads").string();
	fail_chmod = 0; fail_fchmod = EPERM;
	check(!manager.init(data.string(), &error) && error.find("fchmod") != std::string::npos,
		"write probe detects file permission denial before reporting storage ready");
	fail_fchmod = 0; fail_write = ENOSPC;
	check(!manager.init(data.string(), &error) && error.find("write") != std::string::npos &&
		error.find("errno " + std::to_string(ENOSPC)) != std::string::npos,
		"write probe detects a full disk even when mkdir and stat succeed");
	fail_write = 0; fail_sync = EIO;
	check(!manager.init(data.string(), &error) && error.find("fsync") != std::string::npos,
		"write probe requires durable storage instead of accepting a failed fsync");
	fail_sync = 0;
	check(!fs::exists(data / "downloads" / ".storage-check.tmp"), "failed storage probes clean up their temporary file");
	check(manager.init(data.string(), &error) && error.empty() && state(manager, id, DownloadState::Complete),
		"storage reinitialization preserves an existing completed offline video");
	manager.shutdown();
	fail_opendir = EPERM;
	const bool denied = !manager.init(data.string(), &error) && !manager.storage_available() &&
		error.find("opendir") != std::string::npos && error.find(denied_path) != std::string::npos &&
		error.find("errno " + std::to_string(EPERM)) != std::string::npos;
	fail_opendir = 0;
	const auto recovered_id = manager.enqueue(request("permission-retry"), error);
	check(denied && recovered_id == id && error.empty() && manager.storage_available() &&
		manager.snapshot().size() == 1 && state(manager, id, DownloadState::Complete),
		"unlistable inventory rejects readiness with precise errno and retry preserves the completed entry");
	manager.shutdown();
}

int64_t allocated_bytes(const fs::path& directory) {
	int64_t total = 0;
	for (const auto& file : fs::directory_iterator(directory)) {
		struct stat st{};
		if (::lstat(file.path().c_str(), &st) == 0 && S_ISREG(st.st_mode)) total += st.st_blocks * 512;
	}
	return total;
}

void full_storage_inventory(const fs::path& data) {
	fs::create_directories(data);
	DownloadManager manager; std::string error;
	const auto partial_plan = plan("stored-partial");
	check(manager.init(data.string()), "full-storage fixture initially creates writable storage");
	const auto partial_id = manager.enqueue(request("stored-partial", true), error);
	const auto complete_id = manager.enqueue(request("stored-complete"), error);
	manager.shutdown();
	const auto root = data / "downloads";
	const auto partial = root / partial_id, complete = root / complete_id;
	auto pending = manifest(partial / "manifest.json");
	pending["state"] = static_cast<int>(DownloadState::Downloading); pending["done"] = 1024; pending["total"] = 8192;
	pending["artwork_pending"] = true;
	write(partial / "manifest.json", pending.dump()); write(partial / "media.part", std::string(4096, 'p'));
	const auto checkpoint = json{{"version", 1}, {"kind", "torrent"}, {"source", std::string(40, 'b') + ":4"},
		{"bytes", 2048}, {"total", 8192}, {"extension", ".mp4"}}.dump();
	write(partial / "transfer.json", checkpoint);
	auto finished = manifest(complete / "manifest.json");
	finished["state"] = static_cast<int>(DownloadState::Complete); finished["done"] = finished["total"] = 8192;
	finished["media_name"] = "media.mp4"; finished["artwork_pending"] = false;
	write(complete / "manifest.json", finished.dump()); write(complete / "media.mp4", std::string(8192, 'v'));

	const auto bad_id = 'd' + std::string(32, 'a'), missing_id = 'd' + std::string(32, 'c');
	const auto backup_id = 'd' + std::string(32, 'e'), temporary_id = 'd' + std::string(32, 'f');
	const auto locked_id = 'd' + std::string(32, '1');
	for (const auto& id : {bad_id, missing_id, backup_id, temporary_id, locked_id}) fs::create_directory(root / id);
	write(root / bad_id / "manifest.json", "{damaged"); write(root / bad_id / "media.part", std::string(1024, 'b'));
	write(root / missing_id / "media.part", std::string(3072, 'm'));
	const auto outside = data / "outside.bin"; write(outside, std::string(16384, 'o'));
	fs::create_symlink(outside, root / missing_id / "outside-link");
	auto backup = pending; backup["id"] = backup_id; backup["title"] = "Saved backup title";
	backup["artwork_pending"] = false; backup["removing"] = true;
	write(root / backup_id / "manifest.json", "broken"); write(root / backup_id / "manifest.json.bak", backup.dump());
	write(root / backup_id / "media.part", std::string(2048, 'r'));
	auto temporary = backup; temporary["id"] = temporary_id; temporary["title"] = "Saved temporary title";
	const auto temporary_bytes = temporary.dump();
	write(root / temporary_id / "manifest.json.tmp", temporary_bytes);
	write(root / temporary_id / "media.part", std::string(4096, 't'));
	write(root / locked_id / "media.part", std::string(2048, 'l'));
	const auto bad_size = allocated_bytes(root / bad_id), missing_size = allocated_bytes(root / missing_id);
	denied_path = (root / locked_id).string(); fail_opendir = EACCES; fail_write = ENOSPC;
	check(!manager.init(data.string(), &error) && !manager.storage_available() && manager.storage_readable() &&
		error.find("errno " + std::to_string(ENOSPC)) != std::string::npos,
		"full storage reports unavailable writes while retaining readable inventory");
	check(manager.snapshot().size() == 7, "full storage publishes valid, backup, temporary and orphaned jobs together");
	const auto part = manager.find(partial_id), movie = manager.find(complete_id);
	check(part && !part->recovery_only && part->state == DownloadState::Paused && part->done == 4096 && part->local_path.empty(),
		"an interrupted partial remains visible and paused on full storage");
	check(movie && movie->state == DownloadState::Complete && movie->local_path == (complete / "media.mp4").string(),
		"a validated completed video remains playable on full storage");
	const auto bad = manager.find(bad_id), missing = manager.find(missing_id), locked = manager.find(locked_id);
	check(bad && bad->recovery_only && bad->disk_bytes == bad_size && bad->local_path.empty() && bad->media_id.empty(),
		"malformed metadata exposes allocated bytes without inventing a video identity or playable file");
	check(missing && missing->recovery_only && missing->disk_bytes == missing_size && !missing->playable_while_downloading,
		"missing metadata remains manageable and storage accounting never follows a symlink");
	check(locked && locked->recovery_only && locked->disk_bytes == -1,
		"an unreadable owned folder remains visible with unknown occupied space");
	check(manager.find(backup_id) && manager.find(backup_id)->title == "Saved backup title" &&
		manager.find(temporary_id) && manager.find(temporary_id)->title == "Saved temporary title",
		"valid backup and temporary metadata recover their recorded titles without automatic deletion");
	check(!manager.resume(missing_id) && !manager.pause(missing_id) && !manager.update_poster_source(missing_id, "https://example.invalid/poster.jpg"),
		"recovery-only entries cannot start transfers, pause or receive guessed artwork");
	const auto failures_before = injected_write_failures.load();
	manager.set_enabled(true); std::this_thread::sleep_for(200ms);
	check(injected_write_failures.load() == failures_before && inspect(partial_plan, [](const Fake& fake) { return fake.starts == 0; }),
		"full storage does not repeatedly retry metadata, artwork or queued transfers");
	check(!manager.resume(partial_id) && bytes(partial / "media.part") == std::string(4096, 'p') &&
		bytes(partial / "transfer.json") == checkpoint,
		"a failed explicit resume preserves every partial byte and the last durable transfer checkpoint");
	check(manager.remove(partial_id, error) && error.empty(), "explicit deletion is accepted when its intent manifest cannot be allocated");
	check(until([&] { return !fs::exists(partial); }) && manager.find(complete_id).has_value(),
		"full-storage deletion removes only the selected partial and retains other downloads");
	check(manager.remove(bad_id, error) && until([&] { return !fs::exists(root / bad_id); }),
		"malformed owned storage can be deleted without requiring valid metadata");
	manager.shutdown();
	check(bytes(root / temporary_id / "manifest.json.tmp") == temporary_bytes,
		"a failed shutdown save preserves the only valid recovered temporary manifest");
	check(!fs::exists(root / missing_id / "manifest.json") && !fs::exists(root / locked_id / "manifest.json"),
		"shutdown never creates invented metadata for recovery-only folders");
	fail_write = 0; fail_opendir = 0;
	check(manager.init(data.string()) && manager.find(complete_id)->state == DownloadState::Complete,
		"writable storage can recover without losing the completed offline video");
	check(manager.remove(missing_id, error) && until([&] { return !fs::exists(root / missing_id); }) &&
		bytes(outside) == std::string(16384, 'o'), "explicit orphan deletion unlinks symlinks without deleting their targets");
	manager.shutdown();
	fail_write = EROFS;
	check(!manager.init(data.string()) && manager.storage_readable() && manager.find(complete_id)->state == DownloadState::Complete,
		"read-only storage also preserves the readable offline library");
	manager.shutdown(); fail_write = 0;
}

void completion_reconciliation(const fs::path& data) {
	fs::create_directories(data);
	DownloadManager manager; std::string error;
	check(manager.init(data.string()), "completion reconciliation fixture initializes");
	auto selected = request("completion-recovery", true); selected.stream.file_idx = 4;
	const auto id = manager.enqueue(selected, error); manager.shutdown();
	const auto folder = data / "downloads" / id;
	auto value = manifest(folder / "manifest.json");
	value["state"] = static_cast<int>(DownloadState::Downloading); value["done"] = 4096; value["total"] = 8192;
	value["media_name"] = ""; value["artwork_pending"] = false;
	auto checkpoint = json{{"version", 1}, {"kind", "torrent"}, {"source", selected.stream.info_hash + ":4"},
		{"bytes", 8192}, {"total", 8192}, {"extension", ".mp4"}};
	write(folder / "manifest.json", value.dump()); write(folder / "transfer.json", checkpoint.dump());
	write(folder / "media.mp4", std::string(8192, 'c'));
	check(manager.init(data.string()) && state(manager, id, DownloadState::Complete) &&
		manager.find(id)->local_path == (folder / "media.mp4").string(),
		"a promoted torrent file is reconciled only with exact durable total and selected source identity");
	manager.shutdown();
	checkpoint["source"] = selected.stream.info_hash + ":5";
	write(folder / "manifest.json", value.dump()); write(folder / "transfer.json", checkpoint.dump());
	check(manager.init(data.string()) && !state(manager, id, DownloadState::Complete) && manager.find(id)->local_path.empty(),
		"a checkpoint for another torrent file cannot prove completion");
	manager.shutdown();
	checkpoint["source"] = selected.stream.info_hash + ":4"; checkpoint["bytes"] = 4096;
	write(folder / "manifest.json", value.dump()); write(folder / "transfer.json", checkpoint.dump());
	check(manager.init(data.string()) && manager.find(id)->local_path.empty(), "a partial checkpoint cannot promote a full-length final file");
	manager.shutdown();
	checkpoint["bytes"] = 8192; write(folder / "transfer.json", checkpoint.dump());
	fs::rename(folder / "media.mp4", folder / "media.part"); write(folder / "manifest.json", value.dump());
	check(manager.init(data.string()) && state(manager, id, DownloadState::Paused) && manager.find(id)->local_path.empty(),
		"a full-size part remains paused until the transfer completes its final promotion");
	manager.shutdown();
}

void transfer_storage_failure(const fs::path& data) {
	fs::create_directories(data);
	DownloadManager manager; std::string error;
	const auto first = plan("media-storage-error"), second = plan("queued-storage-error");
	check(manager.init(data.string()), "transfer storage failure fixture initializes");
	const auto first_id = manager.enqueue(request("media-storage-error", true), error);
	auto second_request = request("queued-storage-error", true); second_request.stream.info_hash = std::string(40, 'c');
	const auto second_id = manager.enqueue(second_request, error);
	manager.set_enabled(true);
	check(until([&] { return inspect(first, [](const Fake& f) { return f.progress_calls > 0; }); }),
		"the selected transfer starts before its storage failure is injected");
	const auto revision = manager.revision();
	change(first, [](Fake& f) { f.storage_error = ENOSPC; f.release = true; });
	check(until([&] { return state(manager, first_id, DownloadState::Failed) && !manager.storage_available(); }) &&
		manager.storage_readable() && manager.revision() > revision,
		"a transfer's storage errno blocks further writes and publishes the capability change");
	check(state(manager, second_id, DownloadState::Paused) && inspect(second, [](const Fake& f) { return f.starts == 0; }),
		"queued downloads do not start after the preceding transfer exhausted storage");
	change(first, [](Fake& f) { f.storage_error = 0; });
	check(manager.resume(first_id) && until([&] { return state(manager, first_id, DownloadState::Complete); }),
		"explicit resume rechecks recovered write access before restarting the same source");
	manager.set_enabled(false); manager.shutdown();

	const auto completion = plan("completion-storage-error", true);
	change(completion, [](Fake& f) { f.fail_completion_save = true; });
	check(manager.init(data.string()), "completion metadata failure fixture initializes");
	const auto complete_id = manager.enqueue(request("completion-storage-error"), error);
	manager.set_enabled(true);
	check(until([&] { return state(manager, complete_id, DownloadState::Complete) && !manager.storage_available(); }),
		"a proved complete local video stays complete when only its final metadata save fails");
	const auto saved = manager.find(complete_id);
	check(saved && !saved->local_path.empty() && fs::is_regular_file(saved->local_path) && fs::file_size(saved->local_path) == 8192,
		"failure to save completion metadata does not hide or remove the completed media");
	fail_write = 0; manager.set_enabled(false); manager.shutdown();
	check(manager.init(data.string()) && state(manager, complete_id, DownloadState::Complete),
		"completion metadata persists once writes recover and survives restart");
	manager.shutdown();
}

void basic_queue(const fs::path& data) {
	fs::create_directories(data);
	const auto image = data / "cached.rgba"; artwork(image);
	auto selected = plan("one"), alternative = plan("two");
	DownloadManager manager;
	std::string error;
	check(manager.init(data.string(), &error) && error.empty(), "initialization creates private download storage");
	auto first = request("one"); first.poster_path = first.background_path = first.logo_path = image.string();
	first.release_year = 1999;
	const auto revision = manager.revision();
	const auto id = manager.enqueue(first, error);
	check(!id.empty() && error.empty() && manager.snapshot().size() == 1, "only the explicitly selected stream is queued");
	check(manager.revision() > revision, "inventory revisions notify the UI of a queued stream");
	check(manager.find(id)->release_year == 1999 && manager.find(id)->created_ms > 0 && manager.find(id)->completed_ms == 0,
		"a new request retains its catalog year and queue date without inventing a completion time");
	check(until([&] { auto e = manager.find(id); return e && !e->logo_path.empty(); }), "cached poster, hero and logo are copied on the worker");
	check(inspect(selected, [](const Fake& f) { return f.starts == 0; }), "init and enqueue do not start networking while disabled");
	check(manager.enqueue(first, error) == id && manager.snapshot().size() == 1, "same selected stream is deduplicated");
	const auto second_id = manager.enqueue(request("two"), error);
	check(!second_id.empty() && second_id != id && manager.snapshot().size() == 2, "different stream quality/source remains a separate download");
	manager.set_enabled(true);
	check(until([&] { return inspect(selected, [](const Fake& f) { return f.starts == 1; }); }), "enabled worker starts the first selected transfer");
	check(inspect(alternative, [](const Fake& f) { return f.starts == 0; }), "download queue has one active transfer");
	check(manager.pause(id), "active transfer can be paused");
	check(until([&] { return state(manager, id, DownloadState::Paused) && inspect(selected, [](const Fake& f) { return f.returned; }); }), "pause propagates cancellation to the transport");
	check(manager.find(id)->local_path.empty(), "paused partial media is never exposed as an offline video");
	check(manager.find(id)->done == 4096, "pause retains actual partial byte progress");
	check(manager.remove(second_id, error), "another queued or active transfer can be removed");
	check(until([&] { return !fs::exists(data / "downloads" / second_id); }), "removal waits for transport cancellation then deletes its files");
	check(manager.resume(id), "paused selected stream can resume");
	check(until([&] { return inspect(selected, [](const Fake& f) { return f.starts == 2; }); }), "resume invokes the real transfer boundary again");
	check(inspect(selected, [](const Fake& f) { return f.offset == 4096; }), "safe partial file remains available for transport resume");
	change(selected, [](Fake& f) { f.release = true; });
	check(until([&] { return state(manager, id, DownloadState::Complete); }), "successful transfer is published as complete");
	const auto completed = *manager.find(id);
	check(completed.completed_ms >= completed.created_ms && completed.completed_ms > 0,
		"successful media promotion records an actual completion timestamp");
	check(completed.done == 8192 && completed.total == 8192 && completed.progress == 1, "complete inventory records exact final bytes");
	check(fs::is_regular_file(completed.local_path) && !fs::exists(data / "downloads" / id / "media.part"), "only atomic part promotion creates the offline media path");
	check(bytes(completed.poster_path) == bytes(image) && bytes(completed.background_path) == bytes(image), "offline artwork matches the cached RGBA bytes");
	struct stat st{};
	::stat((data / "downloads" / id / "manifest.json").c_str(), &st);
	check((st.st_mode & 0777) == 0600, "provider credentials are persisted in a private 0600 manifest");
	::stat((data / "downloads").c_str(), &st);
	check((st.st_mode & 0777) == 0777, "download directory retains loader-compatible 0777 permissions");
	check(bytes(data / "downloads" / id / "manifest.json").find("PRIVATE-DOWNLOAD-TOKEN") != std::string::npos, "exact selected stream headers survive persistence");
	fs::remove(image);
	manager.shutdown();
	check(manager.init(data.string()), "inventory can restart without a server or account request");
	check(state(manager, id, DownloadState::Complete), "completed local file survives restart");
	check(manager.find(id)->local_path == completed.local_path && fs::exists(manager.find(id)->logo_path), "restart retains offline playback and artwork independently of art cache");
	check(manager.find(id)->release_year == 1999 && manager.find(id)->created_ms == completed.created_ms &&
		manager.find(id)->completed_ms == completed.completed_ms,
		"release year and both original timestamps survive restart without being replaced by file modification times");
	manager.shutdown();
	auto legacy = json::parse(bytes(data / "downloads" / id / "manifest.json"));
	legacy.erase("release_year"); legacy.erase("completed_ms");
	legacy["subtitle"] = " 1999 "; legacy["created_ms"] = 946684800000ll;
	write(data / "downloads" / id / "manifest.json", legacy.dump());
	check(manager.init(data.string()) && manager.find(id)->release_year == 1999 &&
		manager.find(id)->created_ms == 946684800000ll && manager.find(id)->completed_ms == 0,
		"legacy completed movie recovers only the saved year and admission date, never the rewritten manifest time");
	manager.shutdown();
	legacy["type"] = "series"; legacy["subtitle"] = "2024";
	write(data / "downloads" / id / "manifest.json", legacy.dump());
	check(manager.init(data.string()) && manager.find(id)->release_year == 0,
		"legacy series subtitle cannot be misread as its release year");
	manager.shutdown();
}

void episode_metadata_and_permissions(const fs::path& data) {
	fs::create_directories(data);
	check(download_release_year("2024") == 2024 && download_release_year(" 2018–2023 ") == 2018 &&
		download_release_year("2019 -") == 2019,
		"catalog years and bounded or ongoing series ranges retain their start year");
	check(download_release_year("1080p") == 0 && download_release_year("S01E02 · 2024") == 0 &&
		download_release_year("Movie.2024.1080p.mkv") == 0 && download_release_year("2024 · Provider") == 0 &&
		download_release_year("2024-2019") == 0,
		"resolution, episode text, filenames, provider labels and invalid ranges never become release years");
	const mode_t original_umask = ::umask(0077);
	struct ResetMask { mode_t value; ~ResetMask() { ::umask(value); } } reset{original_umask};
	DownloadManager manager;
	std::string error;
	check(manager.init(data.string(), &error), "restrictive umask still permits automatic download storage");
	auto first = request("episode-metadata");
	first.media_id = "series:metadata"; first.type = "series"; first.video_id = "opaque:selected";
	first.title = "A series"; first.season = 0; first.episode = 7; first.episode_title = "The special episode";
	first.subtitle = "Old incomplete label";
	first.release_year = 2017;
	const auto id = manager.enqueue(first, error);
	check(!id.empty(), "selected episode metadata is accepted without requiring a numeric video ID");
	const auto job = data / "downloads" / id;
	struct stat root_mode{}, job_mode{}, metadata_mode{};
	check(::stat((data / "downloads").c_str(), &root_mode) == 0 && (root_mode.st_mode & 07777) == 0777 &&
		::stat(job.c_str(), &job_mode) == 0 && (job_mode.st_mode & 07777) == 0777,
		"automatically created root and episode directory are explicitly0777 despite umask0077");
	check(::stat((job / "manifest.json").c_str(), &metadata_mode) == 0 && (metadata_mode.st_mode & 0777) == 0600,
		"shared media directories keep source credentials private in their manifest");
	const auto saved = manager.find(id);
	check(saved && saved->title == "A series" && saved->subtitle == "S00E07 · The special episode" &&
		saved->episode_title == "The special episode" && !saved->source_identity.empty(),
		"published episode includes series title, specials season, episode number and episode title");
	check(manager.find_matching(first) && manager.find_matching(first)->id == id,
		"online playback can locate the exact selected download without creating another job");
	auto different = first; different.episode = 8; different.video_id = "opaque:next";
	check(!manager.find_matching(different), "the next episode cannot reuse this episode's download");
	different = first; ++different.stream.file_idx;
	check(!manager.find_matching(different), "another file from the same torrent is not the same download");
	different = first; different.stream.request_headers.push_back("X-Different-Source: 1");
	check(!manager.find_matching(different), "a different authorized source configuration is not silently substituted");
	manager.shutdown();
	const auto manifest_before = bytes(job / "manifest.json");
	const auto unrelated = data / "downloads" / "user-private-directory";
	fs::create_directory(unrelated); ::chmod(unrelated.c_str(), 0700);
	::chmod((data / "downloads").c_str(), 0700); ::chmod(job.c_str(), 0700);
	check(manager.init(data.string(), &error), "restart repairs legacy private download directories automatically");
	struct stat repaired{};
	check(::stat(job.c_str(), &repaired) == 0 && repaired.st_ino == job_mode.st_ino &&
		(repaired.st_mode & 07777) == 0777 && bytes(job / "manifest.json") == manifest_before,
		"restart repairs permissions without recreating the job or overwriting its manifest");
	check(::stat(unrelated.c_str(), &repaired) == 0 && (repaired.st_mode & 0777) == 0700,
		"automatic permission repair leaves unrelated user directories untouched");
	const auto restored = manager.find(id);
	check(restored && restored->release_year == 2017 && restored->created_ms == saved->created_ms &&
		restored->created_ms > 0 && restored->completed_ms == 0 && restored->state == DownloadState::Paused,
		"pending entry restart retains its catalog year and admission time without claiming a completed download");
	check(restored && restored->episode_title == first.episode_title && restored->subtitle == "S00E07 · The special episode" &&
		restored->source_identity == saved->source_identity,
		"episode title and stable selected-source identity survive restart without provider metadata");
	manager.shutdown();
	auto legacy = json::parse(bytes(job / "manifest.json"));
	legacy.erase("episode_title"); legacy["subtitle"] = "S0 · E7 · The special episode";
	write(job / "manifest.json", legacy.dump());
	check(manager.init(data.string(), &error) && manager.find(id)->episode_title == first.episode_title &&
		manager.find(id)->subtitle == "S00E07 · The special episode",
		"older manifests recover their episode title from the previously persisted label");
	manager.shutdown();
}

void crash_and_validation(const fs::path& data) {
	fs::create_directories(data);
	auto running = plan("crash");
	DownloadManager manager; std::string error;
	check(manager.init(data.string()), "crash fixture storage initializes");
	const auto id = manager.enqueue(request("crash"), error);
	manager.set_enabled(true);
	check(until([&] { return inspect(running, [](const Fake& f) { return f.starts == 1; }); }), "crash fixture reaches active transfer");
	manager.shutdown();
	const auto path = data / "downloads" / id;
	auto value = manifest(path / "manifest.json");
	value["state"] = static_cast<int>(DownloadState::Downloading); value["done"] = 8192; value["total"] = 8192;
	write(path / "media.part", std::string(8192, 'x')); write(path / "manifest.json", value.dump());
	check(manager.init(data.string()), "interrupted manifest is accepted on restart");
	check(state(manager, id, DownloadState::Paused) && manager.find(id)->local_path.empty(), "even a full-size .part after crash is restored Paused, never Complete");
	check(inspect(running, [](const Fake& f) { return f.starts == 1; }), "restart never silently starts downloads");
	manager.shutdown();
	value["state"] = static_cast<int>(DownloadState::Complete); value["media_name"] = "media.part";
	value["removing"] = "wrong-type"; value["artwork_pending"] = json::array();
	write(path / "manifest.json", value.dump());
	check(manager.init(data.string()) && state(manager, id, DownloadState::Failed), "malformed booleans cannot crash load and .part cannot masquerade as a complete file");
	manager.shutdown();
	const auto outside = data / "outside.mp4"; write(outside, std::string(8192, 'o'));
	value["media_name"] = "../../outside.mp4"; write(path / "manifest.json", value.dump());
	check(manager.init(data.string()) && manager.find(id)->local_path.empty(), "manifest cannot publish media outside its own job directory");
	check(!manager.remove("../../outside.mp4", error) && bytes(outside).size() == 8192, "delete rejects path traversal identifiers");
	manager.shutdown();
	const auto moved = data / "saved-job"; fs::rename(path, moved); fs::create_directory_symlink(moved, path);
	check(manager.init(data.string()) && manager.snapshot().empty(), "inventory refuses symlinked job directories");
	manager.shutdown(); fs::remove(path); fs::rename(moved, path);
	value["media_name"] = "media.mp4"; value["state"] = static_cast<int>(DownloadState::Complete);
	fs::create_symlink(outside, path / "media.mp4"); write(path / "manifest.json", value.dump());
	check(manager.init(data.string()) && state(manager, id, DownloadState::Failed), "completed media symlink is rejected");
	check(manager.remove(id, error), "removal may unlink an owned symlink without following it");
	check(until([&] { return !fs::exists(path); }) && bytes(outside).size() == 8192, "symlink target outside download storage is preserved");
	manager.shutdown();
}

void progress_persistence(const fs::path& data) {
	fs::create_directories(data);
	const auto running = plan("progress-persistence");
	DownloadManager manager; std::string error;
	check(manager.init(data.string()), "progress persistence fixture initializes");
	const auto selected = request("progress-persistence", true);
	const auto id = manager.enqueue(selected, error);
	const auto job = data / "downloads" / id;
	check(until([&] { return !manifest(job / "manifest.json").value("artwork_pending", true); }),
		"initial artwork state settles before counting progress writes");
	manager.set_enabled(true);
	check(until([&] { return inspect(running, [](const Fake& f) { return f.progress_calls > 0; }); }),
		"first transfer progress finishes metadata persistence");
	const auto first_manifest = bytes(job / "manifest.json");
	const auto initial = json::parse(first_manifest);
	check(initial.at("total") == 8192 && initial.at("done") == 4096 &&
		initial.at("state") == static_cast<int>(DownloadState::Downloading),
		"resolved total is persisted as soon as it becomes known");
	check(initial.at("stream").at("info_hash") == selected.stream.info_hash &&
		initial.at("stream").at("request_headers") == selected.stream.request_headers,
		"metadata persistence retains the exact selected source identity and headers");
	const auto saves = manifest_sync_count.load();
	const auto revision = manager.revision();
	change(running, [](Fake& f) { f.periodic_progress = true; f.progress_done = 6144; });
	const auto started = std::chrono::steady_clock::now();
	check(until([&] {
		return std::chrono::steady_clock::now() - started >= 5500ms &&
			inspect(running, [](const Fake& f) { return f.progress_calls >= 21; });
	}, 6500), "progress continues across the former five-second persistence interval");
	change(running, [](Fake& f) { f.periodic_progress = false; });
	const auto updated = manager.find(id);
	check(updated && updated->done == 6144 && updated->total == 8192 && updated->progress == 0.75 &&
		manager.revision() > revision, "progress-only changes still publish the current UI inventory");
	check(manifest_sync_count.load() == saves && bytes(job / "manifest.json") == first_manifest,
		"periodic progress performs no additional manifest fsync or rewrite");
	std::cout << "Progress persistence: " << inspect(running, [](const Fake& f) { return f.progress_calls; })
		<< " updates across 5.5 seconds, " << manifest_sync_count.load() - saves << " extra manifest flushes\n";

	const auto recovery = data / "interrupted";
	const auto saved_job = recovery / "downloads" / id;
	fs::create_directories(saved_job);
	write(saved_job / "manifest.json", first_manifest);
	write(saved_job / "media.part", std::string(6144, 'a'));
	write(saved_job / "transfer.json", json{{"version", 1}, {"kind", "http"}, {"source", "different-source"},
		{"bytes", 7000000}, {"total", 9000000}}.dump());
	DownloadManager restored;
	check(restored.init(recovery.string()), "an interrupted inventory snapshot restores without a live transfer");
	const auto recovered = restored.find(id);
	check(recovered && recovered->state == DownloadState::Paused && recovered->done == 6144 &&
		recovered->total == 8192 && recovered->progress == 0.75 && recovered->local_path.empty(),
		"restart recovers current partial bytes and the persisted total without trusting foreign checkpoint metadata");
	restored.shutdown();
	check(manifest(saved_job / "manifest.json").at("state") == static_cast<int>(DownloadState::Paused),
		"restored state remains durable on shutdown");

	const auto before_pause = manifest_sync_count.load();
	check(manager.pause(id) && until([&] { return inspect(running, [](const Fake& f) { return f.returned; }); }),
		"pause stops the transfer after progress-only updates");
	manager.set_enabled(false);
	const auto paused = manifest(job / "manifest.json");
	check(manifest_sync_count.load() > before_pause && paused.at("state") == static_cast<int>(DownloadState::Paused) &&
		paused.at("done") == 6144 && paused.at("total") == 8192,
		"pause still flushes the latest state and bytes to the manifest");
	const auto before_resume = manifest_sync_count.load();
	check(until([&] { return manager.resume(id); }) && manifest_sync_count.load() > before_resume &&
		manifest(job / "manifest.json").at("state") == static_cast<int>(DownloadState::Queued),
		"resume durably queues the same selected source before starting it");
	change(running, [](Fake& f) { f.release = true; });
	manager.set_enabled(true);
	check(until([&] { return state(manager, id, DownloadState::Complete); }),
		"a transfer with progress-only snapshots can still complete normally");
	const auto complete = manifest(job / "manifest.json");
	check(complete.at("state") == static_cast<int>(DownloadState::Complete) && complete.at("done") == 8192 &&
		complete.at("total") == 8192 && complete.at("media_name") == "media.mp4",
		"completion durably records the final file and exact byte count");
	manager.shutdown();
}

void deferred_resume(const fs::path& data) {
	fs::create_directories(data);
	const auto running = plan("deferred-resume");
	change(running, [](Fake& f) { f.release_cancel = false; });
	DownloadManager manager; std::string error;
	check(manager.init(data.string()), "deferred resume fixture initializes");
	const auto id = manager.enqueue(request("deferred-resume", true), error);
	const auto job = data / "downloads" / id;
	manager.set_enabled(true);
	check(until([&] { return inspect(running, [](const Fake& f) { return f.progress_calls > 0; }); }),
		"deferred resume fixture has one active writer");
	check(!manager.resume(id), "resume cannot restart an active uncancelled writer");
	check(manager.pause(id) && until([&] { return inspect(running, [](const Fake& f) { return f.cancel_seen; }); }),
		"pause publishes its state while the old writer is still cancelling");
	check(manager.resume(id) && state(manager, id, DownloadState::Queued) &&
		manifest(job / "manifest.json").at("state") == static_cast<int>(DownloadState::Queued),
		"resume during cancellation durably queues the same source without waiting for the writer");
	check(inspect(running, [](const Fake& f) { return f.starts == 1 && !f.returned; }),
		"a deferred resume never overlaps the original writer");
	change(running, [](Fake& f) { f.release_cancel = true; });
	check(until([&] { return inspect(running, [](const Fake& f) { return f.starts == 2 && f.progress_calls > 1; }); }),
		"the queued source restarts after its previous writer has returned");
	check(inspect(running, [](const Fake& f) { return f.offset == 4096; }),
		"deferred resume retains the acknowledged partial bytes");

	change(running, [](Fake& f) { f.release_cancel = false; });
	check(manager.pause(id) && until([&] { return inspect(running, [](const Fake& f) { return f.cancel_seen; }); }) &&
		manager.resume(id) && manager.pause(id), "a subsequent pause overrides an accepted deferred resume");
	change(running, [](Fake& f) { f.release_cancel = true; });
	check(until([&] { return inspect(running, [](const Fake& f) { return f.returned; }); }) &&
		state(manager, id, DownloadState::Paused), "overridden resume remains paused when cancellation finishes");
	check(inspect(running, [](const Fake& f) { return f.starts == 2; }),
		"overriding the deferred request does not start a third writer");
	manager.set_enabled(false);
	check(until([&] { return manager.resume(id); }), "a paused source can be queued again after the explicit override");
	change(running, [](Fake& f) { f.release_cancel = false; });
	manager.set_enabled(true);
	check(until([&] { return inspect(running, [](const Fake& f) { return f.starts == 3; }); }) &&
		manager.pause(id) && until([&] { return inspect(running, [](const Fake& f) { return f.cancel_seen; }); }),
		"disable fixture pauses the next writer during cancellation");
	check(manager.resume(id), "the running job accepts a deferred resume before disabling downloads");
	manager.set_enabled(false);
	check(state(manager, id, DownloadState::Paused), "disabling downloads overrides an accepted deferred resume");
	change(running, [](Fake& f) { f.release_cancel = true; });
	check(until([&] { return inspect(running, [](const Fake& f) { return f.returned; }); }) &&
		until([&] { return manager.resume(id); }), "disabled downloads can retain a new explicit resume request");
	change(running, [](Fake& f) { f.release_cancel = false; });
	manager.set_enabled(true);
	check(until([&] { return inspect(running, [](const Fake& f) { return f.starts == 4; }); }) &&
		manager.pause(id) && until([&] { return inspect(running, [](const Fake& f) { return f.cancel_seen; }); }),
		"storage failure fixture pauses the next writer during cancellation");
	fail_sync = EIO;
	check(!manager.resume(id) && state(manager, id, DownloadState::Paused) &&
		!manager.find(id)->error.empty(), "a failed deferred resume checkpoint returns an error and stays paused");
	fail_sync = 0;
	change(running, [](Fake& f) { f.release_cancel = true; });
	check(until([&] { return inspect(running, [](const Fake& f) { return f.returned; }); }) &&
		!manager.storage_available() && state(manager, id, DownloadState::Paused),
		"a failed resume does not retry the unavailable storage or silently start another writer");
	manager.shutdown();
	check(!manifest(job / "manifest.json").value("error", std::string()).empty(),
		"the failed checkpoint diagnostic survives writer cleanup and restart persistence");
	check(manager.init(data.string()) && state(manager, id, DownloadState::Paused) &&
		!manager.find(id)->error.empty(), "restart retains the failed resume reason until the user retries");
	manager.shutdown();
}

void progress_metadata_failure(const fs::path& data) {
	fs::create_directories(data);
	const auto running = plan("metadata-failure");
	change(running, [](Fake& f) { f.hold_progress = true; });
	DownloadManager manager; std::string error;
	check(manager.init(data.string()), "metadata failure fixture initializes");
	const auto id = manager.enqueue(request("metadata-failure"), error);
	const auto job = data / "downloads" / id;
	check(until([&] { return !manifest(job / "manifest.json").value("artwork_pending", true); }),
		"artwork persistence completes before injecting a metadata flush failure");
	manager.set_enabled(true);
	check(until([&] { return inspect(running, [](const Fake& f) { return f.starts == 1; }); }),
		"metadata failure fixture reaches the first progress callback");
	fail_sync = EIO;
	change(running, [](Fake& f) { f.progress_release = true; });
	const bool failed = until([&] { return state(manager, id, DownloadState::Failed); });
	fail_sync = 0;
	const auto entry = manager.find(id);
	check(failed && entry && entry->error == "The download metadata could not be saved." &&
		entry->local_path.empty() && inspect(running, [](const Fake& f) { return f.cancel_seen; }),
		"failure to persist a newly resolved total cancels the writer and reports a storage error");
	change(running, [](Fake& f) { f.release = true; });
	check(manager.resume(id) && until([&] { return state(manager, id, DownloadState::Complete); }),
		"metadata storage failure can be retried after durable writes recover");
	manager.shutdown();
}

void progressive_leases(const fs::path& data) {
	fs::create_directories(data);
	const auto running = plan("progressive-live");
	DownloadManager manager; std::string error;
	check(manager.init(data.string()), "progressive manager storage initializes");
	const auto id = manager.enqueue(request("progressive-live", true), error);
	check(!manager.open_progressive(id, error) && !error.empty(), "queued download cannot open a progressive lease");
	manager.set_enabled(true);
	check(until([&] { const auto e = manager.find(id); return e && e->playable_while_downloading; }),
		"real manager advertises progressive readiness only after transport reports its prefix");
	auto playback = manager.open_progressive(id, error);
	check(playback && error.empty() && playback->descriptor >= 0 && playback->total == 8192,
		"progressive lease opens the one selected part with its known total");
	if (!playback) { manager.shutdown(); return; }
	check(playback->torrent_hash == request("progressive-live", true).stream.info_hash && playback->torrent_file_idx == 4,
		"lease receives the resolved torrent file index, not an unresolved addon guess");
	check(state(manager, id, DownloadState::Downloading) && inspect(running, [](const Fake& f) { return f.starts == 1 && !f.cancel_seen; }),
		"opening progressive playback neither duplicates nor preempts the background torrent");
	check(manager.pause(id) && until([&] { return inspect(running, [](const Fake& f) { return f.returned; }); }),
		"pause waits for the existing transfer to release its writer");
	check(playback->state->snapshot().phase == GrowingFilePhase::Paused && !manager.open_progressive(id, error),
		"pause reaches active playback and prevents starting another partial reader");
	change(running, [](Fake& f) { f.hold_progress = true; f.progress_release = false; });
	const auto old_generation = playback->generation;
	check(manager.resume(id) && until([&] { return inspect(running, [](const Fake& f) { return f.starts == 2; }); }),
		"resuming starts reconciliation of the same saved job");
	check(!manager.find(id)->playable_while_downloading && !manager.open_progressive(id, error),
		"stale on-disk bytes cannot qualify before resumed transfer publishes reconciled progress");
	check(playback->state->snapshot().generation != old_generation,
		"restarted transfer invalidates the old decoder generation before possible truncation");
	change(running, [](Fake& f) { f.progress_release = true; });
	check(until([&] { const auto e = manager.find(id); return e && e->playable_while_downloading; }),
		"resumed download becomes playable again after fresh transport progress");
	auto resumed = manager.open_progressive(id, error);
	check(resumed && resumed->generation != old_generation && manager.snapshot().size() == 1,
		"reopened playback uses the new generation without a second download");
	change(running, [](Fake& f) { f.release = true; });
	check(until([&] { return state(manager, id, DownloadState::Complete); }), "download completes normally while a progressive descriptor is held");
	struct stat st{};
	check(resumed && ::fstat(resumed->descriptor, &st) == 0 && st.st_size == 8192 &&
		resumed->state->snapshot().phase == GrowingFilePhase::Complete && !fs::exists(resumed->path),
		"completion publishes exact final size and held inode survives part rename");
	check(manager.remove(id, error) && until([&] { return !fs::exists(data / "downloads" / id); }),
		"completed download with a held descriptor can be deleted safely");
	check(resumed && resumed->state->snapshot().phase == GrowingFilePhase::Removed,
		"deletion is published to the independent playback lease before job destruction");
	manager.shutdown();
}

void cancelled_unacknowledged_tail(const fs::path& data) {
	fs::create_directories(data);
	const auto running = plan("cancelled-tail");
	change(running, [](Fake& f) { f.cancelled_prefix = 4096; f.cancelled_file_size = 8192; });
	DownloadManager manager; std::string error;
	check(manager.init(data.string()), "cancelled tail fixture initializes");
	const auto id = manager.enqueue(request("cancelled-tail", true), error);
	manager.set_enabled(true);
	check(until([&] { const auto entry = manager.find(id); return entry && entry->playable_while_downloading; }),
		"cancelled tail fixture publishes its acknowledged prefix");
	const auto playback = manager.open_progressive(id, error);
	check(bool(playback), "cancelled tail fixture holds a progressive playback lease");
	check(manager.pause(id), "cancelled tail fixture requests cancellation");
	std::atomic<bool> stop{false};
	check(manager.wait_for_torrent_idle(stop), "cancelled writer releases ownership before the final prefix is inspected");
	const auto entry = manager.find(id);
	check(fs::file_size(data / "downloads" / id / "media.part") == 8192,
		"cancelled writer fixture retains the larger unacknowledged on-disk tail");
	check(entry && entry->state == DownloadState::Paused && entry->done == 4096 &&
		entry->total == 8192 && entry->progress == 0.5f && entry->local_path.empty(),
		"cancelled torrent inventory uses acknowledged bytes and never promotes the full-length tail");
	check(playback && playback->state->snapshot().available == 4096 &&
		playback->state->snapshot().phase == GrowingFilePhase::Paused,
		"progressive playback cannot read the unacknowledged cancelled tail");
	check(manifest(data / "downloads" / id / "manifest.json")["done"] == 4096,
		"cancelled torrent manifest persists the acknowledged prefix");
	manager.shutdown();
}

void snapshot_during_slow_storage(const fs::path& data) {
	fs::create_directories(data);
	const auto cover = data / "cached.rgba"; artwork(cover);
	const auto running = plan("slow-manifest");
	DownloadManager manager; std::string error;
	check(manager.init(data.string()), "slow storage fixture initializes");
	const auto id = manager.enqueue(request("slow-manifest", true), error);
	const auto job_path = data / "downloads" / id;
	check(until([&] { return !manifest(job_path / "manifest.json").value("artwork_pending", true); }),
		"initial artwork state is persisted before the slow-storage fixture starts");
	manager.set_enabled(true);
	check(until([&] { auto e = manager.find(id); return e && e->playable_while_downloading &&
		inspect(running, [](const Fake& f) { return f.progress_calls > 0; }); }),
		"slow storage fixture reaches an active prefix beyond five percent");
	change(running, [](Fake& f) { f.periodic_progress = true; f.progress_done = 6144; });
	const auto waiting = plan("queued-during-slow-commit");
	auto queued = request("queued-during-slow-commit"); queued.poster_path = cover.string();
	std::promise<std::string> committed;
	auto completion = committed.get_future();
	manifest_sync_entered = false; release_manifest_sync = false; hold_manifest_sync = true;
	ui_polling = true;
	const auto submitted_at = std::chrono::steady_clock::now();
	const bool submitted = manager.enqueue_async(queued, [&](std::string new_id, std::string error) {
		committed.set_value(error.empty() ? std::move(new_id) : std::string{});
	});
	ui_polling = false;
	check(submitted && std::chrono::steady_clock::now() - submitted_at < 100ms,
		"selected enqueue submission returns within 100 ms without waiting for its durable commit");
	const bool entered = until([] { return manifest_sync_entered.load(); });
	check(entered, "dedicated enqueue worker reaches the held manifest fsync");
	check(completion.wait_for(0ms) == std::future_status::timeout && manager.snapshot().size() == 1,
		"uncommitted request is neither acknowledged nor published as a queued download");
	const auto progress_before = inspect(running, [](const Fake& f) { return f.progress_calls; });
	const bool transfer_live = until([&] {
		return inspect(running, [&](const Fake& f) { return f.progress_calls >= progress_before + 3; });
	}, 1200);
	check(transfer_live && manager.find(id)->done == 6144,
		"active transfer publishes three further progress callbacks while another job's fsync is held");
	check(inspect(waiting, [](const Fake& f) { return f.starts == 0; }),
		"waiting job cannot start another network transfer during its commit");
	auto polling = std::async(std::launch::async, [&] {
		ui_polling = true;
		for (int n = 0; n < 100; ++n) {
			const auto list = manager.snapshot(); const auto selected = manager.find(id);
			if (list.size() != 1 || !selected || selected->id != id || selected->done != 6144 ||
				selected->remaining_seconds != 10 || selected->connected_peers != 3 || selected->connected_seeders != 2 ||
				!manager.storage_available() || !manager.storage_readable() || !manager.storage_error().empty() ||
				!manager.update_poster(id, cover.string()) || !manager.update_poster_source(id, "https://art.invalid/refreshed.jpg"))
				return false;
		}
		return true;
	});
	const bool responsive = polling.wait_for(100ms) == std::future_status::ready;
	// Release even on regression so a failed deadline cannot hang this suite.
	release_manifest_sync = true; hold_manifest_sync = false;
	check(responsive && polling.get(), "100 UI inventory polls and poster callbacks finish within 100 ms while real worker fsync is blocked");
	check(ui_storage_calls == 0, "UI inventory and poster callbacks perform no file stat, write or fsync");
	check(completion.wait_for(3s) == std::future_status::ready,
		"enqueue result arrives after the durable write is released");
	const auto waiting_id = completion.get();
	check(!waiting_id.empty() && manager.find(waiting_id) && manager.find(waiting_id)->state == DownloadState::Queued,
		"successful completion identifies the one committed selected request");
	check(manifest(data / "downloads" / waiting_id / "manifest.json").at("stream").at("url") == queued.stream.url,
		"accepted queue entry preserves its exact selected stream on disk");
	check(manager.find(waiting_id)->poster_path == cover.string() &&
		!fs::exists(data / "downloads" / waiting_id / "poster.rgba"),
		"waiting cover remains visible without a competing RGBA copy or artwork fsync");
	const auto stable_saves = manifest_sync_count.load();
	const auto callbacks_before = inspect(running, [](const Fake& f) { return f.progress_calls; });
	check(until([&] { return inspect(running, [&](const Fake& f) { return f.progress_calls >= callbacks_before + 3; }); }, 1200) &&
		manifest_sync_count.load() == stable_saves,
		"waiting entry and late poster updates add no recurring manifest writes during active progress");
	manifest_sync_entered = false; release_manifest_sync = false; hold_manifest_sync = true;
	auto pause_write = std::async(std::launch::async, [&] { return manager.pause(id); });
	check(until([] { return manifest_sync_entered.load(); }), "pause reaches a held metadata fsync under the storage mutex");
	auto capabilities = std::async(std::launch::async, [&] {
		return manager.storage_readable() && manager.storage_available() && manager.storage_error().empty();
	});
	const bool capabilities_ready = capabilities.wait_for(100ms) == std::future_status::ready;
	release_manifest_sync = true; hold_manifest_sync = false;
	check(capabilities_ready && capabilities.get() && pause_write.get(),
		"UI storage capability and error queries remain responsive while the storage mutex is held by fsync");
	manager.set_enabled(false);
	check(manager.pause(id), "download remains pausable after slow storage resumes");
	const auto paused = manager.find(id);
	check(paused && paused->remaining_seconds == -1 && paused->connected_peers == -1 && paused->connected_seeders == -1,
		"paused snapshots do not retain stale live ETA or peer counts");
	check(until([&] { return manager.find(waiting_id)->poster_path == (data / "downloads" / waiting_id / "poster.rgba").string(); }),
		"private artwork persistence resumes when the media writer becomes idle");
	manager.shutdown();
	check(manager.init(data.string()) && state(manager, waiting_id, DownloadState::Paused) &&
		!manager.find(waiting_id)->poster_path.empty(),
		"acknowledged queued request and its artwork survive restart without becoming complete");
	manager.shutdown();
}

void asynchronous_queue_lifecycle(const fs::path& data) {
	fs::create_directories(data);
	DownloadManager manager; std::string error;
	check(manager.init(data.string()), "asynchronous lifecycle fixture initializes");
	const auto first_plan = plan("logout-inflight"), second_plan = plan("logout-pending");
	std::promise<std::string> first_result, second_result;
	auto first = first_result.get_future(), second = second_result.get_future();
	manifest_sync_entered = false; release_manifest_sync = false; hold_manifest_sync = true;
	check(manager.enqueue_async(request("logout-inflight"), [&](std::string id, std::string error) {
		first_result.set_value(error.empty() ? std::move(id) : std::string{});
	}), "one selected request enters the dedicated persistence worker");
	check(until([] { return manifest_sync_entered.load(); }), "logout fixture holds an unpublished new-job commit");
	check(manager.enqueue_async(request("logout-pending"), [&](std::string id, std::string error) {
		second_result.set_value(error.empty() ? std::move(id) : std::string{});
	}), "a second selected request waits without starting another persistence thread");
	manager.set_enabled(false);
	manager.set_enabled(true);
	release_manifest_sync = true; hold_manifest_sync = false;
	check(first.wait_for(3s) == std::future_status::ready && second.wait_for(3s) == std::future_status::ready,
		"both pre-logout submissions finish after their durable commits");
	const auto first_id = first.get(), second_id = second.get();
	check(!first_id.empty() && !second_id.empty() && state(manager, first_id, DownloadState::Paused) &&
		state(manager, second_id, DownloadState::Paused),
		"submissions spanning disable and re-enable remain paused instead of auto-starting under a new account");
	check(manifest(data / "downloads" / first_id / "manifest.json").at("state") == static_cast<int>(DownloadState::Paused) &&
		manifest(data / "downloads" / second_id / "manifest.json").at("state") == static_cast<int>(DownloadState::Paused),
		"both in-flight and not-yet-started submissions persist the suspended state before acceptance");
	check(inspect(first_plan, [](const Fake& f) { return f.starts == 0; }) &&
		inspect(second_plan, [](const Fake& f) { return f.starts == 0; }),
		"account suspension cannot briefly start a hidden queued network transfer");
	manager.set_enabled(false);
	std::promise<std::string> duplicate_result;
	auto duplicate = duplicate_result.get_future();
	check(manager.enqueue_async(request("logout-inflight"), [&](std::string id, std::string error) {
		duplicate_result.set_value(error.empty() ? std::move(id) : std::string{});
	}) && duplicate.wait_for(3s) == std::future_status::ready && duplicate.get() == first_id && manager.snapshot().size() == 2,
		"asynchronous duplicate submission returns the existing stable id without another job");
	std::promise<bool> failed_result;
	auto failed = failed_result.get_future();
	fail_sync = EIO;
	check(manager.enqueue_async(request("async-storage-failure"), [&](std::string id, std::string error) {
		failed_result.set_value(id.empty() && !error.empty());
	}), "storage failure fixture reaches asynchronous admission");
	const bool failure_reported = failed.wait_for(3s) == std::future_status::ready && failed.get();
	fail_sync = 0;
	check(failure_reported && manager.snapshot().size() == 2,
		"failed durable enqueue reports failure and never publishes an accepted job");
	check(std::distance(fs::directory_iterator(data / "downloads"), fs::directory_iterator{}) == 2,
		"failed asynchronous enqueue removes its incomplete private directory");
	auto invalid = request("oversized-art-path"); invalid.poster_path.assign(16385, 'a');
	check(!manager.enqueue_async(invalid, {}), "oversized artwork paths cannot consume pending queue memory");

	std::promise<std::string> retained_result;
	auto retained = retained_result.get_future();
	manifest_sync_entered = false; release_manifest_sync = false; hold_manifest_sync = true;
	check(manager.enqueue_async(request("shutdown-inflight"), [&](std::string id, std::string error) {
		retained_result.set_value(error.empty() ? std::move(id) : std::string{});
	}), "shutdown fixture enters a durable enqueue");
	check(until([] { return manifest_sync_entered.load(); }), "shutdown fixture holds the in-flight commit");
	std::atomic<unsigned> cancelled{0}, unexpected{0};
	unsigned pending = 0;
	for (unsigned i = 0; i < 128; ++i) {
		auto large = request("pending-" + std::to_string(i));
		large.stream.description.assign(16384, 'd');
		large.stream.request_headers.assign(64, "X-Private: " + std::string(1000, 'h'));
		large.stream.raw = json{{"unused", std::string(1 << 20, 'r')}};
		if (!manager.enqueue_async(std::move(large), [&](std::string id, std::string error) {
			if (id.empty() && !error.empty()) ++cancelled; else ++unexpected;
		})) break;
		++pending;
	}
	check(pending > 0 && pending < 128, "aggregate request memory bounds admission before the per-job count limit");
	auto closed = std::async(std::launch::async, [&] { manager.shutdown(); });
	check(closed.wait_for(50ms) == std::future_status::timeout,
		"shutdown joins an in-flight durable commit instead of discarding accepted state");
	release_manifest_sync = true; hold_manifest_sync = false;
	check(closed.wait_for(3s) == std::future_status::ready, "shutdown finishes after the owned commit is released");
	closed.get();
	check(retained.wait_for(0ms) == std::future_status::ready, "in-flight committed request receives its completion before shutdown returns");
	const auto retained_id = retained.get();
	check(!retained_id.empty() && cancelled == pending && unexpected == 0,
		"unstarted submissions receive exactly one cancellation while the completed commit retains its id");
	check(!manager.enqueue_async(request("after-shutdown"), {}), "shutdown rejects later asynchronous submissions");
	check(manager.init(data.string()) && manager.snapshot().size() == 3 && state(manager, retained_id, DownloadState::Paused),
		"every acknowledged job survives shutdown and reload without resurrecting unstarted submissions");
	manager.shutdown();
}

void retry_and_remove(const fs::path& data) {
	fs::create_directories(data);
	DownloadManager manager; std::string error; manager.init(data.string());
	auto failed = plan("failure", true); change(failed, [](Fake& f) { f.failure = true; });
	const auto id = manager.enqueue(request("failure"), error); manager.set_enabled(true);
	check(until([&] { return state(manager, id, DownloadState::Failed); }), "transport failure is visible in download inventory");
	check(manager.find(id)->error.find("private-token") == std::string::npos, "provider URLs never escape through download errors");
	change(failed, [](Fake& f) { f.failure = false; });
	check(manager.resume(id), "failed selected stream can be retried");
	check(until([&] { return state(manager, id, DownloadState::Complete); }), "retry can complete and publish the video");
	auto malformed = plan("truncated", true); change(malformed, [](Fake& f) { f.malformed = true; });
	const auto truncated = manager.enqueue(request("truncated"), error);
	check(until([&] { return state(manager, truncated, DownloadState::Failed); }), "wrong final byte count never becomes a complete offline video");
	check(manager.find(truncated)->local_path.empty(), "truncated completion has no playable local path");
	auto held = plan("remove-running"); change(held, [](Fake& f) { f.release_cancel = false; });
	const auto removing = manager.enqueue(request("remove-running"), error);
	check(until([&] { return inspect(held, [](const Fake& f) { return f.starts == 1; }); }), "delete fixture reaches active file writer");
	const auto directory = data / "downloads" / removing;
	const auto outside = data / "keep.txt"; write(outside, "preserve"); fs::create_symlink(outside, directory / "link");
	check(manager.remove(removing, error) && !manager.find(removing), "delete hides selected item immediately without blocking the UI");
	check(until([&] { return inspect(held, [](const Fake& f) { return f.cancel_seen; }); }), "active delete requests transport cancellation");
	check(fs::exists(directory), "manager does not delete files while the transport still owns them");
	change(held, [](Fake& f) { f.release_cancel = true; });
	check(until([&] { return !fs::exists(directory); }) && bytes(outside) == "preserve", "delete cleans selected job only after writer returns");
	auto live = request("live"); live.live = true;
	check(manager.enqueue(live, error).empty() && !error.empty(), "live content cannot enter the finite offline queue");
	auto local = request("local"); local.stream.url = "file:///etc/passwd";
	check(manager.enqueue(local, error).empty(), "non-network descriptor cannot copy arbitrary local files");
	manager.shutdown();
}

void torrent_arbitration(const fs::path& data) {
	fs::create_directories(data);
	DownloadManager manager; std::string error; manager.init(data.string());
	auto bt = plan("bt"); change(bt, [](Fake& f) { f.release_cancel = false; });
	auto req = request("bt", true); req.type = "series"; req.video_id = "tt1234567:2:4"; req.season = 2; req.episode = 4;
	const auto bt_id = manager.enqueue(req, error); manager.set_enabled(true);
	check(until([&] { return inspect(bt, [](const Fake& f) { return f.starts == 1; }); }), "background torrent transfer starts when engine is free");
	check(inspect(bt, [](const Fake& f) { return f.last.season == 2 && f.last.episode == 4; }), "selected episode context reaches torrent file selection");
	manager.set_torrent_playback_active(true);
	auto resolver_lease = manager.acquire_torrent_resolution();
	check(until([&] { return inspect(bt, [](const Fake& f) { return f.cancel_seen; }); }), "foreground torrent requests background torrent cancellation");
	std::atomic<bool> cancel{false};
	auto ready = std::async(std::launch::async, [&] { return manager.wait_for_torrent_idle(cancel); });
	check(ready.wait_for(30ms) == std::future_status::timeout, "foreground cannot acquire engine before background transfer really returns");
	auto direct = plan("http-during-torrent"); const auto direct_id = manager.enqueue(request("http-during-torrent"), error);
	change(bt, [](Fake& f) { f.release_cancel = true; });
	check(ready.wait_for(2s) == std::future_status::ready && ready.get(), "foreground engine becomes available after background transport closes");
	check(until([&] { return state(manager, bt_id, DownloadState::Waiting); }), "preempted torrent remains waiting rather than failed");
	check(until([&] { return inspect(direct, [](const Fake& f) { return f.starts == 1; }); }), "HTTP download can proceed while foreground owns torrent engine");
	manager.set_torrent_playback_active(false);
	check(state(manager, bt_id, DownloadState::Waiting), "UI cancellation cannot release engine while an old foreground resolver still holds its lease");
	check(inspect(direct, [](const Fake& f) { return !f.cancel_seen; }), "torrent ownership changes do not cancel independent HTTP download");
	change(direct, [](Fake& f) { f.release = true; });
	check(until([&] { return state(manager, direct_id, DownloadState::Complete); }), "HTTP job completes normally alongside foreground torrent lifecycle");
	std::this_thread::sleep_for(150ms);
	check(inspect(bt, [](const Fake& f) { return f.starts == 1; }), "background torrent cannot restart in the gap between foreground wait and Engine.start");
	resolver_lease.reset();
	check(until([&] { return inspect(bt, [](const Fake& f) { return f.starts == 2; }); }), "waiting torrent resumes automatically after engine is released");
	change(bt, [](Fake& f) { f.release = true; });
	check(until([&] { return state(manager, bt_id, DownloadState::Complete); }), "resumed selected torrent becomes one offline episode");
	manager.shutdown();
	std::shared_ptr<void> discarded_task_lease;
	{
		DownloadManager temporary;
		discarded_task_lease = temporary.acquire_torrent_resolution();
	}
	discarded_task_lease.reset();
	check(true, "discarding a queued resolver after manager destruction releases its independent token safely");
}
void selected_directories(const fs::path& data) {
 const auto internal = data / "internal", registry = data / "appdata", first = data / "external one", second = data / "external two";
 for (const auto& path : {internal, registry, first, second}) fs::create_directories(path);
 std::string error, legacy_id;
 {
  DownloadManager old; check(old.init(internal.string()), "legacy fixture initializes");
  plan("old-location", true); legacy_id = old.enqueue(request("old-location"), error); old.set_enabled(true);
  check(until([&] { return state(old, legacy_id, DownloadState::Complete); }), "legacy fixture completes before relocation");
  old.shutdown();
 }
 DownloadManager manager;
 check(!manager.init(internal.string(), registry.string(), "", &error) && manager.download_directory().empty(),
  "fresh setup has no automatically selected download directory");
 check(manager.find(legacy_id).has_value(), "legacy downloads remain visible before explicit selection");
 check(manager.enqueue(request("unselected"), error).empty(), "enqueue requires an explicit destination");
 write(first / ".storage-check", "unrelated user file");
 fs::create_directory(first / "private-child"); ::chmod((first / "private-child").c_str(), 0700);
 ::chmod(first.c_str(), 0700); ignore_successful_fchmod = true;
 check(!manager.set_download_directory(first.string(), error) && error.find("0777 permissions") != std::string::npos &&
  error.find("errno " + std::to_string(EPERM)) != std::string::npos,
  "fchmod success without actual0777 is rejected with an actionable permissions error");
 ignore_successful_fchmod = false;
 check(manager.download_directory().empty() && !manager.relocation_status().pending &&
  fs::exists(internal / "downloads" / legacy_id / "media.mp4") && !fs::exists(first / legacy_id),
  "failed permission read-back cannot commit selection or move any original files");
 check(manager.set_download_directory(first.string(), error) && manager.download_directory() == first.string(), "first selection migrates legacy files to the exact target");
 check(manager.find(legacy_id)->local_path == (first / legacy_id / "media.mp4").string() &&
  fs::exists(first / legacy_id / "media.mp4") && !fs::exists(internal / "downloads" / legacy_id), "completed legacy download is physically moved without retaining a copy");
 check(bytes(first / ".storage-check") == "unrelated user file", "selection preserves unrelated user files");
 struct stat selected_mode{}, child_mode{};
 check(::stat(first.c_str(), &selected_mode) == 0 && (selected_mode.st_mode & 0777) == 0777 &&
  ::stat((first / "private-child").c_str(), &child_mode) == 0 && (child_mode.st_mode & 0777) == 0700,
  "explicit selection grants0777 only to the selected directory");
 auto active = plan("selected-active"); change(active, [](Fake& fake) { fake.release_cancel = false; });
 auto next = plan("selected-next", true); plan("selected-paused", true);
 const auto active_id = manager.enqueue(request("selected-active"), error); manager.set_enabled(true);
 check(until([&] { return inspect(active, [](const Fake& fake) { return fake.progress_calls >= 1; }); }), "first selected directory starts its transfer");
 const auto next_id = manager.enqueue(request("selected-next"), error);
 const auto paused_id = manager.enqueue(request("selected-paused"), error); check(manager.pause(paused_id), "queued job can be explicitly paused before migration");
 hold_registry_sync = true; registry_sync_entered = false; release_registry_sync = false;
 std::string move_error;
 auto selection = std::async(std::launch::async, [&] { return manager.set_download_directory(second.string(), move_error); });
 check(until([&] { return inspect(active, [](const Fake& fake) { return fake.cancel_seen; }); }), "relocation cooperatively cancels the active transfer");
 check(fs::exists(first / active_id / "media.part") && !fs::exists(second / active_id) && selection.wait_for(20ms) == std::future_status::timeout,
  "no source path changes until the writer acknowledges cancellation and returns");
 check(manager.enqueue(request("during-move"), error).empty() && !manager.resume(paused_id) && !manager.pause(active_id) && !manager.remove(next_id, error),
  "enqueue/resume/pause/removal mutations are blocked while relocating");
 check(!manager.enqueue_async(request("async-during-move"), {}), "new asynchronous submissions are rejected while moving");
 ui_polling = true; const auto snapshot = manager.snapshot(); const auto status = manager.relocation_status(); ui_polling = false;
 check(snapshot.size() == 4 && status.active && status.destination == second.string() && ui_storage_calls == 0,
  "published inventory and relocation progress need no UI filesystem operations");
 change(active, [](Fake& fake) { fake.release_cancel = true; });
 check(until([&] { return registry_sync_entered.load(); }), "registry becomes durable only after active writer quiescence");
 ui_polling = true; const auto during = manager.download_directory(); const auto during_snapshot = manager.snapshot(); ui_polling = false;
 check(during == first.string() && during_snapshot.size() == 4 && ui_storage_calls == 0, "slow registry fsync does not block published UI reads");
 release_registry_sync = true;
 check(selection.get() && move_error.empty() && manager.download_directory() == second.string(), "destination publishes after every download has moved");
 check(!manager.relocation_status().active && !manager.relocation_status().pending && manager.relocation_status().files_done == 4,
  "successful relocation publishes completed progress and clears pending state");
 check(!fs::exists(first / active_id) && !fs::exists(first / next_id) && !fs::exists(first / paused_id) && !fs::exists(first / legacy_id),
  "active/queued/paused/completed original directories are all retired");
 check(until([&] { return inspect(active, [&](const Fake& fake) { return fake.starts == 2 && fake.last.work_dir == (second / active_id).string() && fake.offset == 4096; }); }),
  "active transfer resumes from its preserved partial prefix in the new folder");
 check(state(manager, paused_id, DownloadState::Paused), "explicitly paused jobs remain paused after moving");
 change(active, [](Fake& fake) { fake.release = true; });
 check(until([&] { return state(manager, active_id, DownloadState::Complete) && state(manager, next_id, DownloadState::Complete); }),
  "active and previously queued work continue in the destination after migration");
 check(manager.resume(paused_id) && until([&] { return state(manager, paused_id, DownloadState::Complete); }), "moved paused job can later resume normally");
 const auto registry_path = registry / "download-directories.json";
 check(manifest(registry_path)["roots"] == json::array({second.string()}) && manifest(registry_path.string() + ".bak")["roots"] == json::array({second.string()}),
  "main and backup registry retain only the completed destination");
 check(bytes(registry_path).find("PRIVATE-DOWNLOAD-TOKEN") == std::string::npos && bytes(registry / "download-relocation.json").empty(),
  "registry is credential-free and completed journal is removed");
 fail_registry_sync = true;
 check(!manager.set_download_directory(first.string(), error) && !error.empty() && manager.download_directory() == second.string(), "registry ENOSPC cannot silently commit a different destination");
 fail_registry_sync = false;
 check(manager.relocation_status().pending && manager.find(active_id).has_value() && fs::exists(second / active_id / "media.mp4"),
  "failed registry transaction keeps original files visible and a resumable journal");
 check(manager.enqueue(request("pending-move"), error).empty() && !manager.resume(paused_id), "pending relocation blocks unrelated new mutations until retry");
 check(manager.set_download_directory(first.string(), error) && !manager.relocation_status().pending && manager.find(active_id)->local_path.rfind(first.string(), 0) == 0,
  "retry completes the original transaction without orphaning files");
 fs::create_directory_symlink(first, data / "redirected");
 check(!manager.set_download_directory((data / "redirected").string(), error), "symlink destination is refused");
 check(!manager.set_download_directory((first / ".." / "external two").string(), error), "traversal spelling is refused");
 manager.shutdown();
 check(!manager.set_download_directory(second.string(), error), "selector cannot restart a shut-down manager");
 check(manager.init(internal.string(), registry.string(), second.string(), &error) && manager.download_directory() == first.string() && manager.snapshot().size() == 4,
  "restart preserves all stable download IDs and authoritative selected destination");
 manager.shutdown();
 const auto detached = data / "detached"; fs::rename(first, detached);
 check(!manager.init(internal.string(), registry.string(), "", &error) && manager.download_directory() == first.string(), "disconnected drive remains registered without silent fallback");
 check(!manager.set_download_directory(second.string(), error) && manager.download_directory() == first.string(), "inaccessible registered source prevents falsely successful all-download migration");
 fs::rename(detached, first);
 check(manager.set_download_directory(second.string(), error) && manager.snapshot().size() == 4, "reconnected source can migrate all previous downloads");
 manager.shutdown();
 const auto good_registry = bytes(registry_path);
 write(registry_path.string() + ".bak", good_registry); write(registry_path, "{torn main");
 check(manager.init(internal.string(), registry.string(), "", &error) && manager.download_directory() == second.string() && manager.find(active_id).has_value(),
  "valid registry backup restores all moved downloads after a torn main");
 manager.shutdown();
 write(registry_path, "{torn main"); write(registry_path.string() + ".bak", "{torn backup"); write(registry_path.string() + ".tmp", good_registry);
 check(manager.init(internal.string(), registry.string(), "", &error) && manager.download_directory() == second.string(), "valid temporary registry recovers when primary and backup are damaged");
 manager.shutdown();
 for (const char* suffix : {"", ".bak", ".tmp", ".new"}) write(registry_path.string() + suffix, std::string("damaged") + suffix);
 check(!manager.init(internal.string(), registry.string(), "", &error) && error.find("damaged") != std::string::npos, "invalid registry copies report an explicit error");
 check(!manager.set_download_directory(first.string(), error), "damaged registry cannot silently discard previous download roots");
 bool preserved = true;
 for (const char* suffix : {"", ".bak", ".tmp", ".new"}) preserved &= bytes(registry_path.string() + suffix) == std::string("damaged") + suffix;
 check(preserved, "all damaged registry copies remain preserved for recovery");
 manager.shutdown();
}

} // namespace

extern "C" int __real_lstat(const char* path, struct stat* info);
extern "C" int __wrap_lstat(const char* path, struct stat* info) {
	if (ui_polling) ++ui_storage_calls;
	const int code = fail_lstat.load();
	if (code && path == denied_path) { errno = code; return -1; }
	return __real_lstat(path, info);
}
extern "C" int __real_chmod(const char* path, mode_t mode);
extern "C" int __real_mkdir(const char* path, mode_t mode);
extern "C" int __wrap_mkdir(const char* path, mode_t mode) {
	const int code = fail_mkdir.load();
	if (code && path == denied_path) { ++injected_mkdir_failures; errno = code; return -1; }
	return __real_mkdir(path, mode);
}
extern "C" int __wrap_chmod(const char* path, mode_t mode) {
	const int code = fail_chmod.load();
	if (code && path == denied_path) { errno = code; return -1; }
	return __real_chmod(path, mode);
}
extern "C" DIR* __real_opendir(const char* path);
extern "C" DIR* __wrap_opendir(const char* path) {
	const int code = fail_opendir.load();
	if (code && path == denied_path) { errno = code; return nullptr; }
	return __real_opendir(path);
}
extern "C" int __real_fchmod(int fd, mode_t mode);
extern "C" int __wrap_fchmod(int fd, mode_t mode) {
	if (ignore_successful_fchmod.load()) return 0;
	const int code = fail_fchmod.load();
	if (code) { errno = code; return -1; }
	return __real_fchmod(fd, mode);
}
extern "C" ssize_t __real_write(int fd, const void* data, size_t size);
extern "C" ssize_t __wrap_write(int fd, const void* data, size_t size) {
	if (ui_polling) ++ui_storage_calls;
	const int code = fail_write.load();
	if (code) { ++injected_write_failures; errno = code; return -1; }
	return __real_write(fd, data, size);
}
extern "C" int __real_fsync(int fd);
extern "C" int __wrap_fsync(int fd) {
	if (ui_polling) ++ui_storage_calls;
	char path[4096]{};
	const auto descriptor = "/proc/self/fd/" + std::to_string(fd);
	const ssize_t count = ::readlink(descriptor.c_str(), path, sizeof(path) - 1);
	const bool is_manifest = count > 0 && std::string(path, size_t(count)).ends_with("/manifest.json.tmp");
	const bool is_registry = count > 0 && std::string(path, size_t(count)).ends_with("/download-directories.json.tmp");
	if (is_registry && fail_registry_sync.load()) { errno = ENOSPC; return -1; }
	if (is_registry && hold_registry_sync.exchange(false)) {
		registry_sync_entered = true;
		while (!release_registry_sync.load()) std::this_thread::sleep_for(1ms);
	}

	if (is_manifest) ++manifest_sync_count;
	const int code = fail_sync.load();
	if (code) { errno = code; return -1; }
	if (hold_manifest_sync.load()) {
		if (is_manifest && hold_manifest_sync.exchange(false)) {
			manifest_sync_entered = true;
			while (!release_manifest_sync.load()) std::this_thread::sleep_for(1ms);
		}
	}
	return __real_fsync(fd);
}

DownloadTransferResult download_transfer(const DownloadTransferRequest& request,
	const DownloadProgressCallback& progress, const std::atomic<bool>& cancel) {
	std::shared_ptr<Fake> fake;
	{
		std::lock_guard lock(fake_mutex);
		const auto it = fakes.find(request.stream.filename);
		if (it == fakes.end()) throw std::runtime_error("missing test plan");
		fake = it->second; ++fake->starts; fake->cancel_seen = fake->returned = false; fake->last = request;
		fake->offset = fs::exists(request.partial_path) ? static_cast<int64_t>(fs::file_size(request.partial_path)) : 0;
	}
	if (!fs::exists(request.partial_path)) write(request.partial_path, std::string(4096, 'a'));
	{
		std::unique_lock lock(fake_mutex);
		while (fake->hold_progress && !fake->progress_release && !cancel.load()) fake_changed.wait_for(lock, 5ms);
	}
	progress({static_cast<int64_t>(fs::file_size(request.partial_path)), 8192, 128000,
		request.stream.info_hash.empty() ? -1 : 4, 10, 3, 2});
	{
		std::unique_lock lock(fake_mutex);
		++fake->progress_calls;
		while (!fake->release && !cancel.load()) {
			fake_changed.wait_for(lock, fake->periodic_progress ? 250ms : 5ms);
			if (fake->periodic_progress && !fake->release && !cancel.load()) {
				const auto done = fake->progress_done;
				lock.unlock();
				write(request.partial_path, std::string(size_t(done), 'a'));
				progress({done, 8192, 128000, 4, 10, 3, 2});
				lock.lock(); ++fake->progress_calls;
			}
		}
		if (cancel.load()) {
			fake->cancel_seen = true; fake_changed.notify_all();
			while (!fake->release_cancel) fake_changed.wait_for(lock, 5ms);
			if (fake->cancelled_file_size >= 0)
				write(request.partial_path, std::string(size_t(fake->cancelled_file_size), 'a'));
			const auto prefix = fake->cancelled_prefix >= 0 ? fake->cancelled_prefix :
				static_cast<int64_t>(fs::file_size(request.partial_path));
			fake->returned = true;
			return {DownloadTransferStatus::Cancelled, {}, ".mp4", prefix, 8192};
		}
		if (fake->throws) throw std::runtime_error("https://provider.invalid/private-token");
		if (fake->storage_error) {
			fake->returned = true;
			return {DownloadTransferStatus::Error, "Not enough storage space.", ".mp4", 4096, 8192, fake->storage_error};
		}
		if (fake->failure) { fake->returned = true; return {DownloadTransferStatus::Error, "https://provider.invalid/private-token failed", ".mp4", 4096, 8192}; }
	}
	write(request.partial_path, std::string(8192, 'a')); progress({8192, 8192, 128000});
	std::lock_guard lock(fake_mutex); fake->returned = true;
	if (fake->fail_completion_save) fail_write = ENOSPC;
	return {DownloadTransferStatus::Complete, {}, ".mp4", 8192, fake->malformed ? 16384 : 8192};
}

int main(int argc, char** argv) {
	if (argc != 2) return 2;
	const fs::path root = fs::absolute(argv[1]);
	selected_directories(root / "selected-directories");
	storage_permissions_and_retry(root / "storage");
	full_storage_inventory(root / "full-storage");
	completion_reconciliation(root / "completion-recovery");
	transfer_storage_failure(root / "transfer-storage-error");
	basic_queue(root / "queue");
	episode_metadata_and_permissions(root / "episode-metadata");
	crash_and_validation(root / "crash");
	retry_and_remove(root / "remove");
	torrent_arbitration(root / "torrent");
	progress_persistence(root / "progress-persistence");
	deferred_resume(root / "deferred-resume");
	progress_metadata_failure(root / "metadata-failure");
	progressive_leases(root / "progressive");
	cancelled_unacknowledged_tail(root / "cancelled-tail");
	snapshot_during_slow_storage(root / "slow-storage");
	asynchronous_queue_lifecycle(root / "async-lifecycle");
	std::cout << "Download manager: " << checks - failures << '/' << checks << " checks passed\n";
	return failures ? 1 : 0;
}
