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
    // Fit the title to one line of the given width (ellipsised), cached until the
    // item changes. Like Vita_Suwayomi, the title is drawn with nvgText instead
    // of a brls::Label child so a screenful of cells costs almost nothing.
    void cacheTitleText(NVGcontext* vg, float fontSize, float maxWidth);
    // The secondary line under the title: track number normally, or year /
    // duration / item-count when focused.
    std::string secondaryLine() const;

    bool m_pressed = false;  // Touch press feedback overlay
    bool m_focused = false;  // Whether this cell currently has focus
    bool m_thumbLoaded = false;  // Whether GPU texture is currently loaded

    MusicItem m_item;

    // Cached one-line, width-fitted title (drawn via nvgText). Invalidated when
    // the item or the fit width changes.
    std::string m_cachedTitle;
    bool m_titleCached = false;
    float m_cachedTitleWidth = 0.0f;

    // Alive flag - set to false in destructor to prevent use-after-free
    // in async image loader callbacks
    std::shared_ptr<std::atomic<bool>> m_alive;

    // Cover area: m_thumbnailImage is kept as an (empty) layout spacer that
    // reserves the cover's box; the actual cover is a raw NanoVG image handle
    // drawn by draw(). Title/subtitle are drawn with nvgText below it.
    brls::Image* m_thumbnailImage = nullptr;
    int m_nvgCover = 0;   // NanoVG image handle (0 = none); owned by this cell
    int m_coverW = 0;     // source pixel width of the loaded cover
    int m_coverH = 0;     // source pixel height of the loaded cover

    // Focus-only child (absolutely positioned, GONE by default) — negligible
    // per-frame cost since only the focused cell shows it.
    brls::Box* m_buttonHintBox = nullptr;       // Shows the START button hint on focus
    brls::Image* m_buttonHintIcon = nullptr;
};

} // namespace vita_ma
