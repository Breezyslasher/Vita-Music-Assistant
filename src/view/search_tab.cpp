/**
 * Vita Music Assistant - Search Tab implementation
 */

#include "view/search_tab.hpp"
#include "view/media_detail_view.hpp"
#include "view/media_item_cell.hpp"
#include "app/application.hpp"
#include "app/ma_client.hpp"
#include "utils/image_loader.hpp"

namespace vita_ma {

// Helper: parse a media_type string to MediaType enum
static MediaType parseMediaType(const std::string& s) {
    if (s == "track")    return MediaType::TRACK;
    if (s == "album")    return MediaType::ALBUM;
    if (s == "artist")   return MediaType::ARTIST;
    if (s == "playlist") return MediaType::PLAYLIST;
    if (s == "radio")    return MediaType::RADIO;
    return MediaType::UNKNOWN;
}

// Helper: parse a single JSON object into a MusicItem
static MusicItem musicItemFromJson(const Json& j) {
    MusicItem item;
    if (j.has("item_id"))    item.itemId    = j["item_id"].str();
    if (j.has("name"))       item.name      = j["name"].str();
    if (j.has("sort_name"))  item.sortName  = j["sort_name"].str();
    if (j.has("uri"))        item.uri       = j["uri"].str();
    // Extract image URL: try image object, then metadata.images array
    if (j.has("image") && j["image"].type() == Json::OBJECT && j["image"].has("path")) {
        item.imageUrl = MAClient::imageRefFromJson(j["image"]);
        if (j["image"].has("provider")) item.imageProvider = j["image"]["provider"].str();
    } else if (j.has("image") && j["image"].type() == Json::STRING) {
        item.imageUrl = j["image"].str();
    } else if (j.has("metadata") && j["metadata"].type() == Json::OBJECT) {
        const Json& meta = j["metadata"];
        if (meta.has("images") && meta["images"].type() == Json::ARRAY && meta["images"].size() > 0) {
            const Json& img = meta["images"][static_cast<size_t>(0)];
            item.imageUrl = MAClient::imageRefFromJson(img);
            if (img.has("provider")) item.imageProvider = img["provider"].str();
        }
    }

    if (j.has("media_type")) {
        item.mediaType = parseMediaType(j["media_type"].str());
    }

    // Track fields
    if (j.has("track_number")) item.trackNumber = j["track_number"].intVal();
    if (j.has("disc_number"))  item.discNumber  = j["disc_number"].intVal();
    if (j.has("duration"))     item.duration    = j["duration"].intVal();

    // Artist name - may be a string or nested in "artists" array
    if (j.has("artist")) {
        item.artistName = j["artist"].str();
    } else if (j.has("artists") && j["artists"].size() > 0) {
        const auto& firstArtist = j["artists"][static_cast<size_t>(0)];
        if (firstArtist.has("name")) {
            item.artistName = firstArtist["name"].str();
        }
    }

    if (j.has("album"))      item.albumName  = j["album"].str();
    if (j.has("year"))       item.year       = j["year"].intVal();
    if (j.has("version"))    item.version    = j["version"].str();
    if (j.has("subtype"))    item.subtype    = j["subtype"].str();
    if (j.has("biography"))  item.biography  = j["biography"].str();
    if (j.has("item_count")) item.itemCount  = j["item_count"].intVal();
    if (j.has("is_editable")) item.isEditable = j["is_editable"].boolVal();
    if (j.has("favorite"))   item.favorite   = j["favorite"].boolVal();
    if (j.has("provider"))   item.provider   = j["provider"].str();

    return item;
}

// Helper: parse a JSON array into a vector of MusicItems
static std::vector<MusicItem> musicItemsFromJsonArray(const Json& arr) {
    std::vector<MusicItem> items;
    for (size_t i = 0; i < arr.size(); i++) {
        items.push_back(musicItemFromJson(arr[i]));
    }
    return items;
}

SearchTab::SearchTab() {
    this->setAxis(brls::Axis::COLUMN);
    this->setJustifyContent(brls::JustifyContent::FLEX_START);
    this->setAlignItems(brls::AlignItems::STRETCH);
    this->setPadding(20);
    this->setGrow(1.0f);

    // Title
    m_titleLabel = new brls::Label();
    m_titleLabel->setText("Search");
    m_titleLabel->setFontSize(28);
    m_titleLabel->setMarginBottom(20);
    this->addView(m_titleLabel);

    // Search input label (acts as button to open keyboard)
    m_searchLabel = new brls::Label();
    m_searchLabel->setText("Tap to search...");
    m_searchLabel->setFontSize(20);
    m_searchLabel->setMarginBottom(10);
    m_searchLabel->setFocusable(true);

    m_searchLabel->registerClickAction([this](brls::View* view) {
        brls::Application::getImeManager()->openForText([this](std::string text) {
            m_searchQuery = text;
            m_searchLabel->setText(std::string("Search: ") + text);
            performSearch(text);
        }, "Search", "Enter search query", 256, m_searchQuery);
        return true;
    });
    m_searchLabel->addGestureRecognizer(new brls::TapGestureRecognizer(m_searchLabel));

    this->addView(m_searchLabel);

    // Results label
    m_resultsLabel = new brls::Label();
    m_resultsLabel->setText("");
    m_resultsLabel->setFontSize(18);
    m_resultsLabel->setMarginBottom(10);
    this->addView(m_resultsLabel);

    // Scrollable content for results
    m_scrollView = new brls::ScrollingFrame();
    m_scrollView->setGrow(1.0f);
    m_scrollView->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);

