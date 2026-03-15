#include "app/sendspin_client.hpp"
#include "app/ma_client.hpp"
#include "player/mpv_player.hpp"
#include "app.h"
#include <borealis.hpp>
#include <cstring>

#ifdef __vita__
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#endif

namespace vita_ma {

// ============================================================================
// SendspinClient
// ============================================================================

SendspinClient& SendspinClient::instance() {
    static SendspinClient instance;
    return instance;
}

bool SendspinClient::startStream(const std::string& sendspinUrl) {
    stopStream();

    setState(SendspinState::CONNECTING);
    brls::Logger::info("Sendspin: connecting to {}", sendspinUrl);

    // Initialize audio pipe for MPV
    if (!initAudioPipe()) {
        brls::Logger::error("Sendspin: failed to create audio pipe");
        setState(SendspinState::ERROR);
        return false;
    }

    // Set up WebSocket callbacks
    m_ws.setOnMessage([this](const std::string& msg) {
        onMessage(msg);
    });
    m_ws.setOnClose([this](int code, const std::string& reason) {
        onClose(code, reason);
    });

    // Connect to the Sendspin endpoint
    if (!m_ws.connect(sendspinUrl)) {
        brls::Logger::error("Sendspin: connection failed");
        cleanupAudioPipe();
        setState(SendspinState::ERROR);
        return false;
    }

    setState(SendspinState::BUFFERING);

    // Tell MPV to play from our pipe
    auto& mpv = MpvPlayer::instance();
    if (!mpv.isInitialized()) {
        mpv.init();
    }
    mpv.loadUrl(getLocalStreamUrl());

    return true;
}

void SendspinClient::stopStream() {
    if (m_state.load() == SendspinState::DISCONNECTED) return;

    brls::Logger::info("Sendspin: stopping stream");
    m_ws.disconnect();
    MpvPlayer::instance().stop();
    cleanupAudioPipe();
    setState(SendspinState::DISCONNECTED);
}

void SendspinClient::pauseStream() {
    if (m_state.load() == SendspinState::STREAMING) {
        MpvPlayer::instance().pause();
        setState(SendspinState::PAUSED);
    }
}

void SendspinClient::resumeStream() {
    if (m_state.load() == SendspinState::PAUSED) {
        MpvPlayer::instance().play();
        setState(SendspinState::STREAMING);
    }
}

std::string SendspinClient::getLocalStreamUrl() const {
    if (!m_pipePath.empty()) {
        return m_pipePath;
    }
    // Fallback: use stdin pipe protocol
    return "fd://0";
}

void SendspinClient::onMessage(const std::string& message) {
    // Sendspin protocol messages are JSON for control, binary for audio
    // Check if this is a control message
    if (message.empty()) return;

    if (message[0] == '{') {
        // JSON control message
        auto json = Json::parse(message);

        if (json.has("type")) {
            std::string type = json["type"].str();

            if (type == "audio_format") {
                // Server tells us the audio format
                if (json.has("codec")) m_format.codec = json["codec"].str();
                if (json.has("sample_rate")) m_format.sample_rate = json["sample_rate"].intVal();
                if (json.has("channels")) m_format.channels = json["channels"].intVal();
                if (json.has("bits_per_sample")) m_format.bits_per_sample = json["bits_per_sample"].intVal();
                if (json.has("bitrate")) m_format.bitrate = json["bitrate"].intVal();

                brls::Logger::info("Sendspin: audio format: {} {}Hz {}ch {}bit",
                    m_format.codec, m_format.sample_rate, m_format.channels, m_format.bits_per_sample);
            }
            else if (type == "stream_start") {
                setState(SendspinState::STREAMING);
                brls::Logger::info("Sendspin: stream started");
            }
            else if (type == "stream_end") {
                brls::Logger::info("Sendspin: stream ended");
                // Don't disconnect - server may send next track
            }
            else if (type == "metadata") {
                // Track metadata update
                if (m_metadataCallback) {
                    std::string key = json.has("key") ? json["key"].str() : "";
                    std::string value = json.has("value") ? json["value"].str() : "";
                    brls::sync([this, key, value]() {
                        m_metadataCallback(key, value);
                    });
                }
            }
            else if (type == "error") {
                std::string error = json.has("message") ? json["message"].str() : "Unknown error";
                brls::Logger::error("Sendspin: error: {}", error);
                setState(SendspinState::ERROR);
            }
        }
    } else {
        // Binary audio data (received as text frame - shouldn't happen normally)
        // Real binary data comes through a different path
        feedAudioData(reinterpret_cast<const uint8_t*>(message.data()), message.size());
    }
}

void SendspinClient::onClose(int code, const std::string& reason) {
    brls::Logger::info("Sendspin: connection closed ({}: {})", code, reason);
    setState(SendspinState::DISCONNECTED);
    cleanupAudioPipe();
}

void SendspinClient::setState(SendspinState state) {
    m_state.store(state);
    if (m_stateCallback) {
        brls::sync([this, state]() {
            m_stateCallback(state);
        });
    }
}

bool SendspinClient::initAudioPipe() {
#ifdef __vita__
    m_pipePath = "ux0:data/VitaMA/audio_pipe";
    // On Vita, we use a regular file as a circular buffer
    m_pipeFd = sceIoOpen(m_pipePath.c_str(),
        SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    return m_pipeFd >= 0;
#else
    m_pipePath = "/tmp/vita_ma_audio_pipe";
    // Remove existing pipe
    unlink(m_pipePath.c_str());
    // Create FIFO
    if (mkfifo(m_pipePath.c_str(), 0666) != 0) {
        return false;
    }
    // Open non-blocking for writing
    m_pipeFd = open(m_pipePath.c_str(), O_WRONLY | O_NONBLOCK);
    return m_pipeFd >= 0;
#endif
}

void SendspinClient::cleanupAudioPipe() {
    if (m_pipeFd >= 0) {
#ifdef __vita__
        sceIoClose(m_pipeFd);
#else
        close(m_pipeFd);
        unlink(m_pipePath.c_str());
#endif
        m_pipeFd = -1;
    }

    std::lock_guard<std::mutex> lock(m_bufferMutex);
    m_audioBuffer.clear();
}

void SendspinClient::feedAudioData(const uint8_t* data, size_t size) {
    if (m_pipeFd < 0 || size == 0) return;

    // Write to pipe for MPV consumption
#ifdef __vita__
    sceIoWrite(m_pipeFd, data, size);
#else
    write(m_pipeFd, data, size);
#endif

    // Also buffer for seeking/recovery
    {
        std::lock_guard<std::mutex> lock(m_bufferMutex);
        if (m_audioBuffer.size() + size > MAX_BUFFER_SIZE) {
            // Evict oldest data
            size_t evict = m_audioBuffer.size() + size - MAX_BUFFER_SIZE;
            m_audioBuffer.erase(m_audioBuffer.begin(), m_audioBuffer.begin() + evict);
        }
        m_audioBuffer.insert(m_audioBuffer.end(), data, data + size);
    }

    // Notify audio callback if set
    if (m_audioCallback) {
        m_audioCallback(data, size);
    }
}

// ============================================================================
// RemotePlaybackController
// ============================================================================

RemotePlaybackController& RemotePlaybackController::instance() {
    static RemotePlaybackController instance;
    return instance;
}

void RemotePlaybackController::registerAsPlayer(const std::string& playerId,
                                                  const std::string& playerName) {
    brls::Logger::info("RemotePlayback: registering as player '{}' ({})", playerName, playerId);

    // The MA server will see this Vita as a player that can receive streams
    // This is done through the MA client connection
    // The server sends play commands to this player, which triggers Sendspin streaming

    auto& app = App::instance();
    app.setPlayerId(playerId);
}

void RemotePlaybackController::playQueueLocally(const std::string& queueId) {
    brls::Logger::info("RemotePlayback: starting local playback for queue {}", queueId);
    m_currentQueueId = queueId;
    m_localPlayback.store(true);

    // Request stream URL from MA server
    MAClient::instance().getStreamUrl(queueId, [this](bool success, const Json& result) {
        if (!success) {
            brls::Logger::error("RemotePlayback: failed to get stream URL");
            m_localPlayback.store(false);
            return;
        }

        std::string streamUrl;
        if (result.type() == Json::STRING) {
            streamUrl = result.str();
        } else if (result.has("url")) {
            streamUrl = result["url"].str();
        }

        if (streamUrl.empty()) {
            brls::Logger::error("RemotePlayback: empty stream URL");
            m_localPlayback.store(false);
            return;
        }

        // If it's a direct HTTP stream URL, play directly with MPV
        if (streamUrl.find("http://") == 0 || streamUrl.find("https://") == 0) {
            brls::Logger::info("RemotePlayback: playing direct stream: {}", streamUrl);
            auto& mpv = MpvPlayer::instance();
            if (!mpv.isInitialized()) mpv.init();
            mpv.loadUrl(streamUrl);
        }
        // If it's a WebSocket Sendspin URL, use the Sendspin client
        else if (streamUrl.find("ws://") == 0 || streamUrl.find("wss://") == 0) {
            brls::Logger::info("RemotePlayback: starting Sendspin stream: {}", streamUrl);
            SendspinClient::instance().startStream(streamUrl);
        }
        else {
            brls::Logger::error("RemotePlayback: unknown stream URL format: {}", streamUrl);
            m_localPlayback.store(false);
        }
    });
}

void RemotePlaybackController::stopLocalPlayback() {
    if (!m_localPlayback.load()) return;

    brls::Logger::info("RemotePlayback: stopping local playback");
    SendspinClient::instance().stopStream();
    MpvPlayer::instance().stop();
    m_localPlayback.store(false);
    m_currentQueueId.clear();
}

} // namespace vita_ma
