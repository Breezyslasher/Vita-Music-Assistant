/**
 * Vita Music Assistant - Music Assistant Client for PlayStation Vita
 * Borealis-based Application
 */

#pragma once

#include <string>
#include <functional>

// Application version
#define VMA_VERSION "2.0.0"
#define VMA_VERSION_NUM 200

// Music Assistant client identification
#define MA_CLIENT_ID "vita-music-assistant-001"
#define MA_CLIENT_NAME "Vita Music Assistant"
#define MA_CLIENT_VERSION VMA_VERSION
#define MA_PLATFORM "PlayStation Vita"
#define MA_DEVICE "PS Vita"

namespace vita_ma {

// Theme options
enum class AppTheme {
    SYSTEM = 0,
    LIGHT = 1,
    DARK = 2
};

// Audio quality options
enum class AudioQuality {
    LOSSLESS = 0,       // FLAC lossless
    HIGH = 1,           // 320kbps
    NORMAL = 2,         // 192kbps
    LOW = 3             // 96kbps
};

// Default action when selecting a track in album view
enum class TrackDefaultAction {
    PLAY_NEXT = 0,
    PLAY_NOW_REPLACE = 1,
    ADD_TO_BOTTOM = 2,
    PLAY_NOW_CLEAR = 3,
    ASK_EACH_TIME = 4
};

// Application settings structure
struct AppSettings {
    // UI Settings
    AppTheme theme = AppTheme::DARK;
    bool debugLogging = true;
    bool showDebugTab = true;

    // Layout Settings
    bool collapseSidebar = false;
    std::string sidebarOrder;

    // Content Display Settings
    bool showPlaylists = true;
    bool hideTitlesInGrid = false;

    // Playback Settings
    bool autoPlayNext = true;
    int seekInterval = 10;  // seconds
    int controlsAutoHideSeconds = 5;

    // Audio Settings
    AudioQuality audioQuality = AudioQuality::NORMAL;

    // Network Settings
    int connectionTimeout = 180;

    // Music Settings
    TrackDefaultAction trackDefaultAction = TrackDefaultAction::ASK_EACH_TIME;
    bool backgroundMusic = true;

    // Remote Access
    std::string remoteId;
    bool remoteAccessEnabled = false;
};

/**
 * Application singleton - manages app lifecycle and global state
 */
class Application {
public:
    static Application& getInstance();

    // Initialize and run the application
    bool init();
    void run();
    void shutdown();

    // Navigation
    void pushLoginActivity();
    void pushMainActivity();
    void pushPlayerActivity(const std::string& queueId);

    // Authentication state
    bool isLoggedIn() const { return !m_authToken.empty(); }
    const std::string& getAuthToken() const { return m_authToken; }
    void setAuthToken(const std::string& token) { m_authToken = token; }
    const std::string& getServerUrl() const { return m_serverUrl; }
    void setServerUrl(const std::string& url) { m_serverUrl = url; }

    // Settings persistence
    bool loadSettings();
    bool saveSettings();

    // User info
    const std::string& getUsername() const { return m_username; }
    void setUsername(const std::string& name) { m_username = name; }

    // Application settings access
    AppSettings& getSettings() { return m_settings; }
    const AppSettings& getSettings() const { return m_settings; }

    // Apply theme
    void applyTheme();

    // Apply log level based on settings
    void applyLogLevel();

    // Get quality string for display
    static std::string getQualityString(AudioQuality quality);
    static std::string getThemeString(AppTheme theme);

private:
    Application() = default;
    ~Application() = default;
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    bool m_initialized = false;
    std::string m_authToken;
    std::string m_serverUrl;
    std::string m_username;
    AppSettings m_settings;
};

} // namespace vita_ma
