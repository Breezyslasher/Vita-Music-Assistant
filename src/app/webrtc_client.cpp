#include "app/webrtc_client.hpp"
#include "utils/http_client.hpp"
#include <borealis.hpp>
#include <rtc/rtc.hpp>
#include <chrono>
#include <cstring>

namespace vita_ma {

// ---------------------------------------------------------------------------
// Helpers

static bool hexDecode(const std::string& hex, std::string& out) {
    if (hex.size() % 2 != 0) return false;
    out.clear();
    out.reserve(hex.size() / 2);
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < hex.size(); i += 2) {
        int hi = nibble(hex[i]);
        int lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    return true;
}

bool WebRTCClient::isRemoteId(const std::string& s) {
    // MA-XXXX-XXXX (alphanumeric groups)
    if (s.size() != 12) return false;
    if (s[0] != 'M' || s[1] != 'A' || s[2] != '-' || s[7] != '-') return false;
    for (size_t i : {3, 4, 5, 6, 8, 9, 10, 11}) {
        char c = s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
    }
    return true;
}

WebRTCClient& WebRTCClient::instance() {
    static WebRTCClient inst;
    return inst;
}

// ---------------------------------------------------------------------------
// Signaling

bool WebRTCClient::connectRemote(const std::string& remoteId) {
    disconnect();

    m_remoteId = remoteId;
    setState(WebRTCState::CONNECTING_SIGNALING);
    brls::Logger::info("RemoteAccess: connecting to signaling for {}", remoteId);

    m_signalingWs.setOnMessage([this](const std::string& msg) {
        onSignalingMessage(msg);
    });
    m_signalingWs.setOnClose([this](int code, const std::string& reason) {
        brls::Logger::info("RemoteAccess: signaling closed ({}: {})", code, reason);
        // The signaling socket is only needed during negotiation; once the
        // peer connection is up, losing it is harmless.
        WebRTCState s = m_state.load();
        if (s == WebRTCState::CONNECTING_SIGNALING || s == WebRTCState::SIGNALING_CONNECTED ||
            s == WebRTCState::NEGOTIATING) {
            setState(WebRTCState::ERROR, "Signaling connection lost");
        }
    });
    m_signalingWs.setOnError([this](const std::string& error) {
        brls::Logger::error("RemoteAccess: signaling error: {}", error);
    });

    if (!m_signalingWs.connect(m_signalingUrl)) {
        setState(WebRTCState::ERROR, "Could not reach signaling server");
        return false;
    }

    Json msg;
    msg["type"] = Json("connect-request");
    msg["remoteId"] = Json(m_remoteId);
    if (!sendSignaling(msg)) {
        setState(WebRTCState::ERROR, "Failed to send connect-request");
        return false;
    }
    setState(WebRTCState::SIGNALING_CONNECTED);
    return true;
}

bool WebRTCClient::sendSignaling(const Json& msg) {
    return m_signalingWs.send(msg.dump());
}

void WebRTCClient::disconnect() {
    teardownPeerConnection();
    m_signalingWs.disconnect();
    m_sessionId.clear();
    // Fail any in-flight proxy fetches
    {
        std::lock_guard<std::mutex> lock(m_proxyMutex);
        for (auto& [id, pending] : m_proxyPending) {
            pending->done = true;
            pending->status = 0;
        }
        m_proxyPending.clear();
    }
    m_proxyCv.notify_all();
    setState(WebRTCState::DISCONNECTED);
}

void WebRTCClient::onSignalingMessage(const std::string& message) {
    Json msg = Json::parse(message);
    if (!msg.has("type")) return;
    std::string type = msg["type"].str();

    if (type == "connected") {
        if (msg.has("sessionId")) m_sessionId = msg["sessionId"].str();
        brls::Logger::info("RemoteAccess: paired with {} (session {})",
                           m_remoteId, m_sessionId);
    }
    else if (type == "session-ready") {
        if (msg.has("sessionId")) m_sessionId = msg["sessionId"].str();
        brls::Logger::info("RemoteAccess: session ready, starting WebRTC negotiation");
        setState(WebRTCState::NEGOTIATING);
        startPeerConnection(msg.has("iceServers") ? msg["iceServers"] : Json(Json::ARRAY));
    }
    else if (type == "answer") {
        std::string sdp, sdpType = "answer";
        if (msg.has("data")) {
            const Json& data = msg["data"];
            if (data.has("sdp")) sdp = data["sdp"].str();
            if (data.has("type")) sdpType = data["type"].str();
        }
        if (sdp.empty()) return;
        std::lock_guard<std::mutex> lock(m_rtcMutex);
        if (!m_pc) return;
        try {
            m_pc->setRemoteDescription(rtc::Description(sdp, sdpType));
            brls::Logger::info("RemoteAccess: applied SDP answer");
        } catch (const std::exception& e) {
            brls::Logger::error("RemoteAccess: bad answer: {}", e.what());
            setState(WebRTCState::ERROR, "Invalid SDP answer");
        }
    }
    else if (type == "ice-candidate") {
        std::string cand, mid;
        if (msg.has("data")) {
            const Json& data = msg["data"];
            if (data.has("candidate")) cand = data["candidate"].str();
            if (data.has("sdpMid")) mid = data["sdpMid"].str();
        }
        if (cand.empty()) return;
        std::lock_guard<std::mutex> lock(m_rtcMutex);
        if (!m_pc) return;
        try {
            m_pc->addRemoteCandidate(rtc::Candidate(cand, mid));
        } catch (const std::exception& e) {
            brls::Logger::warning("RemoteAccess: bad remote candidate: {}", e.what());
        }
    }
    else if (type == "error") {
        std::string detail = msg.has("message") ? msg["message"].str()
                                                 : "Unknown signaling error";
        brls::Logger::error("RemoteAccess: signaling error: {}", detail);
        setState(WebRTCState::ERROR, detail);
    }
}

// ---------------------------------------------------------------------------
// WebRTC peer connection

void WebRTCClient::startPeerConnection(const Json& iceServersJson) {
    rtc::Configuration config;

    // Parse ICE servers provided by the gateway (STUN, and TURN with HA Cloud)
    if (iceServersJson.type() == Json::ARRAY) {
        for (size_t i = 0; i < iceServersJson.size(); i++) {
            const Json& srv = iceServersJson[i];
            std::string username = srv.has("username") ? srv["username"].str() : "";
            std::string credential = srv.has("credential") ? srv["credential"].str() : "";

            std::vector<std::string> urls;
            if (srv.has("urls")) {
                const Json& u = srv["urls"];
                if (u.type() == Json::ARRAY) {
                    for (size_t j = 0; j < u.size(); j++) urls.push_back(u[j].str());
                } else {
                    urls.push_back(u.str());
                }
            } else if (srv.has("url")) {
                urls.push_back(srv["url"].str());
            }

            for (const auto& url : urls) {
                if (url.empty()) continue;
                try {
                    rtc::IceServer ice(url);
                    if (!username.empty()) ice.username = username;
                    if (!credential.empty()) ice.password = credential;
                    config.iceServers.push_back(std::move(ice));
                    brls::Logger::info("RemoteAccess: ICE server {}", url);
                } catch (const std::exception& e) {
                    brls::Logger::warning("RemoteAccess: skipping ICE server '{}': {}",
                                          url, e.what());
                }
            }
        }
    }
    if (config.iceServers.empty()) {
        config.iceServers.emplace_back("stun:stun.l.google.com:19302");
    }

    std::lock_guard<std::mutex> lock(m_rtcMutex);
    try {
        m_pc = std::make_shared<rtc::PeerConnection>(config);
    } catch (const std::exception& e) {
        brls::Logger::error("RemoteAccess: failed to create peer connection: {}", e.what());
        setState(WebRTCState::ERROR, "Failed to create peer connection");
        return;
    }

    m_pc->onLocalDescription([this](rtc::Description desc) {
        Json msg;
        msg["type"] = Json(desc.typeString());
        msg["sessionId"] = Json(m_sessionId);
        Json data;
        data["sdp"] = Json(std::string(desc));
        data["type"] = Json(desc.typeString());
        msg["data"] = data;
        sendSignaling(msg);
        brls::Logger::info("RemoteAccess: sent SDP {}", desc.typeString());
    });

    m_pc->onLocalCandidate([this](rtc::Candidate cand) {
        Json msg;
        msg["type"] = Json("ice-candidate");
        msg["sessionId"] = Json(m_sessionId);
        Json data;
        data["candidate"] = Json(std::string(cand));
        data["sdpMid"] = Json(cand.mid());
        data["sdpMLineIndex"] = Json(0);
        msg["data"] = data;
        sendSignaling(msg);
    });

    m_pc->onStateChange([this](rtc::PeerConnection::State state) {
        switch (state) {
            case rtc::PeerConnection::State::Connected:
                brls::Logger::info("RemoteAccess: peer connection established");
                break;
            case rtc::PeerConnection::State::Disconnected:
            case rtc::PeerConnection::State::Failed:
            case rtc::PeerConnection::State::Closed: {
                brls::Logger::info("RemoteAccess: peer connection lost");
                bool wasConnected = (m_state.load() == WebRTCState::CONNECTED);
                if (m_state.load() != WebRTCState::DISCONNECTED) {
                    setState(WebRTCState::ERROR, "Peer connection lost");
                }
                if (wasConnected && m_closedCb) m_closedCb();
                break;
            }
            default:
                break;
        }
    });

    // Create the two channels the MA gateway bridges. Creating a channel
    // triggers automatic offer generation (-> onLocalDescription above).
    try {
        m_apiChannel = m_pc->createDataChannel("ma-api");
        m_sendspinChannel = m_pc->createDataChannel("sendspin");
    } catch (const std::exception& e) {
        brls::Logger::error("RemoteAccess: failed to create data channels: {}", e.what());
        setState(WebRTCState::ERROR, "Failed to create data channels");
        return;
    }

    m_apiChannel->onOpen([this]() {
        brls::Logger::info("RemoteAccess: ma-api channel open");
        setState(WebRTCState::CONNECTED);
    });
    m_apiChannel->onMessage([this](rtc::message_variant data) {
        if (std::holds_alternative<std::string>(data)) {
            onApiChannelMessage(std::get<std::string>(data));
        }
    });

    m_sendspinChannel->onOpen([this]() {
        brls::Logger::info("RemoteAccess: sendspin channel open");
        std::function<void()> cb;
        {
            std::lock_guard<std::mutex> lock(m_rtcMutex);
            m_sendspinOpen = true;
            cb = m_sendspinOpenCb;
        }
        if (cb) cb();
    });
    m_sendspinChannel->onMessage([this](rtc::message_variant data) {
        if (std::holds_alternative<std::string>(data)) {
            if (m_sendspinTextCb) m_sendspinTextCb(std::get<std::string>(data));
        } else {
            const rtc::binary& bin = std::get<rtc::binary>(data);
            if (m_sendspinBinaryCb && !bin.empty()) {
                m_sendspinBinaryCb(reinterpret_cast<const uint8_t*>(bin.data()), bin.size());
            }
        }
    });
}

void WebRTCClient::teardownPeerConnection() {
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::DataChannel> api, sendspin;
    {
        std::lock_guard<std::mutex> lock(m_rtcMutex);
        pc = std::move(m_pc);
        api = std::move(m_apiChannel);
        sendspin = std::move(m_sendspinChannel);
        m_pc.reset();
        m_apiChannel.reset();
        m_sendspinChannel.reset();
        m_sendspinOpen = false;
    }
    if (api) api->close();
    if (sendspin) sendspin->close();
    if (pc) pc->close();
}

void WebRTCClient::setSendspinOpenCallback(std::function<void()> cb) {
    bool alreadyOpen;
    {
        std::lock_guard<std::mutex> lock(m_rtcMutex);
        m_sendspinOpenCb = cb;
        alreadyOpen = m_sendspinOpen;
    }
    if (alreadyOpen && cb) cb();
}

// ---------------------------------------------------------------------------
// Data plane

bool WebRTCClient::sendApi(const std::string& message) {
    std::shared_ptr<rtc::DataChannel> ch;
    {
        std::lock_guard<std::mutex> lock(m_rtcMutex);
        ch = m_apiChannel;
    }
    if (!ch || !ch->isOpen()) return false;
    try {
        return ch->send(message);
    } catch (const std::exception& e) {
        brls::Logger::error("RemoteAccess: sendApi failed: {}", e.what());
        return false;
    }
}

bool WebRTCClient::sendSendspinText(const std::string& message) {
    std::shared_ptr<rtc::DataChannel> ch;
    {
        std::lock_guard<std::mutex> lock(m_rtcMutex);
        ch = m_sendspinChannel;
    }
    if (!ch || !ch->isOpen()) return false;
    try {
        return ch->send(message);
    } catch (const std::exception& e) {
        brls::Logger::error("RemoteAccess: sendSendspinText failed: {}", e.what());
        return false;
    }
}

bool WebRTCClient::sendSendspinBinary(const uint8_t* data, size_t size) {
    std::shared_ptr<rtc::DataChannel> ch;
    {
        std::lock_guard<std::mutex> lock(m_rtcMutex);
        ch = m_sendspinChannel;
    }
    if (!ch || !ch->isOpen()) return false;
    try {
        return ch->send(reinterpret_cast<const std::byte*>(data), size);
    } catch (const std::exception& e) {
        brls::Logger::error("RemoteAccess: sendSendspinBinary failed: {}", e.what());
        return false;
    }
}

void WebRTCClient::onApiChannelMessage(const std::string& message) {
    // http-proxy responses are consumed here; everything else is MA API traffic
    if (handleHttpProxyResponse(message)) return;
    if (m_apiMessageCb) m_apiMessageCb(message);
}

// ---------------------------------------------------------------------------
// HTTP tunneling

bool WebRTCClient::handleHttpProxyResponse(const std::string& message) {
    // Cheap pre-filter before a full JSON parse: the type field appears near
    // the start of the gateway's JSON, so only inspect the first bytes.
    if (message.size() < 30) return false;
    size_t typePos = message.find("http-proxy-response");
    if (typePos == std::string::npos || typePos > 60) return false;

    Json msg = Json::parse(message);
    if (!msg.has("type") || msg["type"].str() != "http-proxy-response") return false;

    std::string id = msg.has("id") ? msg["id"].str() : "";
    int status = msg.has("status") ? msg["status"].intVal() : 0;
    std::string body;
    if (msg.has("body")) hexDecode(msg["body"].str(), body);

    {
        std::lock_guard<std::mutex> lock(m_proxyMutex);
        auto it = m_proxyPending.find(id);
        if (it != m_proxyPending.end()) {
            it->second->status = status;
            it->second->body = std::move(body);
            it->second->done = true;
        }
    }
    m_proxyCv.notify_all();
    return true;
}

bool WebRTCClient::httpProxyFetch(const std::string& method, const std::string& path,
                                  int timeoutMs, int& statusOut, std::string& bodyOut) {
    if (!isConnected()) return false;

    std::string id = "vita-proxy-" + std::to_string(m_proxyIdCounter.fetch_add(1));
    auto pending = std::make_shared<ProxyPending>();
    {
        std::lock_guard<std::mutex> lock(m_proxyMutex);
        m_proxyPending[id] = pending;
    }

    Json req;
    req["type"] = Json("http-proxy-request");
    req["id"] = Json(id);
    req["method"] = Json(method);
    req["path"] = Json(path);
    req["headers"] = Json(Json::OBJECT);

    if (!sendApi(req.dump())) {
        std::lock_guard<std::mutex> lock(m_proxyMutex);
        m_proxyPending.erase(id);
        return false;
    }

    std::unique_lock<std::mutex> lock(m_proxyMutex);
    bool ok = m_proxyCv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                 [&]() { return pending->done; });
    m_proxyPending.erase(id);
    if (!ok || pending->status == 0) return false;

    statusOut = pending->status;
    bodyOut = std::move(pending->body);
    return true;
}

// ---------------------------------------------------------------------------
// State / misc

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
