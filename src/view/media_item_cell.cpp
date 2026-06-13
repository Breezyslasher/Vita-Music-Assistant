/**
 * Vita Music Assistant - Media Item Cell implementation
 */

#include "view/media_item_cell.hpp"
#include "app/ma_types.hpp"
#include "app/application.hpp"
#include "app/ma_client.hpp"
#include "utils/image_loader.hpp"
#include <algorithm>

namespace vita_ma {

namespace {
// Paints the cover from a raw NanoVG image handle owned by the cell. It sits
// where the old brls::Image cover sat in the child list, so the focus "start"
// hint (added to the cell after it) still draws on top. It reads the cell's
// handle/size through pointers, so it always reflects the latest loaded cover.
class CoverView : public brls::Image {
public:
    const int* coverHandle = nullptr;
    const int* coverW = nullptr;
    const int* coverH = nullptr;

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override {
        if (!coverHandle || *coverHandle == 0) return;
        if (!coverW || !coverH || *coverW <= 0 || *coverH <= 0) return;
        if (width <= 0.0f || height <= 0.0f) return;

        // Cover-fit: scale to fill the box, center-cropping the overflow. The
        // rounded-rect fill path clips both the corners and the overflow.
        float scale = std::max(width / static_cast<float>(*coverW),
                               height / static_cast<float>(*coverH));
        float sw = static_cast<float>(*coverW) * scale;
        float sh = static_cast<float>(*coverH) * scale;
        float ox = x + (width - sw) * 0.5f;
        float oy = y + (height - sh) * 0.5f;
        NVGpaint paint = nvgImagePattern(vg, ox, oy, sw, sh, 0.0f, *coverHandle, 1.0f);
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y, width, height, 4.0f);
        nvgFillPaint(vg, paint);
        nvgFill(vg);
    }
};
}  // namespace

MediaItemCell::MediaItemCell()
    : m_alive(std::make_shared<std::atomic<bool>>(true)) {
    this->setAxis(brls::Axis::COLUMN);
    this->setJustifyContent(brls::JustifyContent::FLEX_START);
    this->setAlignItems(brls::AlignItems::CENTER);
    this->setPadding(5);
    this->setFocusable(true);
    this->setCornerRadius(8);
    this->setBackgroundColor(nvgRGBA(50, 50, 50, 255));

    // Cover view - square album art, painted from a NanoVG handle (see CoverView).
    // Kept VISIBLE so its draw() runs; it paints nothing until a cover loads.
    auto* cover = new CoverView();
    cover->coverHandle = &m_nvgCover;
    cover->coverW = &m_coverW;
    cover->coverH = &m_coverH;
    cover->setWidth(110);
    cover->setHeight(110);
    cover->setCornerRadius(4);
    m_thumbnailImage = cover;
    this->addView(m_thumbnailImage);

    // Title label
    m_titleLabel = new brls::Label();
    m_titleLabel->setFontSize(12);
    m_titleLabel->setMarginTop(5);
    m_titleLabel->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    this->addView(m_titleLabel);

    // Subtitle label (e.g. track number)
    m_subtitleLabel = new brls::Label();
    m_subtitleLabel->setFontSize(10);
    m_subtitleLabel->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    m_subtitleLabel->setVisibility(brls::Visibility::GONE);
    this->addView(m_subtitleLabel);

    // Description label (shows on focus)
    m_descriptionLabel = new brls::Label();
    m_descriptionLabel->setFontSize(9);
    m_descriptionLabel->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    m_descriptionLabel->setVisibility(brls::Visibility::GONE);
    this->addView(m_descriptionLabel);

    // Button hint box (shown on focus for album/artist/playlist items)
    // Small pill badge in top-right corner of album art
    m_buttonHintBox = new brls::Box();
    m_buttonHintBox->setAxis(brls::Axis::ROW);
    m_buttonHintBox->setJustifyContent(brls::JustifyContent::CENTER);
    m_buttonHintBox->setAlignItems(brls::AlignItems::CENTER);
    m_buttonHintBox->setPositionType(brls::PositionType::ABSOLUTE);
    m_buttonHintBox->setPositionTop(7);    // Small offset from top edge
    m_buttonHintBox->setPositionRight(7);  // Anchor to top-right corner
    m_buttonHintBox->setWidth(40);
    m_buttonHintBox->setHeight(16);
    m_buttonHintBox->setVisibility(brls::Visibility::GONE);

    m_buttonHintIcon = new brls::Image();
    m_buttonHintIcon->setWidth(40);
    m_buttonHintIcon->setHeight(16);
    m_buttonHintIcon->setScalingType(brls::ImageScalingType::FIT);
    m_buttonHintBox->addView(m_buttonHintIcon);

    m_buttonHintLabel = new brls::Label();
    m_buttonHintLabel->setFontSize(8);
    m_buttonHintLabel->setTextColor(nvgRGBA(255, 255, 255, 220));
    m_buttonHintBox->addView(m_buttonHintLabel);

    this->addView(m_buttonHintBox);
}

