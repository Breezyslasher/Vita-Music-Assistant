#include "app/ma_client.hpp"
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

bool MAClient::connect(const std::string& serverUrl, const std::string& authToken) {
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

    return m_ws.connect(wsUrl);
}

void MAClient::disconnect() {
    m_shouldReconnect.store(false);
    m_ws.disconnect();
    m_authenticated.store(false);
}

bool MAClient::isConnected() const {
    return m_ws.isConnected() && m_authenticated.load();
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
        if (m_serverInfo.schema_version >= 28 && !m_authToken.empty()) {
            // Send auth command
            Json kwargs;
            kwargs["token"] = Json(m_authToken);
            sendCommand("auth", kwargs, [this](bool success, const Json& result) {
                if (success) {
                    m_authenticated.store(true);
                    brls::Logger::info("MA: authenticated successfully");
                    flushPreAuthQueue();
                    if (m_eventCallback) {
                        m_eventCallback(MAEvent::CONNECTED, Json());
                    }
                } else {
                    brls::Logger::error("MA: authentication failed");
                    if (m_eventCallback) {
                        m_eventCallback(MAEvent::DISCONNECTED, Json());
                    }
                }
            });
        } else {
            // No auth needed or no token provided
            m_authenticated.store(true);
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

    if (m_eventCallback) {
        m_eventCallback(MAEvent::DISCONNECTED, Json());
    }

    if (m_shouldReconnect.load()) {
        attemptReconnect();
    }
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
    if (!m_authenticated.load() && command != "auth") {
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
    if (!m_ws.send(json)) {
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
    Json args;
    args["item_id"] = Json(itemId);
    args["provider_instance_id_or_domain"] = Json(provider);
    args["page"] = Json(page);
    sendCommand("music/playlists/playlist_tracks", args, std::move(cb));
}

void MAClient::createPlaylist(const std::string& name, MAResponseCallback cb) {
    Json args;
    args["name"] = Json(name);
    sendCommand("music/playlists/create", args, std::move(cb));
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
    args["media_type"] = Json(mediaType);
    args["item_id"] = Json(itemId);
    sendCommand("music/favorites/add_item", args, std::move(cb));
}

void MAClient::removeFromFavorites(const std::string& mediaType, const std::string& itemId,
                                    MAResponseCallback cb) {
    Json args;
    args["media_type"] = Json(mediaType);
    args["item_id"] = Json(itemId);
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

void MAClient::queueMoveItem(const std::string& queueId, const std::string& itemId,
                              int posShift, MAResponseCallback cb) {
    Json args;
    args["queue_id"] = Json(queueId);
    args["queue_item_id"] = Json(itemId);
    args["pos_shift"] = Json(posShift);
    sendCommand("player_queues/move_item", args, std::move(cb));
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

void MAClient::getStreamUrl(const std::string& queueId, MAResponseCallback cb) {
    Json args;
    args["queue_id"] = Json(queueId);
    sendCommand("player_queues/get_stream_url", args, std::move(cb));
}

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

std::string MAClient::getThumbnailUrl(const std::string& imageUrl, int width, int height) {
    if (imageUrl.empty()) return "";

    // If it's already a full URL, return as-is
    if (imageUrl.find("http://") == 0 || imageUrl.find("https://") == 0) {
        return imageUrl;
    }

    // Build server base URL
    std::string base = m_serverUrl;
    if (!base.empty() && base.back() == '/') base.pop_back();

    // If it starts with /, treat as a server-relative path
    if (!imageUrl.empty() && imageUrl.front() == '/') {
        std::string url = base + imageUrl;
        if (width > 0 || height > 0) {
            url += (url.find('?') != std::string::npos) ? "&" : "?";
            if (width > 0) url += "size=" + std::to_string(width);
        }
        return url;
    }

    // Otherwise, use the imageproxy endpoint with the path
    std::string url = base + "/imageproxy?path=" + urlEncode(imageUrl);
    if (width > 0) {
        url += "&size=" + std::to_string(width);
    }

    return url;
}

void MAClient::deletePlaylist(const std::string& itemId, MAResponseCallback cb) {
    Json args;
    args["item_id"] = Json(itemId);
    sendCommand("music/playlists/delete", args, std::move(cb));
}

// App singleton implementation (legacy, used for player ID storage)
App& App::instance() {
    static App s_instance;
    return s_instance;
}

} // namespace vita_ma
