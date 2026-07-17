#include "app/ma_client.hpp"
#include "app/webrtc_client.hpp"
#include <borealis.hpp>
#include <cstring>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstdio>

namespace vita_ma {

// ============================================================================
// Minimal JSON Implementation
// ============================================================================

void Json::skipWhitespace(const std::string& str, size_t& pos) {
    while (pos < str.size() && (str[pos] == ' ' || str[pos] == '\t' ||
           str[pos] == '\n' || str[pos] == '\r')) {
        pos++;
    }
}

std::string Json::parseString(const std::string& str, size_t& pos) {
    if (pos >= str.size() || str[pos] != '"') return "";
    pos++; // skip opening quote

    std::string result;
    while (pos < str.size() && str[pos] != '"') {
        if (str[pos] == '\\' && pos + 1 < str.size()) {
            pos++;
            switch (str[pos]) {
                case '"': result += '"'; break;
                case '\\': result += '\\'; break;
                case '/': result += '/'; break;
                case 'n': result += '\n'; break;
                case 'r': result += '\r'; break;
                case 't': result += '\t'; break;
                case 'b': result += '\b'; break;
                case 'f': result += '\f'; break;
                case 'u': {
                    // Unicode escape - simplified: just skip 4 hex chars
                    if (pos + 4 < str.size()) {
                        pos += 4;
                    }
                    result += '?';
                    break;
                }
                default: result += str[pos]; break;
            }
        } else {
            result += str[pos];
        }
        pos++;
    }
    if (pos < str.size()) pos++; // skip closing quote
    return result;
}

Json Json::parseNumber(const std::string& str, size_t& pos) {
    size_t start = pos;
    bool isFloat = false;

    if (pos < str.size() && str[pos] == '-') pos++;
    while (pos < str.size() && str[pos] >= '0' && str[pos] <= '9') pos++;
    if (pos < str.size() && str[pos] == '.') {
        isFloat = true;
        pos++;
        while (pos < str.size() && str[pos] >= '0' && str[pos] <= '9') pos++;
    }
    if (pos < str.size() && (str[pos] == 'e' || str[pos] == 'E')) {
        isFloat = true;
        pos++;
        if (pos < str.size() && (str[pos] == '+' || str[pos] == '-')) pos++;
        while (pos < str.size() && str[pos] >= '0' && str[pos] <= '9') pos++;
    }

    std::string numStr = str.substr(start, pos - start);
    Json j(NUMBER);
    j.m_numVal = strtod(numStr.c_str(), nullptr);
    return j;
}

Json Json::parseObject(const std::string& str, size_t& pos) {
    Json obj(OBJECT);
    pos++; // skip {
    skipWhitespace(str, pos);

    while (pos < str.size() && str[pos] != '}') {
        skipWhitespace(str, pos);
        if (str[pos] == '}') break;

        std::string key = parseString(str, pos);
        skipWhitespace(str, pos);
        if (pos < str.size() && str[pos] == ':') pos++;
        skipWhitespace(str, pos);

        obj.m_objMap[key] = parseValue(str, pos);
        skipWhitespace(str, pos);
        if (pos < str.size() && str[pos] == ',') pos++;
    }
    if (pos < str.size()) pos++; // skip }
    return obj;
}

Json Json::parseArray(const std::string& str, size_t& pos) {
    Json arr(ARRAY);
    pos++; // skip [
    skipWhitespace(str, pos);

    while (pos < str.size() && str[pos] != ']') {
        skipWhitespace(str, pos);
        if (str[pos] == ']') break;

        arr.m_arrVec.push_back(parseValue(str, pos));
        skipWhitespace(str, pos);
        if (pos < str.size() && str[pos] == ',') pos++;
    }
    if (pos < str.size()) pos++; // skip ]
    return arr;
}

Json Json::parseValue(const std::string& str, size_t& pos) {
    skipWhitespace(str, pos);
    if (pos >= str.size()) return Json(NULL_TYPE);

    switch (str[pos]) {
        case '"': {
            Json j(STRING);
            j.m_strVal = parseString(str, pos);
            return j;
        }
        case '{': return parseObject(str, pos);
        case '[': return parseArray(str, pos);
        case 't':
            if (str.substr(pos, 4) == "true") { pos += 4; return Json(true); }
            break;
        case 'f':
            if (str.substr(pos, 5) == "false") { pos += 5; return Json(false); }
            break;
        case 'n':
            if (str.substr(pos, 4) == "null") { pos += 4; return Json(NULL_TYPE); }
            break;
        default:
            if (str[pos] == '-' || (str[pos] >= '0' && str[pos] <= '9')) {
                return parseNumber(str, pos);
            }
            break;
    }
    pos++;
    return Json(NULL_TYPE);
}

Json Json::parse(const std::string& str) {
    size_t pos = 0;
    return parseValue(str, pos);
}

static std::string escapeJsonString(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 10);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

std::string Json::dump() const {
    switch (m_type) {
        case NULL_TYPE: return "null";
        case BOOL: return m_boolVal ? "true" : "false";
        case NUMBER: {
            if (m_numVal == static_cast<int64_t>(m_numVal)) {
                return std::to_string(static_cast<int64_t>(m_numVal));
            }
            char buf[64];
            snprintf(buf, sizeof(buf), "%.6g", m_numVal);
            return buf;
        }
        case STRING: return "\"" + escapeJsonString(m_strVal) + "\"";
        case OBJECT: {
            std::string s = "{";
            bool first = true;
            for (auto& kv : m_objMap) {
                if (!first) s += ",";
                first = false;
                s += "\"" + escapeJsonString(kv.first) + "\":" + kv.second.dump();
            }
            s += "}";
            return s;
        }
        case ARRAY: {
            std::string s = "[";
            for (size_t i = 0; i < m_arrVec.size(); i++) {
                if (i > 0) s += ",";
                s += m_arrVec[i].dump();
            }
            s += "]";
            return s;
        }
    }
    return "null";
}

Json& Json::operator[](const std::string& key) {
    m_type = OBJECT;
    return m_objMap[key];
}

const Json& Json::operator[](const std::string& key) const {
    static Json nullJson(NULL_TYPE);
    auto it = m_objMap.find(key);
    if (it != m_objMap.end()) return it->second;
    return nullJson;
}

bool Json::has(const std::string& key) const {
    return m_objMap.find(key) != m_objMap.end();
}

Json& Json::operator[](size_t index) {
    m_type = ARRAY;
    if (index >= m_arrVec.size()) m_arrVec.resize(index + 1);
    return m_arrVec[index];
}

const Json& Json::operator[](size_t index) const {
    static Json nullJson(NULL_TYPE);
    if (index >= m_arrVec.size()) return nullJson;
    return m_arrVec[index];
}

void Json::push_back(const Json& val) {
    m_type = ARRAY;
    m_arrVec.push_back(val);
}

size_t Json::size() const {
    if (m_type == ARRAY) return m_arrVec.size();
    if (m_type == OBJECT) return m_objMap.size();
    return 0;
}

// ============================================================================
// MAClient Implementation
// ============================================================================

MAClient& MAClient::instance() {
    static MAClient instance;
    return instance;
}

std::string MAClient::generateMessageId() {
    int id = m_msgCounter.fetch_add(1);
    return std::to_string(id);
}

bool MAClient::isRemoteId(const std::string& s) {
    return WebRTCClient::isRemoteId(s);
}

bool MAClient::connect(const std::string& serverUrl, const std::string& authToken) {
    // A fresh connect re-enables auto-reconnect (it may have been disabled by a
    // prior auth failure or explicit disconnect).
    m_shouldReconnect.store(true);

    // Remote IDs go through WebRTC remote access
    if (isRemoteId(serverUrl)) {
        return connectRemote(serverUrl, authToken);
    }
    m_remoteMode.store(false);

    m_serverUrl = serverUrl;
    m_authToken = authToken;

    // Convert HTTP URL to WebSocket URL (case-insensitive scheme matching)
    std::string wsUrl = serverUrl;
    // Build a lowercase copy of the scheme portion for comparison
    std::string schemeLower;
    for (size_t i = 0; i < wsUrl.size() && i < 8; i++)
        schemeLower += static_cast<char>(std::tolower(static_cast<unsigned char>(wsUrl[i])));

    if (schemeLower.substr(0, 8) == "https://") {
        wsUrl = "wss://" + wsUrl.substr(8);
    } else if (schemeLower.substr(0, 7) == "http://") {
        wsUrl = "ws://" + wsUrl.substr(7);
    } else if (schemeLower.substr(0, 5) != "ws://" && schemeLower.substr(0, 6) != "wss://") {
        wsUrl = "ws://" + wsUrl;
    }

    // Append /ws path if not present
    if (wsUrl.find("/ws") == std::string::npos) {
        if (wsUrl.back() != '/') wsUrl += "/";
        wsUrl += "ws";
    }

    brls::Logger::info("MA: connecting to {}", wsUrl);

    // Set up callbacks
    m_ws.setOnMessage([this](const std::string& msg) { onMessage(msg); });
    m_ws.setOnError([this](const std::string& err) { onError(err); });
    m_ws.setOnClose([this](int code, const std::string& reason) { onClose(code, reason); });

    // No subprotocol: the MA server doesn't advertise any, and requesting
    // "json" makes aiohttp log a protocol-mismatch warning on every connect.
    {
        std::lock_guard<std::mutex> lock(m_authWaitMutex);
        m_authResolved = false;
    }
    if (!m_ws.connect(wsUrl)) return false;
    // Block until the async auth exchange resolves so callers can trust the
    // result (the server_info -> auth handshake runs on the receive thread).
    return waitForAuthOutcome(15000);
}

bool MAClient::connectRemote(const std::string& remoteId, const std::string& authToken) {
    m_serverUrl = remoteId;
    m_authToken = authToken;
    m_remoteMode.store(true);

    brls::Logger::info("MA: connecting remotely via {}", remoteId);

    WebRTCClient& rtc = WebRTCClient::instance();
    // The bridged local WS API greets with server_info exactly like a direct
    // connection, so the whole existing message/auth flow applies unchanged.
    rtc.setApiMessageCallback([this](const std::string& msg) { onMessage(msg); });
    rtc.setClosedCallback([this]() { onClose(0, "remote connection lost"); });

    {
        std::lock_guard<std::mutex> lock(m_authWaitMutex);
        m_authResolved = false;
    }
    if (!rtc.connectRemote(remoteId)) return false;
    // Block until the async auth (or socket login) resolves; without this a
    // failed credential login would still be reported as connected.
    return waitForAuthOutcome(20000);
}

bool MAClient::sendRaw(const std::string& json) {
    if (m_remoteMode.load()) {
        return WebRTCClient::instance().sendApi(json);
    }
    return m_ws.send(json);
}

void MAClient::disconnect() {
    m_shouldReconnect.store(false);
    if (m_remoteMode.load()) {
        WebRTCClient::instance().disconnect();
    }
    m_ws.disconnect();
    m_authenticated.store(false);
}

bool MAClient::isConnected() const {
    bool transportUp = m_remoteMode.load()
        ? WebRTCClient::instance().isConnected()
        : m_ws.isConnected();
    return transportUp && m_authenticated.load();
}

void MAClient::onMessage(const std::string& message) {
    Json msg = Json::parse(message);

    // Check if this is the initial server info message
    if (msg.has("server_id")) {
        m_serverInfo.server_id = msg["server_id"].str();
        m_serverInfo.server_name = msg["server_name"].str();
        m_serverInfo.server_version = msg["server_version"].str();
        m_serverInfo.schema_version = msg["schema_version"].intVal();
        m_serverInfo.min_supported_schema = msg["min_supported_schema_version"].intVal();

        brls::Logger::info("MA: server {} v{} (schema {})",
            m_serverInfo.server_name, m_serverInfo.server_version,
            m_serverInfo.schema_version);

        // Check if auth is required
        if (m_serverInfo.schema_version >= 28 &&
            (!m_authToken.empty() || !m_pendingLoginUser.empty())) {
            if (m_authToken.empty()) {
                // No token but credentials were provided: log in over the
                // socket itself ('auth/login' is callable unauthenticated),
                // which also works across WebRTC remote connections.
                sendLoginCommand();
            } else {
                sendAuthCommand();
            }
        } else {
            // No auth needed or no token provided
            m_authenticated.store(true);
            resolveAuthWait();
            brls::Logger::info("MA: connected (no auth required)");
            if (m_eventCallback) {
                m_eventCallback(MAEvent::CONNECTED, Json());
            }
        }
        return;
    }

    // Check if this is a response to a pending command
    if (msg.has("message_id")) {
        std::string msgId = msg["message_id"].str();
        MAResponseCallback cb;

        {
            std::lock_guard<std::mutex> lock(m_callbackMutex);
            auto it = m_pendingCallbacks.find(msgId);
            if (it != m_pendingCallbacks.end()) {
                cb = std::move(it->second);
                m_pendingCallbacks.erase(it);
            }
        }

        if (cb) {
            if (msg.has("error_code")) {
                brls::Logger::error("MA: command error: {}", msg["details"].str());
                cb(false, msg);
            } else {
                cb(true, msg["result"]);
            }
        }
        return;
    }

    // Check if this is an event
    if (msg.has("event")) {
        MAEvent event = parseEventType(msg["event"].str());
        if (m_eventCallback) {
            m_eventCallback(event, msg.has("data") ? msg["data"] : Json());
        }
        return;
    }
}

void MAClient::onError(const std::string& error) {
    brls::Logger::error("MA: WebSocket error: {}", error);
}

void MAClient::onClose(int code, const std::string& reason) {
    brls::Logger::info("MA: connection closed ({}): {}", code, reason);
    m_authenticated.store(false);
    resolveAuthWait();

    if (m_eventCallback) {
        m_eventCallback(MAEvent::DISCONNECTED, Json());
    }

    if (m_shouldReconnect.load()) {
        attemptReconnect();
    }
}

void MAClient::resolveAuthWait() {
    {
        std::lock_guard<std::mutex> lock(m_authWaitMutex);
        m_authResolved = true;
    }
    m_authWaitCv.notify_all();
}

bool MAClient::waitForAuthOutcome(int timeoutMs) {
    std::unique_lock<std::mutex> lock(m_authWaitMutex);
    m_authWaitCv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                          [this]() { return m_authResolved; });
    return m_authenticated.load();
}

