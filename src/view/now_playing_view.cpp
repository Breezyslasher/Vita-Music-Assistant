#include "view/now_playing_view.hpp"
#include "app/ma_client.hpp"
#include "app/ma_types.hpp"
#include "utils/image_loader.hpp"
#include <borealis.hpp>
#include <cstdio>

namespace vita_ma {

NowPlayingView::NowPlayingView() {
    this->setAxis(brls::Axis::COLUMN);

    // --- Mini Player Bar (shown at bottom of screen) ---
    m_miniBar = new brls::Box();
    m_miniBar->setAxis(brls::Axis::ROW);
    m_miniBar->setAlignItems(brls::AlignItems::CENTER);
    m_miniBar->setHeight(56);
    m_miniBar->setPadding(4, 12, 4, 12);
    m_miniBar->setFocusable(true);
    m_miniBar->setCornerRadius(8);

    // Mini album art
    m_miniArt = new brls::Image();
    m_miniArt->setSize(brls::Size(48, 48));
    m_miniArt->setCornerRadius(4);
    m_miniBar->addView(m_miniArt);

    // Text container
    auto* miniText = new brls::Box();
    miniText->setAxis(brls::Axis::COLUMN);
    miniText->setGrow(1.0f);

    m_miniTitle = new brls::Label();
    m_miniTitle->setFontSize(14);
    m_miniTitle->setSingleLine(true);
    miniText->addView(m_miniTitle);

    m_miniArtist = new brls::Label();
    m_miniArtist->setFontSize(12);
    m_miniArtist->setTextColor(nvgRGBA(180, 180, 180, 255));
    m_miniArtist->setSingleLine(true);
    miniText->addView(m_miniArtist);

    m_miniBar->addView(miniText);

    // Play/Pause indicator
    auto* playIcon = new brls::Label();
    playIcon->setText("||");
    playIcon->setFontSize(16);
    m_miniBar->addView(playIcon);

    m_miniBar->registerClickAction([this](...) {
        setExpanded(!m_expanded);
        return true;
    });

    this->addView(m_miniBar);

    // --- Expanded Now Playing View ---
    m_expandedView = new brls::Box();
    m_expandedView->setAxis(brls::Axis::COLUMN);
    m_expandedView->setAlignItems(brls::AlignItems::CENTER);
    m_expandedView->setPadding(32);
    m_expandedView->setGrow(1.0f);
    m_expandedView->setVisibility(brls::Visibility::GONE);

    // Large album art
    m_albumArt = new brls::Image();
    m_albumArt->setSize(brls::Size(256, 256));
    m_albumArt->setCornerRadius(12);
    m_albumArt->setMarginBottom(24);
    m_expandedView->addView(m_albumArt);

    // Track title
    m_trackTitle = new brls::Label();
    m_trackTitle->setFontSize(20);
    m_trackTitle->setMarginBottom(4);
    m_expandedView->addView(m_trackTitle);

    // Track artist
    m_trackArtist = new brls::Label();
    m_trackArtist->setFontSize(16);
    m_trackArtist->setTextColor(nvgRGBA(180, 180, 180, 255));
    m_trackArtist->setMarginBottom(24);
    m_expandedView->addView(m_trackArtist);

    // Time display
    auto* timeRow = new brls::Box();
    timeRow->setAxis(brls::Axis::ROW);
    timeRow->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    timeRow->setWidth(300);

    m_positionLabel = new brls::Label();
    m_positionLabel->setFontSize(12);
    m_positionLabel->setText("0:00");
    timeRow->addView(m_positionLabel);

    m_durationLabel = new brls::Label();
    m_durationLabel->setFontSize(12);
    m_durationLabel->setText("0:00");
    timeRow->addView(m_durationLabel);

    m_expandedView->addView(timeRow);

    // Transport controls
    auto* controls = new brls::Box();
    controls->setAxis(brls::Axis::ROW);
    controls->setJustifyContent(brls::JustifyContent::CENTER);
    controls->setAlignItems(brls::AlignItems::CENTER);
    controls->setMarginTop(16);

    // Shuffle button
    auto* shuffleBtn = new brls::Label();
    shuffleBtn->setText("SHF");
    shuffleBtn->setFontSize(14);
    shuffleBtn->setMarginRight(24);
    shuffleBtn->setFocusable(true);
    shuffleBtn->registerClickAction([](...) {
        std::string playerId = App::instance().getPlayerId();
        if (!playerId.empty()) {
            // Toggle shuffle (we don't track state locally, just send toggle)
            MAClient::instance().queueShuffle(playerId, true);
        }
        return true;
    });
    controls->addView(shuffleBtn);

    // Previous button
    auto* prevBtn = new brls::Label();
    prevBtn->setText("|<");
    prevBtn->setFontSize(20);
    prevBtn->setMarginRight(24);
    prevBtn->setFocusable(true);
    prevBtn->registerClickAction([](...) {
        std::string playerId = App::instance().getPlayerId();
        if (!playerId.empty()) {
            MAClient::instance().queuePrevious(playerId);
        }
        return true;
    });
    controls->addView(prevBtn);

    // Play/Pause button
    auto* playPauseBtn = new brls::Label();
    playPauseBtn->setText(">||");
    playPauseBtn->setFontSize(24);
    playPauseBtn->setMarginRight(24);
    playPauseBtn->setFocusable(true);
    playPauseBtn->registerClickAction([](...) {
        std::string playerId = App::instance().getPlayerId();
        if (!playerId.empty()) {
            Json args;
            args["queue_id"] = Json(playerId);
            MAClient::instance().sendCommand("player_queues/play_pause", args, nullptr);
        }
        return true;
    });
    controls->addView(playPauseBtn);

    // Next button
    auto* nextBtn = new brls::Label();
    nextBtn->setText(">|");
    nextBtn->setFontSize(20);
    nextBtn->setMarginRight(24);
    nextBtn->setFocusable(true);
    nextBtn->registerClickAction([](...) {
        std::string playerId = App::instance().getPlayerId();
        if (!playerId.empty()) {
            MAClient::instance().queueNext(playerId);
        }
        return true;
    });
    controls->addView(nextBtn);

    // Repeat button
    auto* repeatBtn = new brls::Label();
    repeatBtn->setText("RPT");
    repeatBtn->setFontSize(14);
    repeatBtn->setFocusable(true);
    repeatBtn->registerClickAction([](...) {
        std::string playerId = App::instance().getPlayerId();
        if (!playerId.empty()) {
            MAClient::instance().queueRepeat(playerId, "all");
        }
        return true;
    });
    controls->addView(repeatBtn);

    m_expandedView->addView(controls);

    // Volume controls
    auto* volRow = new brls::Box();
    volRow->setAxis(brls::Axis::ROW);
    volRow->setJustifyContent(brls::JustifyContent::CENTER);
    volRow->setAlignItems(brls::AlignItems::CENTER);
    volRow->setMarginTop(24);

    auto* volDown = new brls::Label();
    volDown->setText("VOL-");
    volDown->setFontSize(14);
    volDown->setMarginRight(16);
    volDown->setFocusable(true);
    volDown->registerClickAction([](...) {
        MAClient::instance().playerVolumeDown(App::instance().getPlayerId());
        return true;
    });
    volRow->addView(volDown);

    auto* volLabel = new brls::Label();
    volLabel->setText("Volume");
    volLabel->setFontSize(14);
    volLabel->setMarginRight(16);
    volRow->addView(volLabel);

    auto* volUp = new brls::Label();
    volUp->setText("VOL+");
    volUp->setFontSize(14);
    volUp->setFocusable(true);
    volUp->registerClickAction([](...) {
        MAClient::instance().playerVolumeUp(App::instance().getPlayerId());
        return true;
    });
    volRow->addView(volUp);

    m_expandedView->addView(volRow);

    // Collapse button
    auto* collapseBtn = new brls::Label();
    collapseBtn->setText("Minimize");
    collapseBtn->setFontSize(14);
    collapseBtn->setFocusable(true);
    collapseBtn->setMarginTop(16);
    collapseBtn->registerClickAction([this](...) {
        setExpanded(false);
        return true;
    });
    m_expandedView->addView(collapseBtn);

    this->addView(m_expandedView);
}

void NowPlayingView::updateState(const QueueState& state) {
    // Use current item from queue state
    if (state.currentIndex >= 0 && static_cast<size_t>(state.currentIndex) < state.items.size()) {
        auto& item = state.items[state.currentIndex];
        m_miniTitle->setText(item.name.empty() ? "Not Playing" : item.name);
        m_miniArtist->setText(item.artistName);
        m_trackTitle->setText(item.name);
        m_trackArtist->setText(item.artistName);
    } else {
        m_miniTitle->setText("Not Playing");
        m_miniArtist->setText("");
        m_trackTitle->setText("");
        m_trackArtist->setText("");
    }

    m_positionLabel->setText(formatTime(static_cast<float>(state.elapsed)));
    m_durationLabel->setText(formatTime(static_cast<float>(state.duration)));
}

void NowPlayingView::setExpanded(bool expanded) {
    m_expanded = expanded;
    if (expanded) {
        m_miniBar->setVisibility(brls::Visibility::GONE);
        m_expandedView->setVisibility(brls::Visibility::VISIBLE);
    } else {
        m_miniBar->setVisibility(brls::Visibility::VISIBLE);
        m_expandedView->setVisibility(brls::Visibility::GONE);
    }
}

std::string NowPlayingView::formatTime(float seconds) {
    int totalSecs = static_cast<int>(seconds);
    int mins = totalSecs / 60;
    int secs = totalSecs % 60;
    char buf[16];
    snprintf(buf, sizeof(buf), "%d:%02d", mins, secs);
    return buf;
}

brls::View* NowPlayingView::create() {
    return new NowPlayingView();
}

} // namespace vita_ma
