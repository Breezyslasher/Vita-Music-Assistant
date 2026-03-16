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

    // Queue overlay dismiss on tap
    if (queueOverlay) {
        queueOverlay->addGestureRecognizer(new brls::TapGestureRecognizer(
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
        brls::Logger::error("PlayerActivity: No current track in queue");
        m_loadingMedia = false;
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
    std::string playerId = App::instance().getPlayerId();
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
                item.imageUrl = result["image"]["path"].str();
                if (result["image"].has("provider")) item.imageProvider = result["image"]["provider"].str();
            } else if (result.has("image") && result["image"].type() == Json::STRING) {
                item.imageUrl = result["image"].str();
            } else if (result.has("metadata") && result["metadata"].type() == Json::OBJECT) {
                const Json& meta = result["metadata"];
                if (meta.has("images") && meta["images"].type() == Json::ARRAY && meta["images"].size() > 0) {
                    const Json& img = meta["images"][static_cast<size_t>(0)];
                    if (img.has("path")) item.imageUrl = img["path"].str();
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
    MpvPlayer& player = MpvPlayer::getInstance();

    if (player.isPlaying()) {
        player.pause();
        m_isPlaying = false;
    } else if (player.isPaused()) {
        player.play();
        m_isPlaying = true;
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
    if (!m_isQueueMode) return;

    MusicQueue& queue = MusicQueue::getInstance();
    queue.setShuffle(!queue.isShuffleEnabled());

    updateQueueDisplay();
    updateShuffleIcon();

    MpvPlayer::getInstance().showOSD(
        queue.isShuffleEnabled() ? "Shuffle: ON" : "Shuffle: OFF", 1.5);
}

void PlayerActivity::toggleRepeat() {
    if (!m_isQueueMode) return;

    MusicQueue& queue = MusicQueue::getInstance();
    queue.cycleRepeatMode();

    updateQueueDisplay();
    updateRepeatIcon();

    // Show OSD feedback
    const char* modeStr = "Repeat: OFF";
    if (queue.getRepeatMode() == RepeatMode::ONE) {
        modeStr = "Repeat: ONE";
    } else if (queue.getRepeatMode() == RepeatMode::ALL) {
        modeStr = "Repeat: ALL";
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

    // Refresh queue list overlay if visible - rebuild when the queue
    // version changed (size, order, or shuffle toggle), otherwise just
    // update the current-track highlight
    if (m_queueOverlayVisible && queueList) {
        uint32_t currentVersion = queue.getVersion();
        if (m_cachedQueueVersion != currentVersion) {
            // Queue changed (shuffle toggled, tracks added/removed, etc.)
            populateQueueList();
        } else {
            // Just update highlight on current track rows
            int currentIdx = queue.getCurrentIndex();
            for (auto& pair : m_queueRowData) {
                brls::Box* row = static_cast<brls::Box*>(pair.first);
                bool isCurrent = (pair.second.trackIdx == currentIdx);
                if (isCurrent) {
                    row->setBackgroundColor(nvgRGBA(70, 90, 210, 150));
                    row->setBorderColor(nvgRGBA(120, 160, 255, 200));
                    row->setBorderThickness(1.5f);
                } else {
                    row->setBackgroundColor(nvgRGBA(255, 255, 255, 8));
                    row->setBorderColor(nvgRGBA(0, 0, 0, 0));
                    row->setBorderThickness(0);
                }
            }
            updateQueueTitle();
        }
    }
}

// Queue list overlay methods

void PlayerActivity::showQueueOverlay() {
    if (m_queueOverlayVisible) {
        hideQueueOverlay();
        return;
    }

    m_queueOverlayVisible = true;

    // Only rebuild the queue list if the queue has actually changed since we
    // last populated it.  Otherwise reuse the cached rows for instant reopen.
    MusicQueue& showQueue = MusicQueue::getInstance();
    uint32_t currentVersion = showQueue.getVersion();
    if (m_cachedQueueVersion == 0 || m_cachedQueueVersion != currentVersion ||
        !queueList || queueList->getChildren().empty()) {
        populateQueueList();
    } else {
        // Rows are cached - just update the current-track highlight
        int currentIdx = showQueue.getCurrentIndex();
        for (auto& pair : m_queueRowData) {
            brls::Box* row = static_cast<brls::Box*>(pair.first);
            bool isCurrent = (pair.second.trackIdx == currentIdx);
            if (isCurrent) {
                row->setBackgroundColor(nvgRGBA(70, 90, 210, 150));
                row->setBorderColor(nvgRGBA(120, 160, 255, 200));
                row->setBorderThickness(1.5f);
            } else {
                row->setBackgroundColor(nvgRGBA(255, 255, 255, 8));
                row->setBorderColor(nvgRGBA(0, 0, 0, 0));
                row->setBorderThickness(0);
            }
        }
        updateQueueTitle();
    }

    if (queueOverlay) {
        queueOverlay->setVisibility(brls::Visibility::VISIBLE);
        queueOverlay->registerAction("Back", brls::ControllerButton::BUTTON_B, [this](brls::View* view) {
            hideQueueOverlay();
            return true;
        });

        // L button = move focused song up in queue (swap + live renumber)
        queueOverlay->registerAction("Move Up", brls::ControllerButton::BUTTON_LB, [this](brls::View* view) {
            if (!queueList) return true;
            auto& children = queueList->getChildren();
            brls::View* focused = brls::Application::getCurrentFocus();
            for (int i = 0; i < (int)children.size(); i++) {
                if (children[i] == focused) {
                    if (i > 0) {
                        MusicQueue& queue = MusicQueue::getInstance();
                        bool shuffled = queue.isShuffleEnabled();
                        const auto& shuffleOrder = queue.getShuffleOrder();
                        int fromIdx = (shuffled && i < (int)shuffleOrder.size()) ? shuffleOrder[i] : i;
                        int toIdx = (shuffled && (i-1) < (int)shuffleOrder.size()) ? shuffleOrder[i-1] : (i-1);
                        queue.moveTrack(fromIdx, toIdx);
                        swapQueueRows(i, i - 1);
                        renumberQueueRows();
                        m_cachedQueueVersion = queue.getVersion();
                        // Give focus to the moved row at its new position
                        if (i - 1 >= 0 && i - 1 < (int)queueList->getChildren().size()) {
                            brls::Application::giveFocus(queueList->getChildren()[i - 1]);
                        }
                    }
                    break;
                }
            }
            return true;
        });

        // R button = move focused song down in queue (swap + live renumber)
        queueOverlay->registerAction("Move Down", brls::ControllerButton::BUTTON_RB, [this](brls::View* view) {
            if (!queueList) return true;
            auto& children = queueList->getChildren();
            brls::View* focused = brls::Application::getCurrentFocus();
            for (int i = 0; i < (int)children.size(); i++) {
                if (children[i] == focused) {
                    if (i < (int)children.size() - 1) {
                        MusicQueue& queue = MusicQueue::getInstance();
                        bool shuffled = queue.isShuffleEnabled();
                        const auto& shuffleOrder = queue.getShuffleOrder();
                        int fromIdx = (shuffled && i < (int)shuffleOrder.size()) ? shuffleOrder[i] : i;
                        int toIdx = (shuffled && (i+1) < (int)shuffleOrder.size()) ? shuffleOrder[i+1] : (i+1);
                        queue.moveTrack(fromIdx, toIdx);
                        swapQueueRows(i, i + 1);
                        renumberQueueRows();
                        m_cachedQueueVersion = queue.getVersion();
                        // Give focus to the moved row at its new position
                        if (i + 1 < (int)queueList->getChildren().size()) {
                            brls::Application::giveFocus(queueList->getChildren()[i + 1]);
                        }
                    }
                    break;
                }
            }
            return true;
        });

        // Give focus to the currently playing track in the list
        // When batching is active, focus is deferred until the final batch completes
        if (!m_queueBatchActive) {
            MusicQueue& queue = MusicQueue::getInstance();
            int focusIdx = 0;
            if (queue.isShuffleEnabled()) {
                focusIdx = queue.getShufflePosition();
            } else {
                focusIdx = queue.getCurrentIndex();
            }
            // Convert absolute queue index to child index within rendered window
            int childFocusIdx = focusIdx - m_queueWindowStart;
            if (queueList && !queueList->getChildren().empty()) {
                childFocusIdx = std::min(childFocusIdx, (int)queueList->getChildren().size() - 1);
                if (childFocusIdx < 0) childFocusIdx = 0;
                brls::Application::giveFocus(queueList->getChildren()[childFocusIdx]);
            }
            // Reset overlay title focusable state (was set temporarily during list rebuild)
            if (queueOverlayTitle) {
                queueOverlayTitle->setFocusable(false);
            }
        }
    }
}

void PlayerActivity::hideQueueOverlay() {
    m_queueOverlayVisible = false;
    m_queueBatchActive = false;  // Cancel any in-progress batch
    if (queueOverlay) {
        queueOverlay->setVisibility(brls::Visibility::GONE);
    }
    // Restore focus to queue button (fall back to music play button if unavailable)
    if (queueBtn && queueBtn->getVisibility() == brls::Visibility::VISIBLE) {
        brls::Application::giveFocus(queueBtn);
    } else if (musicPlayBtn) {
        brls::Application::giveFocus(musicPlayBtn);
    }
}

void PlayerActivity::createQueueRow(int displayIdx, int trackIdx, const QueueItem& track, bool isCurrent) {
    MAClient& client = MAClient::instance();

    // Row container: [cover art] [title + artist] [duration]
    brls::Box* row = new brls::Box();
    row->setAxis(brls::Axis::ROW);
    row->setJustifyContent(brls::JustifyContent::FLEX_START);
    row->setAlignItems(brls::AlignItems::CENTER);
    row->setPaddingTop(7);
    row->setPaddingBottom(7);
    row->setPaddingLeft(12);
    row->setPaddingRight(12);
    row->setCornerRadius(10);
    row->setFocusable(true);
    row->setMarginBottom(3);

    if (isCurrent) {
        row->setBackgroundColor(nvgRGBA(70, 90, 210, 150));
        row->setBorderColor(nvgRGBA(120, 160, 255, 200));
        row->setBorderThickness(1.5f);
    } else {
        row->setBackgroundColor(nvgRGBA(255, 255, 255, 8));
    }

    // Cover art thumbnail (48x48)
    brls::Image* thumb = new brls::Image();
    thumb->setWidth(48);
    thumb->setHeight(48);
    thumb->setCornerRadius(8);
    thumb->setScalingType(brls::ImageScalingType::FIT);
    thumb->setMarginRight(14);

    // Defer thumbnail loading - URL resolved lazily when row becomes visible
    m_deferredThumbs.push_back({thumb, track.thumb, track.thumbProvider, track.ratingKey, false});
    row->addView(thumb);

    // Text container: title on top, artist below
    brls::Box* textBox = new brls::Box();
    textBox->setAxis(brls::Axis::COLUMN);
    textBox->setJustifyContent(brls::JustifyContent::CENTER);
    textBox->setGrow(1.0f);

    // Track number + title
    brls::Label* titleLbl = new brls::Label();
    std::string titleStr;
    if (isCurrent) {
        titleStr = "> " + track.title;
    } else {
        char numBuf[8];
        snprintf(numBuf, sizeof(numBuf), "%d. ", displayIdx + 1);
        titleStr = numBuf + track.title;
    }
    titleLbl->setText(titleStr);
    titleLbl->setFontSize(15);
    titleLbl->setTextColor(isCurrent ? nvgRGB(170, 210, 255) : nvgRGB(240, 240, 240));
    textBox->addView(titleLbl);

    // Artist name
    if (!track.artist.empty()) {
        brls::Label* artistLbl = new brls::Label();
        artistLbl->setText(track.artist);
        artistLbl->setFontSize(12);
        artistLbl->setTextColor(isCurrent ? nvgRGBA(170, 210, 255, 180) : nvgRGB(170, 170, 170));
        artistLbl->setMarginTop(2);
        textBox->addView(artistLbl);
    }

    row->addView(textBox);

    // Duration label on the right side
    if (track.duration > 0) {
        brls::Label* durLbl = new brls::Label();
        int durMin = track.duration / 60;
        int durSec = track.duration % 60;
        char durBuf[16];
        snprintf(durBuf, sizeof(durBuf), "%d:%02d", durMin, durSec);
        durLbl->setText(durBuf);
        durLbl->setFontSize(12);
        durLbl->setTextColor(nvgRGB(140, 140, 140));
        durLbl->setMarginLeft(8);
        row->addView(durLbl);
    }

    // Store the track data mapping for this row
    m_queueRowData[row] = {trackIdx, track.title};

    // Swipe left to remove track from queue
    // Handlers look up position dynamically so they stay valid after reordering
    row->addGestureRecognizer(new brls::PanGestureRecognizer(
        [this, row](brls::PanGestureStatus status, brls::Sound* soundToPlay) {
            if (status.state == brls::GestureState::UNSURE || status.state == brls::GestureState::START) {
                float deltaX = status.position.x - status.startPosition.x;
                row->setTranslationX(deltaX);
                float alpha = 1.0f - std::min(1.0f, std::abs(deltaX) / 200.0f);
                row->setAlpha(std::max(0.2f, alpha));
            } else if (status.state == brls::GestureState::END) {
                float deltaX = status.position.x - status.startPosition.x;
                float threshold = 120.0f;
                if (deltaX < -threshold) {
                    // Look up current track index dynamically
                    auto it = m_queueRowData.find(row);
                    if (it != m_queueRowData.end()) {
                        int tIdx = it->second.trackIdx;
                        MusicQueue& queue = MusicQueue::getInstance();
                        if (tIdx != queue.getCurrentIndex()) {
                            int dIdx = findQueueRowDisplayIndex(row);
                            // Sync remove to server
                            queue.removeTrack(tIdx);
                            if (dIdx >= 0) {
                                brls::sync([this, dIdx]() {
                                    removeQueueRow(dIdx);
                                });
                            }
                        } else {
                            row->setTranslationX(0);
                            row->setAlpha(1.0f);
                        }
                    }
                } else {
                    row->setTranslationX(0);
                    row->setAlpha(1.0f);
                }
            } else if (status.state == brls::GestureState::FAILED) {
                row->setTranslationX(0);
                row->setAlpha(1.0f);
            }
        }, brls::PanAxis::HORIZONTAL));

    // Vertical pan: scroll passthrough OR hold-to-drag reorder
    // When the user touches a row and moves vertically, this gesture fires
    // (which blocks the ScrollingFrame's own scroll). To fix scrolling:
    //  - If hold threshold NOT met: programmatically scroll the ScrollingFrame
    //  - If hold threshold met (finger held still): switch to drag-reorder mode
    // The STAY state is handled so the dragged row follows the finger in real time.
    row->addGestureRecognizer(new brls::PanGestureRecognizer(
        [this, row](brls::PanGestureStatus status, brls::Sound* soundToPlay) {
            float deltaY = status.position.y - status.startPosition.y;

            if (status.state == brls::GestureState::UNSURE) {
                // First touch - record hold start time and initial scroll position
                if (!m_dragState.active && m_dragState.draggedRow != row) {
                    m_dragState.holdStart = std::chrono::steady_clock::now();
                    m_dragState.holdMet = false;
                    m_dragState.draggedRow = row;
                    m_dragState.originalDisplayIdx = findQueueRowDisplayIndex(row);
                    m_dragState.targetDisplayIdx = m_dragState.originalDisplayIdx;
                    m_dragState.scrollPassthrough = true;
                    m_dragState.initialScrollY = queueScroll ? queueScroll->getContentOffsetY() : 0.0f;
                    m_dragState.dragStartY = 0.0f;
                    auto it = m_queueRowData.find(row);
                    m_dragState.draggedTrackIdx = (it != m_queueRowData.end()) ? it->second.trackIdx : -1;
                    brls::Logger::debug("Drag: touch start on displayIdx={} trackIdx={} scrollY={:.1f}",
                        m_dragState.originalDisplayIdx, m_dragState.draggedTrackIdx, m_dragState.initialScrollY);
                }
            } else if (status.state == brls::GestureState::START ||
                       status.state == brls::GestureState::STAY) {
                // Check if we should transition from scroll mode to drag mode
                if (!m_dragState.holdMet) {
                    auto now = std::chrono::steady_clock::now();
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - m_dragState.holdStart).count();
                    if (elapsed >= HOLD_THRESHOLD_MS && std::abs(deltaY) < ROW_HEIGHT_PX * 0.5f) {
                        m_dragState.holdMet = true;
                        m_dragState.active = true;
                        m_dragState.scrollPassthrough = false;
                        m_dragState.originalDisplayIdx = findQueueRowDisplayIndex(row);
                        m_dragState.targetDisplayIdx = m_dragState.originalDisplayIdx;
                        m_dragState.dragStartY = status.position.y;
                        m_dragState.dragStartScrollY = queueScroll ? queueScroll->getContentOffsetY() : 0.0f;
                        brls::Logger::debug("Drag: activated at displayIdx={} trackIdx={} fingerY={:.1f}",
                            m_dragState.originalDisplayIdx, m_dragState.draggedTrackIdx, status.position.y);
                        // Compute scroll view's absolute screen Y from the row's
                        // known content position and its absolute screen position
                        float rowContentY = m_dragState.originalDisplayIdx * ROW_HEIGHT_PX + 4.0f;
                        m_dragState.scrollViewTop = row->getY() - rowContentY + m_dragState.dragStartScrollY;
                        // Visual feedback: elevate the dragged row
                        row->setBackgroundColor(nvgRGBA(90, 110, 220, 160));
                    }
                }

                if (m_dragState.scrollPassthrough) {
                    if (queueScroll) {
                        // Dead zone: ignore small movements so the list doesn't
                        // jump the instant the finger twitches
                        constexpr float SCROLL_DEAD_ZONE = 12.0f;
                        if (std::abs(deltaY) < SCROLL_DEAD_ZONE)
                            return;

                        // Dampen so scrolling feels more natural on the small screen
                        constexpr float SCROLL_DAMPING = 0.55f;
                        float adjusted = (deltaY > 0)
                            ? (deltaY - SCROLL_DEAD_ZONE) * SCROLL_DAMPING
                            : (deltaY + SCROLL_DEAD_ZONE) * SCROLL_DAMPING;

                        float newOffset = m_dragState.initialScrollY - adjusted;

                        // Clamp to valid range
                        if (newOffset < 0) newOffset = 0;
                        float scrollViewHeight = queueScroll->getHeight();
                        int numRows = queueList ? (int)queueList->getChildren().size() : 0;
                        float contentHeight = numRows * ROW_HEIGHT_PX + 8.0f; // +8 for padding
                        float maxScroll = contentHeight - scrollViewHeight;
                        if (maxScroll < 0) maxScroll = 0;
                        if (newOffset > maxScroll) newOffset = maxScroll;

                        queueScroll->setContentOffsetY(newOffset, false);

                        // Auto-expand the queue window when scrolling near the
                        // bottom of rendered rows (focus events don't fire during
                        // touch scroll, so expansion must be triggered here)
                        if (numRows > 0 && m_queueWindowEnd < m_queueTotalCount) {
                            float bottomVisible = newOffset + scrollViewHeight;
                            float triggerY = (numRows - QUEUE_EXPAND_TRIGGER) * ROW_HEIGHT_PX;
                            if (bottomVisible >= triggerY) {
                                brls::Logger::debug("Scroll: expanding window at row {} (windowEnd={} total={})",
                                    numRows, m_queueWindowEnd, m_queueTotalCount);
                                expandQueueWindow(1);
                            }
                        }
                    }
                    return;
                }

                if (!m_dragState.holdMet) return;

                // -- Drag mode: dragged row follows finger --
                float dragDelta = status.position.y - m_dragState.dragStartY;
                float scrollDelta = queueScroll
                    ? (queueScroll->getContentOffsetY() - m_dragState.dragStartScrollY) : 0.0f;

                row->setTranslationY(dragDelta + scrollDelta);
                float effectiveDelta = dragDelta + scrollDelta;

                // Calculate which display position the finger is over
                int origIdx = m_dragState.originalDisplayIdx;
                if (origIdx < 0 || m_dragState.draggedTrackIdx < 0) return;

                MusicQueue& queue = MusicQueue::getInstance();
                int queueSize = queue.getQueueSize();

                // Determine target position based on how many rows the finger crossed
                int rowsOffset = 0;
                if (effectiveDelta > ROW_HEIGHT_PX * 0.5f) {
                    rowsOffset = (int)((effectiveDelta + ROW_HEIGHT_PX * 0.5f) / ROW_HEIGHT_PX);
                } else if (effectiveDelta < -ROW_HEIGHT_PX * 0.5f) {
                    rowsOffset = -(int)((-effectiveDelta + ROW_HEIGHT_PX * 0.5f) / ROW_HEIGHT_PX);
                }
                int newTarget = origIdx + rowsOffset;
                if (newTarget < 0) newTarget = 0;
                if (newTarget >= queueSize) newTarget = queueSize - 1;
                if (newTarget != m_dragState.targetDisplayIdx) {
                    brls::Logger::debug("Drag: target changed {} -> {} (delta={:.1f} rows={} queueSize={} childCount={})",
                        m_dragState.targetDisplayIdx, newTarget, effectiveDelta, rowsOffset,
                        queueSize, queueList ? (int)queueList->getChildren().size() : 0);
                }
                m_dragState.targetDisplayIdx = newTarget;

                // Auto-scroll when the finger is near the top/bottom
                // edge of the scroll view.
                constexpr float AUTO_SCROLL_EDGE = 40.0f;
                constexpr float AUTO_SCROLL_SPEED = 7.0f;
                if (queueScroll && queueList) {
                    float scrollY = queueScroll->getContentOffsetY();
                    float scrollViewHeight = queueScroll->getHeight();
                    // Use rendered row count (not total queue size) for scroll bounds
                    int numRendered = (int)queueList->getChildren().size();
                    float contentHeight = numRendered * ROW_HEIGHT_PX + 8.0f;
                    float maxScroll = contentHeight - scrollViewHeight;
                    if (maxScroll < 0) maxScroll = 0;

                    float fingerInView = status.position.y - m_dragState.scrollViewTop;

                    if (fingerInView > scrollViewHeight - AUTO_SCROLL_EDGE
                        && scrollY < maxScroll) {
                        // Finger near bottom edge - scroll down
                        float newScroll = scrollY + AUTO_SCROLL_SPEED;
                        if (newScroll > maxScroll) newScroll = maxScroll;
                        queueScroll->setContentOffsetY(newScroll, false);
                        // Expand window if nearing the end of rendered rows
                        if (numRendered > 0 && m_queueWindowEnd < m_queueTotalCount) {
                            float bottomVisible = newScroll + scrollViewHeight;
                            float triggerY = (numRendered - QUEUE_EXPAND_TRIGGER) * ROW_HEIGHT_PX;
                            if (bottomVisible >= triggerY) {
                                expandQueueWindow(1);
                            }
                        }
                    } else if (fingerInView < AUTO_SCROLL_EDGE
                               && scrollY > 0) {
                        // Finger near top edge - scroll up
                        float newScroll = scrollY - AUTO_SCROLL_SPEED;
                        if (newScroll < 0) newScroll = 0;
                        queueScroll->setContentOffsetY(newScroll, false);
                    }

                    // Re-read scroll delta after possible auto-scroll
                    scrollDelta = queueScroll->getContentOffsetY() - m_dragState.dragStartScrollY;
                    row->setTranslationY(dragDelta + scrollDelta);
                    effectiveDelta = dragDelta + scrollDelta;
                }

                // Shift displaced rows visually (no data changes yet)
                if (!queueList) return;
                auto& children = queueList->getChildren();
                for (int i = 0; i < (int)children.size(); i++) {
                    if (i == origIdx) continue; // dragged row handled above
                    float shift = 0.0f;
                    if (newTarget > origIdx) {
                        // Dragging down: rows between (origIdx, newTarget] shift up
                        if (i > origIdx && i <= newTarget) {
                            shift = -ROW_HEIGHT_PX;
                        }
                    } else if (newTarget < origIdx) {
                        // Dragging up: rows between [newTarget, origIdx) shift down
                        if (i >= newTarget && i < origIdx) {
                            shift = ROW_HEIGHT_PX;
                        }
                    }
                    children[i]->setTranslationY(shift);
                }
            } else if (status.state == brls::GestureState::END) {
                // Suppress tap/click that fires right after drag ends
                if (m_dragState.holdMet) {
                    m_dragState.justEnded = true;
                }

                // Perform the actual queue reorder now
                int origIdx = m_dragState.originalDisplayIdx;
                int targetIdx = m_dragState.targetDisplayIdx;
                if (m_dragState.holdMet && origIdx >= 0 && targetIdx >= 0 &&
                    origIdx != targetIdx && m_dragState.draggedTrackIdx >= 0) {
                    MusicQueue& queue = MusicQueue::getInstance();
                    bool isShuffled = queue.isShuffleEnabled();
                    const auto& sOrder = queue.getShuffleOrder();

                    int toTrackIdx = (isShuffled && targetIdx < (int)sOrder.size())
                                ? sOrder[targetIdx] : targetIdx;
                    brls::Logger::info("Drag: drop displayIdx {} -> {} (trackIdx {} -> {}, shuffled={})",
                        origIdx, targetIdx, m_dragState.draggedTrackIdx, toTrackIdx, isShuffled);

                    queue.moveTrack(m_dragState.draggedTrackIdx, toTrackIdx);

                    // The displaced rows are already visually in their new
                    // positions (shifted by setTranslationY during the drag).
                    // Commit that order into the layout via removeView/addView,
                    // then clear translations. Since layout and translation
                    // reset happen in the same frame, there's no visible snap.
                    reassignQueueRange(origIdx, targetIdx);
                    renumberQueueRows();
                    m_cachedQueueVersion = queue.getVersion();
                }

                // Clear translations after layout is committed - the views
                // are now at their correct layout positions
                if (queueList) {
                    for (auto* child : queueList->getChildren()) {
                        child->setTranslationY(0);
                    }
                }

                // Reset drag state
                m_dragState.active = false;
                m_dragState.draggedRow = nullptr;
                m_dragState.holdMet = false;
                m_dragState.originalDisplayIdx = -1;
                m_dragState.targetDisplayIdx = -1;
                m_dragState.draggedTrackIdx = -1;
                m_dragState.scrollPassthrough = false;
            } else if (status.state == brls::GestureState::FAILED) {
                // Suppress tap/click that fires right after drag ends
                if (m_dragState.holdMet) {
                    m_dragState.justEnded = true;
                }

                // Reset all visual translations
                if (queueList) {
                    auto& children = queueList->getChildren();
                    for (auto* child : children) {
                        child->setTranslationY(0);
                    }
                }

                // Restore background color
                auto it = m_queueRowData.find(row);
                if (it != m_queueRowData.end()) {
                    MusicQueue& queue = MusicQueue::getInstance();
                    if (it->second.trackIdx == queue.getCurrentIndex()) {
                        row->setBackgroundColor(nvgRGBA(70, 90, 210, 150));
                    } else {
                        row->setBackgroundColor(nvgRGBA(255, 255, 255, 8));
                    }
                } else {
                    row->setBackgroundColor(nvgRGBA(255, 255, 255, 8));
                }
                m_dragState.active = false;
                m_dragState.draggedRow = nullptr;
                m_dragState.holdMet = false;
                m_dragState.originalDisplayIdx = -1;
                m_dragState.targetDisplayIdx = -1;
                m_dragState.draggedTrackIdx = -1;
                m_dragState.scrollPassthrough = false;
            }
        }, brls::PanAxis::VERTICAL));

    // Click handler to play this track - defer to next frame to avoid
    // crash from modifying focus/views while gesture processing is active.
    // Suppress click if a drag just ended (prevents queue from closing after reorder).
    row->registerClickAction([this, row](brls::View* view) {
        if (m_dragState.justEnded) {
            m_dragState.justEnded = false;
            return true;
        }
        auto it = m_queueRowData.find(row);
        if (it != m_queueRowData.end()) {
            int idx = it->second.trackIdx;
            brls::sync([this, idx]() {
                playFromQueue(idx);
            });
        }
        return true;
    });
    row->addGestureRecognizer(new brls::TapGestureRecognizer(row));

    // Lazy-load nearby thumbnails when this row gains focus
    // Use dynamic lookup instead of captured index, since drag reordering
    // changes row positions and would make a captured index stale
    // Also auto-expand the queue window when focus nears the edges
    row->getFocusEvent()->subscribe([this, row](brls::View*) {
        int actualIdx = findQueueRowDisplayIndex(row);
        if (actualIdx >= 0) {
            loadQueueThumbsAroundIndex(actualIdx);
            // Auto-expand window when near the bottom edge
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
    if (!queueList || !queueOverlayTitle) return;
    if (m_queuePopulating) return;  // Prevent re-entrant calls
    m_queuePopulating = true;

    // Cancel any in-progress batched population
    m_queueBatchActive = false;

    // Transfer focus away from queue items before clearing to avoid destroying focused view
    if (!queueList->getChildren().empty() && queueOverlayTitle) {
        queueOverlayTitle->setFocusable(true);
        brls::Application::giveFocus(queueOverlayTitle);
    }

    // Clear existing items
    m_queueRowData.clear();
    queueList->clearViews();

    MusicQueue& queue = MusicQueue::getInstance();
    const auto& tracks = queue.getQueue();
    int currentIndex = queue.getCurrentIndex();
    bool shuffled = queue.isShuffleEnabled();
    const auto& shuffleOrder = queue.getShuffleOrder();

    // Calculate total queue duration
    int totalDuration = 0;
    for (const auto& t : tracks) totalDuration += t.duration;
    int totalMin = totalDuration / 60;
    int totalHrs = totalMin / 60;
    totalMin %= 60;

    // Set title with track count and total duration
    char titleBuf[96];
    if (totalHrs > 0) {
        snprintf(titleBuf, sizeof(titleBuf), "Queue - %d tracks (%dh %dm)%s",
                 (int)tracks.size(), totalHrs, totalMin, shuffled ? " - Shuffled" : "");
    } else {
        snprintf(titleBuf, sizeof(titleBuf), "Queue - %d tracks (%d min)%s",
                 (int)tracks.size(), totalMin, shuffled ? " - Shuffled" : "");
    }
    queueOverlayTitle->setText(std::string(titleBuf) + "\nHold & drag to reorder | Swipe left to remove | LB/RB to move");

    // Prepare deferred thumbnail loading - only load covers for visible rows
    m_deferredThumbs.clear();
    m_deferredThumbs.reserve(tracks.size());

    int count = (int)tracks.size();
    m_queueTotalCount = count;

    // Update cached version so reopening the overlay can skip the rebuild
    m_cachedQueueVersion = queue.getVersion();

    // Determine the render window - for large queues only render a window
    // around the current track to avoid creating thousands of views
    int focusDisplayIdx = shuffled ? queue.getShufflePosition() : currentIndex;
    if (focusDisplayIdx < 0) focusDisplayIdx = 0;

    if (count <= QUEUE_RENDER_LIMIT) {
        // Small queue: render everything
        m_queueWindowStart = 0;
        m_queueWindowEnd = count;
    } else {
        // Large queue: center window around current track
        m_queueWindowStart = std::max(0, focusDisplayIdx - 8);
        m_queueWindowEnd = std::min(count, m_queueWindowStart + QUEUE_RENDER_LIMIT);
        // Adjust start if we hit the end
        if (m_queueWindowEnd == count) {
            m_queueWindowStart = std::max(0, count - QUEUE_RENDER_LIMIT);
        }
    }

    int windowSize = m_queueWindowEnd - m_queueWindowStart;

    // For small windows, create all rows immediately (no batching needed)
    if (windowSize <= QUEUE_BATCH_SIZE) {
        for (int i = m_queueWindowStart; i < m_queueWindowEnd; i++) {
            int trackIdx = (shuffled && i < (int)shuffleOrder.size())
                            ? shuffleOrder[i] : i;
            if (trackIdx < 0 || trackIdx >= (int)tracks.size()) continue;
            const QueueItem& track = tracks[trackIdx];
            bool isCurrent = (trackIdx == currentIndex);
            createQueueRow(i, trackIdx, track, isCurrent);
        }

        // Load thumbnails for the initially visible window
        loadQueueThumbsAroundIndex(focusDisplayIdx - m_queueWindowStart);
        m_queuePopulating = false;
        return;
    }

    // For larger windows, snapshot the data and create rows in batches across frames
    m_queueBatchTracks.assign(tracks.begin(), tracks.end());
    m_queueBatchShuffleOrder.assign(shuffleOrder.begin(), shuffleOrder.end());
    m_queueBatchCurrentIndex = currentIndex;
    m_queueBatchShuffled = shuffled;
    m_queueBatchNext = m_queueWindowStart;
    m_queueBatchTotal = m_queueWindowEnd;
    m_queueBatchActive = true;

    // Create first batch immediately so the UI isn't empty
    populateQueueBatch();

    m_queuePopulating = false;
}

void PlayerActivity::populateQueueBatch() {
    if (!m_queueBatchActive || !queueList || m_destroying) return;

    int end = std::min(m_queueBatchNext + QUEUE_BATCH_SIZE, m_queueBatchTotal);

    for (int i = m_queueBatchNext; i < end; i++) {
        int trackIdx = (m_queueBatchShuffled && i < (int)m_queueBatchShuffleOrder.size())
                        ? m_queueBatchShuffleOrder[i] : i;
        if (trackIdx < 0 || trackIdx >= (int)m_queueBatchTracks.size()) continue;
        const QueueItem& track = m_queueBatchTracks[trackIdx];
        bool isCurrent = (trackIdx == m_queueBatchCurrentIndex);
        createQueueRow(i, trackIdx, track, isCurrent);
    }

    m_queueBatchNext = end;

    if (m_queueBatchNext >= m_queueBatchTotal) {
        // All rows created - finalize
        m_queueBatchActive = false;
        m_queueBatchTracks.clear();
        m_queueBatchShuffleOrder.clear();

        // Load thumbnails for the initially visible window
        MusicQueue& queue = MusicQueue::getInstance();
        int focusIdx = queue.isShuffleEnabled() ? queue.getShufflePosition() : queue.getCurrentIndex();
        // Convert absolute display index to child index within rendered window
        int childFocusIdx = focusIdx - m_queueWindowStart;
        loadQueueThumbsAroundIndex(childFocusIdx);

        // Give focus to the current track now that all rows exist
        if (m_queueOverlayVisible && queueList && !queueList->getChildren().empty()) {
            childFocusIdx = std::min(childFocusIdx, (int)queueList->getChildren().size() - 1);
            if (childFocusIdx < 0) childFocusIdx = 0;
            brls::Application::giveFocus(queueList->getChildren()[childFocusIdx]);
            if (queueOverlayTitle) queueOverlayTitle->setFocusable(false);
        }
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
        MusicQueue& queue = MusicQueue::getInstance();
        int count = (int)queue.getQueue().size();
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

    MusicQueue& queue = MusicQueue::getInstance();
    const auto& tracks = queue.getQueue();
    int count = (int)tracks.size();
    bool shuffled = queue.isShuffleEnabled();
    const auto& shuffleOrder = queue.getShuffleOrder();
    int currentIndex = queue.getCurrentIndex();

    int batchEnd = std::min(m_expandNext + QUEUE_EXPAND_BATCH, m_expandEnd);

    for (int i = m_expandNext; i < batchEnd; i++) {
        int trackIdx = (shuffled && i < (int)shuffleOrder.size())
                        ? shuffleOrder[i] : i;
        if (trackIdx < 0 || trackIdx >= count) continue;
        const QueueItem& track = tracks[trackIdx];
        bool isCurrent = (trackIdx == currentIndex);
        createQueueRow(i, trackIdx, track, isCurrent);
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
    // Queue scroll is 320px with ~62px rows = ~5 visible rows
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
        if (dt.thumbPath.empty() && dt.ratingKey.empty()) continue;

        dt.loaded = true;

        // Load from server
        if (!dt.thumbPath.empty()) {
            std::string thumbUrl = client.getThumbnailUrl(dt.thumbPath, 100, 100, dt.thumbProvider);
            ImageLoader::loadAsync(thumbUrl, [](brls::Image* image) {
                // Thumbnail loaded
            }, dt.image, m_alive);
        }
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

void PlayerActivity::swapQueueRows(int displayIdxA, int displayIdxB, bool skipThumbReload) {
    if (!queueList) return;
    auto& children = queueList->getChildren();
    if (displayIdxA < 0 || displayIdxA >= (int)children.size()) return;
    if (displayIdxB < 0 || displayIdxB >= (int)children.size()) return;
    if (displayIdxA == displayIdxB) return;

    brls::Box* rowA = (brls::Box*)children[displayIdxA];
    brls::Box* rowB = (brls::Box*)children[displayIdxB];

    // Row structure: [thumb(Image)] [textBox(Box)] [durLbl(Label, optional)]
    // textBox children: [titleLbl(Label)] [artistLbl(Label, optional)]
    auto& childrenA = rowA->getChildren();
    auto& childrenB = rowB->getChildren();
    if (childrenA.size() < 2 || childrenB.size() < 2) return;

    MusicQueue& queue = MusicQueue::getInstance();

    // --- Swap QueueRowData between the two rows ---
    auto itA = m_queueRowData.find(rowA);
    auto itB = m_queueRowData.find(rowB);
    if (itA == m_queueRowData.end() || itB == m_queueRowData.end()) return;

    QueueRowData dataA = itA->second;
    QueueRowData dataB = itB->second;
    itA->second = dataB;
    itB->second = dataA;
    // Swap trackIdx back - moveTrack already rearranged the queue array
    // so each display position's trackIdx should stay pointing to its
    // corresponding queue slot (the items swapped in the queue too)
    std::swap(itA->second.trackIdx, itB->second.trackIdx);

    // --- Swap thumbnail images ---
    brls::Image* thumbA = (brls::Image*)childrenA[0];
    brls::Image* thumbB = (brls::Image*)childrenB[0];
    if (displayIdxA < (int)m_deferredThumbs.size() &&
        displayIdxB < (int)m_deferredThumbs.size()) {
        auto& dtA = m_deferredThumbs[displayIdxA];
        auto& dtB = m_deferredThumbs[displayIdxB];
        // Swap deferred thumb entries (thumbPath, thumbProvider, ratingKey, loaded state)
        std::swap(dtA.thumbPath, dtB.thumbPath);
        std::swap(dtA.thumbProvider, dtB.thumbProvider);
        std::swap(dtA.ratingKey, dtB.ratingKey);
        std::swap(dtA.loaded, dtB.loaded);
        // Re-point image pointers to their current rows
        dtA.image = thumbA;
        dtB.image = thumbB;
        // Reload thumbnails to reflect the swap (skip during chained swaps
        // to avoid race conditions - caller will reload after all swaps)
        if (!skipThumbReload) {
            MAClient& swapClient = MAClient::instance();
            if (dtA.loaded && !dtA.thumbPath.empty()) {
                std::string urlA = swapClient.getThumbnailUrl(dtA.thumbPath, 100, 100, dtA.thumbProvider);
                ImageLoader::loadAsync(urlA, [](brls::Image*) {}, thumbA, m_alive);
            } else {
                thumbA->setImageFromRes("img/default_music.png");
            }
            if (dtB.loaded && !dtB.thumbPath.empty()) {
                std::string urlB = swapClient.getThumbnailUrl(dtB.thumbPath, 100, 100, dtB.thumbProvider);
                ImageLoader::loadAsync(urlB, [](brls::Image*) {}, thumbB, m_alive);
            } else {
                thumbB->setImageFromRes("img/default_music.png");
            }
        }
    }

    // --- Swap title and artist labels ---
    brls::Box* textBoxA = (brls::Box*)childrenA[1];
    brls::Box* textBoxB = (brls::Box*)childrenB[1];
    auto& textChildrenA = textBoxA->getChildren();
    auto& textChildrenB = textBoxB->getChildren();

    // Determine current-track status after the data swap
    bool isCurrA = (itA->second.trackIdx == queue.getCurrentIndex());
    bool isCurrB = (itB->second.trackIdx == queue.getCurrentIndex());

    // Update title label for row A (now has dataB's content)
    if (!textChildrenA.empty()) {
        brls::Label* titleLblA = (brls::Label*)textChildrenA[0];
        if (isCurrA) {
            titleLblA->setText("> " + itA->second.title);
            titleLblA->setTextColor(nvgRGB(170, 210, 255));
        } else {
            char numBuf[8];
            snprintf(numBuf, sizeof(numBuf), "%d. ", displayIdxA + m_queueWindowStart + 1);
            titleLblA->setText(numBuf + itA->second.title);
            titleLblA->setTextColor(nvgRGB(240, 240, 240));
        }
    }
    // Update artist label for row A
    if (textChildrenA.size() >= 2) {
        brls::Label* artistLblA = (brls::Label*)textChildrenA[1];
        // Get the artist from the queue data
        int tIdxA = itA->second.trackIdx;
        if (tIdxA >= 0 && tIdxA < queue.getQueueSize()) {
            const QueueItem& trackA = queue.getQueue()[tIdxA];
            artistLblA->setText(trackA.artist);
            artistLblA->setTextColor(isCurrA ? nvgRGBA(170, 210, 255, 180) : nvgRGB(170, 170, 170));
        }
    }

    // Update title label for row B (now has dataA's content)
    if (!textChildrenB.empty()) {
        brls::Label* titleLblB = (brls::Label*)textChildrenB[0];
        if (isCurrB) {
            titleLblB->setText("> " + itB->second.title);
            titleLblB->setTextColor(nvgRGB(170, 210, 255));
        } else {
            char numBuf[8];
            snprintf(numBuf, sizeof(numBuf), "%d. ", displayIdxB + m_queueWindowStart + 1);
            titleLblB->setText(numBuf + itB->second.title);
            titleLblB->setTextColor(nvgRGB(240, 240, 240));
        }
    }
    // Update artist label for row B
    if (textChildrenB.size() >= 2) {
        brls::Label* artistLblB = (brls::Label*)textChildrenB[1];
        int tIdxB = itB->second.trackIdx;
        if (tIdxB >= 0 && tIdxB < queue.getQueueSize()) {
            const QueueItem& trackB = queue.getQueue()[tIdxB];
            artistLblB->setText(trackB.artist);
            artistLblB->setTextColor(isCurrB ? nvgRGBA(170, 210, 255, 180) : nvgRGB(170, 170, 170));
        }
    }

    // --- Swap duration labels using queue data ---
    bool hasDurA = (childrenA.size() >= 3);
    bool hasDurB = (childrenB.size() >= 3);
    if (hasDurA && hasDurB) {
        brls::Label* durA = (brls::Label*)childrenA[2];
        brls::Label* durB = (brls::Label*)childrenB[2];
        // Get durations from queue data (itA/itB already swapped above)
        int tA = itA->second.trackIdx;
        int tB = itB->second.trackIdx;
        auto& tracks = queue.getQueue();
        if (tA >= 0 && tA < (int)tracks.size() && tracks[tA].duration > 0) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d:%02d", tracks[tA].duration / 60, tracks[tA].duration % 60);
            durA->setText(buf);
        }
        if (tB >= 0 && tB < (int)tracks.size() && tracks[tB].duration > 0) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d:%02d", tracks[tB].duration / 60, tracks[tB].duration % 60);
            durB->setText(buf);
        }
    }

    // --- Swap background/border colors (current track highlighting) ---
    if (isCurrA) {
        rowA->setBackgroundColor(nvgRGBA(70, 90, 210, 150));
        rowA->setBorderColor(nvgRGBA(120, 160, 255, 200));
        rowA->setBorderThickness(1.5f);
    } else {
        rowA->setBackgroundColor(nvgRGBA(255, 255, 255, 8));
        rowA->setBorderColor(nvgRGBA(0, 0, 0, 0));
        rowA->setBorderThickness(0);
    }
    if (isCurrB) {
        rowB->setBackgroundColor(nvgRGBA(70, 90, 210, 150));
        rowB->setBorderColor(nvgRGBA(120, 160, 255, 200));
        rowB->setBorderThickness(1.5f);
    } else {
        rowB->setBackgroundColor(nvgRGBA(255, 255, 255, 8));
        rowB->setBorderColor(nvgRGBA(0, 0, 0, 0));
        rowB->setBorderThickness(0);
    }

    queueList->invalidate();
}

void PlayerActivity::reassignQueueRange(int origIdx, int targetIdx) {
    if (!queueList) return;
    auto& children = queueList->getChildren();
    int childCount = (int)children.size();
    if (origIdx < 0 || origIdx >= childCount) return;
    if (targetIdx < 0 || targetIdx >= childCount) return;
    if (origIdx == targetIdx) return;

    MusicQueue& queue = MusicQueue::getInstance();
    int currentTrackIdx = queue.getCurrentIndex();
    bool shuffled = queue.isShuffleEnabled();
    const auto& shuffleOrder = queue.getShuffleOrder();

    // Move the dragged row widget using borealis's own API so the Yoga
    // layout engine properly recalculates positions. The widget keeps
    // its loaded cover texture - no re-fetch needed.
    brls::View* draggedView = children[origIdx];
    queueList->removeView(draggedView, false);  // detach without deleting
    // After removal, indices above origIdx shift down by 1.
    // To land at the correct final position we must adjust:
    // Moving down (orig < target): target was shifted down, so insert at target
    //   (the gap closes above, target-1 is now correct but addView inserts
    //    BEFORE the element at that index, so we still use target)
    // Moving up (orig > target): nothing above target shifted, insert at target
    queueList->addView(draggedView, (size_t)targetIdx);

    // Rotate the deferred thumbnails to stay in sync with children order
    int rangeStart = std::min(origIdx, targetIdx);
    int rangeEnd = std::max(origIdx, targetIdx);
    if (rangeEnd < (int)m_deferredThumbs.size()) {
        if (origIdx < targetIdx) {
            std::rotate(m_deferredThumbs.begin() + origIdx,
                         m_deferredThumbs.begin() + origIdx + 1,
                         m_deferredThumbs.begin() + targetIdx + 1);
        } else {
            std::rotate(m_deferredThumbs.begin() + targetIdx,
                         m_deferredThumbs.begin() + origIdx,
                         m_deferredThumbs.begin() + origIdx + 1);
        }
    }

    // Update lightweight metadata for each row in the affected range:
    // QueueRowData trackIdx and current-track highlight colors.
    // The view content (cover, title text, artist, duration) moved with
    // the widget - we only fix the metadata mapping.
    for (int di = rangeStart; di <= rangeEnd && di < (int)children.size(); di++) {
        brls::Box* rowBox = (brls::Box*)children[di];

        int queueDisplayIdx = di + m_queueWindowStart;
        int trackIdx = (shuffled && queueDisplayIdx < (int)shuffleOrder.size())
                        ? shuffleOrder[queueDisplayIdx] : queueDisplayIdx;

        auto it = m_queueRowData.find(rowBox);
        if (it != m_queueRowData.end()) {
            it->second.trackIdx = trackIdx;
        }

        bool isCurr = (trackIdx == currentTrackIdx);
        if (isCurr) {
            rowBox->setBackgroundColor(nvgRGBA(70, 90, 210, 150));
            rowBox->setBorderColor(nvgRGBA(120, 160, 255, 200));
            rowBox->setBorderThickness(1.5f);
        } else {
            rowBox->setBackgroundColor(nvgRGBA(255, 255, 255, 8));
            rowBox->setBorderColor(nvgRGBA(0, 0, 0, 0));
            rowBox->setBorderThickness(0);
        }
    }

    brls::Logger::debug("Drag: moved row {} -> {} via removeView/addView (no re-fetch)", origIdx, targetIdx);
}

void PlayerActivity::renumberQueueRows() {
    if (!queueList) return;
    MusicQueue& queue = MusicQueue::getInstance();
    auto& children = queueList->getChildren();

    for (int i = 0; i < (int)children.size(); i++) {
        brls::View* child = children[i];
        auto it = m_queueRowData.find(child);
        if (it == m_queueRowData.end()) continue;

        bool isCurr = (it->second.trackIdx == queue.getCurrentIndex());
        const std::string& trackTitle = it->second.title;

        // Use window-offset display number so rows show correct position
        int displayNum = i + m_queueWindowStart + 1;

        auto& rowChildren = ((brls::Box*)child)->getChildren();
        if (rowChildren.size() >= 2) {
            auto& textBoxChildren = ((brls::Box*)rowChildren[1])->getChildren();
            if (!textBoxChildren.empty()) {
                brls::Label* titleLbl = (brls::Label*)textBoxChildren[0];
                if (isCurr) {
                    titleLbl->setText("> " + trackTitle);
                } else {
                    char numBuf[8];
                    snprintf(numBuf, sizeof(numBuf), "%d. ", displayNum);
                    titleLbl->setText(numBuf + trackTitle);
                }
            }
        }
    }
}

void PlayerActivity::removeQueueRow(int displayIdx) {
    if (!queueList) return;
    auto& children = queueList->getChildren();
    if (displayIdx < 0 || displayIdx >= (int)children.size()) return;

    // Remove from track index map
    brls::View* rowToRemove = children[displayIdx];
    m_queueRowData.erase(rowToRemove);

    // If the removed row has focus, transfer focus to a neighbor first
    if (brls::Application::getCurrentFocus() == rowToRemove) {
        if (displayIdx + 1 < (int)children.size()) {
            brls::Application::giveFocus(children[displayIdx + 1]);
        } else if (displayIdx - 1 >= 0) {
            brls::Application::giveFocus(children[displayIdx - 1]);
        } else if (queueOverlayTitle) {
            queueOverlayTitle->setFocusable(true);
            brls::Application::giveFocus(queueOverlayTitle);
        }
    }

    // Remove from deferred thumbnails list
    if (displayIdx < (int)m_deferredThumbs.size()) {
        m_deferredThumbs.erase(m_deferredThumbs.begin() + displayIdx);
    }

    // Remove the view from the list
    queueList->removeView(rowToRemove);

    // Update window tracking after removal
    if (m_queueWindowEnd > 0) m_queueWindowEnd--;
    m_queueTotalCount = MusicQueue::getInstance().getQueueSize();

    // Update track index mappings - indices in MusicQueue shifted after removeTrack
    // We need to update any entries that had track indices > the removed one
    // The removeTrack already happened, so indices have been adjusted in the queue
    // Rebuild the map from the queue's current state
    MusicQueue& queue = MusicQueue::getInstance();
    const auto& tracks = queue.getQueue();
    bool shuffled = queue.isShuffleEnabled();
    const auto& shuffleOrder = queue.getShuffleOrder();

    auto& remainingChildren = queueList->getChildren();
    for (int i = 0; i < (int)remainingChildren.size(); i++) {
        int queueIdx = i + m_queueWindowStart;
        int trackIdx = (shuffled && queueIdx < (int)shuffleOrder.size())
                        ? shuffleOrder[queueIdx] : queueIdx;
        if (trackIdx >= 0 && trackIdx < (int)tracks.size()) {
            m_queueRowData[remainingChildren[i]] = {trackIdx, tracks[trackIdx].title};
        }
    }

    // Update number labels on remaining rows using stored titles
    for (int i = displayIdx; i < (int)remainingChildren.size(); i++) {
        brls::View* child = remainingChildren[i];
        auto it = m_queueRowData.find(child);
        if (it == m_queueRowData.end()) continue;

        bool isCurr = (it->second.trackIdx == queue.getCurrentIndex());
        const std::string& trackTitle = it->second.title;
        int displayNum = i + m_queueWindowStart + 1;

        auto& rowChildren = ((brls::Box*)child)->getChildren();
        if (rowChildren.size() >= 2) {
            auto& textBoxChildren = ((brls::Box*)rowChildren[1])->getChildren();
            if (!textBoxChildren.empty()) {
                brls::Label* titleLbl = (brls::Label*)textBoxChildren[0];
                if (isCurr) {
                    titleLbl->setText("> " + trackTitle);
                } else {
                    char numBuf[8];
                    snprintf(numBuf, sizeof(numBuf), "%d. ", displayNum);
                    titleLbl->setText(numBuf + trackTitle);
                }
            }
        }
    }

    // Update title and sync cached version (rows were updated in-place)
    updateQueueTitle();
    m_cachedQueueVersion = queue.getVersion();
    queueList->invalidate();
}

void PlayerActivity::updateQueueTitle() {
    if (!queueOverlayTitle) return;

    MusicQueue& queue = MusicQueue::getInstance();
    const auto& tracks = queue.getQueue();
    bool shuffled = queue.isShuffleEnabled();

    int totalDuration = 0;
    for (const auto& t : tracks) totalDuration += t.duration;
    int totalMin = totalDuration / 60;
    int totalHrs = totalMin / 60;
    totalMin %= 60;

    char titleBuf[96];
    if (totalHrs > 0) {
        snprintf(titleBuf, sizeof(titleBuf), "Queue - %d tracks (%dh %dm)%s",
                 (int)tracks.size(), totalHrs, totalMin, shuffled ? " - Shuffled" : "");
    } else {
        snprintf(titleBuf, sizeof(titleBuf), "Queue - %d tracks (%d min)%s",
                 (int)tracks.size(), totalMin, shuffled ? " - Shuffled" : "");
    }
    queueOverlayTitle->setText(std::string(titleBuf) + "\nHold & drag to reorder | Swipe left to remove | LB/RB to move");
}

void PlayerActivity::playFromQueue(int index) {
    if (!m_isQueueMode) return;

    MusicQueue& queue = MusicQueue::getInstance();
    if (queue.playTrack(index)) {
        // Hide queue overlay first (safe - just changes visibility)
        hideQueueOverlay();

        // Stop current playback
        MpvPlayer::getInstance().stop();
        m_isPlaying = false;

        // Load selected track
        loadFromQueue();
    }
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

} // namespace vita_ma