void MAClient::setPendingCredentials(const std::string& username, const std::string& password,
                                     const std::string& providerId) {
    m_pendingLoginUser = username;
    m_pendingLoginPass = password;
    m_pendingLoginProvider = providerId.empty() ? "builtin" : providerId;
}

void MAClient::sendAuthCommand() {
    Json kwargs;
    kwargs["token"] = Json(m_authToken);
    sendCommand("auth", kwargs, [this](bool success, const Json& result) {
        if (success) {
            m_authenticated.store(true);
            resolveAuthWait();
            brls::Logger::info("MA: authenticated successfully");
            flushPreAuthQueue();
            if (m_eventCallback) {
                m_eventCallback(MAEvent::CONNECTED, Json());
            }
            // Upgrade a fresh short-lived token to a long-lived one so future
            // connections - especially remote WebRTC ones - never expire.
            // Works over any transport: auth/token/create is a WS command.
            if (m_upgradeToLongLived.load()) {
                m_upgradeToLongLived.store(false);
                upgradeToLongLivedToken();
            }
        } else {
            // The token was rejected (expired/invalid) - distinct from a
            // network drop. Stop the reconnect loop so we don't spin
            // forever replaying a dead token, and ask the app to re-login.
            brls::Logger::error("MA: authentication failed (token rejected)");
            m_shouldReconnect.store(false);
            m_authenticated.store(false);
            resolveAuthWait();
            if (m_onAuthFailed) {
                m_onAuthFailed();
            }
            if (m_eventCallback) {
                m_eventCallback(MAEvent::AUTH_FAILED, Json());
            }
        }
    });
}