MediaItemCell::~MediaItemCell() {
    // Signal to any in-flight async image loads that this cell is destroyed
    if (m_alive) {
        m_alive->store(false);
    }
    // Free the cover texture. The cell is destroyed on the UI thread, so the
    // NanoVG context is valid here.
    if (m_nvgCover != 0) {
        NVGcontext* vg = brls::Application::getNVGContext();
        if (vg) nvgDeleteImage(vg, m_nvgCover);
        m_nvgCover = 0;
    }
}

void MediaItemCell::setItem(const MusicItem& item) {
    m_item = item;

    // Reset cover state so a reused cell reloads art for its new item rather
    // than showing the previous one (cells are normally created fresh, so this
    // is defensive).
    if (m_nvgCover != 0) {
        NVGcontext* vg = brls::Application::getNVGContext();
        if (vg) nvgDeleteImage(vg, m_nvgCover);
        m_nvgCover = 0;
        m_coverW = 0;
        m_coverH = 0;
    }
    m_thumbLoaded = false;

    // All items use square album art (music-only client)
    m_thumbnailImage->setWidth(110);
    m_thumbnailImage->setHeight(110);
    this->setWidth(120);
    this->setHeight(150);

    // Set title
    if (m_titleLabel) {
        std::string title = item.name;
        // Truncate long titles
        if (title.length() > 15) {
            title = title.substr(0, 13) + "...";
        }
        m_originalTitle = title;  // Store truncated title for focus restore
        m_titleLabel->setText(title);
        m_titleLabel->setVisibility(brls::Visibility::VISIBLE);
    }

    // Set subtitle for tracks
    if (m_subtitleLabel) {
        if (item.mediaType == MediaType::TRACK && item.trackNumber > 0) {
            m_subtitleLabel->setText("Track " + std::to_string(item.trackNumber));
            m_subtitleLabel->setVisibility(brls::Visibility::VISIBLE);
        } else {
            m_subtitleLabel->setVisibility(brls::Visibility::GONE);
        }
    }

    // Don't load the cover eagerly here. RecyclingGrid's progressive loader and
    // draw() drive loadThumbnailIfNeeded(), so covers load a few per frame
    // instead of all at once on a large dataset.
}

void MediaItemCell::loadThumbnail() {
    // Mark as loaded up front: the grid (and draw()) call this every frame, so we
    // must not re-enqueue while a load is in flight — and cells with no art must
    // not rebuild their URL every frame either.
    m_thumbLoaded = true;

    if (m_item.imageUrl.empty()) return;

    // Convert relative image paths to full URLs via the server
    std::string fullUrl = MAClient::instance().getThumbnailUrl(m_item.imageUrl, 300, 300, m_item.imageProvider);
    if (fullUrl.empty()) return;

    MediaItemCell* self = this;
    std::shared_ptr<std::atomic<bool>> alive = m_alive;
    ImageLoader::loadCoverAsync(fullUrl, [self, alive](int nvgImg, int w, int h) {
        // Runs on the UI thread. Re-check liveness before touching the cell;
        // if it's gone, free the just-created texture instead of leaking it.
        if (!alive->load()) {
            NVGcontext* vg = brls::Application::getNVGContext();
            if (vg && nvgImg != 0) nvgDeleteImage(vg, nvgImg);
            return;
        }
        if (self->m_nvgCover != 0) {
            NVGcontext* vg = brls::Application::getNVGContext();
            if (vg) nvgDeleteImage(vg, self->m_nvgCover);
        }
        self->m_nvgCover = nvgImg;
        self->m_coverW = w;
        self->m_coverH = h;
    }, m_alive);
}

void MediaItemCell::loadThumbnailIfNeeded() {
    if (m_thumbLoaded) return;
    loadThumbnail();
}

void MediaItemCell::unloadThumbnail() {
    if (m_nvgCover != 0) {
        NVGcontext* vg = brls::Application::getNVGContext();
        if (vg) nvgDeleteImage(vg, m_nvgCover);
        m_nvgCover = 0;
        m_coverW = 0;
        m_coverH = 0;
    }
    m_thumbLoaded = false;
}

void MediaItemCell::reloadThumbnail() {
    loadThumbnailIfNeeded();
}

brls::View* MediaItemCell::create() {
    return new MediaItemCell();
}

