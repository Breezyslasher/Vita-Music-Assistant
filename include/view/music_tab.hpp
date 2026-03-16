/**
 * Vita Music Assistant - Music Tab
 * Displays music libraries with playlists
 */

#pragma once

#include <borealis.hpp>
#include <memory>
#include "app/ma_types.hpp"
#include "view/recycling_grid.hpp"

namespace vita_ma {

// Simple section descriptor for music library
struct MusicSection {
    std::string key;
    std::string title;
};

class MusicTab : public brls::Box {
public:
    MusicTab();
    ~MusicTab() override;

    void onFocusGained() override;
    void willDisappear(bool resetState) override;

private:
    void loadSections();
    void loadContent(const std::string& sectionKey);
    void loadAlbumsByType(const std::string& sectionKey);
    void loadPlaylists();
    void onItemSelected(const MusicItem& item);
    void onPlaylistSelected(const MusicItem& playlist);
    brls::Box* createHorizontalRow(const std::string& title);
    brls::Box* createAlbumScrollRow(const std::string& title, const std::vector<MusicItem>& items);

    // Playlist management
    void showCreatePlaylistDialog();
    void showPlaylistOptionsDialog(const MusicItem& playlist);
    void showAlbumContextMenu(const MusicItem& album);
    void playPlaylistWithQueue(const std::string& playlistId, int startIndex = 0);
    void refreshPlaylists();

    // Button styling helpers
    void styleButton(brls::Button* btn, bool active = false);
    void updateSectionButtonStyles();
    brls::Button* m_activeSectionBtn = nullptr;

    brls::ScrollingFrame* m_scrollView = nullptr;
    brls::Box* m_mainContainer = nullptr;
    brls::Label* m_titleLabel = nullptr;

    // Section selector
    brls::HScrollingFrame* m_sectionsScroll = nullptr;
    brls::Box* m_sectionsBox = nullptr;

    // Playlists row
    brls::Box* m_playlistsRow = nullptr;
    brls::Box* m_playlistsContainer = nullptr;

    // Album categories (scrolling rows by type)
    brls::ScrollingFrame* m_albumCategoriesScroll = nullptr;
    brls::Box* m_albumCategoriesBox = nullptr;

    // Main content grid
    RecyclingGrid* m_contentGrid = nullptr;

    std::vector<MusicSection> m_sections;
    std::vector<MusicItem> m_items;
    std::vector<MusicItem> m_playlists;
    std::string m_currentSection;
    std::string m_currentPlaylistId;          // Currently viewing playlist
    bool m_loaded = false;
    bool m_viewingPlaylist = false;           // True if viewing playlist contents

    // Shared pointer to track if this object is still alive
    std::shared_ptr<bool> m_alive;
};

} // namespace vita_ma
