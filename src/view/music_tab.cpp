#include "view/music_tab.hpp"
#include "app/ma_client.hpp"
#include "view/media_item_cell.hpp"
#include <borealis.hpp>

namespace vita_ma {

MusicTab::MusicTab() {
    this->setAxis(brls::Axis::COLUMN);
    this->setGrow(1.0f);

    m_scrollFrame = new brls::ScrollingFrame();
    m_scrollFrame->setGrow(1.0f);

    m_content = new brls::Box();
    m_content->setAxis(brls::Axis::COLUMN);
    m_content->setPadding(16);

    auto* title = new brls::Label();
    title->setText("Browse");
    title->setFontSize(24);
    title->setMarginBottom(16);
    m_content->addView(title);

    m_scrollFrame->setContentView(m_content);
    this->addView(m_scrollFrame);
}

void MusicTab::onShow() {
    if (!m_loaded) {
        m_loaded = true;
        loadBrowsePath("");
    }
}

void MusicTab::loadBrowsePath(const std::string& path) {
    MAClient::instance().browse(path, [this, path](bool success, const Json& result) {
        if (!success) return;

        // Clear current content except title
        while (m_content->getChildren().size() > 1) {
            m_content->removeView(m_content->getChildren().back());
        }

        // Back button if we're in a subfolder
        if (!path.empty()) {
            auto* backBtn = new brls::Label();
            backBtn->setText("<  Back");
            backBtn->setFontSize(16);
            backBtn->setFocusable(true);
            backBtn->setMarginBottom(12);
            backBtn->registerClickAction([this](...) {
                navigateBack();
                return true;
            });
            m_content->addView(backBtn);
        }

        if (result.type() != Json::ARRAY) return;

        for (size_t i = 0; i < result.size(); i++) {
            auto& item = result[i];

            auto* row = new brls::Box();
            row->setAxis(brls::Axis::ROW);
            row->setAlignItems(brls::AlignItems::CENTER);
            row->setPadding(8);
            row->setGap(12);
            row->setFocusable(true);
            row->setCornerRadius(8);
            row->setHeight(60);

            auto* label = new brls::Label();
            label->setText(item["label"].str());
            label->setFontSize(16);
            label->setGrow(1.0f);
            row->addView(label);

            bool isFolder = item.has("is_folder") && item["is_folder"].boolVal();
            std::string itemPath = item.has("path") ? item["path"].str() : "";
            std::string itemUri = item.has("uri") ? item["uri"].str() : "";

            if (isFolder) {
                auto* arrow = new brls::Label();
                arrow->setText(">");
                arrow->setFontSize(16);
                row->addView(arrow);
            }

            row->registerClickAction([this, isFolder, itemPath, itemUri, path](...) {
                if (isFolder && !itemPath.empty()) {
                    m_pathHistory.push_back(path);
                    loadBrowsePath(itemPath);
                } else if (!itemUri.empty()) {
                    auto& app = App::instance();
                    MAClient::instance().playMedia(
                        app.getQueueId(), itemUri, "play");
                }
                return true;
            });

            m_content->addView(row);
        }
    });
}

void MusicTab::navigateBack() {
    if (m_pathHistory.empty()) {
        loadBrowsePath("");
    } else {
        std::string prev = m_pathHistory.back();
        m_pathHistory.pop_back();
        loadBrowsePath(prev);
    }
}

} // namespace vita_ma
