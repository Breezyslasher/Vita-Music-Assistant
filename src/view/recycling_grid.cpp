/**
 * Vita Music Assistant - Recycling Grid implementation
 * Infinite scroll: automatically fetches next page when user navigates
 * to the last row. Server-side pagination keeps memory low.
 */

#include "view/recycling_grid.hpp"
#include "view/media_item_cell.hpp"
#include "view/media_detail_view.hpp"
#include "utils/image_loader.hpp"
#include <cmath>

namespace vita_ma {

RecyclingGrid::RecyclingGrid() {
    this->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);

    // Content box to hold all items
    m_contentBox = new brls::Box();
    m_contentBox->setAxis(brls::Axis::COLUMN);
    m_contentBox->setPadding(10);
    this->setContentView(m_contentBox);

    // PS Vita screen: 960x544, so 6 columns of ~120px items
    m_columns = 6;
    m_visibleRows = 3;
}

RecyclingGrid::~RecyclingGrid() {
    // If this grid was torn down mid-scroll it may have left ImageLoader's
    // uploads paused; release them so other screens can upload textures.
    if (m_uploadsDeferred) {
        ImageLoader::setDeferTextureUploads(false);
    }
}

void RecyclingGrid::setDataSource(const std::vector<MusicItem>& items) {
    m_items = items;
    m_loading = false;
    m_lastVisibleStart = -1;
    m_lastVisibleEnd = -1;
    // Rows are rebuilt below, so the cached visibility/scroll state is stale.
    m_cachedFirstVisible = -1;
    m_cachedLastVisible = -1;
    m_scrollLoadCooldown = 0;
    rebuildGrid();
}

void RecyclingGrid::appendItems(const std::vector<MusicItem>& newItems) {
    if (newItems.empty()) {
        m_loading = false;
        return;
    }

    size_t oldSize = m_items.size();
    m_items.insert(m_items.end(), newItems.begin(), newItems.end());

    // Render the new items continuing from where we left off
    brls::Box* currentRow = nullptr;
    int itemsInRow = (int)(oldSize % m_columns);
    if (itemsInRow > 0 && m_contentBox->getChildren().size() > 0) {
        auto& children = m_contentBox->getChildren();
        currentRow = dynamic_cast<brls::Box*>(children.back());
        if (!currentRow) itemsInRow = 0;
    } else {
        itemsInRow = 0;
    }

    for (size_t i = oldSize; i < m_items.size(); i++) {
        addCellForItem(currentRow, itemsInRow, i);
    }

    m_renderedCount = m_items.size();
    m_loading = false;

    // Newly added rows default to VISIBLE. Invalidate the caches so the next
    // draw re-evaluates every row (otherwise, if the scroll position is
    // unchanged, the new off-screen rows would never get culled).
    m_cachedFirstVisible = -1;
    m_cachedLastVisible = -1;
    m_lastVisibleStart = -1;
    m_lastVisibleEnd = -1;
    m_scrollLoadCooldown = 0;
}

void RecyclingGrid::setOnItemSelected(std::function<void(const MusicItem&)> callback) {
    m_onItemSelected = callback;
}

void RecyclingGrid::setOnItemStartAction(std::function<void(const MusicItem&)> callback) {
    m_onItemStartAction = callback;
}

void RecyclingGrid::setOnLoadMore(std::function<void()> callback) {
    m_onLoadMore = callback;
}

void RecyclingGrid::setHasMore(bool hasMore) {
    m_hasMore = hasMore;
    m_loading = false;
}

brls::View* RecyclingGrid::getNextFocus(brls::FocusDirection direction, brls::View* currentView) {
    brls::View* next = brls::ScrollingFrame::getNextFocus(direction, currentView);

    // If navigating DOWN and there's nowhere to go, we're at the bottom.
    // Trigger loading the next page if more items are available.
    if (direction == brls::FocusDirection::DOWN && next == nullptr &&
        m_hasMore && !m_loading && m_onLoadMore) {
        m_loading = true;
        // Use brls::sync to defer the fetch so focus resolution completes first
        brls::sync([this]() {
            if (m_onLoadMore) m_onLoadMore();
        });
    }

    return next;
}

