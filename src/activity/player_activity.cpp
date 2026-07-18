/**
 * Vita Music Assistant - Player Activity implementation
 */

#include "activity/player_activity.hpp"
#include "app/application.hpp"
#include "app/ma_client.hpp"
#include "app/ma_types.hpp"
#include "app/music_queue.hpp"
#include "player/mpv_player.hpp"
#include "utils/image_loader.hpp"
#include "utils/http_client.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <sys/stat.h>

#ifdef __vita__
#include <psp2/power.h>
#endif

namespace vita_ma {

// Base temp file path for streaming audio (MPV's HTTP handling crashes on Vita)
// Extension will be added dynamically based on the actual file type


PlayerActivity::PlayerActivity(const std::string& mediaKey)
    : m_mediaKey(mediaKey), m_isLocalFile(false) {
    brls::Logger::debug("PlayerActivity created for media: {}", mediaKey);
}


PlayerActivity* PlayerActivity::createForStream(const std::string& streamUrl, const std::string& title) {
    PlayerActivity* activity = new PlayerActivity("");
    activity->m_isDirectFile = true;  // Use direct file path for stream URLs too
    activity->m_directFilePath = streamUrl;
    activity->m_streamTitle = title;
    brls::Logger::info("PlayerActivity created for stream: {} ({})", title, streamUrl);
    return activity;
}

PlayerActivity* PlayerActivity::createWithQueue(const std::vector<MusicItem>& tracks, int startIndex) {
    PlayerActivity* activity = new PlayerActivity("");
    activity->m_isQueueMode = true;

    MusicQueue& queue = MusicQueue::getInstance();

    // Set up client-side queue
    // TODO: Integrate with Music Assistant server-side queue via playMedia API
    queue.setQueue(tracks, startIndex);

    // Set up track ended callback
    queue.setTrackEndedCallback([activity](const QueueItem* nextTrack) {
        activity->onTrackEnded(nextTrack);
    });

    brls::Logger::info("PlayerActivity created with queue of {} tracks, starting at {}",
                      tracks.size(), startIndex);
    return activity;
}

PlayerActivity* PlayerActivity::createResumeQueue() {
    PlayerActivity* activity = new PlayerActivity("");
    activity->m_isQueueMode = true;
    activity->m_isResuming = true;  // Don't restart playback

    // Resume existing queue - don't reset it
    MusicQueue& queue = MusicQueue::getInstance();

    // Set up track ended callback for the new activity
    queue.setTrackEndedCallback([activity](const QueueItem* nextTrack) {
        activity->onTrackEnded(nextTrack);
    });

    brls::Logger::info("PlayerActivity resumed existing queue at index {}", queue.getCurrentIndex());
    return activity;
}

brls::View* PlayerActivity::createContentView() {
    return brls::View::createFromXMLResource("activity/player.xml");
}

void PlayerActivity::onContentAvailable() {
    brls::Logger::debug("PlayerActivity content available");

    // The up-next list scrolls CENTERED so D-pad Up/Down always change focus and
    // scroll immediately. The default NATURAL only moves focus once the next row
    // is already fully on screen, so UP/DOWN couldn't reach an off-screen track.
    // CENTERED clamps at the ends, so the first/last tracks still sit flush.
    if (queueScroll)
        queueScroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);

#ifdef __vita__
    // Boost CPU/GPU clocks to max for smooth media playback
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
#endif

    // Cancel pending background thumbnail loads (HomeTab, MediaDetailView)
    // to free up network bandwidth for media streaming.
    // We don't setPaused(true) here yet because music queue mode needs to
    // load album art first. setPaused is called later in loadMedia/loadFromQueue
    // right before MPV starts streaming.
    ImageLoader::cancelAll();

    // If music is currently playing in the background and we're starting
    // a non-queue playback (video/episode), stop the music first.
    // Send a "stopped" timeline so the server clears the music session.
    if (!m_isQueueMode) {
        MusicQueue& existingQueue = MusicQueue::getInstance();
        if (!existingQueue.isEmpty()) {
            brls::Logger::info("PlayerActivity: Stopping background music for new playback");
            MpvPlayer::getInstance().stop();
            existingQueue.clear();
        }
    }

    // Load media details
    if (m_isQueueMode) {
        loadFromQueue();
    } else {
        loadMedia();
    }

    // Set up controls
    if (progressSlider) {
        progressSlider->setProgress(0.0f);
        progressSlider->getProgressEvent()->subscribe([this](float progress) {
            // Skip if this is a programmatic update (not user interaction)
            if (m_updatingSlider) return;
            resetControlsIdleTimer();
            // Seek to position
            MpvPlayer& player = MpvPlayer::getInstance();
            double duration = 0.0;
            // For music queue mode, prefer queue metadata duration (full track length)
            // over MPV duration which may only reflect buffered/demuxed portion
            if (m_isQueueMode) {
                const QueueItem* track = MusicQueue::getInstance().getCurrentTrack();
                if (track && track->duration > 0)
                    duration = (double)track->duration;
            }
            if (duration <= 0)
                duration = player.getDuration();
            player.seekTo(duration * progress);
        });
    }

    // Register tap gesture on container to toggle controls (like Suwayomi reader)
    if (playerContainer) {
        playerContainer->addGestureRecognizer(new brls::TapGestureRecognizer(
            [this](brls::TapGestureStatus status, brls::Sound* soundToPlay) {
                if (status.state == brls::GestureState::END) {
                    resetControlsIdleTimer();
                    toggleControls();
                }
            }));
    }

    // Add horizontal swipe gesture on album art area for prev/next track (music mode)
    if (albumArtContainer) {
        albumArtContainer->addGestureRecognizer(new brls::PanGestureRecognizer(
            [this](brls::PanGestureStatus status, brls::Sound* soundToPlay) {
                if (!m_isQueueMode) return;
                if (status.state == brls::GestureState::END) {
                    float deltaX = status.position.x - status.startPosition.x;
                    float threshold = 60.0f; // Minimum swipe distance
                    if (deltaX > threshold) {
                        // Swipe right = previous track
                        playPrevious();
                    } else if (deltaX < -threshold) {
                        // Swipe left = next track
                        playNext();
                    }
                }
            }, brls::PanAxis::HORIZONTAL));
    }

    // Register controller actions
    this->registerAction("Play/Pause", brls::ControllerButton::BUTTON_A, [this](brls::View* view) {
        resetControlsIdleTimer();
        togglePlayPause();
        return true;
    });

    this->registerAction("Back", brls::ControllerButton::BUTTON_B, [this](brls::View* view) {
        resetControlsIdleTimer();
        // If queue overlay is showing, dismiss it instead of leaving player
        if (m_queueOverlayVisible) {
            hideQueueOverlay();
            return true;
        }
        // In music mode with background music enabled, leave without stopping
        if (m_isQueueMode && Application::getInstance().getSettings().backgroundMusic) {
            m_destroying = false;  // Don't mark as destroying - music continues
            brls::Application::popActivity();
            return true;
        }
        brls::Application::popActivity();
        return true;
    });

    // Toggle controls with Y and Start (like Suwayomi reader)
    this->registerAction("Toggle Controls", brls::ControllerButton::BUTTON_START, [this](brls::View* view) {
        toggleControls();
        return true;
    });

    // Queue controls for music (LB/RB for previous/next, triggers for shuffle/repeat)
    if (m_isQueueMode) {
        this->registerAction("Previous", brls::ControllerButton::BUTTON_LB, [this](brls::View* view) {
            playPrevious();
            return true;
        });

        this->registerAction("Next", brls::ControllerButton::BUTTON_RB, [this](brls::View* view) {
            playNext();
            return true;
        });

        this->registerAction("Shuffle", brls::ControllerButton::BUTTON_X, [this](brls::View* view) {
            if (!m_controlsVisible) {
                togglePlayPause();
            } else {
                toggleShuffle();
            }
            return true;
        });

        this->registerAction("Repeat", brls::ControllerButton::BUTTON_Y, [this](brls::View* view) {
            toggleRepeat();
            return true;
        });
    } else {
        // Standard seek for non-queue playback
        this->registerAction("Rewind", brls::ControllerButton::BUTTON_LB, [this](brls::View* view) {
            resetControlsIdleTimer();
            int interval = Application::getInstance().getSettings().seekInterval;
            seek(-interval);
            return true;
        });

        this->registerAction("Forward", brls::ControllerButton::BUTTON_RB, [this](brls::View* view) {
            resetControlsIdleTimer();
            int interval = Application::getInstance().getSettings().seekInterval;
            seek(interval);
            return true;
        });
    }

    // Queue side sheet: tapping the scrim (the area left of the sheet)
    // dismisses it; taps inside the sheet reach the rows.
    if (queueScrim) {
        queueScrim->addGestureRecognizer(new brls::TapGestureRecognizer(
            [this](brls::TapGestureStatus status, brls::Sound* soundToPlay) {
                if (status.state == brls::GestureState::END) {
                    hideQueueOverlay();
                }
            }));
    }

    // Show mode-specific icons and wire touch
    if (m_isQueueMode) {
        // Show music-specific UI elements
        if (musicInfo) musicInfo->setVisibility(brls::Visibility::VISIBLE);
        if (musicTransport) musicTransport->setVisibility(brls::Visibility::VISIBLE);

        // Wire music transport buttons
        if (musicPlayBtn) {
            musicPlayBtn->registerClickAction([this](brls::View* view) {
                togglePlayPause();
                return true;
            });
            musicPlayBtn->addGestureRecognizer(new brls::TapGestureRecognizer(musicPlayBtn));
        }
        if (musicPrevBtn) {
            musicPrevBtn->registerClickAction([this](brls::View* view) {
                playPrevious();
                return true;
            });
            musicPrevBtn->addGestureRecognizer(new brls::TapGestureRecognizer(musicPrevBtn));
        }
        if (musicNextBtn) {
            musicNextBtn->registerClickAction([this](brls::View* view) {
                playNext();
                return true;
            });
            musicNextBtn->addGestureRecognizer(new brls::TapGestureRecognizer(musicNextBtn));
        }

        // Shuffle toggle button
        if (shuffleBtn) {
            shuffleBtn->registerClickAction([this](brls::View* view) {
                toggleShuffle();
                return true;
            });
            shuffleBtn->addGestureRecognizer(new brls::TapGestureRecognizer(shuffleBtn));
            // Set initial icon based on current shuffle state
            updateShuffleIcon();
        }

        // Repeat toggle button
        if (repeatBtn) {
            repeatBtn->registerClickAction([this](brls::View* view) {
                toggleRepeat();
                return true;
            });
            repeatBtn->addGestureRecognizer(new brls::TapGestureRecognizer(repeatBtn));
            // Set initial icon based on current repeat state
            updateRepeatIcon();
        }

        if (queueBtn) {
            queueBtn->setVisibility(brls::Visibility::VISIBLE);
            queueBtn->registerClickAction([this](brls::View* view) {
                if (m_queueOverlayVisible) {
                    hideQueueOverlay();
                } else {
                    showQueueOverlay();
                }
                return true;
            });
            queueBtn->addGestureRecognizer(new brls::TapGestureRecognizer(queueBtn));
        }

        // Queue side-sheet "Clear" control (wired once; lives in the hidden overlay)
        if (queueClearBtn) {
            queueClearBtn->registerClickAction([this](brls::View* view) {
                clearUpcoming();
                return true;
            });
            queueClearBtn->addGestureRecognizer(new brls::TapGestureRecognizer(queueClearBtn));
            // Clear is the top of the sheet's focusable content; route UP back to
            // itself so it can't escape to the player's queue button behind the
            // overlay (which is what happens with the default upward traversal).
            queueClearBtn->setCustomNavigationRoute(brls::FocusDirection::UP, queueClearBtn);
        }

        // Music mode: controls never auto-hide, always visible
        // Override the controls auto-hide for music
        if (controlsBox) {
            controlsBox->setVisibility(brls::Visibility::VISIBLE);
            controlsBox->setAlpha(1.0f);
        }

        // Hide title/artist from bottom controls (shown in musicInfo instead)
        if (titleLabel) titleLabel->setVisibility(brls::Visibility::GONE);
        if (artistLabel) artistLabel->setVisibility(brls::Visibility::GONE);
    }

    if (queueBtn) queueBtn->setCustomNavigationRoute(brls::FocusDirection::DOWN, queueBtn);

    // Player switcher button - allows selecting which player to control
    if (playerSwitchBtn) {
        playerSwitchBtn->setVisibility(brls::Visibility::VISIBLE);
        playerSwitchBtn->setHideHighlightBackground(true);
        playerSwitchBtn->registerClickAction([this](brls::View* view) {
            showPlayerSwitcher();
            return true;
        });
        playerSwitchBtn->addGestureRecognizer(new brls::TapGestureRecognizer(playerSwitchBtn));
    }

