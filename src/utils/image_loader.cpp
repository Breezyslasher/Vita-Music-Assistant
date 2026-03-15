#include "utils/image_loader.hpp"
#include "utils/http_client.hpp"
#include "utils/async.hpp"
#include <borealis.hpp>

namespace vita_ma {

ImageLoader& ImageLoader::instance() {
    static ImageLoader instance;
    return instance;
}

int ImageLoader::getCachedTexture(const std::string& url) {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    auto it = m_cache.find(url);
    if (it != m_cache.end()) {
        it->second.lastUsed = m_generation.fetch_add(1);
        return it->second.textureId;
    }
    return -1;
}

void ImageLoader::loadImage(const std::string& url, ImageLoadedCallback callback) {
    if (url.empty()) return;

    // Check cache first
    int cached = getCachedTexture(url);
    if (cached >= 0) {
        if (callback) callback(cached);
        return;
    }

    // Don't load while paused
    if (m_paused.load()) return;

    uint64_t gen = m_generation.load();

    asyncRun([this, url, callback, gen]() {
        // Check if generation changed (loading was cancelled)
        if (m_paused.load()) return;

        auto data = HttpClient::instance().downloadBinary(url);
        if (data.empty() || data.size() > 4 * 1024 * 1024) return;

        brls::sync([this, url, data, callback]() {
            // Create texture on main thread
            int texId = brls::Application::loadImageFromMemory(
                data.data(), static_cast<int>(data.size()));

            if (texId >= 0) {
                std::lock_guard<std::mutex> lock(m_cacheMutex);
                CacheEntry entry;
                entry.textureId = texId;
                entry.lastUsed = m_generation.fetch_add(1);
                m_cache[url] = entry;

                evictLRU();
            }

            if (callback) callback(texId);
        });
    });
}

void ImageLoader::clearCache() {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    // Note: textures should be freed by Borealis/NanoVG
    m_cache.clear();
}

void ImageLoader::evictLRU(int maxEntries) {
    // Already under lock from caller or called internally
    if (static_cast<int>(m_cache.size()) <= maxEntries) return;

    // Find oldest entries
    while (static_cast<int>(m_cache.size()) > maxEntries) {
        auto oldest = m_cache.begin();
        uint64_t minGen = UINT64_MAX;
        for (auto it = m_cache.begin(); it != m_cache.end(); ++it) {
            if (it->second.lastUsed < minGen) {
                minGen = it->second.lastUsed;
                oldest = it;
            }
        }
        m_cache.erase(oldest);
    }
}

} // namespace vita_ma
