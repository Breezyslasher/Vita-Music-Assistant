#include "view/media_item_cell.hpp"
#include "utils/image_loader.hpp"
#include <borealis.hpp>

namespace vita_ma {

MediaItemCell::MediaItemCell() {
    this->setAxis(brls::Axis::COLUMN);
    this->setJustifyContent(brls::JustifyContent::FLEX_START);
    this->setAlignItems(brls::AlignItems::CENTER);
    this->setPadding(8);
    this->setCornerRadius(8);
    this->setFocusable(true);

    // Album art / thumbnail
    m_image = new brls::Image();
    m_image->setSize(brls::Size(130, 130));
    m_image->setCornerRadius(6);
    m_image->setScalingType(brls::ImageScalingType::FIT);
    this->addView(m_image);

    // Title
    m_title = new brls::Label();
    m_title->setFontSize(14);
    m_title->setMarginTop(6);
    m_title->setMaxWidth(130);
    m_title->setSingleLine(true);
    this->addView(m_title);

    // Subtitle
    m_subtitle = new brls::Label();
    m_subtitle->setFontSize(12);
    m_subtitle->setTextColor(nvgRGBA(180, 180, 180, 255));
    m_subtitle->setMaxWidth(130);
    m_subtitle->setSingleLine(true);
    this->addView(m_subtitle);
}

void MediaItemCell::setData(const MediaItem& item) {
    m_item = item;
    setTitle(item.name);
    setSubtitle(item.subtitle);
    if (!item.image_url.empty()) {
        setImage(item.image_url);
    }
}

void MediaItemCell::setTitle(const std::string& title) {
    if (m_title) m_title->setText(title);
}

void MediaItemCell::setSubtitle(const std::string& subtitle) {
    if (m_subtitle) m_subtitle->setText(subtitle);
}

void MediaItemCell::setImage(const std::string& url) {
    if (url.empty()) return;

    ImageLoader::instance().loadImage(url, [this](int texId) {
        if (texId >= 0 && m_image) {
            m_image->setImageFromMem(reinterpret_cast<unsigned char*>(&texId), 0);
        }
    });
}

brls::View* MediaItemCell::create() {
    return new MediaItemCell();
}

} // namespace vita_ma