    // Initialize player name label and fetch player list for name resolution
    {
        auto& client = MAClient::instance();
        if (client.isConnected()) {
            auto aliveWeak = std::weak_ptr<std::atomic<bool>>(m_alive);
            client.getPlayers([this, aliveWeak](bool success, const Json& result) {
                if (!success || result.type() != Json::ARRAY) return;
                std::vector<PlayerInfo> players;
                for (size_t i = 0; i < result.size(); i++) {
                    const Json& p = result[i];
                    PlayerInfo info;
                    if (p.has("player_id")) info.playerId = p["player_id"].str();
                    if (p.has("name")) info.name = p["name"].str();
                    if (p.has("type")) info.type = p["type"].str();
                    if (p.has("available")) info.available = p["available"].boolVal();
                    if (!info.playerId.empty()) players.push_back(info);
                }
                brls::sync([this, players, aliveWeak]() {
                    auto alive = aliveWeak.lock();
                    if (!alive || !alive->load()) return;
                    m_availablePlayers = players;
                    m_ownPlayerId = findOwnPlayerId(players);

                    // Adopt our own registered Sendspin player as the active
                    // player when nothing is explicitly selected. This routes
                    // local Vita playback through MA's real player_id (instead
                    // of the raw client id) so transport commands, the current
                    // track, and cover art all resolve correctly - and it keeps
                    // us from showing a broken duplicate "This Vita" entry.
                    auto& settings = Application::getInstance().getSettings();
                    if (settings.selectedPlayerId.empty() && !m_ownPlayerId.empty()) {
                        settings.selectedPlayerId = m_ownPlayerId;
                        Application::getInstance().saveSettings();
                        loadRemotePlayerState();
                    }
                    updatePlayerNameLabel();
                });
            });
        }
        // Set initial label even before server responds
        updatePlayerNameLabel();
    }

    // Wire up MA server events so remote player state updates in real-time
    {
        auto aliveWeak = std::weak_ptr<std::atomic<bool>>(m_alive);
        MAClient::instance().setEventCallback([this, aliveWeak](MAEvent event, const Json& data) {
            auto alive = aliveWeak.lock();
            if (!alive || !alive->load()) return;
            if (event == MAEvent::PLAYER_UPDATED) {
                brls::sync([this, data, aliveWeak]() {
                    auto a = aliveWeak.lock();
                    if (!a || !a->load()) return;
                    onRemotePlayerEvent(data);
                });
            } else if (event == MAEvent::QUEUE_UPDATED || event == MAEvent::QUEUE_ITEMS_UPDATED) {
                brls::sync([this, data, aliveWeak]() {
                    auto a = aliveWeak.lock();
                    if (!a || !a->load()) return;
                    onRemoteQueueEvent(data);
                });
            }
        });
    }

    // Start update timer
    m_updateTimer.setCallback([this]() {
        updateProgress();
    });
    m_updateTimer.start(1000); // Update every second

    // Start with controls hidden if auto-hide is enabled
    int autoHide = Application::getInstance().getSettings().controlsAutoHideSeconds;
    if (autoHide > 0 && !m_isQueueMode) {
        hideControls();
    }
}

void PlayerActivity::willDisappear(bool resetState) {
    brls::Activity::willDisappear(resetState);

    // Clear event callback to prevent use-after-free
    MAClient::instance().setEventCallback(nullptr);

    // Re-enable background thumbnail loading now that playback is ending
    ImageLoader::setPaused(false);

#ifdef __vita__
    // Restore reduced clock speeds for browsing (saves battery)
    scePowerSetArmClockFrequency(333);
    scePowerSetBusClockFrequency(166);
    scePowerSetGpuClockFrequency(166);
    scePowerSetGpuXbarClockFrequency(111);
#endif

    // If background music is enabled and we're in queue mode, don't stop playback
    if (m_isQueueMode && Application::getInstance().getSettings().backgroundMusic && !m_destroying) {
        brls::Logger::info("PlayerActivity: Leaving with background music enabled, not stopping");
        m_updateTimer.stop();
        if (m_alive) m_alive->store(false);
        return;
    }

    // Mark as destroying to prevent timer, image loader, and batch callbacks
    m_destroying = true;
    m_queueBatchActive = false;
    if (m_alive) {
        m_alive->store(false);
    }

    // Stop update timer first
    m_updateTimer.stop();

    // Clear any pending deferred init (user backed out before timer fired)
    m_pendingPlayUrl.clear();
    m_pendingPlayTitle.clear();

    // Stop playback and save progress
    MpvPlayer& player = MpvPlayer::getInstance();

    // Only try to save progress if player is in a valid state
    if (player.isInitialized() && (player.isPlaying() || player.isPaused())) {
        double position = player.getPosition();
        double duration = 0.0;

        // For music queue mode, prefer queue metadata duration (full track length)
        // over MPV duration which may only reflect buffered/demuxed portion
        if (m_isQueueMode) {
            const QueueItem* track = MusicQueue::getInstance().getCurrentTrack();
            if (track && track->duration > 0) {
                duration = (double)track->duration;
            }
        }
        if (duration <= 0)
            duration = player.getDuration();

        if (position > 0) {
            brls::Logger::info("PlayerActivity: Stopped at position {:.1f}s", position);
        }
    }

    // Save queue state
    if (m_isQueueMode) {
        MusicQueue::getInstance().saveState();
    }

    // Stop playback (safe to call even if not playing)
    if (player.isInitialized()) {
        player.stop();
    }

    m_isPlaying = false;
}