void MAClient::sendLoginCommand() {
    Json kwargs;
    kwargs["username"] = Json(m_pendingLoginUser);
    kwargs["password"] = Json(m_pendingLoginPass);
    kwargs["provider_id"] = Json(m_pendingLoginProvider);
    kwargs["device_name"] = Json(std::string("PS Vita"));
    // One-shot: never retain credentials past the attempt
    m_pendingLoginUser.clear();
    m_pendingLoginPass.clear();
    m_pendingLoginProvider.clear();

    brls::Logger::info("MA: logging in over the socket (auth/login)");
    sendCommand("auth/login", kwargs, [this](bool success, const Json& result) {
        std::string newToken;
        if (success && result.type() == Json::OBJECT &&
            result.has("success") && result["success"].boolVal()) {
            // Server 2.9.x returns "access_token"; newer builds return "token"
            if (result.has("access_token")) newToken = result["access_token"].str();
            if (newToken.empty() && result.has("token")) newToken = result["token"].str();
        }
        if (!newToken.empty()) {
            m_authToken = newToken;
            // The login token is short-lived: upgrade it after auth so it is
            // durable for future (remote) connections.
            m_upgradeToLongLived.store(true);
            sendAuthCommand();
        } else {
            std::string err = "Login failed";
            if (success && result.type() == Json::OBJECT && result.has("error"))
                err = result["error"].str();
            brls::Logger::error("MA: socket login failed: {}", err);
            m_shouldReconnect.store(false);
            m_authenticated.store(false);
            resolveAuthWait();
            if (m_onAuthFailed) {
                m_onAuthFailed();
            }
            if (m_eventCallback) {
                m_eventCallback(MAEvent::AUTH_FAILED, Json());
            }
        }
    });
}

