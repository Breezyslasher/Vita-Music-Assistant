#pragma once

#include <borealis.hpp>
#include "view/now_playing_view.hpp"

namespace vita_ma {

class MainActivity : public brls::Activity {
public:
    MainActivity();

    brls::View* createContentView() override;

    void onEvent(MAEvent event, const Json& data);

private:
    NowPlayingView* m_nowPlaying = nullptr;

    void connectToServer();
    void setupEventHandlers();
};

} // namespace vita_ma
