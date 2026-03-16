/**
 * Vita Music Assistant - Library Tab
 * Browse music library content (Artists, Albums, Tracks, Playlists)
 */

#pragma once

#include <borealis.hpp>
#include <memory>
#include "app/ma_types.hpp"
#include "view/recycling_grid.hpp"

namespace vita_ma {

// Forward declaration - defined in library_tab.cpp
enum class MusicCategory;

// View mode for library tab browsing
enum class LibraryTabViewMode {
    ALL_ITEMS,
    FILTERED
};

class LibraryTab : public brls::Box {
public:
    LibraryTab();
    ~LibraryTab();

    void onFocusGained() override;
    void willDisappear(bool resetState) override;

private:
    void onCategorySelected(MusicCategory category);
    void loadCategoryContent(MusicCategory category);
    void onItemSelected(const MusicItem& item);
    void showAlbumContextMenu(const MusicItem& album);
    void showAllItems();
    void updateViewModeButtons();

    // Button styling
    void styleButton(brls::Button* btn, bool active);
    void updateSectionButtonStyles();
    brls::Button* m_activeSectionBtn = nullptr;

    brls::Label* m_titleLabel = nullptr;
    brls::HScrollingFrame* m_sectionsScroll = nullptr;
    brls::Box* m_sectionsBox = nullptr;

    // View mode buttons
    brls::Box* m_viewModeBox = nullptr;
    brls::Button* m_allBtn = nullptr;
    brls::Button* m_backBtn = nullptr;

    RecyclingGrid* m_contentGrid = nullptr;

    std::vector<MusicItem> m_items;
    int m_currentCategory = 1; // default to Albums (MusicCategory::ALBUMS)
    std::string m_currentCategoryName = "Albums";
    LibraryTabViewMode m_viewMode = LibraryTabViewMode::ALL_ITEMS;
    bool m_loaded = false;

    // Alive flag for crash prevention on quick tab switching
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

} // namespace vita_ma
