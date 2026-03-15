#include "view/search_tab.hpp"
#include "app/ma_client.hpp"
#include "view/media_item_cell.hpp"
#include "view/horizontal_scroll_row.hpp"
#include <borealis.hpp>

namespace vita_ma {

SearchTab::SearchTab() {
    this->setAxis(brls::Axis::COLUMN);
    this->setGrow(1.0f);

    m_scrollFrame = new brls::ScrollingFrame();
    m_scrollFrame->setGrow(1.0f);

    m_content = new brls::Box();
    m_content->setAxis(brls::Axis::COLUMN);
    m_content->setPadding(16);

    auto* title = new brls::Label();
    title->setText("Search");
    title->setFontSize(24);
    title->setMarginBottom(16);
    m_content->addView(title);

    // Search button (opens IME keyboard on Vita)
    auto* searchBtn = new brls::Label();
    searchBtn->setText("Tap to search...");
    searchBtn->setFontSize(16);
    searchBtn->setFocusable(true);
    searchBtn->setPadding(12, 16, 12, 16);
    searchBtn->setCornerRadius(8);
    searchBtn->setMarginBottom(16);

    searchBtn->registerClickAction([this](...) {
        showSearchInput();
        return true;
    });
    m_content->addView(searchBtn);

    // Results container
    m_resultsBox = new brls::Box();
    m_resultsBox->setAxis(brls::Axis::COLUMN);
    m_content->addView(m_resultsBox);

    m_scrollFrame->setContentView(m_content);
    this->addView(m_scrollFrame);
}

void SearchTab::onShow() {
    // Nothing needed on show
}

void SearchTab::showSearchInput() {
    // Use Borealis IME input dialog
    brls::Swkbd::openForText([this](const std::string& text) {
        if (!text.empty()) {
            performSearch(text);
        }
    }, "Search Music", "Enter artist, album, or track name", 256, m_lastQuery);
}

void SearchTab::performSearch(const std::string& query) {
    m_lastQuery = query;
    m_resultsBox->clearViews();

    auto* loadingLabel = new brls::Label();
    loadingLabel->setText("Searching for \"" + query + "\"...");
    loadingLabel->setFontSize(14);
    m_resultsBox->addView(loadingLabel);

    MAClient::instance().search(query, [this](bool success, const Json& result) {
        m_resultsBox->clearViews();

        if (!success) {
            auto* errorLabel = new brls::Label();
            errorLabel->setText("Search failed. Check connection.");
            errorLabel->setFontSize(14);
            m_resultsBox->addView(errorLabel);
            return;
        }

        // Parse categorized results
        struct ResultCategory {
            std::string label;
            std::string key;
        };

        ResultCategory categories[] = {
            {"Artists", "artists"},
            {"Albums", "albums"},
            {"Tracks", "tracks"},
            {"Playlists", "playlists"},
            {"Radio", "radio"}
        };

        bool anyResults = false;

        for (auto& cat : categories) {
            if (!result.has(cat.key)) continue;
            auto& items = result[cat.key];
            if (items.type() != Json::ARRAY || items.size() == 0) continue;

            anyResults = true;

            auto* sectionLabel = new brls::Label();
            sectionLabel->setText(cat.label);
            sectionLabel->setFontSize(18);
            sectionLabel->setMarginTop(12);
            sectionLabel->setMarginBottom(8);
            m_resultsBox->addView(sectionLabel);

            auto* row = new HorizontalScrollRow();
            row->setHeight(200);

            for (size_t i = 0; i < items.size(); i++) {
                auto& item = items[i];
                auto* cell = new MediaItemCell();

                MediaItem mediaItem;
                mediaItem.item_id = item["item_id"].str();
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

                row->addCell(cell);
            }

            m_resultsBox->addView(row);
        }

        if (!anyResults) {
            auto* noResults = new brls::Label();
            noResults->setText("No results found.");
            noResults->setFontSize(14);
            m_resultsBox->addView(noResults);
        }
    });
}

} // namespace vita_ma