void RecyclingGrid::addCellForItem(brls::Box*& currentRow, int& itemsInRow, size_t index) {
    if (itemsInRow == 0) {
        currentRow = new brls::Box();
        currentRow->setAxis(brls::Axis::ROW);
        currentRow->setJustifyContent(brls::JustifyContent::FLEX_START);
        currentRow->setMarginBottom(10);
        m_contentBox->addView(currentRow);
    }

    auto* cell = new MediaItemCell();
    cell->setItem(m_items[index]);
    cell->setMarginRight(10);

    int idx = (int)index;
    cell->registerClickAction([this, idx](brls::View* view) {
        onItemClicked(idx);
        return true;
    });
    cell->addGestureRecognizer(new brls::TapGestureRecognizer(cell));

    // Register START button action for album items
    if (m_items[index].mediaType == MediaType::ALBUM && m_onItemStartAction) {
        MusicItem capturedItem = m_items[index];
        cell->registerAction("Options", brls::ControllerButton::BUTTON_START,
            [this, capturedItem](brls::View* view) {
                if (m_onItemStartAction) {
                    m_onItemStartAction(capturedItem);
                }
                return true;
            });
    }

    // Register START button action for artists
    if (m_items[index].mediaType == MediaType::ARTIST) {
        MusicItem capturedItem = m_items[index];
        cell->registerAction("Options", brls::ControllerButton::BUTTON_START,
            [capturedItem](brls::View* view) {
                MediaDetailView::showArtistContextMenuStatic(capturedItem);
                return true;
            });
    }

    // Register START button action for playlists
    if (m_items[index].mediaType == MediaType::PLAYLIST && m_onItemStartAction) {
        MusicItem capturedItem = m_items[index];
        cell->registerAction("Options", brls::ControllerButton::BUTTON_START,
            [this, capturedItem](brls::View* view) {
                if (m_onItemStartAction) {
                    m_onItemStartAction(capturedItem);
                }
                return true;
            });
    }

    currentRow->addView(cell);

    itemsInRow++;
    if (itemsInRow >= m_columns) {
        itemsInRow = 0;
    }
}

void RecyclingGrid::draw(NVGcontext* vg, float x, float y, float width, float height,
                          brls::Style style, brls::FrameContext* ctx) {
    // Cull off-screen rows BEFORE drawing so ScrollingFrame::draw() skips their
    // entire subtree (borealis short-circuits non-VISIBLE views in View::frame).
    updateRowVisibility();

    brls::ScrollingFrame::draw(vg, x, y, width, height, style, ctx);

    // After drawing: pause/resume uploads based on scroll speed, then refresh the
    // loaded-texture window (covers touch scrolling, which never moves focus).
    updateScrollGating();
    updateVisibleTextures();
}

void RecyclingGrid::updateRowVisibility() {
    if (!m_contentBox) return;
    auto& rows = m_contentBox->getChildren();
    if (rows.empty()) return;

    float scrollY = this->getContentOffsetY();  // positive = scrolled down
    float viewH = this->getHeight();
    int rowCount = static_cast<int>(rows.size());

    // Keep one row of slack above and two below so D-pad focus can always move
    // onto an adjacent (and therefore still-focusable) VISIBLE cell.
    int firstVisible = std::max(0, static_cast<int>(scrollY / ROW_PITCH) - 1);
    int lastVisible = std::min(rowCount, static_cast<int>((scrollY + viewH) / ROW_PITCH) + 2);

    // Nothing moved across a row boundary — leave visibility as-is.
    if (firstVisible == m_cachedFirstVisible && lastVisible == m_cachedLastVisible) return;

    if (m_cachedFirstVisible < 0) {
        // First evaluation (or after a rebuild): set every row in one pass.
        for (int i = 0; i < rowCount; i++) {
            brls::Visibility v = (i >= firstVisible && i < lastVisible)
                ? brls::Visibility::VISIBLE : brls::Visibility::INVISIBLE;
            rows[i]->setVisibility(v);
        }
    } else {
        // Incremental: hide rows that left the window, show rows that entered it.
        for (int i = m_cachedFirstVisible; i < firstVisible; i++)
            if (i >= 0 && i < rowCount) rows[i]->setVisibility(brls::Visibility::INVISIBLE);
        for (int i = lastVisible; i < m_cachedLastVisible; i++)
            if (i >= 0 && i < rowCount) rows[i]->setVisibility(brls::Visibility::INVISIBLE);
        for (int i = firstVisible; i < lastVisible; i++)
            if (i >= 0 && i < rowCount) rows[i]->setVisibility(brls::Visibility::VISIBLE);
    }

    m_cachedFirstVisible = firstVisible;
    m_cachedLastVisible = lastVisible;
}

