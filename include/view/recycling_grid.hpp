/**
 * Vita Music Assistant - Recycling Grid
 * Memory-efficient grid view with infinite scroll pagination.
 * Automatically fetches the next page when scrolling near the bottom.
 */

#pragma once

#include <borealis.hpp>
#include "app/ma_types.hpp"
#include <functional>

namespace vita_ma {

class RecyclingGrid : public brls::ScrollingFrame {
public:
    RecyclingGrid();

    void setDataSource(const std::vector<MusicItem>& items);
    // Append additional items (called when next page arrives from server)
    void appendItems(const std::vector<MusicItem>& newItems);
    void setOnItemSelected(std::function<void(const MusicItem&)> callback);
    void setOnItemStartAction(std::function<void(const MusicItem&)> callback);

    // Called automatically when user scrolls to the bottom.
    // Owner should fetch the next page and call appendItems().
    void setOnLoadMore(std::function<void()> callback);

    // Tell the grid whether more items are available on the server
    void setHasMore(bool hasMore);

    // Override to detect when user tries to scroll past the bottom
    brls::View* getNextFocus(brls::FocusDirection direction, brls::View* currentView) override;

    // Override draw to manage texture loading for visible rows
    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override;

    static brls::View* create();

private:
    void rebuildGrid();
    void onItemClicked(int index);
    void addCellForItem(brls::Box*& currentRow, int& itemsInRow, size_t index);
    void updateVisibleTextures();

    std::vector<MusicItem> m_items;
    std::function<void(const MusicItem&)> m_onItemSelected;
    std::function<void(const MusicItem&)> m_onItemStartAction;
    std::function<void()> m_onLoadMore;

    brls::Box* m_contentBox = nullptr;
    int m_columns = 6;
    int m_visibleRows = 3;
    size_t m_renderedCount = 0;

    bool m_hasMore = false;
    bool m_loading = false;  // Prevents duplicate fetch requests

    // Texture management: only keep textures loaded for rows near the viewport
    int m_lastVisibleStart = -1;
    int m_lastVisibleEnd = -1;
    static constexpr int TEXTURE_BUFFER_ROWS = 3;  // Extra rows above/below viewport to keep loaded
};

} // namespace vita_ma