void MAClient::upgradeToLongLivedToken() {
    // auth/token/create -> returns a new long-lived token string (expires_at
    // null). We swap our in-memory token to it (so the reconnect loop uses the
    // durable one) and hand it to the app to persist.
    Json kwargs;
    kwargs["name"] = Json("Vita Music Assistant");
    brls::Logger::info("MA: requesting long-lived token");
    sendCommand("auth/token/create", kwargs, [this](bool success, const Json& result) {
        if (!success || result.type() != Json::STRING) {
            brls::Logger::warning("MA: long-lived token upgrade failed; keeping current token");
            return;
        }
        std::string newToken = result.str();
        if (newToken.empty()) {
            brls::Logger::warning("MA: long-lived token upgrade returned empty token");
            return;
        }
        m_authToken = newToken;
        brls::Logger::info("MA: upgraded to long-lived token");
        if (m_onTokenUpgraded) {
            m_onTokenUpgraded(newToken);
        }
    });
}

void MAClient::attemptReconnect() {
    brls::Logger::info("MA: scheduling reconnect...");
    // Reconnect after 5 seconds
    std::thread([this]() {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        if (m_shouldReconnect.load()) {
            brls::Logger::info("MA: attempting reconnect");
            connect(m_serverUrl, m_authToken);
        }
    }).detach();
}

