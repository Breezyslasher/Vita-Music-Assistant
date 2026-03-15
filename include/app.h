#pragma once

#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <memory>
#include <borealis.hpp>

namespace vita_ma {

// Media types matching Music Assistant server
enum class MediaType {
    ARTIST,
    ALBUM,
    TRACK,
    PLAYLIST,
    RADIO,
    UNKNOWN
};

// Repeat modes
enum class RepeatMode {
    OFF,
    ALL,
    ONE
};

// Player state
enum class PlayerState {
    IDLE,
    PLAYING,
    PAUSED,
    BUFFERING
};

// Image type for artwork
enum class ImageType {
    THUMB,
    FANART,
    LOGO,
    LANDSCAPE
};

// Artist info
struct ArtistItem {
    std::string item_id;
    std::string name;
    std::string sort_name;
    std::string image_url;
    bool in_library = false;
    bool is_favorite = false;
};

// Album info
struct AlbumItem {
    std::string item_id;
    std::string name;
    std::string sort_name;
    std::string artist;
    std::string image_url;
    std::string year;
    std::string album_type;
    bool in_library = false;
    bool is_favorite = false;
};

// Track info
struct TrackItem {
    std::string item_id;
    std::string name;
    std::string artist;
    std::string album;
    std::string image_url;
    std::string uri;
    std::string provider;
    int duration = 0;         // seconds
    int track_number = 0;
    int disc_number = 0;
    bool in_library = false;
    bool is_favorite = false;
};

// Playlist info
struct PlaylistItem {
    std::string item_id;
    std::string name;
    std::string owner;
    std::string image_url;
    bool in_library = false;
    bool is_favorite = false;
    bool is_editable = false;
};

// Radio station info
struct RadioItem {
    std::string item_id;
    std::string name;
    std::string image_url;
    bool in_library = false;
    bool is_favorite = false;
};

// Generic media item (for browse results, search results)
struct MediaItem {
    MediaType media_type = MediaType::UNKNOWN;
    std::string item_id;
    std::string name;
    std::string subtitle;    // artist name, owner, etc.
    std::string image_url;
    std::string uri;
    bool in_library = false;
    bool is_favorite = false;

    // Cached texture
    int texture_id = -1;
};

// Browse result from the server
struct BrowseItem {
    std::string label;
    std::string path;
    std::string image_url;
    bool is_folder = false;
    MediaType media_type = MediaType::UNKNOWN;
    std::string item_id;
    std::string uri;
};

// Queue item (track in the play queue)
struct QueueItem {
    std::string queue_item_id;
    std::string name;
    std::string artist;
    std::string image_url;
    std::string uri;
    int duration = 0;
    int index = 0;
};

// Current player queue state
struct QueueState {
    std::string queue_id;
    std::string current_item_id;
    int current_index = 0;
    float elapsed_time = 0.0f;
    float duration = 0.0f;
    PlayerState state = PlayerState::IDLE;
    RepeatMode repeat_mode = RepeatMode::OFF;
    bool shuffle_enabled = false;
    int volume = 50;
    bool muted = false;
    std::string current_track_name;
    std::string current_track_artist;
    std::string current_track_image;
};

// Server info
struct ServerInfo {
    std::string server_id;
    std::string server_name;
    std::string server_version;
    int schema_version = 0;
    int min_supported_schema = 0;
};

// Navigation history entry
struct NavEntry {
    std::string path;
    int scroll_position = 0;
};

class App {
public:
    static App& instance();

    // Lifecycle
    void init();
    void shutdown();
    bool isRunning() const { return m_running; }

    // Server connection
    std::string getServerUrl() const { return m_serverUrl; }
    void setServerUrl(const std::string& url) { m_serverUrl = url; }
    std::string getAuthToken() const { return m_authToken; }
    void setAuthToken(const std::string& token) { m_authToken = token; }
    std::string getPlayerId() const { return m_playerId; }
    void setPlayerId(const std::string& id) { m_playerId = id; }
    std::string getQueueId() const { return m_queueId; }
    void setQueueId(const std::string& id) { m_queueId = id; }
    ServerInfo getServerInfo() const { return m_serverInfo; }
    void setServerInfo(const ServerInfo& info) { m_serverInfo = info; }
    bool isConnected() const { return m_connected; }
    void setConnected(bool connected) { m_connected = connected; }

    // Queue state
    QueueState getQueueState() const;
    void setQueueState(const QueueState& state);

    // Settings
    void loadSettings();
    void saveSettings();
    std::string getSettingsPath() const;
    std::string getDataPath() const;
    std::string getLogPath() const;

    // Background playback
    bool isBackgroundPlaybackEnabled() const { return m_bgPlaybackEnabled; }
    void setBackgroundPlaybackEnabled(bool enabled) { m_bgPlaybackEnabled = enabled; }

    // Audio quality
    std::string getAudioQuality() const { return m_audioQuality; }
    void setAudioQuality(const std::string& quality) { m_audioQuality = quality; }

private:
    App() = default;
    ~App() = default;
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    bool m_running = false;
    bool m_connected = false;
    bool m_bgPlaybackEnabled = true;

    std::string m_serverUrl;
    std::string m_authToken;
    std::string m_playerId = "vita-music-assistant";
    std::string m_queueId;
    std::string m_audioQuality = "flac";

    ServerInfo m_serverInfo;
    QueueState m_queueState;
    mutable std::mutex m_queueMutex;
};

} // namespace vita_ma
