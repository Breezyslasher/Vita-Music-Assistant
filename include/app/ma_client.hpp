#pragma once

#include "app.h"
#include "app/websocket_client.hpp"
#include <string>
#include <functional>
#include <map>
#include <mutex>
#include <atomic>

namespace vita_ma {

// JSON helper - minimal JSON builder/parser for Vita (no external deps)
class Json {
public:
    enum Type { OBJECT, ARRAY, STRING, NUMBER, BOOL, NULL_TYPE };

    Json() : m_type(OBJECT) {}
    Json(Type t) : m_type(t) {}
    Json(const std::string& s) : m_type(STRING), m_strVal(s) {}
    Json(const char* s) : m_type(STRING), m_strVal(s ? s : "") {}
    Json(int n) : m_type(NUMBER), m_numVal(n) {}
    Json(double n) : m_type(NUMBER), m_numVal(n) {}
    Json(bool b) : m_type(BOOL), m_boolVal(b) {}

    static Json parse(const std::string& str);
    std::string dump() const;

    // Object access
    Json& operator[](const std::string& key);
    const Json& operator[](const std::string& key) const;
    bool has(const std::string& key) const;

    // Array access
    Json& operator[](size_t index);
    const Json& operator[](size_t index) const;
    void push_back(const Json& val);
    size_t size() const;

    // Value access
    std::string str() const { return m_strVal; }
    int intVal() const { return static_cast<int>(m_numVal); }
    double numVal() const { return m_numVal; }
    bool boolVal() const { return m_boolVal; }
    Type type() const { return m_type; }
    bool isNull() const { return m_type == NULL_TYPE; }

    // Setters
    void setStr(const std::string& s) { m_type = STRING; m_strVal = s; }
    void setNum(double n) { m_type = NUMBER; m_numVal = n; }
    void setBool(bool b) { m_type = BOOL; m_boolVal = b; }

private:
    Type m_type = NULL_TYPE;
    std::string m_strVal;
    double m_numVal = 0;
    bool m_boolVal = false;
    std::map<std::string, Json> m_objMap;
    std::vector<Json> m_arrVec;

    static Json parseValue(const std::string& str, size_t& pos);
    static Json parseObject(const std::string& str, size_t& pos);
    static Json parseArray(const std::string& str, size_t& pos);
    static std::string parseString(const std::string& str, size_t& pos);
    static Json parseNumber(const std::string& str, size_t& pos);
    static void skipWhitespace(const std::string& str, size_t& pos);
};

// Event types from Music Assistant
enum class MAEvent {
    CONNECTED,
    DISCONNECTED,
    AUTH_FAILED,        // token rejected by the server (expired/invalid)
    QUEUE_UPDATED,
    QUEUE_ITEMS_UPDATED,
    QUEUE_TIME_UPDATED,
    PLAYER_UPDATED,
    MEDIA_ITEM_UPDATED,
    PROVIDERS_UPDATED,
    UNKNOWN
};

// Callback for command responses
using MAResponseCallback = std::function<void(bool success, const Json& result)>;
// Callback for events
using MAEventCallback = std::function<void(MAEvent event, const Json& data)>;

class MAClient {
public:
    static MAClient& instance();

    // Connection. serverUrl may be an http(s)/ws(s) URL for a direct
    // connection, or a Remote ID (26-char base32) for WebRTC remote access -
    // connect() dispatches automatically.
    bool connect(const std::string& serverUrl, const std::string& authToken = "");
    // Connect through MA remote access (WebRTC data channel via the signaling
    // server). Requires an auth token obtained from a previous direct login.
    bool connectRemote(const std::string& remoteId, const std::string& authToken = "");
    void disconnect();
    bool isConnected() const;

    // True when connected (or connecting) via WebRTC remote access
    bool isRemoteMode() const { return m_remoteMode.load(); }
    // True if the string looks like a Remote ID rather than a URL
    static bool isRemoteId(const std::string& s);

    // Set event callback
    void setEventCallback(MAEventCallback cb) { m_eventCallback = std::move(cb); }