MAEvent MAClient::parseEventType(const std::string& eventStr) {
    if (eventStr == "queue_updated") return MAEvent::QUEUE_UPDATED;
    if (eventStr == "queue_items_updated") return MAEvent::QUEUE_ITEMS_UPDATED;
    if (eventStr == "queue_time_updated") return MAEvent::QUEUE_TIME_UPDATED;
    if (eventStr == "player_updated") return MAEvent::PLAYER_UPDATED;
    if (eventStr == "media_item_updated") return MAEvent::MEDIA_ITEM_UPDATED;
    if (eventStr == "providers_updated") return MAEvent::PROVIDERS_UPDATED;
    return MAEvent::UNKNOWN;
}

void MAClient::sendCommand(const std::string& command, const Json& kwargs,
                            MAResponseCallback cb) {
    // Queue commands that arrive before authentication completes
    // (except the "auth" command itself)
    if (!m_authenticated.load() && command != "auth" && command != "auth/login") {
        brls::Logger::debug("MA: queuing command '{}' until auth completes", command);
        std::lock_guard<std::mutex> lock(m_preAuthMutex);
        m_preAuthQueue.push_back({command, kwargs, std::move(cb)});
        return;
    }

    std::string msgId = generateMessageId();

    Json msg;
    msg["message_id"] = Json(msgId);
    msg["command"] = Json(command);
    if (kwargs.type() == Json::OBJECT && kwargs.size() > 0) {
        msg["args"] = kwargs;
    }

    if (cb) {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        m_pendingCallbacks[msgId] = std::move(cb);
    }

    std::string json = msg.dump();
    if (!sendRaw(json)) {
        brls::Logger::error("MA: failed to send command: {}", command);
        if (cb) {
            std::lock_guard<std::mutex> lock(m_callbackMutex);
            m_pendingCallbacks.erase(msgId);
        }
    }
}

void MAClient::flushPreAuthQueue() {
    std::vector<QueuedCommand> queued;
    {
        std::lock_guard<std::mutex> lock(m_preAuthMutex);
        queued.swap(m_preAuthQueue);
    }
    if (!queued.empty()) {
        brls::Logger::info("MA: flushing {} queued commands after auth", queued.size());
    }
    for (auto& cmd : queued) {
        sendCommand(cmd.command, cmd.kwargs, std::move(cmd.cb));
    }
}

// ============================================================================
// Library Commands
// ============================================================================

void MAClient::getLibraryArtists(MAResponseCallback cb, const std::string& search,
                                  int limit, int offset) {
    Json args;
    if (!search.empty()) args["search"] = Json(search);
    args["limit"] = Json(limit);
    args["offset"] = Json(offset);
    // Show all library items, not just favorites
    sendCommand("music/artists/library_items", args, std::move(cb));
}

void MAClient::getArtist(const std::string& itemId, MAResponseCallback cb,
                          const std::string& provider) {
    Json args;
    args["item_id"] = Json(itemId);
    args["provider_instance_id_or_domain"] = Json(provider);
    sendCommand("music/artists/get", args, std::move(cb));
}

void MAClient::getArtistAlbums(const std::string& itemId, MAResponseCallback cb,
                                const std::string& provider) {
    Json args;
    args["item_id"] = Json(itemId);
    args["provider_instance_id_or_domain"] = Json(provider);
    sendCommand("music/artists/artist_albums", args, std::move(cb));
}

void MAClient::getArtistTracks(const std::string& itemId, MAResponseCallback cb,
                                const std::string& provider) {
    Json args;
    args["item_id"] = Json(itemId);
    args["provider_instance_id_or_domain"] = Json(provider);
    sendCommand("music/artists/artist_tracks", args, std::move(cb));
}