void PlayerActivity::loadFromQueue() {
    // Prevent rapid re-entry
    if (m_loadingMedia) {
        brls::Logger::debug("PlayerActivity: Already loading media, skipping");
        return;
    }
    m_loadingMedia = true;

    MusicQueue& queue = MusicQueue::getInstance();
    const QueueItem* track = queue.getCurrentTrack();

    if (!track) {
        brls::Logger::info("PlayerActivity: No current track in queue, showing empty player");
        m_loadingMedia = false;
        m_isResuming = false;

        // Show the player UI with no track info — user can use the player switcher
        if (musicTitleLabel) musicTitleLabel->setText("No track playing");
        if (musicArtistLabel) musicArtistLabel->setText("");
        if (titleLabel) titleLabel->setText("No track playing");
        if (musicInfo) musicInfo->setVisibility(brls::Visibility::VISIBLE);
        if (musicTransport) musicTransport->setVisibility(brls::Visibility::VISIBLE);

        // Ensure controls are visible and properly laid out
        if (controlsBox) {
            controlsBox->setVisibility(brls::Visibility::VISIBLE);
            controlsBox->setAlpha(1.0f);
        }

        // Hide the progress bar and time labels since nothing is playing
        if (progressSlider) progressSlider->setVisibility(brls::Visibility::GONE);
        if (timeElapsedLabel) timeElapsedLabel->setVisibility(brls::Visibility::GONE);
        if (timeRemainingLabel) timeRemainingLabel->setVisibility(brls::Visibility::GONE);

        // Hide title/artist from bottom controls (shown in musicInfo instead)
        if (titleLabel) titleLabel->setVisibility(brls::Visibility::GONE);
        if (artistLabel) artistLabel->setVisibility(brls::Visibility::GONE);

        // Hide queue info since there's no queue
        if (queueLabel) queueLabel->setVisibility(brls::Visibility::GONE);

        // Check if a remote player is selected — load its state
        const auto& selectedId = Application::getInstance().getSettings().selectedPlayerId;
        if (!selectedId.empty()) {
            loadRemotePlayerState();
        }
        return;
    }

    brls::Logger::info("PlayerActivity: Loading track from queue: {} - {}",
                      track->artist, track->title);

    // If resuming and MPV is already playing/paused, just update the UI
    // without restarting the track (user pressed circle to return to player)
    MpvPlayer& resumePlayer = MpvPlayer::getInstance();
    if (m_isResuming && resumePlayer.isInitialized() &&
        (resumePlayer.isPlaying() || resumePlayer.isPaused())) {
        brls::Logger::info("PlayerActivity: Resuming existing playback, skipping reload");
        m_isPlaying = resumePlayer.isPlaying();
        m_mediaKey = track->ratingKey;
        m_isResuming = false;

        // Update display labels and album art, then return without reloading
        if (musicTitleLabel) musicTitleLabel->setText(track->title);
        if (musicArtistLabel) musicArtistLabel->setText(track->artist);
        if (titleLabel) titleLabel->setText(track->title);
        if (artistLabel) {
            artistLabel->setText(track->artist);
            artistLabel->setVisibility(track->artist.empty()
                ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
        }
        updateQueueDisplay();

        // Load album art from server
        if (albumArt && !track->ratingKey.empty()) {
            if (!track->thumb.empty()) {
                MAClient& client = MAClient::instance();
                std::string thumbUrl = client.getThumbnailUrl(track->thumb, 300, 300, track->thumbProvider);
                ImageLoader::setPaused(false);
                ImageLoader::loadAsync(thumbUrl, [](brls::Image* img) {
                    img->setVisibility(brls::Visibility::VISIBLE);
                }, albumArt, m_alive);
                ImageLoader::setPaused(true);
            }
        }

        // Show music UI elements
        if (musicInfo) musicInfo->setVisibility(brls::Visibility::VISIBLE);
        if (musicTransport) musicTransport->setVisibility(brls::Visibility::VISIBLE);

        updatePlayPauseLabel();
        m_loadingMedia = false;
        return;
    }

    // Update display - use music info labels (between cover and play controls)
    if (musicTitleLabel) {
        musicTitleLabel->setText(track->title);
    }
    if (musicArtistLabel) {
        musicArtistLabel->setText(track->artist);
    }
    // Also update bottom controls title for non-music fallback
    if (titleLabel) {
        titleLabel->setText(track->title);
    }
    if (artistLabel) {
        artistLabel->setText(track->artist);
        // Only show the artist label if there's actually text to display
        artistLabel->setVisibility(track->artist.empty()
            ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
    }

    // Update queue info display
    updateQueueDisplay();

    // Use the rating key to get the playback URL
    m_mediaKey = track->ratingKey;

    // Pause image loading and invalidate stale in-flight loads from previous
    // pages before queuing any new loads for this track.
    ImageLoader::setPaused(true);
    ImageLoader::cancelAll();

    m_isLocalFile = false;
    MAClient& client = MAClient::instance();

    // Load album art from server
    if (albumArt && !track->thumb.empty()) {
        std::string thumbUrl = client.getThumbnailUrl(track->thumb, 300, 300, track->thumbProvider);
        ImageLoader::setPaused(false);
        ImageLoader::loadAsync(thumbUrl, [](brls::Image* img) {
            img->setVisibility(brls::Visibility::VISIBLE);
        }, albumArt, m_alive);
        ImageLoader::setPaused(true);
        albumArt->setVisibility(brls::Visibility::VISIBLE);
    }

    // Use player_queues/play_media to play the track via Sendspin.
    // The Sendspin client handles receiving audio from MA server and
    // feeding it to MPV via an audio pipe.
    // Use the selected player ID from settings if set, otherwise use local Vita player
    const auto& settings = Application::getInstance().getSettings();
    std::string playerId = settings.selectedPlayerId.empty()
        ? App::instance().getPlayerId()
        : settings.selectedPlayerId;
    std::string trackUri = track->uri;
    std::string trackTitle = track->title;
    auto alive = m_alive;

    if (playerId.empty()) {
        brls::Logger::error("No player registered - Sendspin not connected");
        m_loadingMedia = false;
        return;
    }

    if (trackUri.empty()) {
        // Fallback: construct URI from item_id
        trackUri = "library://track/" + track->ratingKey;
    }

    brls::Logger::info("Playing '{}' via player_queues/play_media (player={})",
                       trackTitle, playerId);

    client.playMedia(playerId, trackUri, "play",
        [this, trackTitle, alive](bool success, const Json& result) {
        if (!alive->load()) return;

        if (!success) {
            brls::Logger::error("Failed to play track '{}': {}",
                trackTitle, result.has("details") ? result["details"].str() : "unknown error");
            brls::sync([this, alive]() {
                if (!alive->load()) return;
                m_loadingMedia = false;
            });
            return;
        }

        brls::Logger::info("Track '{}' queued for playback via Sendspin", trackTitle);
        // Audio will arrive via Sendspin binary frames and be played by MPV
        // through the audio pipe. No need to call loadFromQueueWithUrl.
        brls::sync([this, alive]() {
            if (!alive->load()) return;
            m_loadingMedia = false;
        });
    });
}

void PlayerActivity::loadFromQueueWithUrl(const std::string& url, const std::string& trackTitle) {
    MpvPlayer& player = MpvPlayer::getInstance();

    // Set audio-only mode BEFORE initializing
    player.setAudioOnly(true);

    // Only clear image cache on first MPV init to free memory for the player.
    // On subsequent track changes MPV is already allocated, and clearing the
    // cache forces covers/queue thumbnails to be re-downloaded from the server.
    if (!player.isInitialized()) {
        ImageLoader::clearCache();
    }

    // Stream audio directly via MPV (transcode API returns mp3 stream or local file)
    if (!player.isInitialized()) {
        // Defer MPV init + load to after activity transition completes
        m_pendingPlayUrl = url;
        m_pendingPlayTitle = trackTitle;
        m_pendingIsAudio = true;
        m_isPlaying = true;
        m_loadingMedia = false;
        return;
    }

    // Player already initialized (track change) - load immediately
    if (!player.loadUrl(url, trackTitle)) {
        brls::Logger::error("Failed to load URL: {}", url);
        m_loadingMedia = false;
        return;
    }

    m_isPlaying = true;
    m_loadingMedia = false;
}

void PlayerActivity::loadMedia() {
    // Prevent rapid re-entry
    if (m_loadingMedia) {
        brls::Logger::debug("PlayerActivity: Already loading media, skipping");
        return;
    }
    m_loadingMedia = true;

    // Handle direct file playback (debug/testing)
    if (m_isDirectFile) {
        brls::Logger::info("PlayerActivity: Playing direct file: {}", m_directFilePath);

        // Use stream title if set, otherwise extract filename from path
        std::string displayTitle;
        if (!m_streamTitle.empty()) {
            displayTitle = m_streamTitle;
        } else {
            size_t lastSlash = m_directFilePath.find_last_of("/\\");
            displayTitle = (lastSlash != std::string::npos)
                ? m_directFilePath.substr(lastSlash + 1)
                : m_directFilePath;
        }

        if (titleLabel) {
            titleLabel->setText(displayTitle);
        }

        // Detect if this is an audio file
        std::string lowerPath = m_directFilePath;
        for (auto& c : lowerPath) c = tolower(c);
        bool isAudioFile = (lowerPath.find(".mp3") != std::string::npos ||
                           lowerPath.find(".m4a") != std::string::npos ||
                           lowerPath.find(".aac") != std::string::npos ||
                           lowerPath.find(".flac") != std::string::npos ||
                           lowerPath.find(".ogg") != std::string::npos ||
                           lowerPath.find(".wav") != std::string::npos ||
                           lowerPath.find(".wma") != std::string::npos);

        brls::Logger::info("PlayerActivity: File type detection - audio: {}", isAudioFile);

        // Pause image loading and free cache to reclaim memory for MPV
        ImageLoader::setPaused(true);
        ImageLoader::cancelAll();
        ImageLoader::clearCache();

        MpvPlayer& player = MpvPlayer::getInstance();

        // Set audio-only mode BEFORE initializing (to skip render context)
        player.setAudioOnly(isAudioFile);

        if (!player.isInitialized()) {
            // Defer MPV init + load to after activity transition completes.
            // initRenderContext() creates GXM resources and loadUrl() spawns
            // decoder threads that use the shared GXM context - both conflict
            // with NanoVG drawing during the borealis show phase.
            m_pendingPlayUrl = m_directFilePath;
            m_pendingPlayTitle = m_streamTitle.empty() ? "Test File" : m_streamTitle;
            m_pendingIsAudio = isAudioFile;
            m_loadingMedia = false;
            return;
        }

        // Player already initialized - load immediately
        std::string loadTitle = m_streamTitle.empty() ? "Test File" : m_streamTitle;
        if (!player.loadUrl(m_directFilePath, loadTitle)) {
            brls::Logger::error("Failed to load direct file: {}", m_directFilePath);
            m_loadingMedia = false;
            return;
        }

        m_isPlaying = true;
        m_loadingMedia = false;
        return;
    }

    // Remote playback from server - fetch track details and play via queue
    m_mediaType = MediaType::TRACK;

    if (titleLabel) {
        titleLabel->setText("Loading...");
    }

    brls::Logger::info("PlayerActivity: Loading single track from server: {}", m_mediaKey);

    // Fetch track details from MA API, then create a single-item queue
    auto alive = m_alive;
    std::string trackId = m_mediaKey;

    MAClient::instance().getTrack(trackId,
        [this, trackId, alive](bool success, const Json& result) {
            if (!alive->load()) return;

            if (!success || result.type() != Json::OBJECT) {
                brls::Logger::error("PlayerActivity: Failed to fetch track details for: {}", trackId);
                brls::sync([this, alive]() {
                    if (!alive->load()) return;
                    m_loadingMedia = false;
                    if (titleLabel) titleLabel->setText("Failed to load track");
                });
                return;
            }

            // Build a MusicItem from the track response
            MusicItem item;
            item.itemId    = result.has("item_id")    ? result["item_id"].str()    : trackId;
            item.name      = result.has("name")       ? result["name"].str()       : "Unknown";
            item.uri       = result.has("uri")        ? result["uri"].str()        : "";
            item.provider  = result.has("provider")   ? result["provider"].str()   : "library";

            // Extract image URL: try image object, then metadata.images array
            if (result.has("image") && result["image"].type() == Json::OBJECT && result["image"].has("path")) {
                item.imageUrl = MAClient::imageRefFromJson(result["image"]);
                if (result["image"].has("provider")) item.imageProvider = result["image"]["provider"].str();
            } else if (result.has("image") && result["image"].type() == Json::STRING) {
                item.imageUrl = result["image"].str();
            } else if (result.has("metadata") && result["metadata"].type() == Json::OBJECT) {
                const Json& meta = result["metadata"];
                if (meta.has("images") && meta["images"].type() == Json::ARRAY && meta["images"].size() > 0) {
                    const Json& img = meta["images"][static_cast<size_t>(0)];
                    item.imageUrl = MAClient::imageRefFromJson(img);
                    if (img.has("provider")) item.imageProvider = img["provider"].str();
                }
            }
            item.mediaType = MediaType::TRACK;

            if (result.has("artist_name"))  item.artistName = result["artist_name"].str();
            if (result.has("album_name"))   item.albumName  = result["album_name"].str();
            if (result.has("duration"))     item.duration    = result["duration"].intVal();

            brls::Logger::info("PlayerActivity: Got track: {} - {}", item.artistName, item.name);

            brls::sync([this, item, alive]() {
                if (!alive->load() || m_destroying) {
                    m_loadingMedia = false;
                    return;
                }

                // Create a single-item queue and switch to queue mode
                std::vector<MusicItem> tracks = { item };
                MusicQueue& queue = MusicQueue::getInstance();
                queue.setQueue(tracks, 0);

                // Set up track ended callback
                queue.setTrackEndedCallback([this](const QueueItem* nextTrack) {
                    this->onTrackEnded(nextTrack);
                });

                m_isQueueMode = true;
                m_loadingMedia = false;  // loadFromQueue will set this again
                loadFromQueue();
            });
        });
    brls::Logger::debug("PlayerActivity: loadMedia - async track fetch started");
    m_loadingMedia = false;
}

void PlayerActivity::updateProgress() {
    // Don't update if destroying
    if (m_destroying) return;

    // For remote players, poll state periodically instead of local MPV updates
    if (isRemotePlayer()) {
        pollRemotePlayerState();
        return;
    }

    // Deferred MPV initialization (Phase 1 of 2):
    // Create MPV and its GXM render context, but do NOT call loadUrl yet.
    // loadUrl spawns decoder threads that use the shared GXM context via
    // hwdec=vita-copy. If the decoder thread starts before NanoVG has drawn
    // at least one clean frame after initRenderContext(), the concurrent GXM
    // access crashes. So we schedule loadUrl via brls::sync for the NEXT frame.
    if (!m_pendingPlayUrl.empty()) {
        std::string url = m_pendingPlayUrl;
        std::string title = m_pendingPlayTitle;
        bool isAudio = m_pendingIsAudio;
        m_pendingPlayUrl.clear();
        m_pendingPlayTitle.clear();

        brls::Logger::info("PlayerActivity: Performing deferred MPV init (phase 1: create context)...");

        MpvPlayer& player = MpvPlayer::getInstance();
        player.setAudioOnly(isAudio);

        if (!player.isInitialized()) {
            if (!player.init()) {
                brls::Logger::error("PlayerActivity: Deferred MPV init failed");
                return;
            }
        }

        // Phase 2: schedule loadUrl for the NEXT main-loop iteration.
        // brls::sync callbacks execute between frames, so NanoVG will draw one
        // complete frame with the freshly-created GXM state before the decoder
        // thread gets a chance to touch the shared GXM context.
        auto alive = m_alive;
        brls::sync([this, url, title, isAudio, alive]() {
            if (!alive->load() || m_destroying) return;

            brls::Logger::info("PlayerActivity: Deferred MPV load (phase 2: loadUrl)...");

            MpvPlayer& player = MpvPlayer::getInstance();

            if (player.loadUrl(url, title)) {
                m_isPlaying = true;
                updatePlayPauseLabel();
                brls::Logger::info("PlayerActivity: Deferred load started successfully");
            } else {
                brls::Logger::error("PlayerActivity: Deferred loadUrl failed");
            }
        });
        return;
    }

    MpvPlayer& player = MpvPlayer::getInstance();

    if (!player.isInitialized()) {
        return;
    }

    // Always process MPV events to handle state transitions
    player.update();

    // Skip UI updates while MPV is still loading - be gentle on Vita's limited hardware
    if (player.isLoading()) {
        return;
    }

    // Handle pending seek when playback becomes ready
    if (m_pendingSeek > 0.0 && player.isPlaying()) {
        player.seekTo(m_pendingSeek);
        m_pendingSeek = 0.0;
    }

    double position = player.getPosition();
    double duration = 0.0;

    // For music queue mode, prefer queue metadata duration (full track length)
    // over MPV duration which may only reflect buffered/demuxed portion
    if (m_isQueueMode) {
        const QueueItem* track = MusicQueue::getInstance().getCurrentTrack();
        if (track && track->duration > 0) {
            duration = (double)track->duration;
        }
    }
    if (duration <= 0)
        duration = player.getDuration();

    if (duration > 0) {
        if (progressSlider) {
            m_updatingSlider = true;
            progressSlider->setProgress((float)(position / duration));
            m_updatingSlider = false;
        }

        // Update time labels: elapsed on left, remaining on right
        {
            int posMin = (int)position / 60;
            int posSec = (int)position % 60;
            int remaining = std::max(0, (int)(duration - position));
            int remMin = remaining / 60;
            int remSec = remaining % 60;

            char elapsedStr[16];
            snprintf(elapsedStr, sizeof(elapsedStr), "%d:%02d", posMin, posSec);
            char remainStr[16];
            snprintf(remainStr, sizeof(remainStr), "-%d:%02d", remMin, remSec);

            if (timeElapsedLabel) timeElapsedLabel->setText(elapsedStr);
            if (timeRemainingLabel) timeRemainingLabel->setText(remainStr);
        }
    }

    // Auto-hide controls after inactivity
    int autoHide = Application::getInstance().getSettings().controlsAutoHideSeconds;
    if (autoHide > 0 && m_controlsVisible) {
        m_controlsIdleSeconds++;
        if (m_controlsIdleSeconds >= autoHide) {
            hideControls();
        }
    }

    // Detect playback end: check hasEnded() regardless of m_isPlaying
    // to avoid missing the event if m_isPlaying was synced to false
    // in a previous frame before the ENDED state was set.
    if (player.hasEnded() && !m_endHandled) {
        m_endHandled = true;  // Prevent multiple triggers
        m_isPlaying = false;
        brls::Logger::info("PlayerActivity: Playback ended (mediaType={}, queueMode={})",
            (int)m_mediaType, m_isQueueMode);

        if (m_isQueueMode) {
            // Notify queue that track ended - it will call onTrackEnded
            MusicQueue::getInstance().onTrackEnded();
        } else {
            // Non-queue single track ended - just exit
            brls::sync([this]() {
                brls::Application::popActivity();
            });
        }
    }

    // Keep play/pause label in sync with actual player state
    bool actuallyPlaying = player.isPlaying();
    if (actuallyPlaying != m_isPlaying) {
        m_isPlaying = actuallyPlaying;
        updatePlayPauseLabel();
    }
}

void PlayerActivity::togglePlayPause() {
    if (isRemotePlayer()) {
        // Send play/pause command to the remote player's queue
        std::string queueId = getActivePlayerId();
        auto& client = MAClient::instance();
        if (m_isPlaying) {
            client.queuePause(queueId);
            m_isPlaying = false;
        } else {
            client.queuePlay(queueId);
            m_isPlaying = true;
        }
        updatePlayPauseLabel();
        return;
    }

    MpvPlayer& player = MpvPlayer::getInstance();

    if (player.isPlaying()) {
        player.pause();
        m_isPlaying = false;
        // Also pause on the server so the MA dashboard stays in sync
        std::string queueId = getActivePlayerId();
        if (!queueId.empty()) {
            MAClient::instance().queuePause(queueId);
        }
    } else if (player.isPaused()) {
        player.play();
        m_isPlaying = true;
        // Also resume on the server
        std::string queueId = getActivePlayerId();
        if (!queueId.empty()) {
            MAClient::instance().queuePlay(queueId);
        }
    }
    updatePlayPauseLabel();
}

void PlayerActivity::updatePlayPauseLabel() {
    if (musicPlayIcon) {
        musicPlayIcon->setImageFromRes(m_isPlaying ? "icons/pause.png" : "icons/play.png");
    }
}

void PlayerActivity::seek(int seconds) {
    MpvPlayer& player = MpvPlayer::getInstance();
    player.seekRelative(seconds);
}

// Queue control methods

void PlayerActivity::playNext() {
    if (isRemotePlayer()) {
        std::string queueId = getActivePlayerId();
        MAClient::instance().queueNext(queueId, [this](bool success, const Json&) {
            if (success) {
                brls::sync([this]() {
                    if (m_alive && m_alive->load()) loadRemotePlayerState();
                });
            }
        });
        return;
    }

    if (!m_isQueueMode) return;

    MusicQueue& queue = MusicQueue::getInstance();
    if (queue.playNext()) {
        // Stop current playback
        MpvPlayer::getInstance().stop();
        m_isPlaying = false;

        // Load next track
        loadFromQueue();
    } else {
        brls::Logger::info("PlayerActivity: No next track");
    }
}

void PlayerActivity::playPrevious() {
    if (isRemotePlayer()) {
        std::string queueId = getActivePlayerId();
        MAClient::instance().queuePrevious(queueId, [this](bool success, const Json&) {
            if (success) {
                brls::sync([this]() {
                    if (m_alive && m_alive->load()) loadRemotePlayerState();
                });
            }
        });
        return;
    }

    if (!m_isQueueMode) return;

    MpvPlayer& player = MpvPlayer::getInstance();

    // If we're more than 3 seconds in, restart current track
    if (player.getPosition() > 3.0) {
        player.seekTo(0);
        return;
    }

    MusicQueue& queue = MusicQueue::getInstance();
    if (queue.playPrevious()) {
        // Stop current playback
        player.stop();
        m_isPlaying = false;

        // Load previous track
        loadFromQueue();
    } else {
        // Just restart current track
        player.seekTo(0);
    }
}

void PlayerActivity::toggleShuffle() {
    MusicQueue& queue = MusicQueue::getInstance();
    bool newState = !queue.isShuffleEnabled();

    if (m_isQueueMode) {
        queue.setShuffle(newState);
        updateQueueDisplay();
    }
    updateShuffleIcon();

    // Send to server for both local and remote players
    std::string queueId = getActivePlayerId();
    if (!queueId.empty()) {
        MAClient::instance().queueShuffle(queueId, newState);
    }

    MpvPlayer::getInstance().showOSD(
        newState ? "Shuffle: ON" : "Shuffle: OFF", 1.5);
}

void PlayerActivity::toggleRepeat() {
    MusicQueue& queue = MusicQueue::getInstance();

    if (m_isQueueMode) {
        queue.cycleRepeatMode();
        updateQueueDisplay();
    }
    updateRepeatIcon();

    // Determine the new repeat mode string for the server
    RepeatMode mode = queue.getRepeatMode();
    std::string modeServerStr = "off";
    const char* modeStr = "Repeat: OFF";
    if (mode == RepeatMode::ONE) {
        modeServerStr = "one";
        modeStr = "Repeat: ONE";
    } else if (mode == RepeatMode::ALL) {
        modeServerStr = "all";
        modeStr = "Repeat: ALL";
    }

    // Send to server for both local and remote players
    std::string queueId = getActivePlayerId();
    if (!queueId.empty()) {
        MAClient::instance().queueRepeat(queueId, modeServerStr);
    }

    MpvPlayer::getInstance().showOSD(modeStr, 1.5);
}

void PlayerActivity::updateShuffleIcon() {
    if (!shuffleIcon) return;
    MusicQueue& queue = MusicQueue::getInstance();
    if (queue.isShuffleEnabled()) {
        shuffleIcon->setImageFromRes("icons/shuffle-variant.png");
    } else {
        shuffleIcon->setImageFromRes("icons/shuffle-disabled.png");
    }
}

void PlayerActivity::updateRepeatIcon() {
    if (!repeatIcon) return;
    MusicQueue& queue = MusicQueue::getInstance();
    switch (queue.getRepeatMode()) {
        case RepeatMode::OFF:
            repeatIcon->setImageFromRes("icons/repeat-off.png");
            break;
        case RepeatMode::ALL:
            repeatIcon->setImageFromRes("icons/repeat.png");
            break;
        case RepeatMode::ONE:
            repeatIcon->setImageFromRes("icons/repeat-once.png");
            break;
    }
}

void PlayerActivity::onTrackEnded(const QueueItem* nextTrack) {
    if (m_destroying) return;

    if (nextTrack) {
        brls::Logger::info("PlayerActivity: Auto-advancing to next track: {}", nextTrack->title);

        // Load the next track
        brls::sync([this]() {
            loadFromQueue();
        });
    } else {
        brls::Logger::info("PlayerActivity: Queue ended, stopping playback");
        brls::sync([this]() {
            // Stop playback but keep player open so user can queue more songs
            m_isPlaying = false;
            updatePlayPauseLabel();
            MpvPlayer::getInstance().showOSD("Queue ended", 2.0);
        });
    }
}

void PlayerActivity::updateQueueDisplay() {
    if (!m_isQueueMode) return;

    MusicQueue& queue = MusicQueue::getInstance();

    if (queueLabel) {
        char queueInfo[64];

        // Build status string
        std::string status;
        if (queue.isShuffleEnabled()) {
            status += " [Shuffle]";
        }
        if (queue.getRepeatMode() == RepeatMode::ONE) {
            status += " [Repeat 1]";
        } else if (queue.getRepeatMode() == RepeatMode::ALL) {
            status += " [Repeat]";
        }

        // Show shuffle position when shuffled, otherwise raw queue index
        int displayPos = queue.isShuffleEnabled()
            ? queue.getShufflePosition() + 1
            : queue.getCurrentIndex() + 1;

        snprintf(queueInfo, sizeof(queueInfo), "Track %d of %d%s",
                displayPos,
                queue.getQueueSize(),
                status.c_str());

        queueLabel->setText(queueInfo);
        queueLabel->setVisibility(brls::Visibility::VISIBLE);
    }
    // The queue side sheet renders the SERVER queue and refreshes via
    // QUEUE_UPDATED events (see onRemoteQueueEvent), not from MusicQueue.
}

// ---------------------------------------------------------------------------
// Queue side sheet (Direction-A, ported 1:1 from Vita_plex).
//
// The sheet always renders the Music Assistant SERVER queue for the active
// player (the Vita's own sendspin player when no remote player is selected):
// playback already goes through player_queues/play_media in every mode, so the
// server queue is the single source of truth. Every queue mutation (play,
// remove, move, clear) is an MA API call; m_queueSnapshot mirrors the server
// state locally so the UI can update optimistically between events.

void PlayerActivity::showQueueOverlay() {
    if (m_queueOverlayVisible) {
        hideQueueOverlay();
        return;
    }

    if (!MAClient::instance().isConnected()) {
        brls::Application::notify("Not connected to server");
        return;
    }

    m_queueOverlayVisible = true;

    if (queueOverlay) {
        queueOverlay->setVisibility(brls::Visibility::VISIBLE);

        queueOverlay->registerAction("Back", brls::ControllerButton::BUTTON_B, [this](brls::View* view) {
            // While a track is grabbed, B drops it (keeps the queue open) rather
            // than closing — press again to close.
            if (m_queueGrabActive) { setQueueGrab(false); return true; }
            hideQueueOverlay();
            return true;
        });
        // X = remove the focused up-next track
        queueOverlay->registerAction("Remove", brls::ControllerButton::BUTTON_X, [this](brls::View* view) {
            removeFocusedQueueTrack();
            return true;
        });
        // L / R = move the focused up-next track earlier / later
        queueOverlay->registerAction("Move Up", brls::ControllerButton::BUTTON_LB, [this](brls::View* view) {
            moveFocusedQueueTrack(-1);
            return true;
        });
        queueOverlay->registerAction("Move Down", brls::ControllerButton::BUTTON_RB, [this](brls::View* view) {
            moveFocusedQueueTrack(1);
            return true;
        });
        // START picks up / drops the focused track for reorder.
        queueOverlay->registerAction("Move", brls::ControllerButton::BUTTON_START, [this](brls::View* view) {
            toggleQueueGrab();
            return true;
        });
        // While grabbed, Up/Down move the held track; otherwise return false so
        // borealis does its normal list navigation (it only navigates when the
        // button isn't consumed by an action).
        queueOverlay->registerAction("", brls::ControllerButton::BUTTON_NAV_UP, [this](brls::View* view) {
            if (!m_queueGrabActive) return false;
            moveFocusedQueueTrack(-1);
            return true;
        }, /*hidden*/ true, /*allowRepeating*/ true);
        queueOverlay->registerAction("", brls::ControllerButton::BUTTON_NAV_DOWN, [this](brls::View* view) {
            if (!m_queueGrabActive) return false;
            moveFocusedQueueTrack(1);
            return true;
        }, /*hidden*/ true, /*allowRepeating*/ true);
    }

    // Clear button: drop everything after the current track
    // Fetch a fresh snapshot of the server queue, then build the sheet
    fetchQueueSnapshot(true);
}

void PlayerActivity::fetchQueueSnapshot(bool showWhenReady) {
    std::string queueId = getActivePlayerId();
    if (queueId.empty()) return;

    if (m_queueFetchInFlight) {
        m_queueRefetchQueued = true;
        return;
    }
    m_queueFetchInFlight = true;

    auto aliveWeak = std::weak_ptr<std::atomic<bool>>(m_alive);

    // First read the queue object for the authoritative current_index, then
    // the item list. Both are cheap reads; the sheet builds once both landed.
    MAClient::instance().getQueue(queueId, [this, aliveWeak, queueId, showWhenReady](bool qOk, const Json& queueObj) {
        int serverCurrentIdx = -1;
        if (qOk && queueObj.type() == Json::OBJECT && queueObj.has("current_index") &&
            queueObj["current_index"].type() == Json::NUMBER) {
            serverCurrentIdx = queueObj["current_index"].intVal();
        }

        MAClient::instance().getQueueItems(queueId, [this, aliveWeak, serverCurrentIdx, showWhenReady](bool success, const Json& result) {
            if (!success || result.type() != Json::ARRAY) {
                brls::sync([this, aliveWeak]() {
                    auto alive = aliveWeak.lock();
                    if (!alive || !alive->load()) return;
                    m_queueFetchInFlight = false;
                    m_queueRefetchQueued = false;
                    brls::Application::notify("Failed to load queue");
                });
                return;
            }

            // Parse queue items into the shared QueueItem shape.
            // ratingKey carries the MA queue_item_id for queue API calls.
            std::string currentUri = m_remoteCurrentUri;
            std::vector<QueueItem> items;
            int currentIdx = serverCurrentIdx;
            for (size_t i = 0; i < result.size(); i++) {
                const Json& item = result[i];
                QueueItem qi;
                if (item.has("queue_item_id")) qi.ratingKey = item["queue_item_id"].str();
                if (item.has("uri")) qi.uri = item["uri"].str();
                if (item.has("name")) qi.title = item["name"].str();
                if (item.has("duration")) qi.duration = item["duration"].intVal();
                if (item.has("media_item") && item["media_item"].type() == Json::OBJECT) {
                    const Json& mi = item["media_item"];
                    if (mi.has("name")) qi.title = mi["name"].str();
                    if (mi.has("artists") && mi["artists"].type() == Json::ARRAY && mi["artists"].size() > 0) {
                        if (mi["artists"][0].has("name")) qi.artist = mi["artists"][0]["name"].str();
                    }
                    if (mi.has("image") && mi["image"].type() == Json::OBJECT) {
                        qi.thumb = MAClient::imageRefFromJson(mi["image"]);
                        if (mi["image"].has("provider")) qi.thumbProvider = mi["image"]["provider"].str();
                    }
                    if (qi.uri.empty() && mi.has("uri")) qi.uri = mi["uri"].str();
                }
                if (item.has("image") && item["image"].type() == Json::OBJECT) {
                    if (qi.thumb.empty()) qi.thumb = MAClient::imageRefFromJson(item["image"]);
                    if (qi.thumbProvider.empty() && item["image"].has("provider"))
                        qi.thumbProvider = item["image"]["provider"].str();
                }
                qi.index = (int)i;
                // Fallback current-track detection when the server didn't give
                // us an index: match the playing track's URI
                if (currentIdx < 0 && !currentUri.empty() && !qi.uri.empty() &&
                    qi.uri == currentUri)
                    currentIdx = (int)i;
                items.push_back(std::move(qi));
            }

            brls::sync([this, aliveWeak, items, currentIdx, showWhenReady]() {
                auto alive = aliveWeak.lock();
                if (!alive || !alive->load()) return;
                m_queueFetchInFlight = false;
                if (m_queueRefetchQueued) {
                    // More server changes landed while this fetch ran - the
                    // data is already stale, go straight to the next round.
                    m_queueRefetchQueued = false;
                    fetchQueueSnapshot(showWhenReady);
                    return;
                }
                m_queueSnapshot = items;
                m_queueSnapshotCurrentIdx = currentIdx;
                // Don't yank the rows out from under an active drag or a
                // grabbed track; the next event after the drop will refresh.
                if (m_queueGrabActive || m_dragState.active) return;
                if (m_queueOverlayVisible && (showWhenReady || queueList)) {
                    populateQueueList();
                }
            });
        }, 500, 0);
    });
}

void PlayerActivity::hideQueueOverlay() {
    m_queueOverlayVisible = false;
    m_queueGrabActive = false;   // drop any held track when the sheet closes
    m_queueBatchActive = false;  // Cancel any in-progress batch
    m_focusedQueueRow = nullptr;
    m_grabLift.reset(0.0f);      // stop any in-flight pickup animation
    if (queueOverlay) {
        queueOverlay->setVisibility(brls::Visibility::GONE);
    }
    // Rows render a point-in-time server snapshot; drop them so the next open
    // always rebuilds from a fresh fetch.
    if (queueList) queueList->clearViews();
    m_queueRowData.clear();
    m_deferredThumbs.clear();
    // Restore focus to queue button (fall back to play button if unavailable)
    if (queueBtn && queueBtn->getVisibility() == brls::Visibility::VISIBLE) {
        brls::Application::giveFocus(queueBtn);
    } else if (m_isQueueMode && musicPlayBtn) {
        brls::Application::giveFocus(musicPlayBtn);
    }
}

void PlayerActivity::createQueueRow(int displayIdx, int trackIdx, const QueueItem& track, bool isCurrent) {
    (void)displayIdx;
    (void)isCurrent;  // the playing track lives in the Now Playing block, never in this list

    // Row: [grip] [thumb] [title / artist] [duration] [remove x]
    brls::Box* row = new brls::Box();
    row->setAxis(brls::Axis::ROW);
    row->setJustifyContent(brls::JustifyContent::FLEX_START);
    row->setAlignItems(brls::AlignItems::CENTER);
    row->setHeight(52);
    row->setPaddingLeft(10);
    row->setPaddingRight(10);
    row->setCornerRadius(9);
    row->setFocusable(true);
    row->setMarginBottom(2);
    row->setBackgroundColor(nvgRGBA(0, 0, 0, 0));

    // Drag-handle glyph (3 stacked bars) - a visual affordance for reordering
    brls::Box* grip = new brls::Box();
    grip->setAxis(brls::Axis::COLUMN);
    grip->setJustifyContent(brls::JustifyContent::CENTER);
    grip->setAlignItems(brls::AlignItems::CENTER);
    grip->setWidth(14);
    grip->setMarginRight(8);
    for (int b = 0; b < 3; b++) {
        brls::Box* bar = new brls::Box();
        bar->setWidth(12);
        bar->setHeight(2);
        bar->setCornerRadius(1);
        bar->setBackgroundColor(nvgRGB(138, 138, 144));
        if (b < 2) bar->setMarginBottom(3);
        grip->addView(bar);
    }
    row->addView(grip);

    // Cover art thumbnail (38x38), loaded lazily when the row nears the viewport
    brls::Image* thumb = new brls::Image();
    thumb->setWidth(38);
    thumb->setHeight(38);
    thumb->setCornerRadius(6);
    thumb->setScalingType(brls::ImageScalingType::FIT);
    thumb->setMarginRight(11);
    m_deferredThumbs.push_back({thumb, track.thumb, track.thumbProvider, track.ratingKey, false});
    row->addView(thumb);

    // Title (white) over artist (muted), each ellipsized to a single line
    brls::Box* meta = new brls::Box();
    meta->setAxis(brls::Axis::COLUMN);
    meta->setJustifyContent(brls::JustifyContent::CENTER);
    meta->setGrow(1.0f);

    brls::Label* titleLbl = new brls::Label();
    titleLbl->setText(track.title);
    titleLbl->setFontSize(14);
    titleLbl->setTextColor(nvgRGB(255, 255, 255));
    titleLbl->setSingleLine(true);
    meta->addView(titleLbl);

    if (!track.artist.empty()) {
        brls::Label* artistLbl = new brls::Label();
        artistLbl->setText(track.artist);
        artistLbl->setFontSize(12);
        artistLbl->setTextColor(nvgRGB(180, 180, 186));
        artistLbl->setSingleLine(true);
        artistLbl->setMarginTop(1);
        meta->addView(artistLbl);
    }
    row->addView(meta);

    // Duration (tabular m:ss)
    brls::Label* durLbl = new brls::Label();
    if (track.duration > 0) {
        char durBuf[16];
        snprintf(durBuf, sizeof(durBuf), "%d:%02d", track.duration / 60, track.duration % 60);
        durLbl->setText(durBuf);
    } else {
        durLbl->setText("");
    }
    durLbl->setFontSize(12);
    durLbl->setTextColor(nvgRGB(138, 138, 144));
    durLbl->setMarginLeft(8);
    row->addView(durLbl);

    // Remove (x) affordance - reserved space, revealed only while focused
    brls::Box* removeBtn = new brls::Box();
    removeBtn->setAxis(brls::Axis::ROW);
    removeBtn->setJustifyContent(brls::JustifyContent::CENTER);
    removeBtn->setAlignItems(brls::AlignItems::CENTER);
    removeBtn->setWidth(24);
    removeBtn->setHeight(24);
    removeBtn->setCornerRadius(6);
    removeBtn->setMarginLeft(6);
    removeBtn->setVisibility(brls::Visibility::INVISIBLE);
    brls::Image* removeIcon = new brls::Image();
    removeIcon->setWidth(12);
    removeIcon->setHeight(12);
    removeIcon->setScalingType(brls::ImageScalingType::FIT);
    removeIcon->setImageFromRes("icons/cross.png");
    removeBtn->addView(removeIcon);
    row->addView(removeBtn);

    // Row -> track mapping (looked up dynamically by the handlers below)
    m_queueRowData[row] = {trackIdx, track.title, removeBtn};

    // Swipe left to remove this track from the queue
    row->addGestureRecognizer(new brls::PanGestureRecognizer(
        [this, row](brls::PanGestureStatus status, brls::Sound* soundToPlay) {
            if (status.state == brls::GestureState::UNSURE || status.state == brls::GestureState::START) {
                float deltaX = status.position.x - status.startPosition.x;
                if (deltaX > 0) { row->setTranslationX(0); row->setAlpha(1.0f); return; }
                row->setTranslationX(deltaX);
                float alpha = 1.0f - std::min(1.0f, std::abs(deltaX) / 200.0f);
                row->setAlpha(std::max(0.2f, alpha));
            } else if (status.state == brls::GestureState::END) {
                float deltaX = status.position.x - status.startPosition.x;
                if (deltaX < -120.0f) {
                    auto it = m_queueRowData.find(row);
                    if (it != m_queueRowData.end()) {
                        int tIdx = it->second.trackIdx;
                        brls::sync([this, tIdx]() { removeQueueTrackByIndex(tIdx); });
                        return;
                    }
                }
                row->setTranslationX(0);
                row->setAlpha(1.0f);
            } else if (status.state == brls::GestureState::FAILED) {
                row->setTranslationX(0);
                row->setAlpha(1.0f);
            }
        }, brls::PanAxis::HORIZONTAL));

    // Vertical pan: hold briefly to drag-reorder, otherwise scroll the list.
    // Touch users get reordering back without an L/R controller; the dragged
    // row follows the finger while neighbours slide out of the way, and the
    // drop commits via the server move_item call + rebuild.
    row->addGestureRecognizer(new brls::PanGestureRecognizer(
        [this, row](brls::PanGestureStatus status, brls::Sound* soundToPlay) {
            constexpr float rowH = 54.0f;  // 52 height + 2 margin
            float deltaY = status.position.y - status.startPosition.y;

            if (status.state == brls::GestureState::UNSURE) {
                if (!m_dragState.active && m_dragState.draggedRow != row) {
                    m_dragState.holdStart = std::chrono::steady_clock::now();
                    m_dragState.holdMet = false;
                    m_dragState.active = false;
                    m_dragState.draggedRow = row;
                    m_dragState.scrollPassthrough = true;
                    m_dragState.initialScrollY = queueScroll ? queueScroll->getContentOffsetY() : 0.0f;
                    m_dragState.originalDisplayIdx = findQueueRowDisplayIndex(row);
                    m_dragState.targetDisplayIdx = m_dragState.originalDisplayIdx;
                    auto it = m_queueRowData.find(row);
                    m_dragState.draggedTrackIdx = (it != m_queueRowData.end()) ? it->second.trackIdx : -1;
                }
            } else if (status.state == brls::GestureState::START ||
                       status.state == brls::GestureState::STAY) {
                // Promote a still-held touch into a drag
                if (!m_dragState.holdMet && m_dragState.draggedRow == row) {
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - m_dragState.holdStart).count();
                    if (elapsed >= HOLD_THRESHOLD_MS && std::abs(deltaY) < rowH * 0.5f) {
                        m_dragState.holdMet = true;
                        m_dragState.active = true;
                        m_dragState.scrollPassthrough = false;
                        m_dragState.originalDisplayIdx = findQueueRowDisplayIndex(row);
                        m_dragState.targetDisplayIdx = m_dragState.originalDisplayIdx;
                        m_dragState.dragStartY = status.position.y;
                        m_dragState.dragStartScrollY = queueScroll ? queueScroll->getContentOffsetY() : 0.0f;
                        row->setBackgroundColor(nvgRGBA(229, 160, 13, 60));  // lift cue
                    }
                }

                // Not yet a drag: forward the vertical motion to the scroll view
                if (m_dragState.scrollPassthrough && m_dragState.draggedRow == row) {
                    if (queueScroll && std::abs(deltaY) >= 10.0f) {
                        float newOffset = m_dragState.initialScrollY - deltaY;
                        if (newOffset < 0) newOffset = 0;
                        float viewH = queueScroll->getHeight();
                        int n = queueList ? (int)queueList->getChildren().size() : 0;
                        float maxScroll = std::max(0.0f, n * rowH + 8.0f - viewH);
                        if (newOffset > maxScroll) newOffset = maxScroll;
                        queueScroll->setContentOffsetY(newOffset, false);
                    }
                    return;
                }
                if (!m_dragState.holdMet || m_dragState.draggedRow != row) return;

                // Drag mode: row follows the finger, displaced rows slide aside
                float scrollDelta = queueScroll
                    ? (queueScroll->getContentOffsetY() - m_dragState.dragStartScrollY) : 0.0f;
                float eff = (status.position.y - m_dragState.dragStartY) + scrollDelta;
                row->setTranslationY(eff);

                // Auto-scroll while the finger sits near the top/bottom edge of
                // the viewport, so a drag can reach rows past the visible window.
                if (queueScroll) {
                    constexpr float EDGE = 44.0f;
                    constexpr float SPEED = 9.0f;
                    float viewH = queueScroll->getHeight();
                    float fingerInView = status.position.y - queueScroll->getY();
                    float scrollY = queueScroll->getContentOffsetY();
                    int n = queueList ? (int)queueList->getChildren().size() : 0;
                    float maxScroll = std::max(0.0f, n * rowH + 8.0f - viewH);
                    if (fingerInView > viewH - EDGE && scrollY < maxScroll) {
                        queueScroll->setContentOffsetY(std::min(maxScroll, scrollY + SPEED), false);
                    } else if (fingerInView < EDGE && scrollY > 0) {
                        queueScroll->setContentOffsetY(std::max(0.0f, scrollY - SPEED), false);
                    }
                    // Re-read the scroll offset so the dragged row stays under the
                    // finger and the target index reflects the new scroll position.
                    scrollDelta = queueScroll->getContentOffsetY() - m_dragState.dragStartScrollY;
                    eff = (status.position.y - m_dragState.dragStartY) + scrollDelta;
                    row->setTranslationY(eff);
                }

                int origIdx = m_dragState.originalDisplayIdx;
                int childCount = queueList ? (int)queueList->getChildren().size() : 0;
                int newTarget = origIdx + (int)std::lround(eff / rowH);
                if (newTarget < 0) newTarget = 0;
                if (newTarget > childCount - 1) newTarget = childCount - 1;
                m_dragState.targetDisplayIdx = newTarget;

                if (queueList) {
                    auto& children = queueList->getChildren();
                    for (int i = 0; i < (int)children.size(); i++) {
                        if (i == origIdx) continue;
                        float shift = 0.0f;
                        if (newTarget > origIdx && i > origIdx && i <= newTarget) shift = -rowH;
                        else if (newTarget < origIdx && i >= newTarget && i < origIdx) shift = rowH;
                        children[i]->setTranslationY(shift);
                    }
                }
            } else if (status.state == brls::GestureState::END ||
                       status.state == brls::GestureState::FAILED) {
                bool didDrag = m_dragState.holdMet && m_dragState.draggedRow == row;
                int origIdx = m_dragState.originalDisplayIdx;
                int targetIdx = m_dragState.targetDisplayIdx;
                int fromTrack = m_dragState.draggedTrackIdx;

                if (queueList) for (auto* c : queueList->getChildren()) c->setTranslationY(0);

                bool committed = false;
                if (didDrag && status.state == brls::GestureState::END &&
                    origIdx >= 0 && targetIdx >= 0 && origIdx != targetIdx && fromTrack >= 0) {
                    int toTrack = targetIdx + m_queueWindowStart;
                    if (toTrack >= 0 && toTrack < (int)m_queueSnapshot.size() && toTrack != fromTrack &&
                        fromTrack < (int)m_queueSnapshot.size()) {
                        std::string itemId = m_queueSnapshot[fromTrack].ratingKey;
                        if (!itemId.empty()) {
                            MAClient::instance().queueMoveItem(getActivePlayerId(), itemId,
                                                               toTrack - fromTrack);
                        }
                        // Mirror the move locally so the rebuild matches the server
                        QueueItem moved = m_queueSnapshot[fromTrack];
                        m_queueSnapshot.erase(m_queueSnapshot.begin() + fromTrack);
                        m_queueSnapshot.insert(m_queueSnapshot.begin() + toTrack, moved);
                        committed = true;
                        brls::sync([this, targetIdx]() {
                            populateQueueList();
                            m_dragState.justEnded = false;
                            if (queueList) {
                                auto& ch = queueList->getChildren();
                                if (!ch.empty()) {
                                    int t = std::min(std::max(targetIdx, 0), (int)ch.size() - 1);
                                    brls::Application::giveFocus(ch[t]);
                                }
                            }
                        });
                    }
                }
                // Suppress the click that fires right after a drag gesture
                m_dragState.justEnded = didDrag;
                if (!committed) {
                    row->setBackgroundColor(m_focusedQueueRow == row
                        ? nvgRGB(58, 58, 70) : nvgRGBA(0, 0, 0, 0));
                }

                m_dragState.active = false;
                m_dragState.holdMet = false;
                m_dragState.draggedRow = nullptr;
                m_dragState.scrollPassthrough = false;
                m_dragState.originalDisplayIdx = -1;
                m_dragState.targetDisplayIdx = -1;
                m_dragState.draggedTrackIdx = -1;
            }
        }, brls::PanAxis::VERTICAL));

    // Tap / A to play this track (suppressed right after a drag-reorder). While
    // a track is grabbed for reorder, A/OK drops it instead of playing — so
    // click-to-play still works normally whenever nothing is grabbed.
    row->registerClickAction([this, row](brls::View* view) {
        if (m_dragState.justEnded) {
            m_dragState.justEnded = false;
            return true;
        }
        if (m_queueGrabActive) {
            setQueueGrab(false);
            return true;
        }
        auto it = m_queueRowData.find(row);
        if (it != m_queueRowData.end()) {
            int idx = it->second.trackIdx;
            brls::sync([this, idx]() {
                hideQueueOverlay();
                MAClient::instance().playQueueIndex(getActivePlayerId(), idx);
            });
        }
        return true;
    });
    row->addGestureRecognizer(new brls::TapGestureRecognizer(row));

    // On focus: reveal this row's remove affordance, tint its background, and
    // lazy-load nearby thumbnails. Restores the previously focused row.
    row->getFocusEvent()->subscribe([this, row](brls::View*) {
        if (m_focusedQueueRow && m_focusedQueueRow != row) {
            m_focusedQueueRow->setBackgroundColor(nvgRGBA(0, 0, 0, 0));
            // Seat any lifted row back if focus genuinely moves off it.
            m_focusedQueueRow->setTranslationX(0.0f);
            m_focusedQueueRow->setShadowVisibility(false);
            auto pit = m_queueRowData.find(m_focusedQueueRow);
            if (pit != m_queueRowData.end() && pit->second.removeBtn) {
                pit->second.removeBtn->setVisibility(brls::Visibility::INVISIBLE);
            }
        }
        m_focusedQueueRow = row;
        // Grabbed rows get a gold "lifted" tint (re-applied here so it survives
        // the rebuild + re-focus that each move triggers); otherwise the normal
        // focus tint.
        row->setBackgroundColor(m_queueGrabActive ? nvgRGBA(229, 160, 13, 90)
                                                  : nvgRGB(58, 58, 70));
        auto it = m_queueRowData.find(row);
        if (it != m_queueRowData.end() && it->second.removeBtn) {
            it->second.removeBtn->setVisibility(brls::Visibility::VISIBLE);
        }
        int actualIdx = findQueueRowDisplayIndex(row);
        if (actualIdx >= 0) {
            loadQueueThumbsAroundIndex(actualIdx);
            // Nearing the bottom of the rendered window: extend it
            int childCount = queueList ? (int)queueList->getChildren().size() : 0;
            if (actualIdx >= childCount - QUEUE_EXPAND_TRIGGER &&
                m_queueWindowEnd < m_queueTotalCount) {
                expandQueueWindow(1);
            }
        }
    });

    queueList->addView(row);
}

void PlayerActivity::populateQueueList() {
    if (!queueList) return;
    if (m_queuePopulating) return;  // Prevent re-entrant calls
    m_queuePopulating = true;

    m_queueBatchActive = false;  // Cancel any in-progress batched population

    // Park focus on the Clear button before tearing down rows so we never
    // destroy the focused view out from under borealis.
    m_focusedQueueRow = nullptr;
    m_grabLift.reset(0.0f);  // stop any in-flight lift before rows are destroyed
    if (!queueList->getChildren().empty() && queueClearBtn) {
        brls::Application::giveFocus(queueClearBtn);
    }
    m_queueRowData.clear();
    queueList->clearViews();
    m_deferredThumbs.clear();

    // The server queue snapshot is already in play order (the server applies
    // shuffle when items are enqueued), so no shuffle indirection here.
    const auto& tracks = m_queueSnapshot;
    int count = (int)tracks.size();
    int currentIndex = m_queueSnapshotCurrentIdx;

    m_lastRenderedCurrentIndex = currentIndex;
    m_queueTotalCount = count;

    // Refresh the "Now Playing" header from the current track
    updateNowPlayingBlock();

    // Everything after the current track is "Up Next".
    int firstUpNext = currentIndex + 1;
    int upcoming = (count > firstUpNext) ? (count - firstUpNext) : 0;

    if (queueUpNextLabel) {
        if (upcoming > 0) {
            char buf[32];
            snprintf(buf, sizeof(buf), "UP NEXT \xC2\xB7 %d", upcoming);
            queueUpNextLabel->setText(buf);
        } else {
            queueUpNextLabel->setText("UP NEXT");
        }
    }

    // Window the render so a huge queue doesn't spawn thousands of rows.
    // Only ever expands toward the end (downward).
    m_queueWindowStart = firstUpNext < 0 ? 0 : firstUpNext;
    m_queueWindowEnd = std::min(count, m_queueWindowStart + QUEUE_RENDER_LIMIT);
    int windowSize = std::max(0, m_queueWindowEnd - m_queueWindowStart);

    if (upcoming <= 0) {
        brls::Label* empty = new brls::Label();
        empty->setText("Nothing up next");
        empty->setFontSize(13);
        empty->setTextColor(nvgRGB(124, 124, 132));
        empty->setMarginTop(10);
        empty->setMarginLeft(10);
        queueList->addView(empty);
        if (m_queueOverlayVisible && queueClearBtn) {
            brls::Application::giveFocus(queueClearBtn);
        }
        m_queueFocusTargetChild = -1;
        m_queuePopulating = false;
        return;
    }

    auto addMoreLabel = [this, count]() {
        if (m_queueWindowEnd < count) {
            brls::Label* more = new brls::Label();
            char mbuf[48];
            snprintf(mbuf, sizeof(mbuf), "+%d more", count - m_queueWindowEnd);
            more->setText(mbuf);
            more->setFontSize(12);
            more->setTextColor(nvgRGB(124, 124, 132));
            more->setMarginTop(8);
            more->setMarginLeft(10);
            queueList->addView(more);
        }
    };

    if (windowSize <= QUEUE_BATCH_SIZE) {
        for (int pos = m_queueWindowStart; pos < m_queueWindowEnd; pos++) {
            createQueueRow(pos, pos, tracks[pos], false);
        }
        addMoreLabel();
        linkFirstRowToClear();
        loadQueueThumbsAroundIndex(0);
        if (m_queueOverlayVisible && queueList && !queueList->getChildren().empty()) {
            // After a reorder, land on the moved track's new slot; otherwise the
            // first row.
            int fc = (m_queueFocusTargetChild >= 0) ? m_queueFocusTargetChild : 0;
            fc = std::min(std::max(fc, 0), (int)queueList->getChildren().size() - 1);
            brls::Application::giveFocus(queueList->getChildren()[fc]);
        }
        m_queueFocusTargetChild = -1;
        m_queuePopulating = false;
        return;
    }

    // Larger window: create rows in batches across frames to avoid a UI freeze
    m_queueBatchNext = m_queueWindowStart;
    m_queueBatchTotal = m_queueWindowEnd;
    m_queueBatchActive = true;
    populateQueueBatch();

    m_queuePopulating = false;
}

void PlayerActivity::updateNowPlayingBlock() {
    const QueueItem* cur = nullptr;
    if (m_queueSnapshotCurrentIdx >= 0 && m_queueSnapshotCurrentIdx < (int)m_queueSnapshot.size())
        cur = &m_queueSnapshot[m_queueSnapshotCurrentIdx];
    if (!cur) {
        if (queueNowPlaying) queueNowPlaying->setVisibility(brls::Visibility::GONE);
        if (queueNpLabel)    queueNpLabel->setVisibility(brls::Visibility::GONE);
        return;
    }
    if (queueNpLabel)    queueNpLabel->setVisibility(brls::Visibility::VISIBLE);
    if (queueNowPlaying) queueNowPlaying->setVisibility(brls::Visibility::VISIBLE);

    if (queueNpTitle) {
        queueNpTitle->setText(cur->title);
        queueNpTitle->setSingleLine(true);
    }
    if (queueNpArtist) {
        queueNpArtist->setText(cur->artist);
        queueNpArtist->setSingleLine(true);
        queueNpArtist->setVisibility(cur->artist.empty()
            ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
    }
    if (queueNpThumb) {
        if (!cur->thumb.empty()) {
            std::string url = MAClient::instance().getThumbnailUrl(cur->thumb, 120, 120,
                                                                   cur->thumbProvider);
            ImageLoader::setPaused(false);
            ImageLoader::loadAsync(url, [](brls::Image*) {}, queueNpThumb, m_alive);
            ImageLoader::setPaused(true);
        }
    }
}

void PlayerActivity::removeQueueTrackByIndex(int trackIdx) {
    if (trackIdx < 0 || trackIdx >= (int)m_queueSnapshot.size()) return;
    if (trackIdx == m_queueSnapshotCurrentIdx) return;  // never drop the playing track here

    std::string itemId = m_queueSnapshot[trackIdx].ratingKey;
    if (!itemId.empty()) {
        MAClient::instance().queueDeleteItem(getActivePlayerId(), itemId);
    }
    m_queueSnapshot.erase(m_queueSnapshot.begin() + trackIdx);
    if (trackIdx < m_queueSnapshotCurrentIdx) m_queueSnapshotCurrentIdx--;
    populateQueueList();
}

void PlayerActivity::linkFirstRowToClear() {
    // The up-next list lives in a ScrollingFrame, which traps UP navigation at
    // its first row. Route UP off that row to the Clear button sitting above the
    // list so focus can escape upward. (Entering the list via DOWN from Clear is
    // not trapped, so it needs no route.) The route points from the ephemeral
    // row to the stable Clear button, so it can't dangle across rebuilds.
    if (!queueList || !queueClearBtn) return;
    auto& children = queueList->getChildren();
    if (!children.empty()) {
        children[0]->setCustomNavigationRoute(brls::FocusDirection::UP, queueClearBtn);
    }
}

void PlayerActivity::removeFocusedQueueTrack() {
    if (!m_queueOverlayVisible) return;
    brls::View* focused = brls::Application::getCurrentFocus();
    auto it = m_queueRowData.find(focused);
    if (it == m_queueRowData.end()) return;
    removeQueueTrackByIndex(it->second.trackIdx);
}

void PlayerActivity::moveFocusedQueueTrack(int direction) {
    if (!m_queueOverlayVisible || !queueList) return;
    brls::View* focused = brls::Application::getCurrentFocus();

    auto& children = queueList->getChildren();
    int childIdx = -1;
    for (int i = 0; i < (int)children.size(); i++) {
        if (children[i] == focused) { childIdx = i; break; }
    }
    if (childIdx < 0) return;
    int targetChild = childIdx + direction;
    if (targetChild < 0 || targetChild >= (int)children.size()) return;

    auto itFrom = m_queueRowData.find(children[childIdx]);
    auto itTo   = m_queueRowData.find(children[targetChild]);
    if (itFrom == m_queueRowData.end() || itTo == m_queueRowData.end()) return;

    // A row's position in the rendered window is its queue index (window child
    // index + window start); the snapshot is already in play order.
    const int absFrom = itFrom->second.trackIdx;
    const int absTo   = m_queueWindowStart + targetChild;
    if (absFrom < 0 || absFrom >= (int)m_queueSnapshot.size()) return;
    if (absTo < 0 || absTo >= (int)m_queueSnapshot.size()) return;

    // Server-side move: shift the item one position in the queue.
    std::string itemId = m_queueSnapshot[absFrom].ratingKey;
    if (!itemId.empty()) {
        MAClient::instance().queueMoveItem(getActivePlayerId(), itemId, direction);
    }
    // Mirror locally (adjacent swap)
    std::swap(m_queueSnapshot[absFrom], m_queueSnapshot[absTo]);

    // Swap the two adjacent row views in place — never a full rebuild. This
    // covers every adjacent reorder (mid-list and first row), so moving a track
    // (including to the very top) doesn't tear down and rebuild the list:
    // no cover-reload flicker, and the row keeps focus since its view isn't
    // destroyed.
    brls::View* rowFrom = children[childIdx];
    queueList->removeView(rowFrom, /*free=*/false);  // detach, don't destroy
    queueList->addView(rowFrom, targetChild);        // re-insert at new slot
    // The move swapped the two tracks' queue indices; swap the rows' stored
    // trackIdx to match.
    std::swap(itFrom->second.trackIdx, itTo->second.trackIdx);
    // Row order (and possibly the first row) changed — keep the UP->Clear escape
    // routes correct around the swap.
    refixQueueUpRoutes(std::min(childIdx, targetChild));
    // The row never lost focus, so giveFocus is a no-op and won't scroll; scroll
    // the moved row into view explicitly so grab-mode hold-to-move follows it.
    brls::Application::giveFocus(rowFrom);
    scrollQueueToChild(targetChild);
}

void PlayerActivity::refixQueueUpRoutes(int lo) {
    if (!queueList || !queueClearBtn) return;
    auto& ch = queueList->getChildren();
    // Re-point the rows whose correct upward neighbour could have changed: the
    // two swapped rows (lo, lo+1) and the one just below (lo+2), whose neighbour
    // is now a different object. Row 0 escapes to Clear; the rest point at the
    // row above. Rows outside this window keep their existing (still-correct)
    // routes. Only track rows are focusable / routable — skip the "+N more"
    // label.
    for (int i = std::max(0, lo); i <= lo + 2 && i < (int)ch.size(); i++) {
        if (m_queueRowData.find(ch[i]) == m_queueRowData.end()) continue;
        if (i == 0) {
            ch[0]->setCustomNavigationRoute(brls::FocusDirection::UP, queueClearBtn);
        } else {
            ch[i]->setCustomNavigationRoute(brls::FocusDirection::UP, ch[i - 1]);
        }
    }
}

void PlayerActivity::scrollQueueToChild(int idx) {
    if (!queueScroll || !queueList || idx < 0) return;
    constexpr float rowH = 54.0f;  // 52 height + 2 margin (matches the rows)
    float viewH = queueScroll->getHeight();
    if (viewH <= 0.0f) return;
    int n = (int)queueList->getChildren().size();
    float maxScroll = std::max(0.0f, n * rowH + 8.0f - viewH);
    // Center the row (clamped at the ends), matching the list's CENTERED nav so
    // a grab-mode move keeps the held row centered as the list scrolls under it.
    float newOffset = (idx * rowH + rowH / 2.0f) - viewH / 2.0f;
    newOffset = std::min(std::max(newOffset, 0.0f), maxScroll);
    if (std::abs(newOffset - queueScroll->getContentOffsetY()) > 0.5f)
        queueScroll->setContentOffsetY(newOffset, false);
}

void PlayerActivity::toggleQueueGrab() {
    if (!m_queueOverlayVisible) return;
    if (m_queueGrabActive) {
        setQueueGrab(false);
        return;
    }
    // Only pick up an actual up-next track row — not the Clear button, the
    // "+N more" label, or the empty-state text.
    brls::View* focused = brls::Application::getCurrentFocus();
    if (!focused || m_queueRowData.find(focused) == m_queueRowData.end())
        return;
    setQueueGrab(true);
}

void PlayerActivity::setQueueGrab(bool on) {
    m_queueGrabActive = on;
    // Lift the grabbed (focused) row with a gold tint, mirroring the sidebar
    // editor's grabbed-row cue; restore the normal focus tint on drop.
    if (m_focusedQueueRow) {
        m_focusedQueueRow->setBackgroundColor(on ? nvgRGBA(229, 160, 13, 90)
                                                  : nvgRGB(58, 58, 70));
        // Slide the row out (pop on pickup) / settle it back (on drop), with a
        // shadow while held so the lift reads physically, not just by colour.
        animateGrabLift(on);
    }
    if (on) {
        MpvPlayer::getInstance().showOSD("Moving track \xC2\xB7 Up/Down to move, OK to drop", 2.0);
    }
}

void PlayerActivity::animateGrabLift(bool lifted) {
    brls::Box* row = m_focusedQueueRow;
    if (!row) return;
    // Capture the specific row so the animation always targets it (not whoever
    // holds focus later) — a quick drop-then-navigate can't tug a different
    // row. The m_queueRowData lookup guards against the row being torn down by
    // a rebuild mid-animation (populateQueueList clears the map first).
    // Detach any prior end callback first so the reset() below can't fire a
    // stale shadow-cleanup (e.g. a re-pickup that interrupts a settle-back).
    m_grabLift.setEndCallback([](bool) {});
    m_grabLift.reset(m_grabLift.getValue());
    if (lifted) {
        row->setShadowType(brls::ShadowType::GENERIC);
        row->setShadowVisibility(true);
        // Overshoot past the seated-out position, then settle: a tactile "pop".
        m_grabLift.addStep(1.18f, 110, brls::EasingFunction::quadraticOut);
        m_grabLift.addStep(1.0f, 90, brls::EasingFunction::quadraticOut);
    } else {
        m_grabLift.addStep(0.0f, 140, brls::EasingFunction::quadraticOut);
        // Drop the shadow and zero the offset only once fully seated again.
        m_grabLift.setEndCallback([this, row](bool) {
            if (m_queueRowData.count(row)) {
                row->setTranslationX(0.0f);
                row->setShadowVisibility(false);
            }
        });
    }
    m_grabLift.setTickCallback([this, row] {
        if (m_queueRowData.count(row))
            row->setTranslationX(m_grabLift.getValue() * kGrabLiftPx);
    });
    m_grabLift.start();
}

void PlayerActivity::clearUpcoming() {
    int cur = m_queueSnapshotCurrentIdx;
    if ((int)m_queueSnapshot.size() <= cur + 1) return;

    // Delete upcoming items by id, back-to-front (ids stay valid regardless,
    // but this keeps server indices stable while the deletes land).
    for (int i = (int)m_queueSnapshot.size() - 1; i > cur; i--) {
        const std::string& itemId = m_queueSnapshot[i].ratingKey;
        if (!itemId.empty()) {
            MAClient::instance().queueDeleteItem(getActivePlayerId(), itemId);
        }
    }
    m_queueSnapshot.resize(std::max(cur + 1, 0));
    populateQueueList();
    MpvPlayer::getInstance().showOSD("Cleared up next", 1.5);
}

void PlayerActivity::populateQueueBatch() {
    if (!m_queueBatchActive || !queueList || m_destroying) return;

    int end = std::min(m_queueBatchNext + QUEUE_BATCH_SIZE, m_queueBatchTotal);

    for (int i = m_queueBatchNext; i < end; i++) {
        if (i < 0 || i >= (int)m_queueSnapshot.size()) continue;
        createQueueRow(i, i, m_queueSnapshot[i], false);
    }

    m_queueBatchNext = end;

    if (m_queueBatchNext >= m_queueBatchTotal) {
        // All rows created - finalize
        m_queueBatchActive = false;

        loadQueueThumbsAroundIndex(0);
        linkFirstRowToClear();

        // Land focus on the first up-next row — or, after a reorder, on the
        // moved track's new slot.
        if (m_queueOverlayVisible && queueList && !queueList->getChildren().empty()) {
            int childFocusIdx = (m_queueFocusTargetChild >= 0) ? m_queueFocusTargetChild : 0;
            childFocusIdx = std::min(childFocusIdx, (int)queueList->getChildren().size() - 1);
            if (childFocusIdx < 0) childFocusIdx = 0;
            brls::Application::giveFocus(queueList->getChildren()[childFocusIdx]);
            if (queueOverlayTitle) queueOverlayTitle->setFocusable(false);
        }
        m_queueFocusTargetChild = -1;
    } else {
        // Schedule next batch on the next frame via brls::sync
        brls::sync([this]() {
            populateQueueBatch();
        });
    }
}

void PlayerActivity::expandQueueWindow(int direction) {
    if (!queueList || m_queueBatchActive || m_destroying) return;

    if (direction > 0) {
        // Expand downward - kick off async batch creation
        int count = (int)m_queueSnapshot.size();
        if (m_queueWindowEnd >= count) return;  // Already at the end
        if (m_expandActive) return;  // Already expanding

        m_expandNext = m_queueWindowEnd;
        m_expandEnd = std::min(count, m_queueWindowEnd + QUEUE_EXPAND_CHUNK);
        m_expandActive = true;
        brls::Logger::debug("Queue: starting async expand {} -> {} (total={})",
            m_expandNext, m_expandEnd, count);
        // Create first batch immediately so content appears right away
        expandQueueBatch();
    }
}

void PlayerActivity::expandQueueBatch() {
    if (!m_expandActive || !queueList || m_destroying) return;

    int count = (int)m_queueSnapshot.size();
    int batchEnd = std::min(m_expandNext + QUEUE_EXPAND_BATCH, m_expandEnd);

    for (int i = m_expandNext; i < batchEnd; i++) {
        if (i < 0 || i >= count) continue;
        createQueueRow(i, i, m_queueSnapshot[i], false);
    }

    int oldWindowEnd = m_queueWindowEnd;
    m_queueWindowEnd = batchEnd;
    m_expandNext = batchEnd;

    // Load thumbnails for newly added rows
    int thumbStart = oldWindowEnd - m_queueWindowStart;
    loadQueueThumbsAroundIndex(std::max(0, thumbStart));

    if (m_expandNext >= m_expandEnd) {
        // Expansion complete
        m_expandActive = false;
        brls::Logger::debug("Queue: async expand complete, windowEnd={}", m_queueWindowEnd);
    } else {
        // Schedule next batch on the next frame
        brls::sync([this]() {
            expandQueueBatch();
        });
    }
}

void PlayerActivity::loadQueueThumbsAroundIndex(int displayIndex) {
    if (m_deferredThumbs.empty()) return;

    // Load thumbnails for a window around the given display index
    int start = std::max(0, displayIndex - QUEUE_THUMB_BUFFER);
    int end = std::min((int)m_deferredThumbs.size(), displayIndex + QUEUE_THUMB_BUFFER + 6);

    // Temporarily unpause so loadAsync accepts the requests, then re-pause.
    // The async workers no longer check the pause flag, so queued loads
    // will complete even after we re-pause here.
    ImageLoader::setPaused(false);

    MAClient& client = MAClient::instance();

    for (int i = start; i < end; i++) {
        auto& dt = m_deferredThumbs[i];
        if (dt.loaded) continue;
        if (dt.thumbPath.empty()) continue;

        dt.loaded = true;

        std::string thumbUrl = client.getThumbnailUrl(dt.thumbPath, 100, 100, dt.thumbProvider);
        ImageLoader::loadAsync(thumbUrl, [](brls::Image* image) {
            // Thumbnail loaded
        }, dt.image, m_alive);
    }

    ImageLoader::setPaused(true);
}

int PlayerActivity::findQueueRowDisplayIndex(brls::View* row) {
    if (!queueList) return -1;
    auto& children = queueList->getChildren();
    for (int i = 0; i < (int)children.size(); i++) {
        if (children[i] == row) return i;
    }
    return -1;
}

// Controls visibility toggle (like Suwayomi reader settings show/hide)

void PlayerActivity::toggleControls() {
    if (m_controlsVisible) {
        hideControls();
    } else {
        showControls();
    }
}

void PlayerActivity::resetControlsIdleTimer() {
    m_controlsIdleSeconds = 0;
}

void PlayerActivity::showControls() {
    m_controlsVisible = true;
    resetControlsIdleTimer();
    if (controlsBox) {
        controlsBox->setAlpha(1.0f);
        controlsBox->setVisibility(brls::Visibility::VISIBLE);
    }
    if (titleLabel) {
        titleLabel->setVisibility(brls::Visibility::VISIBLE);
    }
}

void PlayerActivity::hideControls() {
    // Don't hide controls in music mode
    if (m_isQueueMode) return;

    m_controlsVisible = false;
    if (controlsBox) {
        controlsBox->setAlpha(0.0f);
        controlsBox->setVisibility(brls::Visibility::GONE);
    }
}

void PlayerActivity::updatePlayerNameLabel() {
    if (!playerNameLabel) return;
    const auto& selectedId = Application::getInstance().getSettings().selectedPlayerId;
    if (selectedId.empty() || (!m_ownPlayerId.empty() && selectedId == m_ownPlayerId)) {
        playerNameLabel->setText("This Vita");
        return;
    }
    // Look up the player name from cached list
    for (const auto& p : m_availablePlayers) {
        if (p.playerId == selectedId) {
            playerNameLabel->setText(p.name);
            return;
        }
    }
    // Fallback: show truncated ID
    playerNameLabel->setText(selectedId.substr(0, 16));
}

std::string PlayerActivity::getActivePlayerId() const {
    const auto& selectedId = Application::getInstance().getSettings().selectedPlayerId;
    if (!selectedId.empty()) return selectedId;
    return App::instance().getPlayerId();
}

std::string PlayerActivity::findOwnPlayerId(const std::vector<PlayerInfo>& players) const {
    // Our Sendspin client id (e.g. "vita_ma_player"); MA derives the player_id
    // from it, sometimes with a provider prefix.
    const std::string clientId = App::instance().getPlayerId();
    std::string name = Application::getInstance().getSettings().sendspinPlayerName;
    if (name.empty()) name = "PS Vita";

    // 1) player_id equals or embeds our client id.
    if (!clientId.empty()) {
        for (const auto& p : players) {
            if (p.playerId == clientId || p.playerId.find(clientId) != std::string::npos)
                return p.playerId;
        }
    }
    // 2) fall back to matching the display name.
    for (const auto& p : players) {
        if (p.name == name) return p.playerId;
    }
    return "";
}

bool PlayerActivity::isRemotePlayer() const {
    return !Application::getInstance().getSettings().selectedPlayerId.empty();
}

void PlayerActivity::loadRemotePlayerState() {
    const auto& playerId = Application::getInstance().getSettings().selectedPlayerId;
    if (playerId.empty()) return;

    auto& client = MAClient::instance();
    if (!client.isConnected()) return;

    auto aliveWeak = std::weak_ptr<std::atomic<bool>>(m_alive);

    // Fetch the player's queue item count for the queue info label
    client.getQueueItems(playerId, [this, playerId, aliveWeak](bool success, const Json& result) {
        if (!success) return;

        int totalItems = 0;
        if (result.type() == Json::ARRAY) totalItems = (int)result.size();

        brls::sync([this, totalItems, aliveWeak]() {
            auto alive = aliveWeak.lock();
            if (!alive || !alive->load()) return;

            brls::Logger::info("PlayerActivity: Remote player has {} queue items", totalItems);

            if (totalItems == 0) {
                if (queueLabel) queueLabel->setVisibility(brls::Visibility::GONE);
            } else if (queueLabel) {
                queueLabel->setText(std::to_string(totalItems) + " tracks in queue");
                queueLabel->setVisibility(brls::Visibility::VISIBLE);
            }
        });
    }, 50, 0);

    // Fetch the player state for current track info, playback state, and elapsed time
    // The players/get response uses: current_media.title, current_media.artist (string),
    // current_media.image_url (flat URL), current_media.duration, current_media.uri
    Json playerArgs;
    playerArgs["player_id"] = Json(playerId);
    client.sendCommand("players/get", playerArgs, [this, aliveWeak](bool success, const Json& result) {
        if (!success || result.type() != Json::OBJECT) return;

        std::string currentName, currentArtist, currentImageUrl, currentUri;
        int elapsed = 0, duration = 0;
        std::string state;

        if (result.has("elapsed_time")) elapsed = result["elapsed_time"].intVal();
        if (result.has("state")) state = result["state"].str();

        if (result.has("current_media") && result["current_media"].type() == Json::OBJECT) {
            const Json& media = result["current_media"];
            // MA API uses "title" and "artist" (singular string), not "name"/"artists"
            if (media.has("title")) currentName = media["title"].str();
            if (media.has("artist")) currentArtist = media["artist"].str();
            if (media.has("image_url")) currentImageUrl = media["image_url"].str();
            if (media.has("uri")) currentUri = media["uri"].str();
            if (media.has("duration")) duration = media["duration"].intVal();
            // Fallback: try "name" in case of older MA versions
            if (currentName.empty() && media.has("name")) currentName = media["name"].str();
        }

        brls::sync([this, currentName, currentArtist, currentImageUrl, currentUri,
                    elapsed, duration, state, aliveWeak]() {
            auto alive = aliveWeak.lock();
            if (!alive || !alive->load()) return;

            // Update remote state tracking
            m_remoteState = state;
            m_remoteElapsed = elapsed;
            m_remoteDuration = duration;
            m_remoteCurrentUri = currentUri;

            // Update play/pause button state
            bool isPlaying = (state == "playing");
            if (isPlaying != m_isPlaying) {
                m_isPlaying = isPlaying;
                updatePlayPauseLabel();
            }

            if (currentName.empty()) {
                // No current track - show idle state
                if (state == "idle" || state == "off") {
                    if (musicTitleLabel) musicTitleLabel->setText("No track playing");
                    if (musicArtistLabel) musicArtistLabel->setText("");
                    if (titleLabel) titleLabel->setText("No track playing");
                    if (progressSlider) progressSlider->setVisibility(brls::Visibility::GONE);
                    if (timeElapsedLabel) timeElapsedLabel->setVisibility(brls::Visibility::GONE);
                    if (timeRemainingLabel) timeRemainingLabel->setVisibility(brls::Visibility::GONE);
                }
                return;
            }

            brls::Logger::info("PlayerActivity: Remote player: {} - {} [{}] elapsed={}s duration={}s",
                             currentArtist, currentName, state, elapsed, duration);

            if (musicTitleLabel) musicTitleLabel->setText(currentName);
            if (musicArtistLabel) musicArtistLabel->setText(currentArtist);
            if (titleLabel) titleLabel->setText(currentName);
            if (artistLabel) {
                artistLabel->setText(currentArtist);
                artistLabel->setVisibility(currentArtist.empty()
                    ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
            }

            // Show progress elements
            if (progressSlider) progressSlider->setVisibility(brls::Visibility::VISIBLE);
            if (timeElapsedLabel) timeElapsedLabel->setVisibility(brls::Visibility::VISIBLE);
            if (timeRemainingLabel) timeRemainingLabel->setVisibility(brls::Visibility::VISIBLE);

            // Update progress bar and time labels
            if (duration > 0) {
                m_updatingSlider = true;
                if (progressSlider) progressSlider->setProgress((float)elapsed / (float)duration);
                m_updatingSlider = false;

                int posMin = elapsed / 60, posSec = elapsed % 60;
                int rem = std::max(0, duration - elapsed);
                int remMin = rem / 60, remSec = rem % 60;
                char buf[16];
                snprintf(buf, sizeof(buf), "%d:%02d", posMin, posSec);
                if (timeElapsedLabel) timeElapsedLabel->setText(buf);
                snprintf(buf, sizeof(buf), "-%d:%02d", remMin, remSec);
                if (timeRemainingLabel) timeRemainingLabel->setText(buf);
            }

            // Load album art from image_url
            if (albumArt && !currentImageUrl.empty()) {
                MAClient& client = MAClient::instance();
                // image_url from current_media is already a full URL or provider path
                std::string thumbUrl = client.getThumbnailUrl(currentImageUrl, 300, 300, "");
                ImageLoader::setPaused(false);
                ImageLoader::loadAsync(thumbUrl, [](brls::Image* img) {
                    img->setVisibility(brls::Visibility::VISIBLE);
                }, albumArt, m_alive);
                ImageLoader::setPaused(true);
            }
        });
    });
}

void PlayerActivity::pollRemotePlayerState() {
    if (!isRemotePlayer()) return;

    // Only poll every 3 seconds (called from 1-second updateProgress timer)
    m_remotePollCounter++;
    if (m_remotePollCounter < 3) return;
    m_remotePollCounter = 0;

    const auto& playerId = Application::getInstance().getSettings().selectedPlayerId;
    auto& client = MAClient::instance();
    if (!client.isConnected()) return;

    auto aliveWeak = std::weak_ptr<std::atomic<bool>>(m_alive);
    Json args;
    args["player_id"] = Json(playerId);
    client.sendCommand("players/get", args, [this, aliveWeak](bool success, const Json& result) {
        if (!success || result.type() != Json::OBJECT) return;

        std::string currentName, currentArtist, currentImageUrl, currentUri;
        int elapsed = 0, duration = 0;
        std::string state;

        if (result.has("elapsed_time")) elapsed = result["elapsed_time"].intVal();
        if (result.has("state")) state = result["state"].str();

        if (result.has("current_media") && result["current_media"].type() == Json::OBJECT) {
            const Json& media = result["current_media"];
            if (media.has("title")) currentName = media["title"].str();
            if (media.has("artist")) currentArtist = media["artist"].str();
            if (media.has("image_url")) currentImageUrl = media["image_url"].str();
            if (media.has("uri")) currentUri = media["uri"].str();
            if (media.has("duration")) duration = media["duration"].intVal();
            if (currentName.empty() && media.has("name")) currentName = media["name"].str();
        }

        brls::sync([this, currentName, currentArtist, currentImageUrl, currentUri,
                    elapsed, duration, state, aliveWeak]() {
            auto alive = aliveWeak.lock();
            if (!alive || !alive->load()) return;

            // Update state
            m_remoteState = state;
            m_remoteElapsed = elapsed;
            m_remoteDuration = duration;

            // Update play/pause button
            bool isPlaying = (state == "playing");
            if (isPlaying != m_isPlaying) {
                m_isPlaying = isPlaying;
                updatePlayPauseLabel();
            }

            // Check if the track changed
            bool trackChanged = (!currentUri.empty() && currentUri != m_remoteCurrentUri);
            if (trackChanged) {
                m_remoteCurrentUri = currentUri;
                brls::Logger::info("PlayerActivity: Remote track changed: {} - {} [{}]",
                                 currentArtist, currentName, state);
            }

            // Update title/artist if track changed or on first poll
            if (trackChanged || (m_remoteCurrentUri.empty() && !currentName.empty())) {
                m_remoteCurrentUri = currentUri;
                if (musicTitleLabel) musicTitleLabel->setText(currentName);
                if (musicArtistLabel) musicArtistLabel->setText(currentArtist);
                if (titleLabel) titleLabel->setText(currentName);
                if (artistLabel) {
                    artistLabel->setText(currentArtist);
                    artistLabel->setVisibility(currentArtist.empty()
                        ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
                }
                // Load new album art
                if (albumArt && !currentImageUrl.empty()) {
                    MAClient& client = MAClient::instance();
                    std::string thumbUrl = client.getThumbnailUrl(currentImageUrl, 300, 300, "");
                    ImageLoader::setPaused(false);
                    ImageLoader::loadAsync(thumbUrl, [](brls::Image* img) {
                        img->setVisibility(brls::Visibility::VISIBLE);
                    }, albumArt, m_alive);
                    ImageLoader::setPaused(true);
                }
            }

            // Update progress bar and time labels
            if (duration > 0) {
                m_updatingSlider = true;
                if (progressSlider) progressSlider->setProgress((float)elapsed / (float)duration);
                m_updatingSlider = false;

                int posMin = elapsed / 60, posSec = elapsed % 60;
                int rem = std::max(0, duration - elapsed);
                int remMin = rem / 60, remSec = rem % 60;
                char buf[16];
                snprintf(buf, sizeof(buf), "%d:%02d", posMin, posSec);
                if (timeElapsedLabel) timeElapsedLabel->setText(buf);
                snprintf(buf, sizeof(buf), "-%d:%02d", remMin, remSec);
                if (timeRemainingLabel) timeRemainingLabel->setText(buf);
            }

            // Handle idle state
            if (currentName.empty() && (state == "idle" || state == "off")) {
                if (musicTitleLabel) musicTitleLabel->setText("No track playing");
                if (musicArtistLabel) musicArtistLabel->setText("");
            }
        });
    });
}

void PlayerActivity::onRemotePlayerEvent(const Json& data) {
    if (!isRemotePlayer()) return;

    // The event data contains the updated player object
    // Check if this event is for our selected player
    const auto& selectedId = Application::getInstance().getSettings().selectedPlayerId;
    if (data.type() == Json::OBJECT && data.has("player_id")) {
        if (data["player_id"].str() != selectedId) return;
    }

    // Trigger an immediate poll to update UI
    m_remotePollCounter = 3;  // Force next poll cycle
}

void PlayerActivity::onRemoteQueueEvent(const Json& data) {
    // Events for a different queue than the active player's are irrelevant
    std::string activeId = getActivePlayerId();
    if (data.type() == Json::OBJECT && data.has("queue_id")) {
        if (data["queue_id"].str() != activeId) return;
    }

    // The open sheet mirrors the server queue - refetch and rebuild it
    if (m_queueOverlayVisible) {
        fetchQueueSnapshot(false);
    }

    if (isRemotePlayer()) {
        // Refresh queue item count shown under the transport
        auto aliveWeak = std::weak_ptr<std::atomic<bool>>(m_alive);
        MAClient::instance().getQueueItems(activeId, [this, aliveWeak](bool success, const Json& result) {
            if (!success) return;
            int totalItems = 0;
            if (result.type() == Json::ARRAY) totalItems = (int)result.size();
            brls::sync([this, totalItems, aliveWeak]() {
                auto alive = aliveWeak.lock();
                if (!alive || !alive->load()) return;
                if (queueLabel) {
                    if (totalItems > 0) {
                        queueLabel->setText(std::to_string(totalItems) + " tracks in queue");
                        queueLabel->setVisibility(brls::Visibility::VISIBLE);
                    } else {
                        queueLabel->setVisibility(brls::Visibility::GONE);
                    }
                }
            });
        }, 50, 0);

        // Also trigger a player state poll to update current track
        m_remotePollCounter = 3;
    }
}

void PlayerActivity::showPlayerSwitcher() {
    auto& client = MAClient::instance();
    if (!client.isConnected()) {
        brls::Application::notify("Not connected to server");
        return;
    }

    client.getPlayers([this](bool success, const Json& result) {
        if (!success || result.type() != Json::ARRAY) {
            brls::sync([](){ brls::Application::notify("Failed to load players"); });
            return;
        }

        std::vector<PlayerInfo> players;
        for (size_t i = 0; i < result.size(); i++) {
            const Json& p = result[i];
            PlayerInfo info;
            if (p.has("player_id")) info.playerId = p["player_id"].str();
            if (p.has("name")) info.name = p["name"].str();
            if (p.has("type")) info.type = p["type"].str();
            if (p.has("powered")) info.powered = p["powered"].boolVal();
            if (p.has("available")) info.available = p["available"].boolVal();
            if (!info.playerId.empty()) players.push_back(info);
        }

        brls::sync([this, players]() {
            m_availablePlayers = players;
            m_ownPlayerId = findOwnPlayerId(players);

            auto* dialog = new brls::Dialog("Switch Player");
            auto* box = new brls::Box();
            box->setAxis(brls::Axis::COLUMN);
            box->setPadding(16);

            const auto& currentId = Application::getInstance().getSettings().selectedPlayerId;

            // One entry per player. Our own registered Sendspin player is
            // labeled "This Vita" (local playback); no separate pseudo-entry,
            // so it never shows up twice.
            for (size_t i = 0; i < m_availablePlayers.size(); i++) {
                bool isOwn = !m_ownPlayerId.empty() &&
                             m_availablePlayers[i].playerId == m_ownPlayerId;

                auto* btn = new brls::Button();
                btn->setStyle(&brls::BUTTONSTYLE_BORDERLESS);
                std::string pname = isOwn ? "This Vita" : m_availablePlayers[i].name;
                std::string label = pname;
                if (!m_availablePlayers[i].available) label += " (offline)";
                if (m_availablePlayers[i].playerId == currentId) label += "  *";
                btn->setText(label);
                btn->setMarginBottom(4);
                std::string pid = m_availablePlayers[i].playerId;
                btn->registerClickAction([this, dialog, pid, pname](brls::View*) {
                    auto& settings = Application::getInstance().getSettings();
                    settings.selectedPlayerId = pid;
                    Application::getInstance().saveSettings();
                    dialog->close();
                    updatePlayerNameLabel();
                    loadRemotePlayerState();
                    brls::Application::notify("Switched to " + pname);
                    return true;
                });
                box->addView(btn);
            }

            dialog->addView(box);
            dialog->addButton("Cancel", []() {});
            dialog->open();
        });
    });
}

} // namespace vita_ma
