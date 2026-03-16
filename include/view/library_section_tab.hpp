/**
 * Vita Music Assistant - Library Section Tab
 * Shows content for a single music library section (for sidebar mode)
 * Playlists appear as browsable content within the tab
 */

#pragma once

#include <borealis.hpp>
#include <memory>
#include <vector>
#include "app/ma_types.hpp"
#include "view/recycling_grid.hpp"

namespace vita_ma {

// View mode for the library section
enum class LibraryViewMode {
    ALL_ITEMS,      // Show all items in the library
    PLAYLISTS,      // Show playlists
    FILTERED        // Showing items filtered by selection
};

class LibrarySectionTab : public brls::Box {
public:
    LibrarySectionTab(const std::string& sectionKey, const std::string& title, const std::string& sectionType = "");
    ~LibrarySectionTab() override;

    void onFocusGained() override;
    void willDisappear(bool resetState) override;

private:
    void loadContent();
    void loadPlaylists();
    void showAllItems();
    void showPlaylists();
    void onItemSelected(const MusicItem& item);
    void onPlaylistSelected(const MusicItem& playlist);
    void showPlaylistContextMenu(const MusicItem& playlist);
    void showPlaylistOptionsDialog(const MusicItem& playlist);
    void playPlaylistWithQueue(const std::string& playlistId, int startIndex);
    void showPlaylistTrackList(std::vector<MusicItem>&& tracks, const std::string& playlistTitle, const std::string& playlistId);
    void appendTrackListPage();
    void performPlaylistTrackAction(size_t trackIndex);
    void updateViewModeButtons();
    void styleButton(brls::Button* btn, bool active);

    // Check if this tab is still valid (not destroyed)
    bool isValid() const { return m_alive && *m_alive; }

    std::string m_sectionKey;
    std::string m_title;
    std::string m_sectionType;  // "artist", "album", "track", "playlist"

    brls::Label* m_titleLabel = nullptr;

    // View mode selector buttons
    brls::Box* m_viewModeBox = nullptr;
    brls::Button* m_allBtn = nullptr;
    brls::Button* m_playlistsBtn = nullptr;
    brls::Button* m_backBtn = nullptr;  // Back button when in filtered view

    // Main content grid
    RecyclingGrid* m_contentGrid = nullptr;

    // Track list view (for playlist contents)
    brls::ScrollingFrame* m_trackListScroll = nullptr;
    brls::Box* m_trackListBox = nullptr;

    // Pagination for infinite scroll
    void loadNextPage();
    size_t m_pageOffset = 0;
    int m_totalItemCount = 0;
    static constexpr size_t PAGE_SIZE = 60;

    // Data
    std::vector<MusicItem> m_items;
    std::vector<MusicItem> m_playlists;

    // Current playlist track list (stored as member to avoid per-row copies)
    std::vector<MusicItem> m_playlistTracks;
    std::string m_currentPlaylistId;
    size_t m_trackListRendered = 0;  // How many track rows rendered so far
    brls::Button* m_trackListLoadMoreBtn = nullptr;

    static constexpr size_t TRACK_LIST_PAGE_SIZE = 50;

    LibraryViewMode m_viewMode = LibraryViewMode::ALL_ITEMS;
    std::string m_filterTitle;  // Title of current filter
    bool m_loaded = false;
    bool m_playlistsLoaded = false;

    // Shared pointer to track if this object is still alive
    // Used by async callbacks to check validity before updating UI
    std::shared_ptr<bool> m_alive;
};

} // namespace vita_ma
