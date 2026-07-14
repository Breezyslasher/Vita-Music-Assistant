#include "app/webrtc_client.hpp"
#include "utils/http_client.hpp"
#include <borealis.hpp>

namespace vita_ma {

WebRTCClient& WebRTCClient::instance() {
    static WebRTCClient inst;
    return inst;
}

bool WebRTCClient::connectRemote(const std::string& remoteId) {
    if (m_state.load() != WebRTCState::DISCONNECTED) {
        disconnect();
    }

    m_remoteId = remoteId;
    setState(WebRTCState::CONNECTING_SIGNALING);
    brls::Logger::info("RemoteAccess: connecting to signaling for {}", remoteId);

    m_signalingWs.setOnMessage([this](const std::string& msg) {
        onSignalingMessage(msg);
    });
    m_signalingWs.setOnClose([this](int code, const std::string& reason) {
        brls::Logger::info("RemoteAccess: signaling closed ({}: {})", code, reason);
        if (m_state.load() != WebRTCState::ERROR) {
            setState(WebRTCState::DISCONNECTED);
        }
    });
    m_signalingWs.setOnError([this](const std::string& error) {
        brls::Logger::error("RemoteAccess: signaling error: {}", error);
        setState(WebRTCState::ERROR, "Signaling connection failed");
    });

    if (!m_signalingWs.connect(m_signalingUrl)) {
        setState(WebRTCState::ERROR, "Could not reach signaling server");
        return false;
    }

    // Ask the signaling server to pair us with the registered MA instance.
    Json msg;
    msg["type"] = Json(std::string("connect-request"));
    msg["remoteId"] = Json(m_remoteId);
    if (!m_signalingWs.send(msg.dump())) {
        setState(WebRTCState::ERROR, "Failed to send connect-request");
        return false;
    }
    setState(WebRTCState::SIGNALING_CONNECTED);
    return true;
}

void WebRTCClient::disconnect() {
    m_signalingWs.disconnect();
    m_sessionId.clear();
    setState(WebRTCState::DISCONNECTED);
}

void WebRTCClient::onSignalingMessage(const std::string& message) {
    Json msg = Json::parse(message);
    if (!msg.has("type")) return;
    std::string type = msg["type"].str();

    if (type == "connected") {
        // Signaling accepted the connect-request and the server is registered.
        if (msg.has("sessionId")) m_sessionId = msg["sessionId"].str();
        brls::Logger::info("RemoteAccess: paired with {} (session {})",
                           m_remoteId, m_sessionId);
    }
    else if (type == "session-ready") {
        // The gateway is ready for WebRTC negotiation. A real client would now
        // create an RTCPeerConnection with the provided iceServers and send an
        // SDP offer. The Vita has no ICE/DTLS/SCTP stack, so this is where the
        // connection honestly ends.
        if (msg.has("sessionId")) m_sessionId = msg["sessionId"].str();
        setState(WebRTCState::SESSION_READY);
        brls::Logger::error(
            "RemoteAccess: server {} is reachable, but completing the connection "
            "requires a WebRTC P2P transport (ICE/DTLS/SCTP), which is not yet "
            "available on PS Vita. Use a publicly reachable server URL (https://...) "
            "for remote playback instead.", m_remoteId);
        setState(WebRTCState::ERROR,
                 "Server online, but P2P transport is not supported on Vita yet");
        m_signalingWs.disconnect();
    }
    else if (type == "error") {
        std::string detail = msg.has("message") ? msg["message"].str()
                                                 : "Unknown signaling error";
        brls::Logger::error("RemoteAccess: signaling error: {}", detail);
        setState(WebRTCState::ERROR, detail);
    }
}

void WebRTCClient::setState(WebRTCState state, const std::string& detail) {
    m_state.store(state);
    if (m_stateCallback) {
        auto cb = m_stateCallback;
        brls::sync([cb, state, detail]() { cb(state, detail); });
    }
}

void WebRTCClient::checkRemoteOnline(const std::string& remoteId,
                                     std::function<void(bool reachable, bool online)> cb) {
    if (remoteId.empty()) {
        if (cb) cb(false, false);
        return;
    }
    std::string url = "https://signaling.music-assistant.io/api/check/" + remoteId;
    brls::async([url, cb]() {
        HttpClient client;
        HttpResponse resp = client.get(url);
        bool reachable = resp.success;
        bool online = false;
        if (resp.success && !resp.body.empty()) {
            Json result = Json::parse(resp.body);
            online = result.has("online") && result["online"].boolVal();
        }
        if (cb) {
            brls::sync([cb, reachable, online]() { cb(reachable, online); });
        }
    });
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
