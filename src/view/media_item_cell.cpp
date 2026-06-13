/**
 * Vita Music Assistant - Media Item Cell implementation
 */

#include "view/media_item_cell.hpp"
#include "app/ma_types.hpp"
#include "app/application.hpp"
#include "app/ma_client.hpp"
#include "utils/image_loader.hpp"
#include <algorithm>
#include <cstdio>

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

    // Title and secondary line are drawn directly with nvgText in draw() (no
    // brls::Label children), so a full screen of cells is cheap to render.

    // START button hint (shown on focus for album/artist/playlist items), a
    // small image in the top-right of the cover. Focus-only, so it is GONE for
    // every cell except the focused one — negligible per-frame cost.
    m_buttonHintBox = new brls::Box();
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

    // New item — the fitted title must be recomputed in draw().
    m_titleCached = false;

    // All items use square album art (music-only client)
    m_thumbnailImage->setWidth(110);
    m_thumbnailImage->setHeight(110);
    this->setWidth(120);
    this->setHeight(150);

    // Title / secondary line are drawn in draw(); nothing to set here.
    // Cover loading is driven by RecyclingGrid's progressive loader and draw(),
    // so covers load a few per frame instead of all at once on a large dataset.
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

    // Box::draw renders the background, border, the cover (CoverView child) and
    // the focus START hint.
    brls::Box::draw(vg, x, y, width, height, style, ctx);

    // Title + secondary line, drawn directly with nvgText below the cover (no
    // Label child views). The cover occupies the top: cell top padding (5) plus
    // a 110px cover, so text starts at y + 115.
    float textTop = y + 5.0f + 110.0f;
    float cx = x + width * 0.5f;
    float maxW = width - 8.0f;

    // Fit/measure first (this sets its own LEFT align internally), then set the
    // centered draw state for the actual nvgText calls.
    cacheTitleText(vg, 12.0f, maxW);

    nvgFontFace(vg, "regular");
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);

    if (!m_cachedTitle.empty()) {
        nvgFontSize(vg, 12.0f);
        nvgFillColor(vg, nvgRGBA(255, 255, 255, 255));
        nvgText(vg, cx, textTop + 4.0f, m_cachedTitle.c_str(), nullptr);
    }

    std::string secondary = secondaryLine();
    if (!secondary.empty()) {
        nvgFontSize(vg, 10.0f);
        nvgFillColor(vg, nvgRGBA(200, 200, 200, 255));
        nvgText(vg, cx, textTop + 20.0f, secondary.c_str(), nullptr);
    }

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
    m_focused = focused;

    // The START hint shows on focus for items that have a context action
    // (albums, artists, playlists). Tracks show their duration instead.
    if (!m_buttonHintBox) return;
    bool showHint = focused && (m_item.mediaType == MediaType::ALBUM ||
                                m_item.mediaType == MediaType::ARTIST ||
                                m_item.mediaType == MediaType::PLAYLIST);
    if (showHint) {
        if (m_buttonHintIcon) m_buttonHintIcon->setImageFromRes("images/start_button.png");
        m_buttonHintBox->setVisibility(brls::Visibility::VISIBLE);
    } else {
        m_buttonHintBox->setVisibility(brls::Visibility::GONE);
    }
}

void MediaItemCell::cacheTitleText(NVGcontext* vg, float fontSize, float maxWidth) {
    if (m_titleCached && m_cachedTitleWidth == maxWidth) return;
    m_titleCached = true;
    m_cachedTitleWidth = maxWidth;

    const std::string& title = m_item.name;
    if (title.empty()) {
        m_cachedTitle.clear();
        return;
    }

    nvgFontFace(vg, "regular");
    nvgFontSize(vg, fontSize);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);

    // Fit to a single line; if it overflows, ellipsise the first row.
    NVGtextRow rows[2];
    int nrows = nvgTextBreakLines(vg, title.c_str(), nullptr, maxWidth, rows, 2);
    if (nrows <= 0) {
        m_cachedTitle = title;
        return;
    }
    if (nrows == 1 && rows[0].end == title.c_str() + title.size()) {
        m_cachedTitle.assign(rows[0].start, rows[0].end);  // fits as-is
        return;
    }
    std::string line(rows[0].start, rows[0].end);
    while (!line.empty() && line.back() == ' ') line.pop_back();
    m_cachedTitle = line + "...";
}

std::string MediaItemCell::secondaryLine() const {
    if (m_focused) {
        if (m_item.mediaType == MediaType::ALBUM && m_item.year > 0)
            return std::to_string(m_item.year);
        if (m_item.mediaType == MediaType::TRACK && m_item.duration > 0) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d:%02d", m_item.duration / 60, m_item.duration % 60);
            return buf;
        }
        if (m_item.mediaType == MediaType::PLAYLIST && m_item.itemCount > 0)
            return std::to_string(m_item.itemCount) + " items";
        return "";
    }
    // Unfocused: track number for tracks.
    if (m_item.mediaType == MediaType::TRACK && m_item.trackNumber > 0)
        return "Track " + std::to_string(m_item.trackNumber);
    return "";
}

} // namespace vita_ma
