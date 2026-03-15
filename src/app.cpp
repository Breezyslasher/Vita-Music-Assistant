#include "app.h"
#include "app/application.hpp"
#include "app/ma_client.hpp"
#include "player/mpv_player.hpp"

#include <fstream>
#include <sstream>
#include <cstring>

#ifdef __vita__
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#endif

namespace vita_ma {

App& App::instance() {
    static App instance;
    return instance;
}

void App::init() {
    m_running = true;

    // Create data directory
#ifdef __vita__
    sceIoMkdir("ux0:data/VitaMA", 0777);
    sceIoMkdir("ux0:data/VitaMA/cache", 0777);
#endif

    loadSettings();

    brls::Logger::info("App initialized, server: {}", m_serverUrl);

    // Start the application (shows login or main activity)
    Application::instance().start();
}

void App::shutdown() {
    m_running = false;

    // Disconnect from server
    MAClient::instance().disconnect();

    // Stop player
    MpvPlayer::instance().destroy();

    saveSettings();
    brls::Logger::info("App shutdown");
}

QueueState App::getQueueState() const {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    return m_queueState;
}

void App::setQueueState(const QueueState& state) {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_queueState = state;
}

std::string App::getDataPath() const {
#ifdef __vita__
    return "ux0:data/VitaMA";
#else
    return "./data";
#endif
}

std::string App::getSettingsPath() const {
    return getDataPath() + "/settings.cfg";
}

std::string App::getLogPath() const {
    return getDataPath() + "/vita_ma.log";
}

// Simple key=value config parser (no external JSON library needed)
void App::loadSettings() {
    std::ifstream file(getSettingsPath());
    if (!file.is_open()) {
        brls::Logger::info("No settings file found, using defaults");
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);

        // Trim whitespace
        while (!key.empty() && key.back() == ' ') key.pop_back();
        while (!value.empty() && value.front() == ' ') value.erase(value.begin());

        if (key == "server_url") m_serverUrl = value;
        else if (key == "auth_token") m_authToken = value;
        else if (key == "player_id") m_playerId = value;
        else if (key == "audio_quality") m_audioQuality = value;
        else if (key == "bg_playback") m_bgPlaybackEnabled = (value == "1" || value == "true");
    }
    file.close();

    brls::Logger::info("Settings loaded: server={}", m_serverUrl);
}

void App::saveSettings() {
    std::ofstream file(getSettingsPath());
    if (!file.is_open()) {
        brls::Logger::error("Failed to save settings");
        return;
    }

    file << "# Vita Music Assistant Settings\n";
    file << "server_url=" << m_serverUrl << "\n";
    file << "auth_token=" << m_authToken << "\n";
    file << "player_id=" << m_playerId << "\n";
    file << "audio_quality=" << m_audioQuality << "\n";
    file << "bg_playback=" << (m_bgPlaybackEnabled ? "1" : "0") << "\n";

    file.close();
    brls::Logger::info("Settings saved");
}

} // namespace vita_ma
