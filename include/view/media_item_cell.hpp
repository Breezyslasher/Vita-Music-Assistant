/**
 * Vita Music Assistant - Media Item Cell
 * A cell for displaying media items in a grid
 */

#pragma once

#include <borealis.hpp>
#include <memory>
#include <atomic>
#include "app/ma_types.hpp"

namespace vita_ma {

class MediaItemCell : public brls::Box {
public:
    MediaItemCell();
    ~MediaItemCell() override;

    void setItem(const MusicItem& item);
    const MusicItem& getItem() const { return m_item; }

    void onFocusGained() override;
    void onFocusLost() override;
    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override;

    static brls::View* create();

    // Unload thumbnail texture to free GPU memory (off-screen cells)
    void unloadThumbnail();
    // Reload thumbnail from cache/network (when cell scrolls back into view)
    void reloadThumbnail();
    // Load the thumbnail once if it hasn't been requested yet. Cheap to call
    // every frame (guarded by m_thumbLoaded). Used by RecyclingGrid to preload
    // cells near the viewport and by draw() so standalone cells load lazily.
    void loadThumbnailIfNeeded();
    bool isThumbnailLoaded() const { return m_thumbLoaded; }

private:
    void loadThumbnail();
    void updateFocusInfo(bool focused);

    bool m_pressed = false;  // Touch press feedback overlay
    bool m_thumbLoaded = false;  // Whether GPU texture is currently loaded

    MusicItem m_item;
    std::string m_originalTitle;  // Store original truncated title

    // Alive flag - set to false in destructor to prevent use-after-free
    // in async image loader callbacks
    std::shared_ptr<std::atomic<bool>> m_alive;

    brls::Image* m_thumbnailImage = nullptr;
    brls::Label* m_titleLabel = nullptr;
    brls::Label* m_subtitleLabel = nullptr;
    brls::Label* m_descriptionLabel = nullptr;  // Shows on focus
    brls::Box* m_buttonHintBox = nullptr;       // Shows button image + text hints on focus
    brls::Image* m_buttonHintIcon = nullptr;
    brls::Label* m_buttonHintLabel = nullptr;
};

} // namespace vita_ma
