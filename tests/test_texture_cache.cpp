#include "texture_cache.h"
#include "gfx/gl_batch.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
namespace {
int checks = 0;
std::mutex image_mutex;
std::condition_variable image_wake;
std::map<std::string, int> image_reads;
bool recover_image = false;
bool hold_generation_decode = false;
bool decode_on_ui = false, upload_off_ui = false, delete_off_ui = false;
const std::thread::id ui_thread = std::this_thread::get_id();
uint32_t next_texture = 1;
size_t created = 0, deleted = 0;
std::map<uint32_t, size_t> live_gpu;
std::map<uint32_t, uint8_t> gpu_pixels;
void expect(bool value, const char* description) {
    ++checks;
    if (!value) throw std::runtime_error(description);
}
template<typename Predicate> void until(Predicate done, const char* message) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    do {
        if (done()) return;
        std::this_thread::sleep_for(3ms);
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error(message);
}
int reads(const std::string& fragment) {
    std::lock_guard<std::mutex> lock(image_mutex);
    int count = 0;
    for (const auto& entry : image_reads) if (entry.first.find(fragment) != std::string::npos) count += entry.second;
    return count;
}
void asynchronous_and_retry() {
    hui::gfx::GlBatch batch;
    TextureCache cache(batch, "/art");
    expect(cache.get("https://untrusted.example/image.png") == 0 && reads("untrusted") == 0,
           "GPU artwork loader never performs URL requests");
    expect(cache.dimensions("missing.rgba") == std::pair<int, int>{0, 0} && reads("missing") == 0,
           "dimension lookup does not schedule work for unavailable textures");
    expect(cache.get("poster.rgba") == 0, "first uncached texture request returns without decoding on the UI thread");
    uint32_t texture = 0;
    until([&] { cache.begin_frame(); texture = cache.get("poster.rgba"); return texture != 0; }, "poster never became a texture");
    expect(reads("poster.rgba") == 1, "a pending/cached image is decoded once");
    expect(cache.get("poster.rgba") == texture, "cached texture handle remains stable");
    expect(cache.dimensions("poster.rgba") == std::pair<int, int>{512, 256} &&
           cache.dimensions("/art/poster.rgba") == std::pair<int, int>{512, 256},
           "dimension lookup exposes actual loaded size with the same relative/absolute cache key");
    expect(!decode_on_ui && !upload_off_ui, "CPU loads are worker-only and GPU uploads are UI-only");
    cache.get("recover.rgba");
    until([&] { cache.begin_frame(); return reads("recover.rgba") == 1 && !cache.pending(); }, "failed image did not settle");
    for (int i = 0; i < 20; ++i) { cache.get("recover.rgba"); cache.begin_frame(); }
    expect(reads("recover.rgba") == 1, "failed local file is not hammered on every frame");
    { std::lock_guard<std::mutex> lock(image_mutex); recover_image = true; }
    for (int i = 0; i < 120; ++i) cache.begin_frame();
    until([&] { cache.begin_frame(); return cache.get("recover.rgba") != 0; }, "fixed local image remained permanently blacklisted");
    expect(reads("recover.rgba") == 2, "local load failure retries after cooldown");
    cache.release();
    expect(cache.dimensions("poster.rgba") == std::pair<int, int>{0, 0},
           "release clears dimensions along with the GPU texture");
    expect(live_gpu.empty() && !delete_off_ui, "release deletes every GPU texture on its owner thread");
}
void upload_budget_and_backpressure() {
    hui::gfx::GlBatch batch;
    TextureCache cache(batch, "/art");
    cache.get("large-first-b.rgba");
    cache.get("large-second-b.rgba");
    until([&] { return reads("large-") == 2; }, "large image workers did not decode");
    bool first = false, second = false;
    size_t maximum_uploads = 0;
    until([&] {
        const auto before = created;
        cache.begin_frame();
        maximum_uploads = std::max(maximum_uploads, created - before);
        first = cache.get("large-first-b.rgba") != 0;
        second = cache.get("large-second-b.rgba") != 0;
        return first && second;
    }, "large images did not finish uploading");
    expect(maximum_uploads == 1, "two 8MiB hero images upload on separate frames");
    cache.release();

    TextureCache rows(batch, "/art");
    for (int i = 0; i < 20; ++i) rows.get("row-" + std::to_string(i) + ".rgba");
    until([&] { return reads("row-") >= 8; }, "row worker pool did not reach backpressure");
    std::this_thread::sleep_for(40ms);
    expect(reads("row-") <= 8, "decoded CPU queue is bounded at six ready plus two active images");
    maximum_uploads = 0;
    until([&] {
        const auto before = created;
        rows.begin_frame();
        maximum_uploads = std::max(maximum_uploads, created - before);
        return !rows.pending();
    }, "row uploads did not drain");
    expect(maximum_uploads <= 2 && maximum_uploads > 0, "a completed poster row respects the two-texture frame limit");
    rows.release();
    expect(live_gpu.empty(), "backpressure/release leaves no live GPU handles");
}
void eviction_and_stale_work() {
    hui::gfx::GlBatch batch;
    TextureCache cache(batch, "/art", 1u << 20);
    for (int i = 0; i < 10; ++i) cache.get("eviction-" + std::to_string(i) + ".rgba");
    until([&] { cache.begin_frame(); return !cache.pending(); }, "eviction fixture did not finish");
    for (int i = 0; i < 4; ++i) cache.begin_frame();
    size_t bytes = 0;
    for (const auto& image : live_gpu) bytes += image.second;
    expect(bytes <= (1u << 20) && deleted > 0, "unused textures are evicted to the GPU budget after the short reuse window");
    cache.release();
    expect(live_gpu.empty(), "eviction and explicit release do not double-own GPU resources");
}
void playback_releases_gallery_and_rejects_old_decodes() {
    hui::gfx::GlBatch batch;
    TextureCache cache(batch, "/art");
    struct ReleaseDecode {
        ~ReleaseDecode() {
            { std::lock_guard<std::mutex> lock(image_mutex); hold_generation_decode = false; }
            image_wake.notify_all();
        }
    } release_decode;
    uint32_t poster = 0;
    until([&] { cache.begin_frame(); poster = cache.get("hd-playback-poster.rgba"); return poster != 0; },
          "high-resolution gallery poster did not load");
    expect(cache.dimensions("hd-playback-poster.rgba") == std::pair<int, int>{720, 1080},
           "gallery uses the full 720x1080 poster before playback");
    const auto original_pixels = gpu_pixels.at(poster);
    { std::lock_guard<std::mutex> lock(image_mutex); hold_generation_decode = true; }
    cache.get("generation-same-path.rgba");
    until([&] { return reads("generation-same-path") == 1; }, "old-generation decode did not start");
    for (int i = 0; i < 12; ++i) cache.get("preplay-backlog-" + std::to_string(i) + ".rgba");
    // One worker is held above. The other reaches six Ready images plus its
    // own seventh decode waiting for queue capacity; further work stays queued.
    until([&] { return reads("preplay-backlog-") == 7; }, "pre-play gallery queue did not reach backpressure");
    const auto decoded_before = reads("preplay-backlog-");
    const auto created_before = created;
    const auto start = std::chrono::steady_clock::now();
    cache.set_playback(true);
    expect(std::chrono::steady_clock::now() - start < 100ms,
           "playback transition does not wait for an in-flight disk/decode worker");
    expect(live_gpu.empty() && cache.dimensions("hd-playback-poster.rgba") == std::pair<int, int>{0, 0},
           "playback immediately releases gallery GPU textures and dimensions");
    uint32_t current = 0;
    until([&] { cache.begin_frame(); current = cache.get("generation-same-path.rgba"); return current != 0; },
          "new-generation same-path playback artwork did not load");
    expect(reads("generation-same-path") == 2 && gpu_pixels.at(current) == 93,
           "new-generation request completes while the obsolete same-path decode is still held");
    expect(created == created_before + 1 && reads("preplay-backlog-") == decoded_before,
           "pre-play Ready and queued gallery images do not upload or restart during playback");
    cache.set_playback(true);
    expect(cache.get("generation-same-path.rgba") == current,
           "repeated playback-mode updates do not evict current overlay artwork");
    { std::lock_guard<std::mutex> lock(image_mutex); hold_generation_decode = false; }
    image_wake.notify_all();
    until([&] { cache.begin_frame(); return !cache.pending(); }, "obsolete generation did not finish without blocking");
    expect(cache.get("generation-same-path.rgba") == current && gpu_pixels.at(current) == 93 && created == created_before + 1,
           "late pre-play decode cannot overwrite or leak a newer same-path GPU texture");
    uint32_t thumbnail = 0;
    until([&] { cache.begin_frame(); thumbnail = cache.get("playback-next-thumb.rgba"); return thumbnail != 0; },
          "up-next thumbnail was incorrectly suppressed during playback");
    expect(thumbnail != 0, "visible player thumbnails continue to upload during playback");
    cache.set_playback(false);
    until([&] { cache.begin_frame(); poster = cache.get("hd-playback-poster.rgba"); return poster != 0; },
          "returning to the gallery did not reload its poster");
    expect(cache.dimensions("hd-playback-poster.rgba") == std::pair<int, int>{720, 1080} &&
           gpu_pixels.at(poster) == original_pixels && reads("hd-playback-poster") == 2,
           "gallery reload preserves full poster dimensions and pixels after playback");
    cache.release();
    expect(live_gpu.empty() && !decode_on_ui && !upload_off_ui && !delete_off_ui,
           "playback transitions preserve worker/render ownership and release all GPU textures");
}
void playback_budget_returns_to_normal() {
    hui::gfx::GlBatch batch;
    TextureCache cache(batch, "/art", 32u << 20);
    cache.set_playback(true);
    for (int i = 0; i < 3; ++i) cache.get("large-play-budget-" + std::to_string(i) + ".rgba");
    until([&] { cache.begin_frame(); return !cache.pending(); }, "playback budget images did not load");
    for (int i = 0; i < 4; ++i) cache.begin_frame();
    size_t bytes = 0;
    for (const auto& image : live_gpu) bytes += image.second;
    expect(bytes <= (16u << 20), "playback evicts unused artwork to its 16MiB cache budget");
    cache.set_playback(false);
    bool all_ready = false;
    until([&] {
        cache.begin_frame(); all_ready = true;
        for (int i = 0; i < 3; ++i) all_ready = (cache.get("large-play-budget-" + std::to_string(i) + ".rgba") != 0) && all_ready;
        return all_ready;
    }, "normal gallery budget was not restored");
    for (int i = 0; i < 4; ++i) cache.begin_frame();
    bytes = 0;
    for (const auto& image : live_gpu) bytes += image.second;
    expect(bytes == (24u << 20), "leaving playback restores the caller's original 32MiB budget");
    cache.release();
    expect(live_gpu.empty(), "playback and gallery budget changes leave no GPU resource leaks");
}
} // namespace

