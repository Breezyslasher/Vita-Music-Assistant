#include "app/webrtc_client.hpp"
#include <borealis.hpp>
#include <cstring>
#include <sstream>
#include <thread>
#include <chrono>
#include <mbedtls/base64.h>

namespace vita_ma {

WebRTCClient& WebRTCClient::instance() {
    static WebRTCClient inst;
    return inst;
}

bool WebRTCClient::connectRemote(const std::string& remoteId, const std::string& authToken) {
    if (m_state.load() != WebRTCState::DISCONNECTED) {
        disconnect();
    }

    m_remoteId = remoteId;
    m_authToken = authToken;
    m_shouldReconnect.store(true);
    m_sendspinChannelOpen.store(false);

    setState(WebRTCState::CONNECTING_SIGNALING);
    brls::Logger::info("WebRTC: connecting to remote {}", remoteId);

    // Set up signaling WebSocket callbacks
    m_signalingWs.setOnMessage([this](const std::string& msg) {
        onSignalingMessage(msg);
    });
    m_signalingWs.setOnClose([this](int code, const std::string& reason) {
        onSignalingClose(code, reason);
    });
    m_signalingWs.setOnError([this](const std::string& error) {
        onSignalingError(error);
    });

    // Connect to signaling server
    if (!m_signalingWs.connect(m_signalingUrl)) {
        brls::Logger::error("WebRTC: failed to connect to signaling server");
        setState(WebRTCState::ERROR);
        return false;
    }

    return true;
}

void WebRTCClient::disconnect() {
    m_shouldReconnect.store(false);
    m_sendspinChannelOpen.store(false);
    m_signalingWs.disconnect();
    m_relayMode = false;
    m_sessionId.clear();
    m_localSdp.clear();
    m_remoteSdp.clear();
    m_localCandidates.clear();
    m_remoteCandidates.clear();
    setState(WebRTCState::DISCONNECTED);
}

bool WebRTCClient::sendMessage(const std::string& message) {
    if (m_state.load() != WebRTCState::CONNECTED) {
        brls::Logger::error("WebRTC: not connected, cannot send message");
        return false;
    }

    sendViaRelay(CHANNEL_MA_API, message);
    return true;
}

bool WebRTCClient::sendSendspinMessage(const std::string& message) {
    if (m_state.load() != WebRTCState::CONNECTED) {
        brls::Logger::error("WebRTC: not connected, cannot send sendspin message");
        return false;
    }

    sendViaRelay(CHANNEL_SENDSPIN, message);
    return true;
}

bool WebRTCClient::sendSendspinBinary(const uint8_t* data, size_t size) {
    if (m_state.load() != WebRTCState::CONNECTED) {
        return false;
    }

    sendBinaryViaRelay(CHANNEL_SENDSPIN, data, size);
    return true;
}

void WebRTCClient::onSignalingMessage(const std::string& message) {
    Json msg = Json::parse(message);

    if (msg.has("type")) {
        std::string type = msg["type"].str();

        if (type == "welcome") {
            // Signaling server accepted our connection
            setState(WebRTCState::SIGNALING_CONNECTED);
            brls::Logger::info("WebRTC: signaling connected, session: {}",
                msg.has("session_id") ? msg["session_id"].str() : "unknown");

            if (msg.has("session_id")) {
                m_sessionId = msg["session_id"].str();
            }

            // Register ourselves and request connection to remote peer
            // Declare both data channels: ma-api and sendspin
            Json connectMsg;
            connectMsg["type"] = Json("connect_to_peer");
            connectMsg["remote_id"] = Json(m_remoteId);
            connectMsg["client_type"] = Json("vita_music_assistant");
            connectMsg["auth_token"] = Json(m_authToken);

            Json channels(Json::ARRAY);
            channels.push_back(Json(CHANNEL_MA_API));
            channels.push_back(Json(CHANNEL_SENDSPIN));
            connectMsg["channels"] = channels;

            m_signalingWs.send(connectMsg.dump());

            setState(WebRTCState::NEGOTIATING);
        }
        else if (type == "peer_connected") {
            // The MA server peer is available
            brls::Logger::info("WebRTC: peer connected, sending offer");
            sendOffer();
        }
        else if (type == "answer") {
            // SDP answer from the MA server
            handleAnswer(msg);
        }
        else if (type == "ice_candidate") {
            // ICE candidate from the MA server
            handleIceCandidate(msg);
        }
        else if (type == "data_channel_open") {
            // Data channel is established (or relay mode activated)
            m_relayMode = true;
            m_sendspinChannelOpen.store(true);
            setState(WebRTCState::CONNECTED);
            brls::Logger::info("WebRTC: data channels open (relay mode) - ma-api + sendspin");

            // Authenticate over the ma-api data channel
            Json authMsg;
            authMsg["message_id"] = Json("webrtc-auth");
            authMsg["command"] = Json("auth");
            Json args;
            args["token"] = Json(m_authToken);
            authMsg["args"] = args;
            sendViaRelay(CHANNEL_MA_API, authMsg.dump());
        }
        else if (type == "relay_message") {
            // Message relayed from the MA server through signaling
            // Check which channel this message is for
            std::string channel = CHANNEL_MA_API; // default for backwards compat
            if (msg.has("channel")) {
                channel = msg["channel"].str();
            }

            if (msg.has("binary") && msg["binary"].boolVal()) {
                // Binary data (base64-encoded) - typically sendspin audio
                if (msg.has("data")) {
                    handleDataChannelBinary(channel, msg["data"].str());
                }
            } else if (msg.has("data")) {
                // Text message
                handleDataChannelMessage(channel, msg["data"].str());
            }
        }
        else if (type == "error") {
            std::string error = msg.has("message") ? msg["message"].str() : "Unknown signaling error";
            brls::Logger::error("WebRTC: signaling error: {}", error);

            if (msg.has("code") && msg["code"].str() == "peer_not_found") {
                brls::Logger::error("WebRTC: remote server {} not available", m_remoteId);
            }
            setState(WebRTCState::ERROR);
        }
        else if (type == "peer_disconnected") {
            brls::Logger::info("WebRTC: remote peer disconnected");
            m_sendspinChannelOpen.store(false);
            if (m_shouldReconnect.load()) {
                attemptReconnect();
            } else {
                setState(WebRTCState::DISCONNECTED);
            }
        }
    }
}

void WebRTCClient::onSignalingClose(int code, const std::string& reason) {
    brls::Logger::info("WebRTC: signaling closed ({}: {})", code, reason);
    m_sendspinChannelOpen.store(false);

    if (m_state.load() == WebRTCState::CONNECTED && m_shouldReconnect.load()) {
        setState(WebRTCState::RECONNECTING);
        attemptReconnect();
    } else {
        setState(WebRTCState::DISCONNECTED);
    }
}

void WebRTCClient::onSignalingError(const std::string& error) {
    brls::Logger::error("WebRTC: signaling error: {}", error);
}

void WebRTCClient::sendOffer() {
    // On PS Vita, we don't have native WebRTC.
    // Request the signaling server to set up relay mode instead.
    // Declare both channels so the server knows we support sendspin.
    Json msg;
    msg["type"] = Json("request_relay");
    msg["remote_id"] = Json(m_remoteId);
    msg["client_id"] = Json(m_sessionId);

    Json channels(Json::ARRAY);
    channels.push_back(Json(CHANNEL_MA_API));
    channels.push_back(Json(CHANNEL_SENDSPIN));
    msg["channels"] = channels;

    m_signalingWs.send(msg.dump());

    brls::Logger::info("WebRTC: requested relay mode with channels: ma-api, sendspin");
}

void WebRTCClient::handleAnswer(const Json& data) {
    if (data.has("sdp")) {
        m_remoteSdp = data["sdp"].str();
        brls::Logger::info("WebRTC: received SDP answer");
    }
}

void WebRTCClient::handleIceCandidate(const Json& data) {
    IceCandidate candidate;
    if (data.has("candidate")) candidate.candidate = data["candidate"].str();
    if (data.has("sdpMid")) candidate.sdpMid = data["sdpMid"].str();
    if (data.has("sdpMLineIndex")) candidate.sdpMLineIndex = data["sdpMLineIndex"].intVal();
    m_remoteCandidates.push_back(candidate);
    brls::Logger::info("WebRTC: received ICE candidate");
}

void WebRTCClient::handleDataChannelMessage(const std::string& channel, const std::string& data) {
    if (channel == CHANNEL_SENDSPIN) {
        // Forward to sendspin text message callback
        if (m_sendspinMessageCallback) {
            brls::sync([this, data]() {
                m_sendspinMessageCallback(data);
            });
        }
    } else {
        // Default: forward to ma-api message callback
        if (m_messageCallback) {
            brls::sync([this, data]() {
                m_messageCallback(data);
            });
        }
    }
}

void WebRTCClient::handleDataChannelBinary(const std::string& channel, const std::string& base64Data) {
    if (channel != CHANNEL_SENDSPIN || !m_sendspinBinaryCallback) {
        return;
    }

    // Decode base64 to binary
    size_t decodedLen = 0;
    mbedtls_base64_decode(nullptr, 0, &decodedLen,
        reinterpret_cast<const unsigned char*>(base64Data.c_str()), base64Data.size());

    if (decodedLen == 0) return;

    std::vector<uint8_t> decoded(decodedLen);
    size_t actualLen = 0;
    int ret = mbedtls_base64_decode(decoded.data(), decodedLen, &actualLen,
        reinterpret_cast<const unsigned char*>(base64Data.c_str()), base64Data.size());

    if (ret != 0) {
        brls::Logger::error("WebRTC: failed to decode sendspin binary data (ret={})", ret);
        return;
    }

    decoded.resize(actualLen);

    // Deliver binary data on the calling thread (receive thread) for low latency.
    // SendspinClient::onBinaryData is already thread-safe.
    m_sendspinBinaryCallback(decoded.data(), actualLen);
}

void WebRTCClient::setState(WebRTCState state) {
    m_state.store(state);
    if (m_stateCallback) {
        brls::sync([this, state]() {
            m_stateCallback(state);
        });
    }
}

void WebRTCClient::sendViaRelay(const std::string& channel, const std::string& message) {
    Json relayMsg;
    relayMsg["type"] = Json("relay_message");
    relayMsg["remote_id"] = Json(m_remoteId);
    relayMsg["channel"] = Json(channel);
    relayMsg["data"] = Json(message);
    m_signalingWs.send(relayMsg.dump());
}

void WebRTCClient::sendBinaryViaRelay(const std::string& channel, const uint8_t* data, size_t size) {
    // Base64-encode the binary data for transport over the text-based relay
    size_t encodedLen = 0;
    mbedtls_base64_encode(nullptr, 0, &encodedLen, data, size);

    std::vector<unsigned char> encoded(encodedLen);
    size_t actualLen = 0;
    int ret = mbedtls_base64_encode(encoded.data(), encodedLen, &actualLen, data, size);
    if (ret != 0) {
        brls::Logger::error("WebRTC: failed to base64-encode binary data (ret={})", ret);
        return;
    }

    Json relayMsg;
    relayMsg["type"] = Json("relay_message");
    relayMsg["remote_id"] = Json(m_remoteId);
    relayMsg["channel"] = Json(channel);
    relayMsg["data"] = Json(std::string(reinterpret_cast<char*>(encoded.data()), actualLen));
    relayMsg["binary"] = Json(true);
    m_signalingWs.send(relayMsg.dump());
}

void WebRTCClient::attemptReconnect() {
    setState(WebRTCState::RECONNECTING);
    brls::Logger::info("WebRTC: scheduling reconnect in 5s...");

    std::thread([this]() {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        if (m_shouldReconnect.load()) {
            brls::Logger::info("WebRTC: attempting reconnect to {}", m_remoteId);
            connectRemote(m_remoteId, m_authToken);
        }
    }).detach();
}

void WebRTCClient::fetchRemoteAccessInfo(MAClient& client,
    std::function<void(bool success, const RemoteAccessInfo& info)> cb) {
    client.sendCommand("remote_access/info", Json(), [cb](bool success, const Json& result) {
        RemoteAccessInfo info;
        if (success) {
            info.enabled = result.has("enabled") ? result["enabled"].boolVal() : false;
            info.connected = result.has("connected") ? result["connected"].boolVal() : false;
            info.remoteId = result.has("remote_id") ? result["remote_id"].str() : "";
            info.signalingUrl = result.has("signaling_url") ? result["signaling_url"].str() :
                "wss://signaling.music-assistant.io/ws";
        }
        if (cb) cb(success, info);
    });
}

} // namespace vita_ma
