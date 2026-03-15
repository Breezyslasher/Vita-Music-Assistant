#include "view/home_tab.hpp"
#include "app/ma_client.hpp"
#include "view/media_item_cell.hpp"
#include <borealis.hpp>

namespace vita_ma {

HomeTab::HomeTab() {
    this->setAxis(brls::Axis::COLUMN);
    this->setGrow(1.0f);

    m_scrollFrame = new brls::ScrollingFrame();
    m_scrollFrame->setGrow(1.0f);

    m_content = new brls::Box();
    m_content->setAxis(brls::Axis::COLUMN);
    m_content->setPadding(16);

    // Title
    auto* title = new brls::Label();
    title->setText("Home");
    title->setFontSize(24);
    title->setMarginBottom(16);
    m_content->addView(title);

    // Recently Played section
    auto* recentLabel = new brls::Label();
    recentLabel->setText("Recently Played");
    recentLabel->setFontSize(18);
    recentLabel->setMarginBottom(8);
    m_content->addView(recentLabel);

    m_recentRow = new HorizontalScrollRow();
    m_recentRow->setHeight(200);
    m_content->addView(m_recentRow);

    // Recommendations section
    auto* recLabel = new brls::Label();
    recLabel->setText("Recommendations");
    recLabel->setFontSize(18);
    recLabel->setMarginTop(16);
    recLabel->setMarginBottom(8);
    m_content->addView(recLabel);

    m_recommendRow = new HorizontalScrollRow();
    m_recommendRow->setHeight(200);
    m_content->addView(m_recommendRow);

    m_scrollFrame->setContentView(m_content);
    this->addView(m_scrollFrame);
}

void HomeTab::onShow() {
    if (!m_loaded) {
        m_loaded = true;
        loadRecentlyPlayed();
        loadRecommendations();
    }
}

void HomeTab::loadRecentlyPlayed() {
    MAClient::instance().getRecentlyPlayed([this](bool success, const Json& result) {
        if (!success || result.type() != Json::ARRAY) return;

        for (size_t i = 0; i < result.size() && i < 20; i++) {
            auto& item = result[i];
            auto* cell = new MediaItemCell();

            MediaItem mediaItem;
            mediaItem.name = item["name"].str();
            if (item.has("artists")) {
                auto& artists = item["artists"];
                if (artists.size() > 0) {
                    mediaItem.subtitle = artists[static_cast<size_t>(0)]["name"].str();
                }
            }
            if (item.has("image")) {
                mediaItem.image_url = item["image"]["url"].str();
            } else if (item.has("metadata") && item["metadata"].has("images")) {
                auto& images = item["metadata"]["images"];
                if (images.size() > 0) {
                    mediaItem.image_url = images[static_cast<size_t>(0)]["url"].str();
                }
            }
            mediaItem.uri = item.has("uri") ? item["uri"].str() : "";
            cell->setData(mediaItem);

            cell->registerClickAction([mediaItem](...) {
                if (!mediaItem.uri.empty()) {
                    auto& app = App::instance();
                    MAClient::instance().playMedia(
                        app.getQueueId(), mediaItem.uri, "play");
                }
                return true;
            });

            m_recentRow->addCell(cell);
        }
    }, 20);
}

void HomeTab::loadRecommendations() {
    MAClient::instance().getRecommendations([this](bool success, const Json& result) {
        if (!success || result.type() != Json::ARRAY) return;

        for (size_t i = 0; i < result.size() && i < 20; i++) {
            auto& item = result[i];
            auto* cell = new MediaItemCell();

            MediaItem mediaItem;
            mediaItem.name = item["name"].str();
            if (item.has("artists") && item["artists"].size() > 0) {
                mediaItem.subtitle = item["artists"][static_cast<size_t>(0)]["name"].str();
            }
            if (item.has("image")) {
                mediaItem.image_url = item["image"]["url"].str();
            }
            mediaItem.uri = item.has("uri") ? item["uri"].str() : "";
            cell->setData(mediaItem);

            cell->registerClickAction([mediaItem](...) {
                if (!mediaItem.uri.empty()) {
                    auto& app = App::instance();
                    MAClient::instance().playMedia(
                        app.getQueueId(), mediaItem.uri, "play");
                }
                return true;
            });

            m_recommendRow->addCell(cell);
        }
    });
}

} // namespace vita_ma