bool load_image_rgba(const std::string& path, std::vector<unsigned char>& pixels, int& w, int& h) {
    int read_count = 0;
    {
        std::unique_lock<std::mutex> lock(image_mutex);
        read_count = ++image_reads[path];
        if (std::this_thread::get_id() == ui_thread) decode_on_ui = true;
        if (path.find("recover") != std::string::npos && !recover_image) { w = h = 0; pixels.clear(); return false; }
        if (path.find("generation-same-path") != std::string::npos && read_count == 1)
            image_wake.wait(lock, [] { return !hold_generation_decode; });
    }
    if (path.find("large-") != std::string::npos) { w = 2048; h = 1024; }
    else if (path.find("hd-playback-poster") != std::string::npos) { w = 720; h = 1080; }
    else { w = 512; h = 256; }
    const uint8_t color = path.find("generation-same-path") != std::string::npos ? (read_count == 1 ? 31 : 93) : 200;
    pixels.assign(size_t(w) * h * 4, color);
    return true;
}
namespace hui::gfx {
GlBatch::~GlBatch() = default;
uint32_t GlBatch::create_texture(int w, int h, const uint8_t* pixels) {
    if (std::this_thread::get_id() != ui_thread) upload_off_ui = true;
    const auto handle = next_texture++;
    live_gpu[handle] = size_t(w) * h * 4;
    gpu_pixels[handle] = pixels[0];
    ++created;
    return handle;
}
} // namespace hui::gfx
extern "C" void glDeleteTextures(GLsizei count, const GLuint* textures) {
    if (std::this_thread::get_id() != ui_thread) delete_off_ui = true;
    for (int i = 0; i < count; ++i) { live_gpu.erase(textures[i]); gpu_pixels.erase(textures[i]); ++deleted; }
}

int main() {
    try {
        asynchronous_and_retry(); upload_budget_and_backpressure(); eviction_and_stale_work();
        playback_releases_gallery_and_rejects_old_decodes(); playback_budget_returns_to_normal();
        std::cout << "PASS " << checks << " texture cache checks (actual scheduler; fake disk pixels/GPU handles).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