void MAClient::getLibraryAlbums(MAResponseCallback cb, const std::string& search,
                                 int limit, int offset) {
    Json args;
    if (!search.empty()) args["search"] = Json(search);
    args["limit"] = Json(limit);
    args["offset"] = Json(offset);
    // Show all library items, not just favorites
    sendCommand("music/albums/library_items", args, std::move(cb));
}

void MAClient::getAlbum(const std::string& itemId, MAResponseCallback cb,
                         const std::string& provider) {
    Json args;
    args["item_id"] = Json(itemId);
    args["provider_instance_id_or_domain"] = Json(provider);
    sendCommand("music/albums/get", args, std::move(cb));
}

void MAClient::getAlbumTracks(const std::string& itemId, MAResponseCallback cb,
                               const std::string& provider) {
    Json args;
    args["item_id"] = Json(itemId);
    args["provider_instance_id_or_domain"] = Json(provider);
    sendCommand("music/albums/album_tracks", args, std::move(cb));
}

void MAClient::getLibraryTracks(MAResponseCallback cb, const std::string& search,
                                 int limit, int offset) {
    Json args;
    if (!search.empty()) args["search"] = Json(search);
    args["limit"] = Json(limit);
    args["offset"] = Json(offset);
    // Show all library items, not just favorites
    sendCommand("music/tracks/library_items", args, std::move(cb));
}

void MAClient::getTrack(const std::string& itemId, MAResponseCallback cb,
                         const std::string& provider) {
    Json args;
    args["item_id"] = Json(itemId);
    args["provider_instance_id_or_domain"] = Json(provider);
    sendCommand("music/tracks/get", args, std::move(cb));
}

void MAClient::getLibraryPlaylists(MAResponseCallback cb, const std::string& search,
                                    int limit, int offset) {
    Json args;
    if (!search.empty()) args["search"] = Json(search);
    args["limit"] = Json(limit);
    args["offset"] = Json(offset);
    sendCommand("music/playlists/library_items", args, std::move(cb));
}

void MAClient::getPlaylist(const std::string& itemId, MAResponseCallback cb,
                            const std::string& provider) {
    Json args;
    args["item_id"] = Json(itemId);
    args["provider_instance_id_or_domain"] = Json(provider);
    sendCommand("music/playlists/get", args, std::move(cb));
}

void MAClient::getPlaylistTracks(const std::string& itemId, MAResponseCallback cb,
                                  int page, const std::string& provider) {
    // MA: playlist_tracks(item_id, provider_instance_id_or_domain, ...) returns
    // the full track list (no 'page' parameter exists). Callers paginate the
    // returned list client-side, so we must not send a 'page' arg.
    (void)page;
    Json args;
    args["item_id"] = Json(itemId);
    args["provider_instance_id_or_domain"] = Json(provider);
    sendCommand("music/playlists/playlist_tracks", args, std::move(cb));
}

void MAClient::createPlaylist(const std::string& name, MAResponseCallback cb) {
    Json args;
    args["name"] = Json(name);
    sendCommand("music/playlists/create_playlist", args, std::move(cb));
}

void MAClient::getLibraryRadios(MAResponseCallback cb, const std::string& search,
                                 int limit, int offset) {
    Json args;
    if (!search.empty()) args["search"] = Json(search);
    args["limit"] = Json(limit);
    args["offset"] = Json(offset);
    sendCommand("music/radios/library_items", args, std::move(cb));
}

void MAClient::search(const std::string& query, MAResponseCallback cb, int limit) {
    Json args;
    args["search_query"] = Json(query);
    args["limit"] = Json(limit);
    sendCommand("music/search", args, std::move(cb));
}

void MAClient::browse(const std::string& path, MAResponseCallback cb) {
    Json args;
    args["path"] = Json(path);
    sendCommand("music/browse", args, std::move(cb));
}

void MAClient::addToFavorites(const std::string& mediaType, const std::string& itemId,
                               MAResponseCallback cb) {
    Json args;
    // API expects a URI string like "library://track/123"
    std::string uri = "library://" + mediaType + "/" + itemId;
    args["item"] = Json(uri);
    sendCommand("music/favorites/add_item", args, std::move(cb));
}

void MAClient::removeFromFavorites(const std::string& mediaType, const std::string& itemId,
                                    MAResponseCallback cb) {
    Json args;
    args["media_type"] = Json(mediaType);
    // MA: remove_item_from_favorites(media_type, library_item_id)
    args["library_item_id"] = Json(itemId);
    sendCommand("music/favorites/remove_item", args, std::move(cb));
}

