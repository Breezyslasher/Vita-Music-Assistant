#pragma once

#include "app/websocket_client.hpp"
#include <string>
#include <functional>
#include <mutex>
#include <atomic>
#include <vector>

namespace vita_ma {

// Sendspin is Music Assistant's native streaming protocol.
// The Vita connects to the MA server's Sendspin port (8927) and registers
// as a player. The server then streams audio data over WebSocket binary
// frames when playback is triggered via player_queues/play_media.
//
// Protocol flow:
// 1. Client connects to ws://server_ip:8927
// 2. Client sends client/hello with player role and supported formats
// 3. Server responds with server/hello - client is now a registered player
// 4. User triggers playback via player_queues/play_media with this player's ID
// 5. Server sends stream/start with audio format info
// 6. Server sends binary audio frames (9-byte header + audio data)
// 7. Client writes audio to pipe that MPV reads from

enum class SendspinState {
    DISCONNECTED,
    CONNECTING,
    HANDSHAKING,    // Waiting for server/hello
    CONNECTED,      // Registered as player, waiting for stream
    BUFFERING,
    STREAMING,
    PAUSED,
    ERROR
};

// Audio format info from Sendspin stream/start message
struct SendspinAudioFormat {
    std::string codec;       // "pcm", "flac", "opus"
    int sample_rate = 44100;
    int channels = 2;
    int bit_depth = 16;
};

// Sendspin binary message types (from protocol spec)
enum class SendspinBinaryType : uint8_t {
    AUDIO_CHUNK = 4,
    ARTWORK_0 = 8,
    ARTWORK_1 = 9,
    ARTWORK_2 = 10,
    ARTWORK_3 = 11,
    VISUALIZATION = 16
};

// Binary frame header: 1 byte type + 8 bytes timestamp (big-endian int64)
static constexpr size_t SENDSPIN_HEADER_SIZE = 9;

// Callback for stream state changes
using StreamStateCallback = std::function<void(SendspinState state)>;
// Callback for stream metadata (track change, etc.)
using StreamMetadataCallback = std::function<void(const std::string& key, const std::string& value)>;

class SendspinClient {
public:
    static SendspinClient& instance();

    // Connect to the MA server's Sendspin port and register as a player.
    // serverIp: MA server IP/hostname
    // sendspinPort: Sendspin port (default 8927)
    // clientId: unique identifier for this Vita player
    // clientName: display name for this player in MA
    bool connect(const std::string& serverIp, int sendspinPort,
                 const std::string& clientId, const std::string& clientName);

    // Disconnect from Sendspin
    void disconnect();

    // State
    SendspinState getState() const { return m_state.load(); }
    bool isConnected() const {
        auto s = m_state.load();
        return s == SendspinState::CONNECTED || s == SendspinState::STREAMING
            || s == SendspinState::BUFFERING || s == SendspinState::PAUSED;
    }
    bool isStreaming() const { return m_state.load() == SendspinState::STREAMING; }
    SendspinAudioFormat getAudioFormat() const { return m_format; }

    // The player_id to use with player_queues/play_media
    // This is the client_id we registered with
    const std::string& getPlayerId() const { return m_clientId; }

    // Callbacks
    void setStateCallback(StreamStateCallback cb) { m_stateCallback = std::move(cb); }
    void setMetadataCallback(StreamMetadataCallback cb) { m_metadataCallback = std::move(cb); }

    // Get the local pipe path for MPV to consume
    std::string getLocalStreamUrl() const;

    // Stop current stream playback (keeps connection alive)
    void stopStream();

private:
    SendspinClient() = default;

    WebSocketClient m_ws;
    std::atomic<SendspinState> m_state{SendspinState::DISCONNECTED};
    SendspinAudioFormat m_format;
    std::string m_clientId;
    std::string m_clientName;

    // Audio pipe for feeding to MPV
    std::string m_pipePath;
    int m_pipeFd = -1;
    std::mutex m_pipeMutex;

    // Callbacks
    StreamStateCallback m_stateCallback;
    StreamMetadataCallback m_metadataCallback;

    // Protocol handlers
    void onTextMessage(const std::string& message);
    void onBinaryData(const uint8_t* data, size_t size);
    void onClose(int code, const std::string& reason);
    void setState(SendspinState state);

    // Handshake
    void sendClientHello();

    // Audio pipe management
    bool initAudioPipe();
    void cleanupAudioPipe();
    void feedAudioData(const uint8_t* data, size_t size);
};

} // namespace vita_ma