void RecyclingGrid::updateScrollGating() {
    float curY = this->getContentOffsetY();
    float frameDelta = std::abs(curY - m_prevScrollY);
    m_prevScrollY = curY;

    bool scrolling = frameDelta > 0.5f;
    if (scrolling) m_scrollSettledFrames = 0;
    else m_scrollSettledFrames++;

    // Defer while moving and for a few frames after stopping, so the flush of a
    // 15-20ms upload lands well clear of the motion.
    bool wantDefer = scrolling || m_scrollSettledFrames < 6;
    if (wantDefer != m_uploadsDeferred) {
        m_uploadsDeferred = wantDefer;
        ImageLoader::setDeferTextureUploads(wantDefer);
    }
}

void RecyclingGrid::updateVisibleTextures() {
    if (!m_contentBox) return;
    auto& rows = m_contentBox->getChildren();
    if (rows.empty()) return;

    // Throttle window churn: each refresh below can queue a row of loads, so
    // recompute at most once every few frames to avoid flooding the loader
    // during fast scrolls (which tanks FPS on the Vita).
    if (m_scrollLoadCooldown > 0) {
        m_scrollLoadCooldown--;
        return;
    }

    float scrollY = this->getContentOffsetY();  // positive = scrolled down
    float viewportH = this->getHeight();
    int rowCount = static_cast<int>(rows.size());

    int firstVisible = std::max(0, static_cast<int>(scrollY / ROW_PITCH) - TEXTURE_BUFFER_ROWS);
    int lastVisible = std::min(rowCount - 1,
                               static_cast<int>((scrollY + viewportH) / ROW_PITCH) + TEXTURE_BUFFER_ROWS);

    // Window unchanged since the last refresh — nothing to load or unload.
    if (firstVisible == m_lastVisibleStart && lastVisible == m_lastVisibleEnd) return;

    m_lastVisibleStart = firstVisible;
    m_lastVisibleEnd = lastVisible;
    m_scrollLoadCooldown = 4;

    // Load covers for rows in the window; unload the rest to bound GPU memory.
    for (int r = 0; r < rowCount; r++) {
        brls::Box* row = dynamic_cast<brls::Box*>(rows[r]);
        if (!row) continue;

        bool shouldLoad = (r >= firstVisible && r <= lastVisible);
        auto& cells = row->getChildren();
        for (auto* child : cells) {
            MediaItemCell* cell = dynamic_cast<MediaItemCell*>(child);
            if (!cell) continue;

            if (shouldLoad) cell->loadThumbnailIfNeeded();
            else cell->unloadThumbnail();
        }
    }
}

void RecyclingGrid::rebuildGrid() {
    m_contentBox->clearViews();
    m_renderedCount = 0;

    if (m_items.empty()) return;

    brls::Box* currentRow = nullptr;
    int itemsInRow = 0;

    for (size_t i = 0; i < m_items.size(); i++) {
        addCellForItem(currentRow, itemsInRow, i);
    }

    m_renderedCount = m_items.size();
}

void RecyclingGrid::onItemClicked(int index) {
    if (index >= 0 && index < (int)m_items.size() && m_onItemSelected) {
        m_onItemSelected(m_items[index]);
    }
}

brls::View* RecyclingGrid::create() {
    return new RecyclingGrid();
}

} // namespace vita_ma
