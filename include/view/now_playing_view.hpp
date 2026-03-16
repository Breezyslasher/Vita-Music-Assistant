#pragma once

#include "app.h"
#include "app/ma_types.hpp"
#include <borealis.hpp>

namespace vita_ma {

class NowPlayingView : public brls::Box {
public:
    NowPlayingView();
    ~NowPlayingView() override = default;

    void updateState(const QueueState& state);
    void setExpanded(bool expanded);
    bool isExpanded() const { return m_expanded; }

    static brls::View* create();

private:
    // Mini player bar
    brls::Box* m_miniBar = nullptr;
    brls::Image* m_miniArt = nullptr;
    brls::Label* m_miniTitle = nullptr;
    brls::Label* m_miniArtist = nullptr;

    // Expanded view
    brls::Box* m_expandedView = nullptr;
    brls::Image* m_albumArt = nullptr;
    brls::Label* m_trackTitle = nullptr;
    brls::Label* m_trackArtist = nullptr;
    brls::Label* m_positionLabel = nullptr;
    brls::Label* m_durationLabel = nullptr;

    bool m_expanded = false;

    std::string formatTime(float seconds);
};

} // namespace vita_ma
