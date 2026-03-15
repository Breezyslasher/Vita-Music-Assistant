#include "activity/player_activity.hpp"
#include "app.h"
#include "app/ma_client.hpp"
#include "player/mpv_player.hpp"
#include "view/now_playing_view.hpp"
#include <borealis.hpp>

namespace vita_ma {

PlayerActivity::PlayerActivity() {}

brls::View* PlayerActivity::createContentView() {
    // Full-screen now playing view
    auto* nowPlaying = new NowPlayingView();
    nowPlaying->setExpanded(true);
    nowPlaying->setGrow(1.0f);

    // Update with current state
    auto state = App::instance().getQueueState();
    nowPlaying->updateState(state);

    return nowPlaying;
}

void PlayerActivity::updateUI() {
    // Called periodically to update playback state
    auto& mpv = MpvPlayer::instance();
    mpv.processEvents();
}

} // namespace vita_ma