    // After the next successful *direct* authentication, mint a long-lived
    // token (auth/token/create) so future connections - including remote
    // WebRTC ones, which can't reach /auth/login - never expire. Set this
    // before a fresh username/password login.
    void setUpgradeToLongLived(bool v) { m_upgradeToLongLived.store(v); }
    // Invoked (on the WS thread) with the freshly minted long-lived token so
    // the app can persist it. m_authToken is already updated when this fires.
    void setTokenUpgradedCallback(std::function<void(const std::string&)> cb) {
        m_onTokenUpgraded = std::move(cb);
    }
    // Invoked (on the WS thread) when the server rejects our token. The
    // reconnect loop is stopped before this fires - the app should surface a
    // re-login prompt. Registered once at startup, never cleared.
    void setAuthFailedCallback(std::function<void()> cb) {
        m_onAuthFailed = std::move(cb);
    }

    // === Music Library Commands ===

    // Artists
    void getLibraryArtists(MAResponseCallback cb, const std::string& search = "",
                           int limit = 500, int offset = 0);
    void getArtist(const std::string& itemId, MAResponseCallback cb,
                   const std::string& provider = "library");
    void getArtistAlbums(const std::string& itemId, MAResponseCallback cb,
                         const std::string& provider = "library");
    void getArtistTracks(const std::string& itemId, MAResponseCallback cb,
                         const std::string& provider = "library");

    // Albums
    void getLibraryAlbums(MAResponseCallback cb, const std::string& search = "",
                          int limit = 500, int offset = 0);
    void getAlbum(const std::string& itemId, MAResponseCallback cb,
                  const std::string& provider = "library");
    void getAlbumTracks(const std::string& itemId, MAResponseCallback cb,
                        const std::string& provider = "library");

    // Tracks
    void getLibraryTracks(MAResponseCallback cb, const std::string& search = "",
                          int limit = 500, int offset = 0);
    void getTrack(const std::string& itemId, MAResponseCallback cb,
                  const std::string& provider = "library");

    // Playlists
    void getLibraryPlaylists(MAResponseCallback cb, const std::string& search = "",
                             int limit = 500, int offset = 0);
    void getPlaylist(const std::string& itemId, MAResponseCallback cb,
                     const std::string& provider = "library");
    void getPlaylistTracks(const std::string& itemId, MAResponseCallback cb,
                           int page = 0, const std::string& provider = "library");
    void createPlaylist(const std::string& name, MAResponseCallback cb);

    // Radio
    void getLibraryRadios(MAResponseCallback cb, const std::string& search = "",
                          int limit = 500, int offset = 0);

    // Search
    void search(const std::string& query, MAResponseCallback cb, int limit = 25);

    // Browse
    void browse(const std::string& path, MAResponseCallback cb);

    // Favorites
    void addToFavorites(const std::string& mediaType, const std::string& itemId,
                        MAResponseCallback cb);
    void removeFromFavorites(const std::string& mediaType, const std::string& itemId,
                             MAResponseCallback cb);

    // Recently played
    void getRecentlyPlayed(MAResponseCallback cb, int limit = 20);
    void getRecommendations(MAResponseCallback cb);

    // === Queue Commands ===

    void playMedia(const std::string& queueId, const std::string& uri,
                   const std::string& option = "play", MAResponseCallback cb = nullptr);
    void queuePlay(const std::string& queueId, MAResponseCallback cb = nullptr);
    void queuePause(const std::string& queueId, MAResponseCallback cb = nullptr);
    void queueStop(const std::string& queueId, MAResponseCallback cb = nullptr);
    void queueNext(const std::string& queueId, MAResponseCallback cb = nullptr);
    void queuePrevious(const std::string& queueId, MAResponseCallback cb = nullptr);
    void queueSeek(const std::string& queueId, int position, MAResponseCallback cb = nullptr);
    void queueShuffle(const std::string& queueId, bool enabled, MAResponseCallback cb = nullptr);
    void queueRepeat(const std::string& queueId, const std::string& mode,
                     MAResponseCallback cb = nullptr);
    void queueClear(const std::string& queueId, MAResponseCallback cb = nullptr);
    void getQueueItems(const std::string& queueId, MAResponseCallback cb,
                       int limit = 500, int offset = 0);
    void getQueue(const std::string& queueId, MAResponseCallback cb);
    void queueMoveItem(const std::string& queueId, const std::string& itemId,
                       int posShift, MAResponseCallback cb = nullptr);
    void queueDeleteItem(const std::string& queueId, const std::string& itemId,
                         MAResponseCallback cb = nullptr);
    void playQueueIndex(const std::string& queueId, int index,
                        MAResponseCallback cb = nullptr);