void MAClient::getRecentlyPlayed(MAResponseCallback cb, int limit) {
    Json args;
    args["limit"] = Json(limit);
    sendCommand("music/recently_played_items", args, std::move(cb));
}

void MAClient::getRecommendations(MAResponseCallback cb) {
    sendCommand("music/recommendations", Json(), std::move(cb));
}

// ============================================================================
// Queue Commands
// ============================================================================

void MAClient::playMedia(const std::string& queueId, const std::string& uri,
                          const std::string& option, MAResponseCallback cb) {
    Json args;
    args["queue_id"] = Json(queueId);
    Json mediaArr(Json::ARRAY);
    mediaArr.push_back(Json(uri));
    args["media"] = mediaArr;
    args["option"] = Json(option);
    sendCommand("player_queues/play_media", args, std::move(cb));
}

void MAClient::queuePlay(const std::string& queueId, MAResponseCallback cb) {
    Json args;
    args["queue_id"] = Json(queueId);
    sendCommand("player_queues/play", args, std::move(cb));
}

void MAClient::queuePause(const std::string& queueId, MAResponseCallback cb) {
    Json args;
    args["queue_id"] = Json(queueId);
    sendCommand("player_queues/pause", args, std::move(cb));
}

void MAClient::queueStop(const std::string& queueId, MAResponseCallback cb) {
    Json args;
    args["queue_id"] = Json(queueId);
    sendCommand("player_queues/stop", args, std::move(cb));
}

void MAClient::queueNext(const std::string& queueId, MAResponseCallback cb) {
    Json args;
    args["queue_id"] = Json(queueId);
    sendCommand("player_queues/next", args, std::move(cb));
}

void MAClient::queuePrevious(const std::string& queueId, MAResponseCallback cb) {
    Json args;
    args["queue_id"] = Json(queueId);
    sendCommand("player_queues/previous", args, std::move(cb));
}

void MAClient::queueSeek(const std::string& queueId, int position, MAResponseCallback cb) {
    Json args;
    args["queue_id"] = Json(queueId);
    args["position"] = Json(position);
    sendCommand("player_queues/seek", args, std::move(cb));
}

void MAClient::queueShuffle(const std::string& queueId, bool enabled, MAResponseCallback cb) {
    Json args;
    args["queue_id"] = Json(queueId);
    args["shuffle_enabled"] = Json(enabled);
    sendCommand("player_queues/shuffle", args, std::move(cb));
}

void MAClient::queueRepeat(const std::string& queueId, const std::string& mode,
                             MAResponseCallback cb) {
    Json args;
    args["queue_id"] = Json(queueId);
    args["repeat_mode"] = Json(mode);
    sendCommand("player_queues/repeat", args, std::move(cb));
}

void MAClient::queueClear(const std::string& queueId, MAResponseCallback cb) {
    Json args;
    args["queue_id"] = Json(queueId);
    sendCommand("player_queues/clear", args, std::move(cb));
}

void MAClient::getQueueItems(const std::string& queueId, MAResponseCallback cb,
                              int limit, int offset) {
    Json args;
    args["queue_id"] = Json(queueId);
    args["limit"] = Json(limit);
    args["offset"] = Json(offset);
    sendCommand("player_queues/items", args, std::move(cb));
}

void MAClient::getQueue(const std::string& queueId, MAResponseCallback cb) {
    Json args;
    args["queue_id"] = Json(queueId);
    sendCommand("player_queues/get", args, std::move(cb));
}

void MAClient::queueMoveItem(const std::string& queueId, const std::string& itemId,
                              int posShift, MAResponseCallback cb) {
    Json args;
    args["queue_id"] = Json(queueId);
    args["queue_item_id"] = Json(itemId);
    args["pos_shift"] = Json(posShift);
    sendCommand("player_queues/move_item", args, std::move(cb));
}

void MAClient::queueDeleteItem(const std::string& queueId, const std::string& itemId,
                                MAResponseCallback cb) {
    Json args;
    args["queue_id"] = Json(queueId);
    args["item_id_or_index"] = Json(itemId);
    sendCommand("player_queues/delete_item", args, std::move(cb));
}

void MAClient::playQueueIndex(const std::string& queueId, int index, MAResponseCallback cb) {
    Json args;
    args["queue_id"] = Json(queueId);
    args["index"] = Json(index);
    sendCommand("player_queues/play_index", args, std::move(cb));
}

// ============================================================================
// Player Commands
// ============================================================================