void MediaItemCell::draw(NVGcontext* vg, float x, float y, float width, float height,
                          brls::Style style, brls::FrameContext* ctx) {
    // A cell only reaches draw() when its row is VISIBLE (borealis skips drawing
    // INVISIBLE views), so this lazily loads covers exactly when they come on
    // screen. Cheap once loaded (guarded inside). Covers cells used outside the
    // grid too (home/search/music/detail rows), which have no preloader.
    loadThumbnailIfNeeded();

    // The cover itself is painted by the CoverView child (so the focus hint,
    // added after it, still draws on top); Box::draw renders all children.
    brls::Box::draw(vg, x, y, width, height, style, ctx);

    // Touch press feedback overlay
    if (m_pressed) {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y, width, height, 8);
        nvgFillColor(vg, nvgRGBA(0, 0, 0, 80));
        nvgFill(vg);
    }
}

void MediaItemCell::onFocusGained() {
    brls::Box::onFocusGained();
    // Focused background color
    this->setBackgroundColor(nvgRGBA(60, 60, 80, 255));
    // Selection border
    this->setBorderColor(nvgRGBA(229, 160, 13, 255));
    this->setBorderThickness(2.0f);
    updateFocusInfo(true);
}

void MediaItemCell::onFocusLost() {
    brls::Box::onFocusLost();
    // Restore default background
    this->setBackgroundColor(nvgRGBA(50, 50, 50, 255));
    this->setBorderColor(nvgRGBA(0, 0, 0, 0));
    this->setBorderThickness(0.0f);
    m_pressed = false;
    updateFocusInfo(false);
}

void MediaItemCell::updateFocusInfo(bool focused) {
    if (!m_titleLabel || !m_descriptionLabel) return;

    if (m_item.mediaType == MediaType::ALBUM) {
        // Show full title and year for albums on focus
        if (focused) {
            m_titleLabel->setText(m_item.name);
            std::string info;
            if (m_item.year > 0) {
                info = std::to_string(m_item.year);
            }
            if (!info.empty()) {
                m_descriptionLabel->setText(info);
                m_descriptionLabel->setVisibility(brls::Visibility::VISIBLE);
            }
            // Show button hint overlay
            if (m_buttonHintBox) {
                if (m_buttonHintIcon) {
                    m_buttonHintIcon->setImageFromRes("images/start_button.png");
                }
                if (m_buttonHintLabel) m_buttonHintLabel->setVisibility(brls::Visibility::GONE);
                m_buttonHintBox->setVisibility(brls::Visibility::VISIBLE);
            }
        } else {
            m_titleLabel->setText(m_originalTitle);
            m_descriptionLabel->setVisibility(brls::Visibility::GONE);
            if (m_buttonHintBox) m_buttonHintBox->setVisibility(brls::Visibility::GONE);
        }
    } else if (m_item.mediaType == MediaType::ARTIST) {
        // Show full title for artists on focus
        if (focused) {
            m_titleLabel->setText(m_item.name);
            // Show button hint overlay
            if (m_buttonHintBox) {
                if (m_buttonHintIcon) {
                    m_buttonHintIcon->setImageFromRes("images/start_button.png");
                }
                if (m_buttonHintLabel) m_buttonHintLabel->setVisibility(brls::Visibility::GONE);
                m_buttonHintBox->setVisibility(brls::Visibility::VISIBLE);
            }
        } else {
            m_titleLabel->setText(m_originalTitle);
            if (m_buttonHintBox) m_buttonHintBox->setVisibility(brls::Visibility::GONE);
        }
    } else if (m_item.mediaType == MediaType::TRACK) {
        // Show full title and duration for tracks on focus
        if (focused) {
            m_titleLabel->setText(m_item.name);
            if (m_item.duration > 0) {
                int minutes = m_item.duration / 60;
                int seconds = m_item.duration % 60;
                char info[16];
                snprintf(info, sizeof(info), "%d:%02d", minutes, seconds);
                m_descriptionLabel->setText(info);
                m_descriptionLabel->setVisibility(brls::Visibility::VISIBLE);
            }
        } else {
            m_titleLabel->setText(m_originalTitle);
            m_descriptionLabel->setVisibility(brls::Visibility::GONE);
        }
    } else if (m_item.mediaType == MediaType::PLAYLIST) {
        // Show full title and item count for playlists on focus
        if (focused) {
            m_titleLabel->setText(m_item.name);
            if (m_item.itemCount > 0) {
                std::string info = std::to_string(m_item.itemCount) + " items";
                m_descriptionLabel->setText(info);
                m_descriptionLabel->setVisibility(brls::Visibility::VISIBLE);
            }
            if (m_buttonHintBox) {
                if (m_buttonHintIcon) {
                    m_buttonHintIcon->setImageFromRes("images/start_button.png");
                }
                if (m_buttonHintLabel) m_buttonHintLabel->setVisibility(brls::Visibility::GONE);
                m_buttonHintBox->setVisibility(brls::Visibility::VISIBLE);
            }
        } else {
            m_titleLabel->setText(m_originalTitle);
            m_descriptionLabel->setVisibility(brls::Visibility::GONE);
            if (m_buttonHintBox) m_buttonHintBox->setVisibility(brls::Visibility::GONE);
        }
    }
}

} // namespace vita_ma
