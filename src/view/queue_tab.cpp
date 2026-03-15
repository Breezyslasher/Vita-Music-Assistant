#include "view/queue_tab.hpp"
#include "app/ma_client.hpp"
#include <borealis.hpp>
#include <cstdio>

namespace vita_ma {

QueueTab::QueueTab() {
    this->setAxis(brls::Axis::COLUMN);
    this->setGrow(1.0f);

    m_scrollFrame = new brls::ScrollingFrame();
    m_scrollFrame->setGrow(1.0f);

    m_content = new brls::Box();
    m_content->setAxis(brls::Axis::COLUMN);
    m_content->setPadding(16);

    auto* title = new brls::Label();
    title->setText("Play Queue");
    title->setFontSize(24);
    title->setMarginBottom(16);
    m_content->addView(title);

    m_scrollFrame->setContentView(m_content);
    this->addView(m_scrollFrame);
}

void QueueTab::onShow() {
    if (!m_loaded) {
        m_loaded = true;
        loadQueueItems();
    }
}

void QueueTab::refresh() {
    m_loaded = false;
    onShow();
}

void QueueTab::loadQueueItems() {
    auto& app = App::instance();
    std::string queueId = app.getQueueId();

    if (queueId.empty()) {
        auto* emptyLabel = new brls::Label();
        emptyLabel->setText("No active queue");
        emptyLabel->setFontSize(14);
        emptyLabel->setTextColor(nvgRGBA(180, 180, 180, 255));
        m_content->addView(emptyLabel);
        return;
    }

    MAClient::instance().getQueueItems(queueId, [this](bool success, const Json& result) {
        // Clear existing items (keep title)
        while (m_content->getChildren().size() > 1) {
            m_content->removeView(m_content->getChildren().back());
        }

        if (!success || result.type() != Json::ARRAY) {
            auto* errLabel = new brls::Label();
            errLabel->setText("Failed to load queue");
            errLabel->setFontSize(14);
            m_content->addView(errLabel);
            return;
        }

        auto state = App::instance().getQueueState();

        for (size_t i = 0; i < result.size(); i++) {
            auto& item = result[i];
            std::string itemId = item.has("queue_item_id") ? item["queue_item_id"].str() : "";
            std::string name = item.has("name") ? item["name"].str() : "Unknown";
            std::string artist;
            if (item.has("media_item") && item["media_item"].has("artists") &&
                item["media_item"]["artists"].size() > 0) {
                artist = item["media_item"]["artists"][static_cast<size_t>(0)]["name"].str();
            }

            int duration = 0;
            if (item.has("duration")) duration = item["duration"].intVal();

            auto* row = new brls::Box();
            row->setAxis(brls::Axis::ROW);
            row->setAlignItems(brls::AlignItems::CENTER);
            row->setHeight(50);
            row->setPadding(4, 8, 4, 8);
            row->setGap(8);
            row->setFocusable(true);

            // Track number
            auto* numLabel = new brls::Label();
            char numBuf[8];
            snprintf(numBuf, sizeof(numBuf), "%zu", i + 1);
            numLabel->setText(numBuf);
            numLabel->setFontSize(14);
            numLabel->setWidth(30);

            // Highlight current track
            bool isCurrent = (itemId == state.current_item_id) ||
                            (static_cast<int>(i) == state.current_index);
            if (isCurrent) {
                numLabel->setTextColor(nvgRGBA(100, 200, 255, 255));
            }
            row->addView(numLabel);

            // Track info
            auto* infoBox = new brls::Box();
            infoBox->setAxis(brls::Axis::COLUMN);
            infoBox->setGrow(1.0f);

            auto* nameLabel = new brls::Label();
            nameLabel->setText(name);
            nameLabel->setFontSize(14);
            nameLabel->setSingleLine(true);
            if (isCurrent) nameLabel->setTextColor(nvgRGBA(100, 200, 255, 255));
            infoBox->addView(nameLabel);

            if (!artist.empty()) {
                auto* artistLabel = new brls::Label();
                artistLabel->setText(artist);
                artistLabel->setFontSize(12);
                artistLabel->setTextColor(nvgRGBA(180, 180, 180, 255));
                artistLabel->setSingleLine(true);
                infoBox->addView(artistLabel);
            }
            row->addView(infoBox);

            // Duration
            if (duration > 0) {
                auto* durLabel = new brls::Label();
                char durBuf[16];
                snprintf(durBuf, sizeof(durBuf), "%d:%02d", duration / 60, duration % 60);
                durLabel->setText(durBuf);
                durLabel->setFontSize(12);
                durLabel->setTextColor(nvgRGBA(180, 180, 180, 255));
                row->addView(durLabel);
            }

            // Click to play this track
            int index = static_cast<int>(i);
            std::string qId = App::instance().getQueueId();
            row->registerClickAction([qId, index](...) {
                MAClient::instance().sendCommand("player_queues/play_index",
                    [index]() {
                        Json args;
                        args["queue_id"] = Json(App::instance().getQueueId());
                        args["index"] = Json(index);
                        return args;
                    }(), nullptr);
                return true;
            });

            m_content->addView(row);
        }

        if (result.size() == 0) {
            auto* emptyLabel = new brls::Label();
            emptyLabel->setText("Queue is empty");
            emptyLabel->setFontSize(14);
            emptyLabel->setTextColor(nvgRGBA(180, 180, 180, 255));
            m_content->addView(emptyLabel);
        }
    }, 100, 0);
}

} // namespace vita_ma
