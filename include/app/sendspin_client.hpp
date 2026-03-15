#pragma once

#include "app/websocket_client.hpp"
#include <string>
#include <functional>
#include <mutex>
#include <atomic>
#include <vector>

namespace vita_ma {

// Sendspin is Music Assistant's streaming protocol.
// It streams audio data over WebSocket (or WebRTC data channel) from the
// MA server to client devices for local playback.
//
// Flow:
// 1. Client requests a stream URL from MA server for a queue
// 2. Server returns a Sendspin WebSocket URL
// 3. Client connects to the Sendspin endpoint
// 4. Server sends audio frames (PCM, FLAC, or Opus encoded)
// 5. Client decodes and plays audio locally via MPV or direct audio output

enum class SendspinState {
    DISCONNECTED,
    CONNECTING,
    BUFFERING,
    STREAMING,
    PAUSED,
    ERROR
};

// Audio format info from Sendspin stream
struct SendspinAudioFormat {
    std::string codec;       // "pcm", "flac", "opus"
    int sample_rate = 44100;
    int channels = 2;
    int bits_per_sample = 16;
    int bitrate = 0;
};

// Callback for received audio data
using AudioDataCallback = std::function<void(const uint8_t* data, size_t size)>;
// Callback for stream state changes
using StreamStateCallback = std::function<void(SendspinState state)>;
// Callback for stream metadata (track change, etc.)
using StreamMetadataCallback = std::function<void(const std::string& key, const std::string& value)>;

class SendspinClient {
public:
    static SendspinClient& instance();

    // Start streaming from a Sendspin URL
    // The URL is obtained from MA server via player_queues/get_stream_url
    bool startStream(const std::string& sendspinUrl);

    // Stop streaming
    void stopStream();

    // Pause/resume stream
    void pauseStream();
    void resumeStream();

    // State
    SendspinState getState() const { return m_state.load(); }
    bool isStreaming() const { return m_state.load() == SendspinState::STREAMING; }
    SendspinAudioFormat getAudioFormat() const { return m_format; }

    // Callbacks
    void setAudioDataCallback(AudioDataCallback cb) { m_audioCallback = std::move(cb); }
    void setStateCallback(StreamStateCallback cb) { m_stateCallback = std::move(cb); }
    void setMetadataCallback(StreamMetadataCallback cb) { m_metadataCallback = std::move(cb); }

    // Get the local pipe/URL for MPV to consume
    // On Vita, we write audio to a pipe that MPV reads from
    std::string getLocalStreamUrl() const;

private:
    SendspinClient() = default;

    WebSocketClient m_ws;
    std::atomic<SendspinState> m_state{SendspinState::DISCONNECTED};
    SendspinAudioFormat m_format;

    // Audio buffer for feeding to MPV
    std::vector<uint8_t> m_audioBuffer;
    std::mutex m_bufferMutex;
    static constexpr size_t MAX_BUFFER_SIZE = 4 * 1024 * 1024; // 4MB

    // Callbacks
    AudioDataCallback m_audioCallback;
    StreamStateCallback m_stateCallback;
    StreamMetadataCallback m_metadataCallback;

    // Pipe file descriptor for MPV playback
    std::string m_pipePath;
    int m_pipeFd = -1;

    void onMessage(const std::string& message);
    void onBinaryData(const uint8_t* data, size_t size);
    void onClose(int code, const std::string& reason);
    void setState(SendspinState state);

    // Initialize the audio pipe for MPV
    bool initAudioPipe();
    void cleanupAudioPipe();

    // Write audio data to the pipe for MPV consumption
    void feedAudioData(const uint8_t* data, size_t size);
};

// Remote playback controller - handles the connection between
// MA server remote control and local Sendspin playback
class RemotePlaybackController {
public:
    static RemotePlaybackController& instance();

    // Register this Vita as a player with the MA server
    void registerAsPlayer(const std::string& playerId, const std::string& playerName);

    // Start playing a queue item locally via Sendspin
    void playQueueLocally(const std::string& queueId);

    // Stop local playback
    void stopLocalPlayback();

    // Is local playback active?
    bool isLocalPlaybackActive() const { return m_localPlayback.load(); }

private:
    RemotePlaybackController() = default;

    std::atomic<bool> m_localPlayback{false};
    std::string m_currentQueueId;
};

} // namespace vita_ma
