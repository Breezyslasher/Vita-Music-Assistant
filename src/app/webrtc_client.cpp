#include "app/webrtc_client.hpp"
#include <borealis.hpp>
#include <cstring>
#include <sstream>
#include <thread>
#include <chrono>

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

    if (m_relayMode) {
        sendViaRelay(message);
        return true;
    }

    // In true P2P mode, send through data channel
    // On Vita, we always use relay mode since we don't have native WebRTC
    sendViaRelay(message);
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
            Json connectMsg;
            connectMsg["type"] = Json("connect_to_peer");
            connectMsg["remote_id"] = Json(m_remoteId);
            connectMsg["client_type"] = Json("vita_music_assistant");
            connectMsg["auth_token"] = Json(m_authToken);
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
            setState(WebRTCState::CONNECTED);
            brls::Logger::info("WebRTC: data channel open (relay mode)");

            // Authenticate over the data channel
            Json authMsg;
            authMsg["message_id"] = Json("webrtc-auth");
            authMsg["command"] = Json("auth");
            Json args;
            args["token"] = Json(m_authToken);
            authMsg["args"] = args;
            sendViaRelay(authMsg.dump());
        }
        else if (type == "relay_message") {
            // Message relayed from the MA server through signaling
            if (msg.has("data")) {
                std::string data = msg["data"].str();
                handleDataChannelMessage(data);
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
    Json msg;
    msg["type"] = Json("request_relay");
    msg["remote_id"] = Json(m_remoteId);
    msg["client_id"] = Json(m_sessionId);
    m_signalingWs.send(msg.dump());

    brls::Logger::info("WebRTC: requested relay mode (no native WebRTC on Vita)");
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

void WebRTCClient::handleDataChannelMessage(const std::string& message) {
    // Forward received messages to the message callback
    // These are standard MA WebSocket API messages
    if (m_messageCallback) {
        brls::sync([this, message]() {
            m_messageCallback(message);
        });
    }
}

void WebRTCClient::setState(WebRTCState state) {
    m_state.store(state);
    if (m_stateCallback) {
        brls::sync([this, state]() {
            m_stateCallback(state);
        });
    }
}

void WebRTCClient::sendViaRelay(const std::string& message) {
    Json relayMsg;
    relayMsg["type"] = Json("relay_message");
    relayMsg["remote_id"] = Json(m_remoteId);
    relayMsg["data"] = Json(message);
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
