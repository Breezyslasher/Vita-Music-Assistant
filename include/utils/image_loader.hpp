#pragma once

#include <string>
#include <functional>
#include <map>
#include <mutex>
#include <atomic>
#include <borealis.hpp>

namespace vita_ma {

using ImageLoadedCallback = std::function<void(int textureId)>;

class ImageLoader {
public:
    static ImageLoader& instance();

    // Load image from URL, returns cached texture or starts async download
    void loadImage(const std::string& url, ImageLoadedCallback callback);

    // Check if image is cached
    int getCachedTexture(const std::string& url);

    // Clear cache
    void clearCache();

    // Pause/resume loading (for during playback)
    void pause() { m_paused.store(true); }
    void resume() { m_paused.store(false); }
    bool isPaused() const { return m_paused.load(); }

    // Evict least recently used entries
    void evictLRU(int maxEntries = 100);

private:
    ImageLoader() = default;

    struct CacheEntry {
        int textureId = -1;
        uint64_t lastUsed = 0;
    };

    std::map<std::string, CacheEntry> m_cache;
    std::mutex m_cacheMutex;
    std::atomic<bool> m_paused{false};
    std::atomic<uint64_t> m_generation{0};
};

} // namespace vita_ma
