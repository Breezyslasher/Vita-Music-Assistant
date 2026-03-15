#pragma once

#include "app.h"
#include "view/recycling_grid.hpp"
#include <borealis.hpp>

namespace vita_ma {

// Sub-tabs for the library: Artists, Albums, Tracks, Playlists, Radio
enum class LibrarySection {
    ARTISTS,
    ALBUMS,
    TRACKS,
    PLAYLISTS,
    RADIO
};

class LibraryTab : public brls::Box {
public:
    LibraryTab();
    ~LibraryTab() override = default;

    void onShow();

private:
    void loadSection(LibrarySection section);
    void onItemSelected(const MediaItem& item);

    brls::Box* m_tabBar = nullptr;
    RecyclingGrid* m_grid = nullptr;
    LibrarySection m_currentSection = LibrarySection::ARTISTS;
    bool m_loaded = false;
};

} // namespace vita_ma
