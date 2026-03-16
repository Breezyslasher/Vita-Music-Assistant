#include "app/sendspin_client.hpp"
#include "app/ma_client.hpp"
#include "player/mpv_player.hpp"
#include "app.h"
#include <borealis.hpp>
#include <cstring>
#include <sstream>
#include <mbedtls/base64.h>

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

        // Extract codec_header if provided (base64-encoded container header)
        // For FLAC this contains the fLaC magic + STREAMINFO metadata block
        // that MPV needs for format detection.
        m_format.codec_header.clear();
        if (player.has("codec_header")) {
            std::string b64 = player["codec_header"].str();
            if (!b64.empty()) {
                size_t decoded_len = 0;
                // First call to get required output size
                mbedtls_base64_decode(nullptr, 0, &decoded_len,
                    reinterpret_cast<const unsigned char*>(b64.c_str()), b64.size());
                if (decoded_len > 0) {
                    m_format.codec_header.resize(decoded_len);
                    size_t actual_len = 0;
                    int ret = mbedtls_base64_decode(m_format.codec_header.data(), decoded_len, &actual_len,
                        reinterpret_cast<const unsigned char*>(b64.c_str()), b64.size());
                    if (ret == 0) {
                        m_format.codec_header.resize(actual_len);
                        brls::Logger::info("Sendspin: got codec_header ({} bytes)", actual_len);
                    } else {
                        brls::Logger::error("Sendspin: failed to decode codec_header (ret={})", ret);
                        m_format.codec_header.clear();
                    }
                }
            }
        }

        // If no codec_header was provided for FLAC, synthesize one.
        // MPV needs the fLaC magic + STREAMINFO block for format probing.
        if (m_format.codec == "flac" && m_format.codec_header.empty()) {
            m_format.codec_header = buildFlacHeader(
                m_format.sample_rate, m_format.channels, m_format.bit_depth);
            brls::Logger::info("Sendspin: synthesized FLAC header ({} bytes)", m_format.codec_header.size());
        }

        brls::Logger::info("Sendspin: stream/start - {} {}Hz {}ch {}bit (header={}B)",
            m_format.codec, m_format.sample_rate, m_format.channels, m_format.bit_depth,
            m_format.codec_header.size());

        // Reset the audio server for the new stream and set the codec
        m_audioServer.resetStream();
        m_audioServer.setCodec(m_format.codec);

        // If we have a codec header, prepend it to the stream so MPV
        // sees a valid container when it starts format probing.
        if (!m_format.codec_header.empty()) {
            m_audioServer.setCodecHeader(m_format.codec_header);
        }

        setState(SendspinState::BUFFERING);

        // Don't start MPV yet - wait for initial audio data to buffer.
        // MPV needs data available immediately for format probing.
        // startMpvPlayback() will be called from onBinaryData() once
        // enough audio has buffered.
        m_mpvStarted = false;
        m_audioChunkCount = 0;
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

    // Only process audio data after stream/start has been received.
    // Without this guard, stale audio from a previous server queue
    // leaks in and starts MPV before format info is available.
    auto state = m_state.load();
    if (state != SendspinState::BUFFERING && state != SendspinState::STREAMING) return;

    // Parse binary header: 1 byte type + 8 bytes timestamp (big-endian)
    uint8_t msgType = data[0];

    if (msgType == static_cast<uint8_t>(SendspinBinaryType::AUDIO_CHUNK)) {
        // Audio data - skip the 9-byte header
        const uint8_t* audioData = data + SENDSPIN_HEADER_SIZE;
        size_t audioSize = size - SENDSPIN_HEADER_SIZE;

        if (audioSize > 0) {
            m_audioChunkCount++;
            if (m_audioChunkCount <= 3) {
                brls::Logger::debug("Sendspin: audio chunk #{} ({} bytes)", m_audioChunkCount, audioSize);
            }

            // Push audio data to the local HTTP server's queue.
            // MPV reads from the HTTP server on its own thread.
            m_audioServer.pushAudioData(audioData, audioSize);

            // Once enough audio has buffered, start MPV playback.
            // This ensures MPV has data for format probing when it connects.
            // Skip if local playback is disabled in settings.
            if (!m_mpvStarted && m_audioServer.hasInitialData()) {
                m_mpvStarted = true;
                if (App::instance().getSettings().localPlayback) {
                    startMpvPlayback();
                } else {
                    brls::Logger::info("Sendspin: local playback disabled, not starting MPV");
                }
            }

            // Transition from buffering to streaming
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

std::vector<uint8_t> SendspinClient::buildFlacHeader(int sampleRate, int channels, int bitDepth) {
    // Build a minimal FLAC file header: fLaC magic + STREAMINFO metadata block.
    // STREAMINFO is 34 bytes of data, preceded by a 4-byte metadata block header.
    // Total: 4 (magic) + 4 (block header) + 34 (STREAMINFO data) = 42 bytes.
    std::vector<uint8_t> header(42, 0);

    // "fLaC" magic
    header[0] = 'f'; header[1] = 'L'; header[2] = 'a'; header[3] = 'C';

    // Metadata block header: bit 7 = last-metadata-block flag (1), bits 6-0 = type (0 = STREAMINFO)
    // Byte 4: 0x80 = last block, type 0
    header[4] = 0x80;
    // Bytes 5-7: block data length = 34 (big-endian 24-bit)
    header[5] = 0x00;
    header[6] = 0x00;
    header[7] = 34;

    // STREAMINFO data (34 bytes starting at offset 8):
    // Bytes 0-1: minimum block size (samples) - use 4096 as default
    header[8] = 0x10; header[9] = 0x00;  // 4096
    // Bytes 2-3: maximum block size (samples) - use 4096 as default
    header[10] = 0x10; header[11] = 0x00; // 4096
    // Bytes 4-6: minimum frame size (0 = unknown)
    header[12] = 0; header[13] = 0; header[14] = 0;
    // Bytes 7-9: maximum frame size (0 = unknown)
    header[15] = 0; header[16] = 0; header[17] = 0;

    // Bytes 10-13 (+ 4 bits of 14): sample rate (20 bits), channels-1 (3 bits), bps-1 (5 bits), total samples high (4 bits)
    // Bits: ssssssss ssssssss ssssrrrr rbbbbbttt
    // Wait, the layout is:
    //   20 bits: sample rate
    //   3 bits:  channels - 1
    //   5 bits:  bits per sample - 1
    //   36 bits: total samples (0 = unknown)
    uint32_t sr = static_cast<uint32_t>(sampleRate);
    uint32_t ch = static_cast<uint32_t>(channels - 1) & 0x07;
    uint32_t bps = static_cast<uint32_t>(bitDepth - 1) & 0x1F;

    // Pack into bytes 18-21 (offsets 10-13 within STREAMINFO)
    header[18] = (sr >> 12) & 0xFF;
    header[19] = (sr >> 4) & 0xFF;
    header[20] = ((sr & 0x0F) << 4) | (ch << 1) | ((bps >> 4) & 0x01);
    header[21] = ((bps & 0x0F) << 4); // total samples high 4 bits = 0

    // Bytes 22-25: total samples low 32 bits = 0 (unknown)
    // Bytes 26-41: MD5 signature = all zeros (unknown)

    return header;
}

void SendspinClient::startMpvPlayback() {
    std::string url = getLocalStreamUrl();
    brls::Logger::info("Sendspin: starting MPV with {}", url);

    brls::sync([url]() {
        auto& mpv = MpvPlayer::getInstance();
        mpv.setAudioOnly(true);
        if (!mpv.isInitialized()) {
            mpv.init();
        } else {
            // Stop any existing playback to clear m_commandPending and
            // reset MPV state. Without this, a stuck LOADING state from
            // a previous failed stream blocks all future loads.
            auto state = mpv.getState();
            if (state != MpvPlayerState::IDLE) {
                brls::Logger::info("Sendspin: stopping MPV (was in state {}) before new load",
                    static_cast<int>(state));
                mpv.stop();
            }
        }
        mpv.loadUrl(url);
    });
}

} // namespace vita_ma