    // === Player Commands ===

    void playerVolumeSet(const std::string& playerId, int level,
                         MAResponseCallback cb = nullptr);
    void playerVolumeUp(const std::string& playerId, MAResponseCallback cb = nullptr);
    void playerVolumeDown(const std::string& playerId, MAResponseCallback cb = nullptr);
    void playerVolumeMute(const std::string& playerId, bool muted,
                          MAResponseCallback cb = nullptr);
    void playerPower(const std::string& playerId, bool powered,
                     MAResponseCallback cb = nullptr);

    // Get stream URL for local playback
    // getStreamUrl removed - use player_queues/play_media + Sendspin instead

    // Get all players
    void getPlayers(MAResponseCallback cb);

    // Delete a playlist
    void deletePlaylist(const std::string& itemId, MAResponseCallback cb = nullptr);

    // Get thumbnail URL for an image path
    // Returns a full URL suitable for image loading
    std::string getThumbnailUrl(const std::string& imageUrl, int width = 0, int height = 0,
                               const std::string& provider = "");

    // Extract an image reference from a serialized MediaItemImage object.
    // Prefers the opaque server-side proxy_id (returned as "proxyid:<id>",
    // consumed by getThumbnailUrl to build the canonical /imageproxy/<id>
    // endpoint); falls back to the raw path for older servers.
    static std::string imageRefFromJson(const Json& imageObj);

    // Server info
    ServerInfo getServerInfo() const { return m_serverInfo; }

    // Send a command to the server (public for WebRTC relay use)
    void sendCommand(const std::string& command, const Json& kwargs,
                     MAResponseCallback cb = nullptr);

private:
    MAClient() = default;
    ~MAClient() = default;
    std::string generateMessageId();

    // Handle incoming messages
    void onMessage(const std::string& message);
    void onError(const std::string& error);
    void onClose(int code, const std::string& reason);

    // Parse events
    MAEvent parseEventType(const std::string& eventStr);

    // Transport: local WebSocket, or the WebRTC remote-access data channel
    WebSocketClient m_ws;
    std::atomic<bool> m_remoteMode{false};
    bool sendRaw(const std::string& json);
    std::string m_serverUrl;
    std::string m_authToken;
    ServerInfo m_serverInfo;
    std::atomic<bool> m_authenticated{false};
    bool m_authRequired = false;

    // Pending commands (awaiting responses)
    std::map<std::string, MAResponseCallback> m_pendingCallbacks;
    std::mutex m_callbackMutex;

    // Commands queued before auth completes
    struct QueuedCommand {
        std::string command;
        Json kwargs;
        MAResponseCallback cb;
    };
    std::vector<QueuedCommand> m_preAuthQueue;
    std::mutex m_preAuthMutex;
    void flushPreAuthQueue();

    // Event callback
    MAEventCallback m_eventCallback;

    // Message ID counter
    std::atomic<int> m_msgCounter{0};

    // Reconnect
    std::atomic<bool> m_shouldReconnect{true};

    // Long-lived token upgrade / auth-failure handling
    std::atomic<bool> m_upgradeToLongLived{false};
    std::function<void(const std::string&)> m_onTokenUpgraded;
    std::function<void()> m_onAuthFailed;
    void attemptReconnect();
    void upgradeToLongLivedToken();
};

} // namespace vita_ma
