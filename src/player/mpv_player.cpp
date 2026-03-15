#include "player/mpv_player.hpp"
#include <borealis.hpp>
#include <mpv/client.h>
#include <cstring>

#ifdef __vita__
#include <psp2/power.h>
#include <psp2/kernel/threadmgr.h>
#endif

namespace vita_ma {

// Property IDs for observation
enum PropertyId {
    PROP_IDLE = 1,
    PROP_PAUSE,
    PROP_DURATION,
    PROP_POSITION,
    PROP_VOLUME,
    PROP_CACHE_SPEED,
    PROP_BUFFERING,
    PROP_EOF,
    PROP_SEEKING,
    PROP_AUDIO_CODEC,
    PROP_AUDIO_PARAMS,
};

MpvPlayer& MpvPlayer::instance() {
    static MpvPlayer instance;
    return instance;
}

MpvPlayer::~MpvPlayer() {
    destroy();
}

bool MpvPlayer::init() {
    if (m_mpv) return true;

    m_mpv = mpv_create();
    if (!m_mpv) {
        brls::Logger::error("MPV: failed to create context");
        return false;
    }

    // Audio-only mode - no video output
    mpv_set_option_string(m_mpv, "vo", "null");
    mpv_set_option_string(m_mpv, "vid", "no");

    // Audio settings
    mpv_set_option_string(m_mpv, "audio-display", "no");
    mpv_set_option_string(m_mpv, "audio-channels", "stereo");

    // Cache settings for streaming
    mpv_set_option_string(m_mpv, "cache", "yes");
    mpv_set_option_string(m_mpv, "cache-secs", "30");
    mpv_set_option_string(m_mpv, "demuxer-max-bytes", "1MiB");
    mpv_set_option_string(m_mpv, "demuxer-max-back-bytes", "512KiB");

    // Network settings
    mpv_set_option_string(m_mpv, "network-timeout", "30");
    mpv_set_option_string(m_mpv, "stream-lavf-o", "reconnect=1,reconnect_streamed=1");

    // Decode settings (optimized for Vita ARM CPU)
    mpv_set_option_string(m_mpv, "audio-samplerate", "48000");

    // Reduce CPU usage
    mpv_set_option_string(m_mpv, "fps", "30");

#ifdef __vita__
    // Vita-specific audio settings
    mpv_set_option_string(m_mpv, "ao", "sdl");
    // Lower decode threads for ARM
    mpv_set_option_string(m_mpv, "ad-lavc-threads", "2");
#endif

    // Logging
    mpv_request_log_messages(m_mpv, "warn");

    int err = mpv_initialize(m_mpv);
    if (err < 0) {
        brls::Logger::error("MPV: init failed: {}", mpv_error_string(err));
        mpv_destroy(m_mpv);
        m_mpv = nullptr;
        return false;
    }

    // Observe properties
    mpv_observe_property(m_mpv, PROP_IDLE, "idle-active", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, PROP_PAUSE, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, PROP_DURATION, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, PROP_POSITION, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, PROP_VOLUME, "volume", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, PROP_CACHE_SPEED, "cache-speed", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, PROP_BUFFERING, "paused-for-cache", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, PROP_EOF, "eof-reached", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, PROP_SEEKING, "seeking", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, PROP_AUDIO_CODEC, "audio-codec-name", MPV_FORMAT_STRING);

    brls::Logger::info("MPV: initialized (audio-only mode)");
    return true;
}

void MpvPlayer::destroy() {
    if (m_mpv) {
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
    }
}

void MpvPlayer::loadUrl(const std::string& url) {
    if (!m_mpv) return;

    brls::Logger::info("MPV: loading {}", url);

    const char* cmd[] = {"loadfile", url.c_str(), "replace", nullptr};
    int err = mpv_command(m_mpv, cmd);
    if (err < 0) {
        brls::Logger::error("MPV: loadfile failed: {}", mpv_error_string(err));
        if (m_onError) m_onError("load_failed");
    }

#ifdef __vita__
    // Boost CPU for audio decode
    scePowerSetArmClockFrequency(333);
#endif
}

void MpvPlayer::play() {
    if (!m_mpv) return;
    int flag = 0;
    mpv_set_property(m_mpv, "pause", MPV_FORMAT_FLAG, &flag);
}

void MpvPlayer::pause() {
    if (!m_mpv) return;
    int flag = 1;
    mpv_set_property(m_mpv, "pause", MPV_FORMAT_FLAG, &flag);
}

void MpvPlayer::togglePause() {
    if (!m_mpv) return;
    const char* cmd[] = {"cycle", "pause", nullptr};
    mpv_command(m_mpv, cmd);
}

void MpvPlayer::stop() {
    if (!m_mpv) return;
    const char* cmd[] = {"stop", nullptr};
    mpv_command(m_mpv, cmd);

#ifdef __vita__
    // Reduce CPU when idle
    scePowerSetArmClockFrequency(266);
#endif
}

void MpvPlayer::seek(float seconds) {
    if (!m_mpv) return;
    std::string pos = std::to_string(seconds);
    const char* cmd[] = {"seek", pos.c_str(), "absolute", nullptr};
    mpv_command(m_mpv, cmd);
}

void MpvPlayer::seekRelative(float seconds) {
    if (!m_mpv) return;
    std::string pos = std::to_string(seconds);
    const char* cmd[] = {"seek", pos.c_str(), "relative", nullptr};
    mpv_command(m_mpv, cmd);
}

void MpvPlayer::setVolume(float vol) {
    if (!m_mpv) return;
    double v = static_cast<double>(vol);
    mpv_set_property(m_mpv, "volume", MPV_FORMAT_DOUBLE, &v);
}

float MpvPlayer::getVolume() const {
    if (!m_mpv) return 0.0f;
    double vol = 0;
    mpv_get_property(m_mpv, "volume", MPV_FORMAT_DOUBLE, &vol);
    return static_cast<float>(vol);
}

void MpvPlayer::setMute(bool muted) {
    if (!m_mpv) return;
    int flag = muted ? 1 : 0;
    mpv_set_property(m_mpv, "mute", MPV_FORMAT_FLAG, &flag);
}

bool MpvPlayer::isMuted() const {
    if (!m_mpv) return false;
    int flag = 0;
    mpv_get_property(m_mpv, "mute", MPV_FORMAT_FLAG, &flag);
    return flag != 0;
}

PlaybackInfo MpvPlayer::getPlaybackInfo() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_info;
}

