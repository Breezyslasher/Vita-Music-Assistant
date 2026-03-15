#include "activity/main_activity.hpp"
#include "app.h"
#include "app/ma_client.hpp"
#include "app/application.hpp"
#include "player/mpv_player.hpp"
#include "view/home_tab.hpp"
#include "view/library_tab.hpp"
#include "view/music_tab.hpp"
#include "view/search_tab.hpp"
#include "view/queue_tab.hpp"
#include "view/settings_tab.hpp"
#include "view/now_playing_view.hpp"
#include <borealis.hpp>

namespace vita_ma {

MainActivity::MainActivity() {}

brls::View* MainActivity::createContentView() {
    auto* root = new brls::Box();
    root->setAxis(brls::Axis::COLUMN);
    root->setGrow(1.0f);

    // Main content area with tab bar
    auto* tabFrame = new brls::TabFrame();
    tabFrame->setGrow(1.0f);

    // Create tabs
    auto* homeTab = new HomeTab();
    auto* libraryTab = new LibraryTab();
    auto* browseTab = new MusicTab();
    auto* searchTab = new SearchTab();
    auto* queueTab = new QueueTab();
    auto* settingsTab = new SettingsTab();

    tabFrame->addTab("Home", homeTab);
    tabFrame->addTab("Library", libraryTab);
    tabFrame->addTab("Browse", browseTab);
    tabFrame->addTab("Search", searchTab);
    tabFrame->addTab("Queue", queueTab);
    tabFrame->addTab("Settings", settingsTab);

    root->addView(tabFrame);

    // Now playing bar at bottom
    m_nowPlaying = new NowPlayingView();
    m_nowPlaying->setHeight(56);
    root->addView(m_nowPlaying);

    // Connect and set up event handlers
    connectToServer();
    setupEventHandlers();

    // Initialize MPV for local playback
    MpvPlayer::instance().init();

    return root;
}

void MainActivity::connectToServer() {
    auto& app = App::instance();
    if (!app.getServerUrl().empty()) {
        if (!MAClient::instance().isConnected()) {
            MAClient::instance().connect(app.getServerUrl(), app.getAuthToken());
        }
    }
}

void MainActivity::setupEventHandlers() {
    MAClient::instance().setEventCallback([this](MAEvent event, const Json& data) {
        onEvent(event, data);
    });
}

void MainActivity::onEvent(MAEvent event, const Json& data) {
    auto& app = App::instance();

    switch (event) {
        case MAEvent::CONNECTED:
            app.setConnected(true);
            app.setServerInfo(MAClient::instance().getServerInfo());
            brls::Logger::info("Main: connected to server");
            break;

        case MAEvent::DISCONNECTED:
            app.setConnected(false);
            brls::Logger::info("Main: disconnected from server");
            break;

        case MAEvent::QUEUE_UPDATED: {
            // Update queue state from event data
            QueueState state = app.getQueueState();
            if (data.has("queue_id")) state.queue_id = data["queue_id"].str();
            if (data.has("state")) {
                std::string s = data["state"].str();
                if (s == "playing") state.state = PlayerState::PLAYING;
                else if (s == "paused") state.state = PlayerState::PAUSED;
                else if (s == "idle") state.state = PlayerState::IDLE;
            }
            if (data.has("shuffle_enabled")) state.shuffle_enabled = data["shuffle_enabled"].boolVal();
            if (data.has("repeat_mode")) {
                std::string rm = data["repeat_mode"].str();
                if (rm == "off") state.repeat_mode = RepeatMode::OFF;
                else if (rm == "all") state.repeat_mode = RepeatMode::ALL;
                else if (rm == "one") state.repeat_mode = RepeatMode::ONE;
            }
            if (data.has("current_item")) {
                auto& ci = data["current_item"];
                if (ci.has("name")) state.current_track_name = ci["name"].str();
                if (ci.has("media_item")) {
                    auto& mi = ci["media_item"];
                    if (mi.has("artists") && mi["artists"].size() > 0) {
                        state.current_track_artist = mi["artists"][static_cast<size_t>(0)]["name"].str();
                    }
                    if (mi.has("image")) {
                        state.current_track_image = mi["image"]["url"].str();
                    }
                }
                if (ci.has("duration")) state.duration = static_cast<float>(ci["duration"].numVal());
            }

            app.setQueueState(state);
            if (m_nowPlaying) m_nowPlaying->updateState(state);
            break;
        }

        case MAEvent::QUEUE_TIME_UPDATED: {
            QueueState state = app.getQueueState();
            if (data.has("elapsed_time")) {
                state.elapsed_time = static_cast<float>(data["elapsed_time"].numVal());
            }
            app.setQueueState(state);
            if (m_nowPlaying) m_nowPlaying->updateState(state);
            break;
        }

        case MAEvent::PLAYER_UPDATED: {
            QueueState state = app.getQueueState();
            if (data.has("volume_level")) {
                state.volume = data["volume_level"].intVal();
            }
            if (data.has("volume_muted")) {
                state.muted = data["volume_muted"].boolVal();
            }
            if (data.has("player_id")) {
                app.setPlayerId(data["player_id"].str());
            }
            app.setQueueState(state);
            break;
        }

        default:
            break;
    }
}

} // namespace vita_ma
