#pragma once

#include <string>
#include <functional>
#include <mutex>
#include <atomic>

// Forward declare mpv handle
struct mpv_handle;

namespace vita_ma {

// Audio playback info
struct PlaybackInfo {
    std::string codec;
    std::string audio_format;
    int sample_rate = 0;
    int channels = 0;
    int bitrate = 0;
    float duration = 0.0f;
    float position = 0.0f;
    float volume = 100.0f;
    float cache_speed = 0.0f;
    bool paused = false;
    bool idle = true;
    bool buffering = false;
    bool seeking = false;
    bool eof = false;
};

// Playback event callback
using PlaybackCallback = std::function<void(const std::string& event)>;

class MpvPlayer {
public:
    static MpvPlayer& instance();

    // Lifecycle
    bool init();
    void destroy();
    bool isInitialized() const { return m_mpv != nullptr; }

    // Playback
    void loadUrl(const std::string& url);
    void play();
    void pause();
    void togglePause();
    void stop();
    void seek(float seconds);
    void seekRelative(float seconds);

    // Volume
    void setVolume(float vol);
    float getVolume() const;
    void setMute(bool muted);
    bool isMuted() const;

    // State
    PlaybackInfo getPlaybackInfo();
    bool isPlaying() const;
    bool isPaused() const;
    bool isIdle() const;

    // Process events (call from main thread)
    void processEvents();

    // Callbacks
    void setOnTrackEnd(PlaybackCallback cb) { m_onTrackEnd = std::move(cb); }
    void setOnError(PlaybackCallback cb) { m_onError = std::move(cb); }
    void setOnPropertyChange(PlaybackCallback cb) { m_onPropertyChange = std::move(cb); }

    // Background playback support
    void enableBackgroundPlayback(bool enable);

private:
    MpvPlayer() = default;
    ~MpvPlayer();

    mpv_handle* m_mpv = nullptr;
    mutable std::mutex m_mutex;

    // Cached state
    PlaybackInfo m_info;
    std::atomic<bool> m_playing{false};
    std::atomic<bool> m_paused{false};
    std::atomic<bool> m_idle{true};

    // Callbacks
    PlaybackCallback m_onTrackEnd;
    PlaybackCallback m_onError;
    PlaybackCallback m_onPropertyChange;
};

} // namespace vita_ma