bool MpvPlayer::isPlaying() const {
    return m_playing.load();
}

bool MpvPlayer::isPaused() const {
    return m_paused.load();
}

bool MpvPlayer::isIdle() const {
    return m_idle.load();
}

void MpvPlayer::processEvents() {
    if (!m_mpv) return;

    while (true) {
        mpv_event* event = mpv_wait_event(m_mpv, 0);
        if (event->event_id == MPV_EVENT_NONE) break;

        switch (event->event_id) {
            case MPV_EVENT_PROPERTY_CHANGE: {
                auto* prop = static_cast<mpv_event_property*>(event->data);

                std::lock_guard<std::mutex> lock(m_mutex);

                switch (event->reply_userdata) {
                    case PROP_IDLE:
                        if (prop->format == MPV_FORMAT_FLAG) {
                            m_info.idle = *static_cast<int*>(prop->data) != 0;
                            m_idle.store(m_info.idle);
                        }
                        break;
                    case PROP_PAUSE:
                        if (prop->format == MPV_FORMAT_FLAG) {
                            m_info.paused = *static_cast<int*>(prop->data) != 0;
                            m_paused.store(m_info.paused);
                            m_playing.store(!m_info.paused && !m_info.idle);
                        }
                        break;
                    case PROP_DURATION:
                        if (prop->format == MPV_FORMAT_DOUBLE) {
                            m_info.duration = static_cast<float>(*static_cast<double*>(prop->data));
                        }
                        break;
                    case PROP_POSITION:
                        if (prop->format == MPV_FORMAT_DOUBLE) {
                            m_info.position = static_cast<float>(*static_cast<double*>(prop->data));
                        }
                        break;
                    case PROP_VOLUME:
                        if (prop->format == MPV_FORMAT_DOUBLE) {
                            m_info.volume = static_cast<float>(*static_cast<double*>(prop->data));
                        }
                        break;
                    case PROP_CACHE_SPEED:
                        if (prop->format == MPV_FORMAT_DOUBLE) {
                            m_info.cache_speed = static_cast<float>(*static_cast<double*>(prop->data));
                        }
                        break;
                    case PROP_BUFFERING:
                        if (prop->format == MPV_FORMAT_FLAG) {
                            m_info.buffering = *static_cast<int*>(prop->data) != 0;
                        }
                        break;
                    case PROP_EOF:
                        if (prop->format == MPV_FORMAT_FLAG) {
                            m_info.eof = *static_cast<int*>(prop->data) != 0;
                        }
                        break;
                    case PROP_SEEKING:
                        if (prop->format == MPV_FORMAT_FLAG) {
                            m_info.seeking = *static_cast<int*>(prop->data) != 0;
                        }
                        break;
                    case PROP_AUDIO_CODEC:
                        if (prop->format == MPV_FORMAT_STRING) {
                            m_info.codec = *static_cast<char**>(prop->data);
                        }
                        break;
                }

                if (m_onPropertyChange) {
                    m_onPropertyChange("property_change");
                }
                break;
            }

            case MPV_EVENT_END_FILE: {
                auto* ef = static_cast<mpv_event_end_file*>(event->data);
                if (ef->reason == MPV_END_FILE_REASON_EOF) {
                    brls::Logger::info("MPV: track ended (EOF)");
                    if (m_onTrackEnd) m_onTrackEnd("eof");
                } else if (ef->reason == MPV_END_FILE_REASON_ERROR) {
                    brls::Logger::error("MPV: playback error: {}", mpv_error_string(ef->error));
                    if (m_onError) m_onError("playback_error");
                }
                break;
            }

            case MPV_EVENT_LOG_MESSAGE: {
                auto* msg = static_cast<mpv_event_log_message*>(event->data);
                if (msg->log_level <= MPV_LOG_LEVEL_WARN) {
                    brls::Logger::warning("MPV [{}]: {}", msg->prefix, msg->text);
                }
                break;
            }

            default:
                break;
        }
    }
}

void MpvPlayer::enableBackgroundPlayback(bool enable) {
#ifdef __vita__
    if (enable) {
        brls::Logger::info("MPV: background playback enabled");
        // On Vita, background audio continues when app is suspended
        // as long as audio output remains active and the app
        // registers as a background music app
    }
#endif
    (void)enable;
}

} // namespace vita_ma
