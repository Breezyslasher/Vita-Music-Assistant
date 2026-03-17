/**
 * Vita Music Assistant - Library Tab implementation
 * Music-only library browser with categories: Artists, Albums, Tracks, Playlists
 */

#include "view/library_tab.hpp"
#include "view/media_item_cell.hpp"
#include "view/media_detail_view.hpp"
#include "activity/player_activity.hpp"
#include "app/application.hpp"
#include "app/ma_client.hpp"
#include "app/music_queue.hpp"
#include "utils/image_loader.hpp"
#include "utils/async.hpp"

namespace vita_ma {

// Music library categories (replaces Plex library sections)
enum class MusicCategory {
    ARTISTS,
    ALBUMS,
    TRACKS,
    PLAYLISTS
};

static const char* categoryName(MusicCategory cat) {
    switch (cat) {
        case MusicCategory::ARTISTS:   return "Artists";
        case MusicCategory::ALBUMS:    return "Albums";
        case MusicCategory::TRACKS:    return "Tracks";
        case MusicCategory::PLAYLISTS: return "Playlists";
    }
    return "Unknown";
}

// Helper to parse a JSON array of music items into MusicItem vector
static std::vector<MusicItem> parseMusicItems(const Json& result, MediaType expectedType) {
    std::vector<MusicItem> items;

    if (result.type() != Json::ARRAY) {
        brls::Logger::error("LibraryTab: Expected array result from server");
        return items;
    }

    for (size_t i = 0; i < result.size(); i++) {
        const Json& obj = result[i];
        MusicItem item;
        item.itemId     = obj.has("item_id")    ? obj["item_id"].str()    : "";
        item.name       = obj.has("name")        ? obj["name"].str()       : "";
        item.sortName   = obj.has("sort_name")   ? obj["sort_name"].str()  : "";
        item.uri        = obj.has("uri")         ? obj["uri"].str()        : "";
        item.mediaType  = expectedType;

        // Extract image URL: try image object, then metadata.images array
        if (obj.has("image") && obj["image"].type() == Json::OBJECT && obj["image"].has("path")) {
            item.imageUrl = obj["image"]["path"].str();
            if (obj["image"].has("provider")) item.imageProvider = obj["image"]["provider"].str();
        } else if (obj.has("image") && obj["image"].type() == Json::STRING) {
            item.imageUrl = obj["image"].str();
        } else if (obj.has("metadata") && obj["metadata"].type() == Json::OBJECT) {
            const Json& meta = obj["metadata"];
            if (meta.has("images") && meta["images"].type() == Json::ARRAY && meta["images"].size() > 0) {
                const Json& img = meta["images"][static_cast<size_t>(0)];
                if (img.has("path")) item.imageUrl = img["path"].str();
                if (img.has("provider")) item.imageProvider = img["provider"].str();
            }
        }

        // Track-specific fields
        if (obj.has("artist_name"))  item.artistName  = obj["artist_name"].str();
        if (obj.has("album_name"))   item.albumName   = obj["album_name"].str();
        if (obj.has("track_number")) item.trackNumber  = obj["track_number"].intVal();
        if (obj.has("disc_number"))  item.discNumber   = obj["disc_number"].intVal();
        if (obj.has("duration"))     item.duration     = obj["duration"].intVal(); // already in seconds

        // Album-specific fields
        if (obj.has("year"))    item.year    = obj["year"].intVal();
        if (obj.has("version")) item.version = obj["version"].str();

        // Playlist-specific fields
        if (obj.has("item_count"))  item.itemCount   = obj["item_count"].intVal();
        if (obj.has("is_editable")) item.isEditable  = obj["is_editable"].boolVal();

        // Common fields
        if (obj.has("favorite")) item.favorite = obj["favorite"].boolVal();
        if (obj.has("provider")) item.provider = obj["provider"].str();

        items.push_back(item);
    }

    return items;
}

LibraryTab::LibraryTab() {
    this->setAxis(brls::Axis::COLUMN);
    this->setJustifyContent(brls::JustifyContent::FLEX_START);
    this->setAlignItems(brls::AlignItems::STRETCH);
    this->setPadding(20);
    this->setGrow(1.0f);

    // Title
    m_titleLabel = new brls::Label();
    m_titleLabel->setText("Library");
    m_titleLabel->setFontSize(28);
    m_titleLabel->setMarginBottom(20);
    this->addView(m_titleLabel);

    // Category buttons row (replaces Plex library sections scroll)
    m_sectionsScroll = new brls::HScrollingFrame();
    m_sectionsScroll->setHeight(50);
    m_sectionsScroll->setMarginBottom(15);

    m_sectionsBox = new brls::Box();
    m_sectionsBox->setAxis(brls::Axis::ROW);
    m_sectionsBox->setJustifyContent(brls::JustifyContent::FLEX_START);
    m_sectionsBox->setAlignItems(brls::AlignItems::CENTER);

    m_sectionsScroll->setContentView(m_sectionsBox);
    this->addView(m_sectionsScroll);

    // Create category buttons: Artists, Albums, Tracks, Playlists
    auto addCategoryButton = [this](const char* label, MusicCategory cat) {
        auto* btn = new brls::Button();
        btn->setText(label);
        btn->setMarginRight(10);
        styleButton(btn, false);
        btn->registerClickAction([this, cat, btn](brls::View* view) {
            m_activeSectionBtn = btn;
            updateSectionButtonStyles();
            onCategorySelected(cat);
            return true;
        });
        m_sectionsBox->addView(btn);
    };

    addCategoryButton("Artists",   MusicCategory::ARTISTS);
    addCategoryButton("Albums",    MusicCategory::ALBUMS);
    addCategoryButton("Tracks",    MusicCategory::TRACKS);
    addCategoryButton("Playlists", MusicCategory::PLAYLISTS);

    // View mode buttons (All / < Back for filtered views)
    m_viewModeBox = new brls::Box();
    m_viewModeBox->setAxis(brls::Axis::ROW);
    m_viewModeBox->setJustifyContent(brls::JustifyContent::FLEX_START);
    m_viewModeBox->setAlignItems(brls::AlignItems::CENTER);
    m_viewModeBox->setMarginBottom(15);

    m_allBtn = new brls::Button();
    m_allBtn->setText("All");
    m_allBtn->setMarginRight(10);
    styleButton(m_allBtn, true);
    m_allBtn->registerClickAction([this](brls::View* view) {
        showAllItems();
        return true;
    });
    m_viewModeBox->addView(m_allBtn);

    m_backBtn = new brls::Button();
    m_backBtn->setText("< Back");
    m_backBtn->setVisibility(brls::Visibility::GONE);
    styleButton(m_backBtn, false);
    m_backBtn->setBackgroundColor(nvgRGBA(80, 60, 50, 200));
    m_backBtn->registerClickAction([this](brls::View* view) {
        showAllItems();
        return true;
    });
    m_viewModeBox->addView(m_backBtn);

    this->addView(m_viewModeBox);

    // Content grid
    m_contentGrid = new RecyclingGrid();
    m_contentGrid->setGrow(1.0f);
    m_contentGrid->setOnItemSelected([this](const MusicItem& item) {
        onItemSelected(item);
    });
    m_contentGrid->setOnItemStartAction([this](const MusicItem& item) {
        showAlbumContextMenu(item);
    });
    this->addView(m_contentGrid);

    // Default to Albums category
    brls::Logger::debug("LibraryTab: Initializing with Albums category");
    auto& children = m_sectionsBox->getChildren();
    if (children.size() > 1) {
        m_activeSectionBtn = dynamic_cast<brls::Button*>(children[1]);
    } else if (!children.empty()) {
        m_activeSectionBtn = dynamic_cast<brls::Button*>(children[0]);
    }
    updateSectionButtonStyles();
    onCategorySelected(MusicCategory::ALBUMS);
}

LibraryTab::~LibraryTab() {
    if (m_alive) { *m_alive = false; }
}

void LibraryTab::willDisappear(bool resetState) {
    brls::Box::willDisappear(resetState);
    if (m_alive) *m_alive = false;
    ImageLoader::cancelAll();
}

void LibraryTab::onFocusGained() {
    brls::Box::onFocusGained();
    m_alive = std::make_shared<bool>(true);

    if (!m_loaded) {
        onCategorySelected(static_cast<MusicCategory>(m_currentCategory));
    }
}

void LibraryTab::styleButton(brls::Button* btn, bool active) {
    btn->setCornerRadius(16);
    btn->setHighlightCornerRadius(16);
    btn->setPadding(6, 16, 6, 16);
    if (active) {
        btn->setBackgroundColor(nvgRGBA(70, 90, 210, 220));
        btn->setBorderColor(nvgRGBA(120, 160, 255, 200));
        btn->setBorderThickness(1.5f);
    } else {
        btn->setBackgroundColor(nvgRGBA(60, 60, 70, 180));
        btn->setBorderColor(nvgRGBA(0, 0, 0, 0));
        btn->setBorderThickness(0);
    }
}

void LibraryTab::updateSectionButtonStyles() {
    if (!m_sectionsBox) return;
    for (auto* child : m_sectionsBox->getChildren()) {
        auto* btn = dynamic_cast<brls::Button*>(child);
        if (btn) {
            styleButton(btn, btn == m_activeSectionBtn);
        }
    }
}

void LibraryTab::updateViewModeButtons() {
    bool inFilteredView = (m_viewMode == LibraryTabViewMode::FILTERED);
    m_backBtn->setVisibility(inFilteredView ? brls::Visibility::VISIBLE : brls::Visibility::GONE);

    bool showModeButtons = (m_viewMode != LibraryTabViewMode::FILTERED);
    m_allBtn->setVisibility(showModeButtons ? brls::Visibility::VISIBLE : brls::Visibility::GONE);

    if (showModeButtons) {
        styleButton(m_allBtn, m_viewMode == LibraryTabViewMode::ALL_ITEMS);
    }
}

void LibraryTab::onCategorySelected(MusicCategory category) {
    m_currentCategory = static_cast<int>(category);
    m_currentCategoryName = categoryName(category);
    m_titleLabel->setText(std::string("Library - ") + m_currentCategoryName);

    // Reset view mode
    m_viewMode = LibraryTabViewMode::ALL_ITEMS;
    updateViewModeButtons();

    loadCategoryContent(category);
}

void LibraryTab::loadCategoryContent(MusicCategory category) {
    brls::Logger::debug("LibraryTab::loadCategoryContent - {}", categoryName(category));

    // Reset pagination state for fresh category load
    m_offset = 0;
    m_hasMore = false;
    m_loadingPage = false;
    m_items.clear();

    MAClient& client = MAClient::instance();
    auto aliveWeak = std::weak_ptr<bool>(m_alive);

    auto onResponse = [this, category, aliveWeak](bool success, const Json& result) {
        if (!success) {
            brls::Logger::error("LibraryTab: Failed to load {} content", categoryName(category));
            brls::sync([aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;
                brls::Application::notify("Failed to load library content");
            });
            return;
        }

        MediaType expectedType = MediaType::UNKNOWN;
        switch (category) {
            case MusicCategory::ARTISTS:   expectedType = MediaType::ARTIST;   break;
            case MusicCategory::ALBUMS:    expectedType = MediaType::ALBUM;    break;
            case MusicCategory::TRACKS:    expectedType = MediaType::TRACK;    break;
            case MusicCategory::PLAYLISTS: expectedType = MediaType::PLAYLIST; break;
        }

        std::vector<MusicItem> items = parseMusicItems(result, expectedType);
        int count = (int)items.size();
        brls::Logger::info("LibraryTab: Got {} {} items (offset {})", count, categoryName(category), m_offset);

        brls::sync([this, items, count, aliveWeak]() {
            auto alive = aliveWeak.lock();
            if (!alive || !*alive) return;

            m_offset += count;
            m_hasMore = (count >= PAGE_SIZE);  // If we got a full page, there may be more
            m_loadingPage = false;

            if (m_items.empty()) {
                // First page - set as data source
                m_items = items;
                if (m_viewMode == LibraryTabViewMode::ALL_ITEMS) {
                    m_contentGrid->setDataSource(m_items);
                }
            } else {
                // Subsequent pages - append
                m_items.insert(m_items.end(), items.begin(), items.end());
                if (m_viewMode == LibraryTabViewMode::ALL_ITEMS) {
                    m_contentGrid->appendItems(items);
                }
            }

            m_contentGrid->setHasMore(m_hasMore);
            m_loaded = true;
        });
    };

    // Set up pagination callback
    m_contentGrid->setOnLoadMore([this]() { loadNextPage(); });

    switch (category) {
        case MusicCategory::ARTISTS:
            client.getLibraryArtists(onResponse, "", PAGE_SIZE, 0);
            break;
        case MusicCategory::ALBUMS:
            client.getLibraryAlbums(onResponse, "", PAGE_SIZE, 0);
            break;
        case MusicCategory::TRACKS:
            client.getLibraryTracks(onResponse, "", PAGE_SIZE, 0);
            break;
        case MusicCategory::PLAYLISTS:
            client.getLibraryPlaylists(onResponse, "", PAGE_SIZE, 0);
            break;
    }
}

void LibraryTab::loadNextPage() {
    if (m_loadingPage || !m_hasMore) return;
    m_loadingPage = true;

    MusicCategory category = static_cast<MusicCategory>(m_currentCategory);
    brls::Logger::debug("LibraryTab: Loading next page at offset {}", m_offset);

    MAClient& client = MAClient::instance();
    auto aliveWeak = std::weak_ptr<bool>(m_alive);

    auto onResponse = [this, category, aliveWeak](bool success, const Json& result) {
        if (!success) {
            brls::sync([this, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;
                m_loadingPage = false;
                m_contentGrid->setHasMore(false);
            });
            return;
        }

        MediaType expectedType = MediaType::UNKNOWN;
        switch (category) {
            case MusicCategory::ARTISTS:   expectedType = MediaType::ARTIST;   break;
            case MusicCategory::ALBUMS:    expectedType = MediaType::ALBUM;    break;
            case MusicCategory::TRACKS:    expectedType = MediaType::TRACK;    break;
            case MusicCategory::PLAYLISTS: expectedType = MediaType::PLAYLIST; break;
        }

        std::vector<MusicItem> items = parseMusicItems(result, expectedType);
        int count = (int)items.size();
        brls::Logger::info("LibraryTab: Got {} more {} items (offset {})", count, categoryName(category), m_offset);

        brls::sync([this, items, count, aliveWeak]() {
            auto alive = aliveWeak.lock();
            if (!alive || !*alive) return;

            m_offset += count;
            m_hasMore = (count >= PAGE_SIZE);
            m_loadingPage = false;

            m_items.insert(m_items.end(), items.begin(), items.end());
            if (m_viewMode == LibraryTabViewMode::ALL_ITEMS) {
                m_contentGrid->appendItems(items);
            }
            m_contentGrid->setHasMore(m_hasMore);
        });
    };

    switch (category) {
        case MusicCategory::ARTISTS:
            client.getLibraryArtists(onResponse, "", PAGE_SIZE, m_offset);
            break;
        case MusicCategory::ALBUMS:
            client.getLibraryAlbums(onResponse, "", PAGE_SIZE, m_offset);
            break;
        case MusicCategory::TRACKS:
            client.getLibraryTracks(onResponse, "", PAGE_SIZE, m_offset);
            break;
        case MusicCategory::PLAYLISTS:
            client.getLibraryPlaylists(onResponse, "", PAGE_SIZE, m_offset);
            break;
    }
}

void LibraryTab::showAllItems() {
    m_viewMode = LibraryTabViewMode::ALL_ITEMS;
    m_titleLabel->setText(std::string("Library - ") + m_currentCategoryName);
    m_contentGrid->setDataSource(m_items);
    m_contentGrid->setHasMore(m_hasMore);
    updateViewModeButtons();
}

void LibraryTab::onItemSelected(const MusicItem& item) {
    // For tracks, play directly
    if (item.mediaType == MediaType::TRACK) {
        // TODO: Use MA queue API (playMedia) to start playback of this track
        // For now, push player activity with the item's ID
        brls::Logger::debug("LibraryTab: Playing track: {}", item.name);
        Application::getInstance().pushPlayerActivity(item.itemId);
        return;
    }

    // For artists, albums, playlists - show detail view
    auto* detailView = new MediaDetailView(item);
    brls::Application::pushActivity(new brls::Activity(detailView));
}

void LibraryTab::showAlbumContextMenu(const MusicItem& album) {
    // Delegate to the fully-implemented static context menus in MediaDetailView.
    // These support both local and remote players via playTracksOnSelectedPlayer.
    if (album.mediaType == MediaType::ARTIST) {
        MediaDetailView::showArtistContextMenuStatic(album);
    } else if (album.mediaType == MediaType::TRACK) {
        MediaDetailView::performTrackActionStatic(album);
    } else {
        // Albums, Playlists, and anything else: use album context menu
        // (works for playlists too since both fetch tracks and queue them)
        MediaDetailView::showAlbumContextMenuStatic(album);
    }
}

} // namespace vita_ma
