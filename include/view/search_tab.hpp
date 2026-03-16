/**
 * Vita Music Assistant - Search Tab
 * Search for media content
 */

#pragma once

#include <borealis.hpp>
#include <memory>
#include "app/ma_types.hpp"
#include "view/recycling_grid.hpp"
#include "view/horizontal_scroll_row.hpp"

namespace vita_ma {

class SearchTab : public brls::Box {
public:
    SearchTab();
    ~SearchTab();

    void onFocusGained() override;
    void willDisappear(bool resetState) override;

private:
    void performSearch(const std::string& query);
    void onItemSelected(const MusicItem& item);
    void populateRow(HorizontalScrollRow* row, const std::vector<MusicItem>& items);

    brls::Label* m_titleLabel = nullptr;
    brls::Label* m_searchLabel = nullptr;
    brls::Label* m_resultsLabel = nullptr;

    // Scrollable content for organized results
    brls::ScrollingFrame* m_scrollView = nullptr;
    brls::Box* m_scrollContent = nullptr;

    // Category labels and rows
    brls::Label* m_albumsLabel = nullptr;
    HorizontalScrollRow* m_albumsRow = nullptr;
    brls::Label* m_tracksLabel = nullptr;
    HorizontalScrollRow* m_tracksRow = nullptr;

    std::string m_searchQuery;
    std::vector<MusicItem> m_results;
    std::vector<MusicItem> m_albums;
    std::vector<MusicItem> m_tracks;

    // Alive flag + generation counter for crash prevention
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
    int m_loadGeneration = 0;
};

} // namespace vita_ma
