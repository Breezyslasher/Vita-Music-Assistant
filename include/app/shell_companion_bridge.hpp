/**
 * Vita Music Assistant - vita-shell-companion bridge
 *
 * Loads the SceShell plugin (lib/vita-shell-companion) at start-up and drives
 * the quick menu's Music row from our own Music Assistant player: the song,
 * play state and cover while it plays, and the row's ⏮ ⏯ ⏭ sent back to MA as
 * queue commands. The row is given back when playback stops.
 *
 * Everything runs from tick() on the main loop; the only other thread is a
 * short-lived cover fetch.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

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

private:
    ShellCompanionBridge() = default;

    void requestState();
    void applyRow();
    void handlePress(int press);
    void maybeFetchCover();

    bool m_open = false;
    uint64_t m_lastPollUs = 0;
    uint64_t m_lastBeatUs = 0;
    bool m_stateInFlight = false;

    // Our own player's now-playing state, from players/get (main thread).
    bool m_active = false;   // a track is loaded (playing or paused)
    bool m_playing = false;
    std::string m_title, m_artist, m_album, m_imageUrl;

    // What the row was last given, to send only changes.
    bool m_rowActive = false;
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
