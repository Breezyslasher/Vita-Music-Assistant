/**
 * Vita Music Assistant - vita-shell-companion bridge
 *
 * Loads the SceShell plugin (lib/vita-shell-companion) at start-up and drives
 * the quick menu's Music row from our own Music Assistant player: the song,
 * play state and cover while it plays, and the row's ⏮ ⏯ ⏭ sent back to MA as
 * queue commands. The row is given back when playback stops.
 *
 * State and presses run from tick() on the main loop. A heartbeat thread
 * keeps the row ours while the app is left with PS (it keeps running as a
 * non-game app, but the UI loop may not), and a short-lived thread fetches
 * covers.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace vita_ma {

class ShellCompanionBridge {
public:
    static ShellCompanionBridge& instance();

    // scInit + scOpen, logging what happened. Safe to call once at start-up.
    void start();
    // Called every main-loop iteration; throttles itself.
    void tick();
    // Give the Music row back (app closing).
    void stop();

    bool isOpen() const { return m_open; }
    // Plugin answered HELLO (0.2.0+); its version is in the log.
    bool hasPluginInfo() const { return m_pluginInfo; }

    // Debug: start the plugin's stream probe (SC_PROBE_*, tone source) or
    // stop it (SC_PROBE_STOP). Results land in the plugin's own log. 0 when
    // sent, < 0 when the plugin is not open or did not grant Music.
    int probe(int mode);

    // The plugin's answer to HELLO lists cap (scPluginHas: "update.vpk", ...).
    bool hasCap(const char* cap) const;
    // Give the row back and hand the plugin a downloaded update VPK to
    // install once the app has quit (scUpdateInstallVpk); the caller quits.
    bool installUpdateVpk(const char* vpkPath);

private:
    ShellCompanionBridge() = default;

    void requestState();
    void applyRow();
    void handlePress(int press);
    void maybeFetchCover();
    void heartbeat();

    bool m_open = false;
    bool m_pluginInfo = false;
    uint32_t m_granted = 0;
    std::string m_caps;  // ',' between, as the plugin sent them
    uint64_t m_lastPollUs = 0;
    uint64_t m_lastBeatUs = 0;
    std::atomic<uint64_t> m_lastTickUs{0};  // main loop's last tick(), for the heartbeat log
    std::atomic<bool> m_heartbeatRun{false};
    std::thread m_heartbeat;
    bool m_stateInFlight = false;

    // Our own player's now-playing state, from players/get (main thread).
    bool m_active = false;   // a track is loaded (playing or paused)
    bool m_playing = false;
    std::string m_title, m_artist, m_album, m_imageUrl;

    // What the row was last given, to send only changes.
    std::atomic<bool> m_rowActive{false};
    int m_sentPlaying = -1;
    std::string m_sentTitle, m_sentArtist, m_sentAlbum;

    // Cover: the URL wanted for the row vs the one written; one fetch at a time.
    std::mutex m_coverMutex;
    std::string m_coverWanted;
    std::string m_coverDone;
    std::atomic<bool> m_coverInFlight{false};
    std::atomic<uint32_t> m_coverGen{0};
    std::atomic<uint32_t> m_lastSerial{0};
};

} // namespace vita_ma
