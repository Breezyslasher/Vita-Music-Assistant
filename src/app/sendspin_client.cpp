#include "app/sendspin_client.hpp"
#include "app/ma_client.hpp"
#include "player/mpv_player.hpp"
#include "app.h"
#include <borealis.hpp>
#include <cstring>
#include <sstream>

#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
#endif

namespace vita_ma {

SendspinClient& SendspinClient::instance() {
    static SendspinClient instance;
    return instance;
}

bool SendspinClient::connect(const std::string& serverIp, int sendspinPort,
                              const std::string& clientId, const std::string& clientName) {
    disconnect();

    m_clientId = clientId;
    m_clientName = clientName;

    setState(SendspinState::CONNECTING);
    brls::Logger::info("Sendspin: connecting to {}:{}/sendspin", serverIp, sendspinPort);

    // Start the local HTTP audio stream server before connecting
    if (!m_audioServer.isRunning()) {
        if (!m_audioServer.start()) {
            brls::Logger::error("Sendspin: failed to start audio stream server");
            setState(SendspinState::ERROR);
            return false;
        }
    }

    // Set up WebSocket callbacks
    m_ws.setOnMessage([this](const std::string& msg) {
        onTextMessage(msg);
    });
    m_ws.setOnBinary([this](const uint8_t* data, size_t size) {
        onBinaryData(data, size);
    });
    m_ws.setOnClose([this](int code, const std::string& reason) {
        onClose(code, reason);
    });

    // Connect to the MA server's Sendspin WebSocket port
    // No subprotocol - the Sendspin server doesn't use WebSocket subprotocols
    std::string url = "ws://" + serverIp + ":" + std::to_string(sendspinPort) + "/sendspin";
    if (!m_ws.connect(url)) {
        brls::Logger::error("Sendspin: connection failed to {}", url);
        setState(SendspinState::ERROR);
        return false;
    }

    // Send the client/hello handshake
    setState(SendspinState::HANDSHAKING);
    sendClientHello();

    return true;
}

void SendspinClient::disconnect() {
    if (m_state.load() == SendspinState::DISCONNECTED) return;

    brls::Logger::info("Sendspin: disconnecting");
    m_ws.disconnect();
    m_audioServer.signalStreamEnd();
    setState(SendspinState::DISCONNECTED);
}

void SendspinClient::stopStream() {
    m_audioServer.signalStreamEnd();
    if (m_state.load() == SendspinState::STREAMING ||
        m_state.load() == SendspinState::BUFFERING) {
        setState(SendspinState::CONNECTED);
    }
}

void SendspinClient::sendClientHello() {
    // Build client/hello message per Sendspin protocol spec
    // We declare ourselves as a player that supports FLAC and PCM
    Json msg;
    msg["type"] = Json("client/hello");

    Json payload;
    payload["client_id"] = Json(m_clientId);
    payload["name"] = Json(m_clientName);
    payload["version"] = Json(1);

    // Supported roles - we are a player
    Json roles(Json::ARRAY);
    roles.push_back(Json("player@v1"));
    payload["supported_roles"] = roles;

    // Player support - declare supported audio formats
    Json playerSupport;

    Json formats(Json::ARRAY);

    // FLAC - lossless, preferred for local network
    Json flacFmt;
    flacFmt["codec"] = Json("flac");
    flacFmt["channels"] = Json(2);
    flacFmt["sample_rate"] = Json(44100);
    flacFmt["bit_depth"] = Json(16);
    formats.push_back(flacFmt);

    // PCM fallback
    Json pcmFmt;
    pcmFmt["codec"] = Json("pcm");
    pcmFmt["channels"] = Json(2);
    pcmFmt["sample_rate"] = Json(44100);
    pcmFmt["bit_depth"] = Json(16);
    formats.push_back(pcmFmt);

    playerSupport["supported_formats"] = formats;
    playerSupport["buffer_capacity"] = Json(4 * 1024 * 1024); // 4MB buffer
    Json cmds(Json::ARRAY);
    cmds.push_back(Json("volume"));
    cmds.push_back(Json("mute"));
    playerSupport["supported_commands"] = cmds;

    payload["player@v1_support"] = playerSupport;
    msg["payload"] = payload;

    brls::Logger::info("Sendspin: sending client/hello as '{}'", m_clientName);
    m_ws.send(msg.dump());
}

void SendspinClient::onTextMessage(const std::string& message) {
    if (message.empty() || message[0] != '{') return;

    Json msg = Json::parse(message);
    if (!msg.has("type")) return;

    std::string type = msg["type"].str();

    if (type == "server/hello") {
        // Handshake complete - we are now registered as a player
        Json payload = msg.has("payload") ? msg["payload"] : msg;
        std::string serverName = payload.has("name") ? payload["name"].str() : "unknown";
        brls::Logger::info("Sendspin: registered with server '{}'", serverName);

        // Send initial player state
        Json stateMsg;
        stateMsg["type"] = Json("client/state");
        Json statePayload;
        Json playerState;
        playerState["state"] = Json("synchronized");
        playerState["volume"] = Json(100);
        playerState["muted"] = Json(false);
        statePayload["player"] = playerState;
        stateMsg["payload"] = statePayload;
        m_ws.send(stateMsg.dump());

        setState(SendspinState::CONNECTED);

        // Store the player ID for use with play_media
        brls::sync([this]() {
            App::instance().setPlayerId(m_clientId);
        });
    }
    else if (type == "stream/start") {
        // Server is starting to stream audio to us
        Json payload = msg.has("payload") ? msg["payload"] : msg;
        Json player = payload.has("player") ? payload["player"] : payload;

        if (player.has("codec")) m_format.codec = player["codec"].str();
        if (player.has("sample_rate")) m_format.sample_rate = player["sample_rate"].intVal();
        if (player.has("channels")) m_format.channels = player["channels"].intVal();
        if (player.has("bit_depth")) m_format.bit_depth = player["bit_depth"].intVal();

        brls::Logger::info("Sendspin: stream/start - {} {}Hz {}ch {}bit",
            m_format.codec, m_format.sample_rate, m_format.channels, m_format.bit_depth);

        // Reset the audio server for the new stream
        m_audioServer.resetStream();

        setState(SendspinState::BUFFERING);

        // Start MPV playback from the local HTTP stream server.
        // MPV will connect to the local server and block-read audio data
        // as it arrives from Sendspin binary frames.
        startMpvPlayback();
    }
    else if (type == "stream/end") {
        brls::Logger::info("Sendspin: stream/end");
        // Signal that no more data will arrive for this stream
        m_audioServer.signalStreamEnd();

        if (m_state.load() == SendspinState::STREAMING ||
            m_state.load() == SendspinState::BUFFERING) {
            setState(SendspinState::CONNECTED);
        }
    }
    else if (type == "stream/clear") {
        brls::Logger::info("Sendspin: stream/clear - clearing buffers");
        m_audioServer.resetStream();
    }
    else if (type == "server/time") {
        // Time sync response - send back our time
        Json timeMsg;
        timeMsg["type"] = Json("client/time");
        Json timePayload;
        auto now = std::chrono::steady_clock::now();
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(
            now.time_since_epoch()).count();
        timePayload["client_transmitted"] = Json(static_cast<int>(us & 0x7FFFFFFF));
        timeMsg["payload"] = timePayload;
        m_ws.send(timeMsg.dump());
    }
    else if (type == "server/command") {
        // Server command (e.g., volume change)
        Json payload = msg.has("payload") ? msg["payload"] : msg;
        if (payload.has("player")) {
            Json playerCmd = payload["player"];
            if (playerCmd.has("volume")) {
                int vol = playerCmd["volume"].intVal();
                brls::Logger::info("Sendspin: volume command: {}", vol);
            }
        }
    }
    else if (type == "group/update") {
        // Group update - ignore for now, Vita is a single player
    }
    else {
        brls::Logger::debug("Sendspin: unhandled message type: {}", type);
    }
}

void SendspinClient::onBinaryData(const uint8_t* data, size_t size) {
    // This runs on the WebSocket receive thread - must be thread-safe
    if (size < SENDSPIN_HEADER_SIZE) return;

    // Parse binary header: 1 byte type + 8 bytes timestamp (big-endian)
    uint8_t msgType = data[0];

    if (msgType == static_cast<uint8_t>(SendspinBinaryType::AUDIO_CHUNK)) {
        // Audio data - skip the 9-byte header
        const uint8_t* audioData = data + SENDSPIN_HEADER_SIZE;
        size_t audioSize = size - SENDSPIN_HEADER_SIZE;

        if (audioSize > 0) {
            // Push audio data to the local HTTP server's queue.
            // MPV reads from the HTTP server on its own thread.
            m_audioServer.pushAudioData(audioData, audioSize);

            // Transition from buffering to streaming on first audio data
            if (m_state.load() == SendspinState::BUFFERING) {
                setState(SendspinState::STREAMING);
            }
        }
    }
    // Ignore artwork and visualization data for now
}

void SendspinClient::onClose(int code, const std::string& reason) {
    brls::Logger::info("Sendspin: connection closed ({}: {})", code, reason);
    m_audioServer.signalStreamEnd();
    setState(SendspinState::DISCONNECTED);
}

void SendspinClient::setState(SendspinState state) {
    m_state.store(state);
    if (m_stateCallback) {
        brls::sync([this, state]() {
            m_stateCallback(state);
        });
    }
}

std::string SendspinClient::getLocalStreamUrl() const {
    return m_audioServer.getStreamUrl(m_format.codec);
}

void SendspinClient::startMpvPlayback() {
    std::string url = getLocalStreamUrl();
    brls::Logger::info("Sendspin: starting MPV with {}", url);

    brls::sync([url]() {
        auto& mpv = MpvPlayer::getInstance();
        mpv.setAudioOnly(true);
        if (!mpv.isInitialized()) {
            mpv.init();
        }
        mpv.loadUrl(url);
    });
}

} // namespace vita_ma
