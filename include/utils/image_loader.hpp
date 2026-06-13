/**
 * Vita Music Assistant - Asynchronous Image Loader
 * Memory-optimized with LRU cache eviction and reduced cache size.
 */

#pragma once

#include <borealis.hpp>
#include <string>
#include <functional>
#include <map>
#include <list>
#include <queue>
#include <mutex>
#include <atomic>
#include <memory>

namespace vita_ma {

class ImageLoader {
public:
    using LoadCallback = std::function<void(brls::Image*)>;

    // Load image asynchronously from URL, using an alive flag to prevent use-after-free.
    // The caller must hold a shared_ptr<std::atomic<bool>> that is set to false when
    // the target brls::Image* is destroyed (e.g. in the cell's destructor).
    static void loadAsync(const std::string& url, LoadCallback callback,
                          brls::Image* target, std::shared_ptr<std::atomic<bool>> alive);

    // Load image synchronously from a local file path into a brls::Image.
    // Returns true on success.
    static bool loadFromFile(const std::string& path, brls::Image* target);

    // Clear image cache
    static void clearCache();

    // Cancel all pending loads (invalidates in-flight callbacks via generation counter)
    static void cancelAll();

    // Pause/resume image loading. While paused, new loadAsync calls are no-ops
    // and in-flight async loads skip the HTTP request. Use when entering playback
    // to stop background thumbnail fetches from competing with media streaming.
    static void setPaused(bool paused);
    static bool isPaused();

    // While true, completed downloads keep their decoded bytes queued instead of
    // uploading them to the GPU. Each setImageFromMem upload costs ~15-20ms on the
    // Vita and stalls the frame, so RecyclingGrid sets this during fast scrolling
    // and clears it once scrolling settles, keeping scroll at 60 FPS. Queued
    // uploads are flushed (a few per frame) as soon as the flag is cleared.
    static void setDeferTextureUploads(bool defer);

    // Get current cache size (for debug display)
    static size_t getCacheSize();

private:
    // LRU cache: list stores URL keys in order of recent use (front = most recent)
    // map stores the data + iterator into the list for O(1) promotion
    struct CacheEntry {
        std::vector<uint8_t> data;
        std::list<std::string>::iterator lruIt;
    };

    static std::map<std::string, CacheEntry> s_cache;
    static std::list<std::string> s_lruOrder;
    static std::mutex s_cacheMutex;
    static std::atomic<uint64_t> s_generation;
    static std::atomic<bool> s_paused;

    // Max cached images - reduced from 30 to 20 to save ~2-4 MB on Vita
    static constexpr size_t MAX_CACHE_SIZE = 20;

    // Batched GPU texture uploads. Background downloads push decoded bytes here
    // instead of calling setImageFromMem directly; a single scheduled callback
    // uploads at most MAX_TEXTURES_PER_FRAME per frame (each upload stalls the
    // Vita GPU ~15-20ms). While s_deferTextureUploads is set the queue is held
    // and re-checked next frame, so scrolling never waits on an upload.
    struct PendingTextureUpdate {
        std::vector<uint8_t> data;
        brls::Image* target = nullptr;
        LoadCallback callback;
        std::shared_ptr<std::atomic<bool>> alive;
        uint64_t gen = 0;  // generation captured at queue time; stale uploads are dropped
    };
    static std::queue<PendingTextureUpdate> s_pendingTextures;
    static std::mutex s_pendingMutex;
    static std::atomic<bool> s_pendingScheduled;
    static std::atomic<bool> s_deferTextureUploads;
    static constexpr int MAX_TEXTURES_PER_FRAME = 1;

    // Queue a decoded image for upload on the main thread (next frame).
    static void queueTextureUpdate(std::vector<uint8_t> data, brls::Image* target,
                                   LoadCallback callback,
                                   std::shared_ptr<std::atomic<bool>> alive, uint64_t gen);
    // Upload a budgeted batch of pending textures (runs on the main thread).
    static void processPendingTextures();
};

} // namespace vita_ma
