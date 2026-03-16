/**
 * Vita Music Assistant - Home Tab implementation
 */

#include "view/home_tab.hpp"
#include "view/media_item_cell.hpp"
#include "view/media_detail_view.hpp"
#include "app/application.hpp"
#include "app/ma_client.hpp"
#include "utils/image_loader.hpp"
#include "utils/async.hpp"

namespace vita_ma {

HomeTab::HomeTab() {
    this->setAxis(brls::Axis::COLUMN);
    this->setJustifyContent(brls::JustifyContent::FLEX_START);
    this->setAlignItems(brls::AlignItems::STRETCH);
    this->setGrow(1.0f);

    // Create vertical scrolling container for the entire tab
    m_scrollView = new brls::ScrollingFrame();
    m_scrollView->setGrow(1.0f);
    m_scrollView->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);

    m_scrollContent = new brls::Box();
    m_scrollContent->setAxis(brls::Axis::COLUMN);
    m_scrollContent->setJustifyContent(brls::JustifyContent::FLEX_START);
    m_scrollContent->setAlignItems(brls::AlignItems::STRETCH);
    m_scrollContent->setPadding(20);

    // Title
    m_titleLabel = new brls::Label();
    m_titleLabel->setText("Home");
    m_titleLabel->setFontSize(28);
    m_titleLabel->setMarginBottom(20);
    m_scrollContent->addView(m_titleLabel);

    // Recently Added Music section
    auto* musicLabel = new brls::Label();
    musicLabel->setText("Recently Added Music");
    musicLabel->setFontSize(22);
    musicLabel->setMarginBottom(10);
    musicLabel->setMarginTop(15);
    m_scrollContent->addView(musicLabel);

    m_musicRow = createMediaRow();
    m_scrollContent->addView(m_musicRow);

    m_scrollView->setContentView(m_scrollContent);
    this->addView(m_scrollView);

    // Load content immediately
    brls::Logger::debug("HomeTab: Loading content...");
    loadContent();
}

HorizontalScrollRow* HomeTab::createMediaRow() {
    auto* row = new HorizontalScrollRow();
    row->setHeight(210);
    row->setMarginBottom(10);
    return row;
}

void HomeTab::populateRow(HorizontalScrollRow* row, const std::vector<MusicItem>& items, bool directPlay) {
    if (!row) return;

    row->clearViews();

    for (const auto& item : items) {
        auto* cell = new MediaItemCell();
        cell->setItem(item);
        cell->setMarginRight(10);

        MusicItem capturedItem = item;
        cell->registerClickAction([this, capturedItem, directPlay](brls::View* view) {
            if (directPlay) {
                // Play directly for recently played items
                Application::getInstance().pushPlayerActivity(capturedItem.itemId);
            } else {
                onItemSelected(capturedItem);
            }
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

    // Add placeholder if empty
    if (items.empty()) {
        auto* placeholder = new brls::Label();
        placeholder->setText("No items");
        placeholder->setFontSize(16);
        placeholder->setMarginLeft(10);
        row->addView(placeholder);
    }
}

HomeTab::~HomeTab() {
    if (m_alive) { *m_alive = false; }
}

void HomeTab::willDisappear(bool resetState) {
    brls::Box::willDisappear(resetState);
    // Invalidate alive flag so pending async callbacks bail out
    if (m_alive) *m_alive = false;
    ImageLoader::cancelAll();
    // Free image cache when leaving home tab to reclaim memory
    ImageLoader::clearCache();

    // Free stored item data to reduce baseline memory
    m_recentMusic.clear();
    m_recentMusic.shrink_to_fit();

    // Mark as not loaded so data is re-fetched when returning
    m_loaded = false;
}

void HomeTab::onFocusGained() {
    brls::Box::onFocusGained();
    // Re-create alive flag so new async callbacks work (old ones still bail out)
    m_alive = std::make_shared<bool>(true);

    if (!m_loaded) {
        loadContent();
    }
}

void HomeTab::loadContent() {
    brls::Logger::debug("HomeTab::loadContent - Starting async load");

    // Load recently played music using MA async API
    MAClient::instance().getRecentlyPlayed(
        [this, aliveWeak = std::weak_ptr<bool>(m_alive)](bool success, const Json& result) {
            if (!success) {
                brls::Logger::error("HomeTab: Failed to fetch recently played music");
                return;
            }

            std::vector<MusicItem> music;

            if (result.type() == Json::ARRAY) {
                for (size_t i = 0; i < result.size(); i++) {
                    const Json& obj = result[i];
                    MusicItem item;
                    item.itemId    = obj.has("item_id")   ? obj["item_id"].str()   : "";
                    item.name      = obj.has("name")      ? obj["name"].str()      : "";
                    item.uri       = obj.has("uri")        ? obj["uri"].str()       : "";
                    item.provider  = obj.has("provider")   ? obj["provider"].str()  : "";

                    // Extract image URL: try image object, then metadata.images array
                    if (obj.has("image") && obj["image"].type() == Json::OBJECT && obj["image"].has("path")) {
                        item.imageUrl = obj["image"]["path"].str();
                    } else if (obj.has("image") && obj["image"].type() == Json::STRING) {
                        item.imageUrl = obj["image"].str();
                    } else if (obj.has("metadata") && obj["metadata"].type() == Json::OBJECT) {
                        const Json& meta = obj["metadata"];
                        if (meta.has("images") && meta["images"].type() == Json::ARRAY && meta["images"].size() > 0) {
                            const Json& img = meta["images"][static_cast<size_t>(0)];
                            if (img.has("path")) item.imageUrl = img["path"].str();
                        }
                    }

                    // Determine media type from uri or media_type field
                    std::string mediaTypeStr = obj.has("media_type") ? obj["media_type"].str() : "";
                    if (mediaTypeStr == "track")         item.mediaType = MediaType::TRACK;
                    else if (mediaTypeStr == "album")    item.mediaType = MediaType::ALBUM;
                    else if (mediaTypeStr == "artist")   item.mediaType = MediaType::ARTIST;
                    else if (mediaTypeStr == "playlist") item.mediaType = MediaType::PLAYLIST;
                    else if (mediaTypeStr == "radio")    item.mediaType = MediaType::RADIO;
                    else                                 item.mediaType = MediaType::TRACK;

                    // Track fields
                    if (obj.has("artist_name"))  item.artistName  = obj["artist_name"].str();
                    if (obj.has("album_name"))   item.albumName   = obj["album_name"].str();
                    if (obj.has("duration"))     item.duration     = obj["duration"].intVal();

                    music.push_back(item);
                }
            }

            brls::Logger::info("HomeTab: Got {} recently played music items", music.size());

            brls::sync([this, music, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;

                m_recentMusic = music;
                populateRow(m_musicRow, m_recentMusic);
            });
        }, 20);

    m_loaded = true;
    brls::Logger::debug("HomeTab: Async content loading started");
}

void HomeTab::onItemSelected(const MusicItem& item) {
    // For tracks, play directly instead of showing detail view
    if (item.mediaType == MediaType::TRACK) {
        Application::getInstance().pushPlayerActivity(item.itemId);
        return;
    }

    // Show media detail view for other types (artists, albums)
    auto* detailView = new MediaDetailView(item);
    brls::Application::pushActivity(new brls::Activity(detailView));
}

} // namespace vita_ma
