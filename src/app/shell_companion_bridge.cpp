/**
 * Vita Music Assistant - vita-shell-companion bridge implementation
 */

#include "app/shell_companion_bridge.hpp"
#include "app/ma_client.hpp"
#include "app/ma_types.hpp"
#include "utils/image_loader.hpp"

#include "shell_companion.h"
#include "sc_cover.h"

#include <borealis.hpp>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <thread>
#include <vector>

namespace vita_ma {

namespace {
constexpr const char* SC_TITLE_ID = "VMASS0001";
constexpr const char* SC_DATA_DIR = "ux0:data/VitaMA";
constexpr const char* SC_APP_NAME = "Music Assistant";
constexpr const char* SC_PLUGIN_PATH = "app0:module/shell_companion.suprx";
// The same file by its installed path. The plugin is loaded into SceShell's
// process, where app0: may not be ours: on a console the app0: load failed
// with 0x80010013 (ENODEV) while the file was there.
constexpr const char* SC_PLUGIN_PATH_INSTALLED = "ux0:app/VMASS0001/module/shell_companion.suprx";

constexpr uint64_t POLL_EVERY_US = 250 * 1000;   // row button presses
constexpr uint64_t BEAT_EVERY_US = 1000 * 1000;  // heartbeat + state refresh
constexpr int ALIVE_LOG_EVERY_S = 5;             // heartbeat thread's "alive" line

uint64_t nowUs() { return sceKernelGetSystemTimeWide(); }

// current_media.image_url is a full URL on newer servers, else a path the
// imageproxy resolves (server-relative in remote mode, tunnelled by
// ImageLoader). Same rule as the player's cover.
std::string resolveCoverUrl(const std::string& imageUrl) {
    if (imageUrl.empty()) return "";
    if (imageUrl.rfind("http://", 0) == 0 || imageUrl.rfind("https://", 0) == 0)
        return imageUrl;
    return MAClient::instance().getThumbnailUrl(imageUrl, 256, 256, "");
}
} // namespace

ShellCompanionBridge& ShellCompanionBridge::instance() {
    static ShellCompanionBridge inst;
    return inst;
}

void ShellCompanionBridge::start() {
    int r = scInit(SC_TITLE_ID, SC_DATA_DIR, SC_APP_NAME, SC_FEAT_ROW | SC_FEAT_MUSIC,
                   SC_PLUGIN_PATH);
    if (r != 0) {
        brls::Logger::error("ShellCompanion: scInit failed ({})", r);
        return;
    }
    char what[256] = {0};
    uint64_t t0 = nowUs();
    r = scOpen(what, sizeof(what));
    brls::Logger::info("ShellCompanion: scOpen -> {} in {}ms: {}", r,
                       (long)((nowUs() - t0) / 1000), what);
    if (r != 0) {
        scInit(SC_TITLE_ID, SC_DATA_DIR, SC_APP_NAME, SC_FEAT_ROW | SC_FEAT_MUSIC,
               SC_PLUGIN_PATH_INSTALLED);
        what[0] = 0;
        t0 = nowUs();
        r = scOpen(what, sizeof(what));
        brls::Logger::info("ShellCompanion: retry from installed path: scOpen -> {} in {}ms: {}",
                           r, (long)((nowUs() - t0) / 1000), what);
    }
    m_open = (r == 0);
    if (!m_open) return;

    m_heartbeatRun = true;
    m_heartbeat = std::thread([this]() { heartbeat(); });

    ScPluginInfo info{};
    if (scPluginInfo(&info) == 0) {
        m_pluginInfo = true;
        m_granted = info.granted;
        m_caps = info.caps;
        brls::Logger::info("ShellCompanion: plugin {} ({}, protocol {}, written {}), "
                           "features 0x{:x}, granted 0x{:x}, caps '{}'",
                           info.version, info.versionCode, info.protocol, info.written,
                           info.features, info.granted, info.caps);
    } else {
        // Another app loaded an older plugin first this boot; it stays in
        // SceShell until the Vita restarts.
        brls::Logger::warning("ShellCompanion: plugin never answered HELLO: older than 0.2.0 "
                              "(loaded by another app?), restart the Vita to load ours");
    }
}

// Runs whether or not the app is in front: re-sends the row's heartbeat every
// second (the row is dropped after SC_ROW_TIMEOUT_S without one) and logs
// every 5 s how long ago the main loop last ticked, which shows on the
// console whether the app, and its UI loop, keep running after PS.
void ShellCompanionBridge::heartbeat() {
    int n = 0;
    while (m_heartbeatRun) {
        sceKernelDelayThread(1000 * 1000);
        if (!m_heartbeatRun) break;
        if (m_rowActive) scRowActivate();
        if (++n % ALIVE_LOG_EVERY_S == 0) {
            uint64_t last = m_lastTickUs.load();
            brls::Logger::info("ShellCompanion: alive, row {}, main loop last ticked {}ms ago",
                               m_rowActive ? "active" : "idle",
                               last ? (long)((nowUs() - last) / 1000) : -1L);
        }
    }
}

bool ShellCompanionBridge::hasCap(const char* cap) const {
    if (!m_open || !m_pluginInfo) return false;
    const std::string want(cap);
    size_t at = 0;
    while (at <= m_caps.size()) {
        size_t end = m_caps.find(',', at);
        if (end == std::string::npos) end = m_caps.size();
        if (m_caps.compare(at, end - at, want) == 0 && end - at == want.size()) return true;
        at = end + 1;
    }
    return false;
}

bool ShellCompanionBridge::installUpdateVpk(const char* vpkPath) {
    if (!m_open) return false;
    int r = scUpdateInstallVpk(vpkPath);
    brls::Logger::info("ShellCompanion: update {} handed to the plugin -> {}", vpkPath, r);
    if (r < 0) return false;
    if (m_rowActive) {
        scRowRelease();
        m_rowActive = false;
    }
    return true;
}

int ShellCompanionBridge::probe(int mode) {
    if (!m_open) {
        brls::Logger::warning("ShellCompanion: probe {} not sent, plugin not open", mode);
        return -1;
    }
    if (m_pluginInfo && !(m_granted & SC_FEAT_MUSIC)) {
        brls::Logger::warning("ShellCompanion: probe {} not sent, Music not granted", mode);
        return -2;
    }
    int r = scProbeStream(mode, nullptr);
    brls::Logger::info("ShellCompanion: probe {} sent -> {}", mode, r);
    return r;
}

void ShellCompanionBridge::tick() {
    if (!m_open) return;
    uint64_t now = nowUs();
    m_lastTickUs = now;

    if (now - m_lastPollUs >= POLL_EVERY_US) {
        m_lastPollUs = now;
        int press = scPoll();
        if (press != SC_NONE) handlePress(press);
    }

    if (now - m_lastBeatUs >= BEAT_EVERY_US) {
        m_lastBeatUs = now;
        requestState();
        applyRow();
    }
}

void ShellCompanionBridge::stop() {
    if (m_heartbeat.joinable()) {
        m_heartbeatRun = false;
        m_heartbeat.join();
    }
    if (m_open && m_rowActive) {
        scRowRelease();
        m_rowActive = false;
        brls::Logger::info("ShellCompanion: row released (app closing)");
    }
}

void ShellCompanionBridge::requestState() {
    if (m_stateInFlight) return;
    auto& client = MAClient::instance();
    std::string playerId = App::instance().getPlayerId();
    if (!client.isConnected() || playerId.empty()) return;

    m_stateInFlight = true;
    Json args;
    args["player_id"] = Json(playerId);
    // The callback runs on the main thread (MAClient dispatches there), as
    // does tick(), so the state fields need no lock.
    client.sendCommand("players/get", args, [this](bool success, const Json& result) {
        m_stateInFlight = false;
        if (!success || result.type() != Json::OBJECT) return;

        std::string state = result.has("state") ? result["state"].str() : "";
        std::string title, artist, album, image;
        if (result.has("current_media") && result["current_media"].type() == Json::OBJECT) {
            const Json& m = result["current_media"];
            if (m.has("title")) title = m["title"].str();
            if (title.empty() && m.has("name")) title = m["name"].str();
            if (m.has("artist")) artist = m["artist"].str();
            if (m.has("album")) album = m["album"].str();
            if (m.has("image_url")) image = m["image_url"].str();
        }
        m_active = !title.empty() && (state == "playing" || state == "paused");
        m_playing = (state == "playing");
        m_title = title;
        m_artist = artist;
        m_album = album;
        m_imageUrl = image;
    });
}

void ShellCompanionBridge::applyRow() {
    if (!m_active) {
        if (m_rowActive) {
            scRowRelease();
            m_rowActive = false;
            m_sentPlaying = -1;
            m_sentTitle.clear();
            m_sentArtist.clear();
            m_sentAlbum.clear();
            {
                std::lock_guard<std::mutex> lk(m_coverMutex);
                m_coverWanted.clear();
                m_coverDone.clear();
            }
            brls::Logger::info("ShellCompanion: playback stopped, row released");
        }
        return;
    }

    // Heartbeat: keeps the row ours while we send it every second.
    scRowActivate();
    if (!m_rowActive) {
        m_rowActive = true;
        brls::Logger::info("ShellCompanion: row activated");
    }

    if (m_title != m_sentTitle || m_artist != m_sentArtist || m_album != m_sentAlbum) {
        scRowSong(m_title.c_str(), m_artist.c_str(), m_album.c_str());
        m_sentTitle = m_title;
        m_sentArtist = m_artist;
        m_sentAlbum = m_album;
        brls::Logger::info("ShellCompanion: row song '{}' / '{}' / '{}'", m_title, m_artist,
                           m_album);
    }
    int playing = m_playing ? 1 : 0;
    if (playing != m_sentPlaying) {
        scRowPlaying(playing);
        m_sentPlaying = playing;
    }

    {
        std::lock_guard<std::mutex> lk(m_coverMutex);
        std::string wanted = resolveCoverUrl(m_imageUrl);
        // A new song's cover: a fetch still running for the old one must not
        // set the row when it finishes.
        if (wanted != m_coverWanted) ++m_coverGen;
        m_coverWanted = wanted;
    }
    maybeFetchCover();
}

void ShellCompanionBridge::maybeFetchCover() {
    std::string wanted;
    {
        std::lock_guard<std::mutex> lk(m_coverMutex);
        if (m_coverWanted == m_coverDone) return;
        wanted = m_coverWanted;
    }
    if (m_coverInFlight.exchange(true)) return;  // one at a time; retried next beat

    uint32_t gen = ++m_coverGen;
    if (wanted.empty()) {
        scRowArt(0);
        {
            std::lock_guard<std::mutex> lk(m_coverMutex);
            m_coverDone = wanted;
        }
        m_coverInFlight = false;
        return;
    }

    std::thread([this, wanted, gen]() {
        std::vector<uint8_t> bytes;
        char why[128] = {0};
        bool ok = ImageLoader::fetchBytes(wanted, bytes);
        if (ok && gen == m_coverGen.load()) {
            uint32_t serial = scCoverNextSerial(m_lastSerial.load(), nowUs());
            if (scCoverWrite(bytes.data(), bytes.size(), serial, why, sizeof(why)) == 0) {
                if (gen == m_coverGen.load()) {
                    scRowArt(serial);
                    m_lastSerial = serial;
                }
            } else {
                ok = false;
            }
        }
        if (!ok) {
            brls::Logger::warning("ShellCompanion: cover not set ({}): {}",
                                  why[0] ? why : "fetch failed", wanted);
            scRowArt(0);
        }
        {
            // Done either way, so a failing cover isn't refetched every second.
            std::lock_guard<std::mutex> lk(m_coverMutex);
            m_coverDone = wanted;
        }
        m_coverInFlight = false;
    }).detach();
}

void ShellCompanionBridge::handlePress(int press) {
    std::string playerId = App::instance().getPlayerId();
    if (playerId.empty()) return;
    auto& client = MAClient::instance();

    switch (press) {
        case SC_PLAY_PAUSE:
            brls::Logger::info("ShellCompanion: row play/pause (was {})",
                               m_playing ? "playing" : "paused");
            if (m_playing) client.queuePause(playerId);
            else client.queuePlay(playerId);
            // Show the change at once; the next players/get confirms it.
            m_playing = !m_playing;
            scRowPlaying(m_playing ? 1 : 0);
            m_sentPlaying = m_playing ? 1 : 0;
            break;
        case SC_NEXT:
            brls::Logger::info("ShellCompanion: row next");
            client.queueNext(playerId);
            break;
        case SC_PREVIOUS:
            brls::Logger::info("ShellCompanion: row previous");
            client.queuePrevious(playerId);
            break;
        default:
            return;
    }
    // Refresh the row from the server on the next beat.
    m_lastBeatUs = 0;
}

} // namespace vita_ma