    m_scrollContent = new brls::Box();
    m_scrollContent->setAxis(brls::Axis::COLUMN);
    m_scrollContent->setJustifyContent(brls::JustifyContent::FLEX_START);
    m_scrollContent->setAlignItems(brls::AlignItems::STRETCH);

    // Albums row
    m_albumsLabel = new brls::Label();
    m_albumsLabel->setText("Albums");
    m_albumsLabel->setFontSize(20);
    m_albumsLabel->setMarginBottom(10);
    m_albumsLabel->setVisibility(brls::Visibility::GONE);
    m_scrollContent->addView(m_albumsLabel);

    m_albumsRow = new HorizontalScrollRow();
    m_albumsRow->setHeight(160);
    m_albumsRow->setMarginBottom(15);
    m_albumsRow->setVisibility(brls::Visibility::GONE);
    m_scrollContent->addView(m_albumsRow);

    // Tracks row
    m_tracksLabel = new brls::Label();
    m_tracksLabel->setText("Tracks");
    m_tracksLabel->setFontSize(20);
    m_tracksLabel->setMarginBottom(10);
    m_tracksLabel->setVisibility(brls::Visibility::GONE);
    m_scrollContent->addView(m_tracksLabel);

    m_tracksRow = new HorizontalScrollRow();
    m_tracksRow->setHeight(160);
    m_tracksRow->setMarginBottom(15);
    m_tracksRow->setVisibility(brls::Visibility::GONE);
    m_scrollContent->addView(m_tracksRow);

    m_scrollView->setContentView(m_scrollContent);
    this->addView(m_scrollView);
}

SearchTab::~SearchTab() {
    if (m_alive) *m_alive = false;
}

void SearchTab::willDisappear(bool resetState) {
    brls::Box::willDisappear(resetState);
    if (m_alive) *m_alive = false;
    m_loadGeneration++;
    ImageLoader::cancelAll();
    ImageLoader::clearCache();
}

void SearchTab::onFocusGained() {
    brls::Box::onFocusGained();
    m_alive = std::make_shared<bool>(true);

    // Focus search label
    if (m_searchLabel) {
        brls::Application::giveFocus(m_searchLabel);
    }
}

void SearchTab::populateRow(HorizontalScrollRow* row, const std::vector<MusicItem>& items) {
    if (!row) return;

    row->clearViews();

    for (const auto& item : items) {
        auto* cell = new MediaItemCell();
        cell->setItem(item);
        cell->setMarginRight(10);

        MusicItem capturedItem = item;
        cell->registerClickAction([this, capturedItem](brls::View* view) {
            onItemSelected(capturedItem);
            return true;
        });
        cell->addGestureRecognizer(new brls::TapGestureRecognizer(cell));

        // Register START button context menus for artists and albums
        if (capturedItem.mediaType == MediaType::ARTIST) {
            cell->registerAction("Options", brls::ControllerButton::BUTTON_START,
                [capturedItem](brls::View* view) {
                    MediaDetailView::showArtistContextMenuStatic(capturedItem);
                    return true;
                });
        } else if (capturedItem.mediaType == MediaType::ALBUM) {
            cell->registerAction("Options", brls::ControllerButton::BUTTON_START,
                [capturedItem](brls::View* view) {
                    MediaDetailView::showAlbumContextMenuStatic(capturedItem);
                    return true;
                });
        }

        row->addView(cell);
    }
}