void MAClient::playerVolumeSet(const std::string& playerId, int level,
                                MAResponseCallback cb) {
    Json args;
    args["player_id"] = Json(playerId);
    args["volume_level"] = Json(level);
    sendCommand("players/cmd/volume_set", args, std::move(cb));
}

void MAClient::playerVolumeUp(const std::string& playerId, MAResponseCallback cb) {
    Json args;
    args["player_id"] = Json(playerId);
    sendCommand("players/cmd/volume_up", args, std::move(cb));
}

void MAClient::playerVolumeDown(const std::string& playerId, MAResponseCallback cb) {
    Json args;
    args["player_id"] = Json(playerId);
    sendCommand("players/cmd/volume_down", args, std::move(cb));
}

void MAClient::playerVolumeMute(const std::string& playerId, bool muted,
                                 MAResponseCallback cb) {
    Json args;
    args["player_id"] = Json(playerId);
    args["muted"] = Json(muted);
    sendCommand("players/cmd/volume_mute", args, std::move(cb));
}

void MAClient::playerPower(const std::string& playerId, bool powered,
                            MAResponseCallback cb) {
    Json args;
    args["player_id"] = Json(playerId);
    args["powered"] = Json(powered);
    sendCommand("players/cmd/power", args, std::move(cb));
}

// getStreamUrl removed - the command player_queues/get_stream_url does not
// exist in the MA API. Audio streaming is now handled via the Sendspin protocol.

void MAClient::getPlayers(MAResponseCallback cb) {
    sendCommand("players/all", Json(), std::move(cb));
}

// Simple URL-encode for imageproxy path parameter
static std::string urlEncode(const std::string& str) {
    std::string encoded;
    for (unsigned char c : str) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += static_cast<char>(c);
        } else {
            char hex[4];
            std::snprintf(hex, sizeof(hex), "%%%02X", c);
            encoded += hex;
        }
    }
    return encoded;
}

std::string MAClient::imageRefFromJson(const Json& imageObj) {
    if (imageObj.type() != Json::OBJECT) return "";
    if (imageObj.has("proxy_id") && imageObj["proxy_id"].type() == Json::STRING &&
        !imageObj["proxy_id"].str().empty()) {
        return "proxyid:" + imageObj["proxy_id"].str();
    }
    if (imageObj.has("path")) return imageObj["path"].str();
    return "";
}

// The canonical /imageproxy/<id> endpoint only serves fixed thumbnail sizes;
// snap a requested size to the smallest allowed size that is not smaller.
static int snapImageproxySize(int requested) {
    static const int allowed[] = {80, 160, 256, 512, 1024};
    for (int s : allowed) {
        if (requested <= s) return s;
    }
    return 1024;
}

std::string MAClient::getThumbnailUrl(const std::string& imageUrl, int width, int height,
                                      const std::string& provider) {
    if (imageUrl.empty()) return "";

    int size = width > 0 ? width : (height > 0 ? height : 300);

    // Server-relative path for remote mode: ImageLoader routes paths starting
    // with '/' through the WebRTC http-proxy tunnel.
    std::string path;
    if (imageUrl.rfind("proxyid:", 0) == 0) {
        // Canonical endpoint: /imageproxy/<proxy_id>?size=N. The legacy
        // ?path=&provider= form is deprecated (the server logs a warning) and
        // only accepts a fixed set of sizes on the new route.
        path = "/imageproxy/" + imageUrl.substr(8) +
               "?size=" + std::to_string(snapImageproxySize(size));
    } else {
        // Older servers (no proxy_id field): legacy imageproxy query form.
        // Routing through the imageproxy keeps images small for the Vita's
        // limited RAM; it accepts both relative paths and full URLs.
        path = "/imageproxy?path=" + urlEncode(imageUrl);
        path += "&size=" + std::to_string(size);
        if (!provider.empty()) {
            path += "&provider=" + urlEncode(provider);
        }
    }

    if (m_remoteMode.load()) {
        return path;
    }

    std::string base = m_serverUrl;
    if (!base.empty() && base.back() == '/') base.pop_back();
    return base + path;
}

void MAClient::deletePlaylist(const std::string& itemId, MAResponseCallback cb) {
    // There is no "music/playlists/remove" command; deleting a playlist from the
    // library is the generic remove_item_from_library(media_type, library_item_id).
    Json args;
    args["media_type"] = Json(std::string("playlist"));
    args["library_item_id"] = Json(itemId);
    sendCommand("music/library/remove_item", args, std::move(cb));
}

// App singleton implementation (legacy, used for player ID storage)
App& App::instance() {
    static App s_instance;
    return s_instance;
}

} // namespace vita_ma
