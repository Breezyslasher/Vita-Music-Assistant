#include "view/library_tab.hpp"
#include "app/ma_client.hpp"
#include "view/media_item_cell.hpp"
#include <borealis.hpp>

namespace vita_ma {

// Data source for library items
class LibraryDataSource : public RecyclingGridDataSource {
public:
    std::vector<MediaItem> items;
    std::function<void(const MediaItem&)> onSelect;
    LibrarySection section;
    int totalCount = 0;
    bool hasMore = false;

    int getItemCount() override { return static_cast<int>(items.size()); }

    brls::View* createCell(int index) override {
        auto* cell = new MediaItemCell();
        if (index >= 0 && index < static_cast<int>(items.size())) {
            cell->setData(items[index]);
        }
        return cell;
    }

    void onItemSelected(int index) override {
        if (index >= 0 && index < static_cast<int>(items.size()) && onSelect) {
            onSelect(items[index]);
        }
    }

    bool canLoadMore() override { return hasMore; }

    void loadMore() override {
        int offset = static_cast<int>(items.size());
        auto* self = this;

        auto cb = [self](bool success, const Json& result) {
            if (!success || result.type() != Json::ARRAY) return;
            for (size_t i = 0; i < result.size(); i++) {
                MediaItem item;
                auto& r = result[i];
                item.item_id = r["item_id"].str();
                item.name = r["name"].str();
                if (r.has("sort_name")) item.name = r["sort_name"].str().empty() ? r["name"].str() : r["name"].str();
                if (r.has("artists") && r["artists"].size() > 0) {
                    item.subtitle = r["artists"][static_cast<size_t>(0)]["name"].str();
                }
                if (r.has("image")) {
                    item.image_url = r["image"]["url"].str();
                }
                item.uri = r.has("uri") ? r["uri"].str() : "";
                self->items.push_back(item);
            }
            self->hasMore = result.size() >= 50;
        };

        switch (section) {
            case LibrarySection::ARTISTS:
                MAClient::instance().getLibraryArtists(cb, "", 50, offset);
                break;
            case LibrarySection::ALBUMS:
                MAClient::instance().getLibraryAlbums(cb, "", 50, offset);
                break;
            case LibrarySection::TRACKS:
                MAClient::instance().getLibraryTracks(cb, "", 50, offset);
                break;
            case LibrarySection::PLAYLISTS:
                MAClient::instance().getLibraryPlaylists(cb, "", 50, offset);
                break;
            case LibrarySection::RADIO:
                MAClient::instance().getLibraryRadios(cb, "", 50, offset);
                break;
        }
    }
};

LibraryTab::LibraryTab() {
    this->setAxis(brls::Axis::COLUMN);
    this->setGrow(1.0f);

    // Section tab bar
    m_tabBar = new brls::Box();
    m_tabBar->setAxis(brls::Axis::ROW);
    m_tabBar->setJustifyContent(brls::JustifyContent::FLEX_START);
    m_tabBar->setGap(4);
    m_tabBar->setPadding(8);
    m_tabBar->setHeight(48);

    const char* tabs[] = {"Artists", "Albums", "Tracks", "Playlists", "Radio"};
    LibrarySection sections[] = {
        LibrarySection::ARTISTS, LibrarySection::ALBUMS,
        LibrarySection::TRACKS, LibrarySection::PLAYLISTS,
        LibrarySection::RADIO
    };

    for (int i = 0; i < 5; i++) {
        auto* btn = new brls::Label();
        btn->setText(tabs[i]);
        btn->setFontSize(14);
        btn->setFocusable(true);
        btn->setPadding(8, 16, 8, 16);
        btn->setCornerRadius(16);

        LibrarySection sec = sections[i];
        btn->registerClickAction([this, sec](...) {
            m_currentSection = sec;
            loadSection(sec);
            return true;
        });

        m_tabBar->addView(btn);
    }
    this->addView(m_tabBar);

    // Grid
    m_grid = new RecyclingGrid();
    m_grid->setGrow(1.0f);
    this->addView(m_grid);
}

void LibraryTab::onShow() {
    if (!m_loaded) {
        m_loaded = true;
        loadSection(m_currentSection);
    }
}

void LibraryTab::loadSection(LibrarySection section) {
    auto* ds = new LibraryDataSource();
    ds->section = section;
    ds->onSelect = [this](const MediaItem& item) { onItemSelected(item); };

    auto cb = [ds](bool success, const Json& result) {
        if (!success || result.type() != Json::ARRAY) return;

        for (size_t i = 0; i < result.size(); i++) {
            MediaItem item;
            auto& r = result[i];
            item.item_id = r["item_id"].str();
            item.name = r["name"].str();
            if (r.has("artists") && r["artists"].size() > 0) {
                item.subtitle = r["artists"][static_cast<size_t>(0)]["name"].str();
            } else if (r.has("owner")) {
                item.subtitle = r["owner"].str();
            }
            if (r.has("image")) {
                item.image_url = r["image"]["url"].str();
            }
            item.uri = r.has("uri") ? r["uri"].str() : "";
            ds->items.push_back(item);
        }
        ds->hasMore = result.size() >= 50;
    };

    switch (section) {
        case LibrarySection::ARTISTS:
            MAClient::instance().getLibraryArtists(cb);
            break;
        case LibrarySection::ALBUMS:
            MAClient::instance().getLibraryAlbums(cb);
            break;
        case LibrarySection::TRACKS:
            MAClient::instance().getLibraryTracks(cb);
            break;
        case LibrarySection::PLAYLISTS:
            MAClient::instance().getLibraryPlaylists(cb);
            break;
        case LibrarySection::RADIO:
            MAClient::instance().getLibraryRadios(cb);
            break;
    }

    m_grid->setDataSource(ds);
}

void LibraryTab::onItemSelected(const MediaItem& item) {
    if (!item.uri.empty()) {
        auto& app = App::instance();
        MAClient::instance().playMedia(app.getQueueId(), item.uri, "play");
    }
}

} // namespace vita_ma