void SearchTab::performSearch(const std::string& query) {
    if (query.empty()) {
        m_resultsLabel->setText("");
        m_results.clear();
        m_albums.clear();
        m_tracks.clear();

        // Hide all rows and labels
        m_albumsLabel->setVisibility(brls::Visibility::GONE);
        m_albumsRow->setVisibility(brls::Visibility::GONE);
        m_tracksLabel->setVisibility(brls::Visibility::GONE);
        m_tracksRow->setVisibility(brls::Visibility::GONE);
        return;
    }

    m_resultsLabel->setText("Searching...");

    // Run search async using MAClient async API
    int gen = ++m_loadGeneration;
    std::weak_ptr<bool> aliveWeak = m_alive;

    MAClient::instance().search(query, [this, gen, aliveWeak](bool success, const Json& result) {
        brls::sync([this, success, result, gen, aliveWeak]() {
            auto alive = aliveWeak.lock();
            if (!alive || !*alive) return;
            if (gen != m_loadGeneration) return;  // Stale result

            if (success) {
                m_results.clear();
                m_albums.clear();
                m_tracks.clear();

                // Music Assistant search returns an object with typed arrays:
                //   { "artists": [...], "albums": [...], "tracks": [...], "playlists": [...] }
                // Or it may return a flat array of items with "media_type" fields.

                if (result.has("albums")) {
                    auto albums = musicItemsFromJsonArray(result["albums"]);
                    for (auto& a : albums) {
                        if (a.mediaType == MediaType::UNKNOWN) a.mediaType = MediaType::ALBUM;
                        m_albums.push_back(std::move(a));
                    }
                }

                if (result.has("artists")) {
                    auto artists = musicItemsFromJsonArray(result["artists"]);
                    for (auto& a : artists) {
                        if (a.mediaType == MediaType::UNKNOWN) a.mediaType = MediaType::ARTIST;
                        // Artists go into the albums row alongside albums
                        m_albums.push_back(std::move(a));
                    }
                }

                if (result.has("tracks")) {
                    auto tracks = musicItemsFromJsonArray(result["tracks"]);
                    for (auto& t : tracks) {
                        if (t.mediaType == MediaType::UNKNOWN) t.mediaType = MediaType::TRACK;
                        m_tracks.push_back(std::move(t));
                    }
                }

                // Handle flat array response (fallback)
                if (result.type() == Json::ARRAY) {
                    for (size_t i = 0; i < result.size(); i++) {
                        MusicItem item = musicItemFromJson(result[i]);
                        if (item.mediaType == MediaType::ALBUM ||
                            item.mediaType == MediaType::ARTIST) {
                            m_albums.push_back(std::move(item));
                        } else if (item.mediaType == MediaType::TRACK) {
                            m_tracks.push_back(std::move(item));
                        }
                    }
                }

                // Combine all parsed items into m_results for total count
                m_results.insert(m_results.end(), m_albums.begin(), m_albums.end());
                m_results.insert(m_results.end(), m_tracks.begin(), m_tracks.end());

                m_resultsLabel->setText("Found " + std::to_string(m_results.size()) + " results");

                // Update album/artist row
                if (!m_albums.empty()) {
                    m_albumsLabel->setText("Albums (" + std::to_string(m_albums.size()) + ")");
                    m_albumsLabel->setVisibility(brls::Visibility::VISIBLE);
                    m_albumsRow->setVisibility(brls::Visibility::VISIBLE);
                    populateRow(m_albumsRow, m_albums);
                } else {
                    m_albumsLabel->setVisibility(brls::Visibility::GONE);
                    m_albumsRow->setVisibility(brls::Visibility::GONE);
                }

                // Update tracks row
                if (!m_tracks.empty()) {
                    m_tracksLabel->setText("Tracks (" + std::to_string(m_tracks.size()) + ")");
                    m_tracksLabel->setVisibility(brls::Visibility::VISIBLE);
                    m_tracksRow->setVisibility(brls::Visibility::VISIBLE);
                    populateRow(m_tracksRow, m_tracks);
                } else {
                    m_tracksLabel->setVisibility(brls::Visibility::GONE);
                    m_tracksRow->setVisibility(brls::Visibility::GONE);
                }

            } else {
                m_resultsLabel->setText("Search failed");
                m_results.clear();
            }
        });
    });
}

void SearchTab::onItemSelected(const MusicItem& item) {
    // For tracks, follow the default track action setting
    if (item.mediaType == MediaType::TRACK) {
        MediaDetailView::performTrackActionStatic(item);
        return;
    }

    // Show media detail view for other types
    auto* detailView = new MediaDetailView(item);
    brls::Application::pushActivity(new brls::Activity(detailView));
}

} // namespace vita_ma
