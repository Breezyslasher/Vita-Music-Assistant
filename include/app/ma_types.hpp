/**
 * Vita Music Assistant - Music Assistant API Client
 * Handles all communication with Music Assistant servers via WebSocket and HTTP
 *
 * Data types and structures for Music Assistant integration.
 */

#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>

namespace vita_ma {

// Media types (music only)
enum class MediaType {
    UNKNOWN,
    ARTIST,
    ALBUM,
    TRACK,
    PLAYLIST,
    RADIO
};

// Music item info (maps to Music Assistant media items)
struct MusicItem {
    std::string itemId;        // MA item ID
    std::string uri;           // library://track/123
    std::string name;
    std::string sortName;
    std::string summary;
    std::string imageUrl;
    MediaType mediaType = MediaType::UNKNOWN;

    // Track info
    std::string artistName;
    std::string albumName;
    std::string artistId;
    std::string albumId;
    int trackNumber = 0;
    int discNumber = 0;
    int duration = 0;          // seconds

    // Album info
    int year = 0;
    std::string version;
    int trackCount = 0;

    // Artist info
    std::string biography;

    // Playlist info
    int itemCount = 0;
    bool isEditable = false;

    // Favorite
    bool favorite = false;

    // Provider
    std::string provider;
};

// Hub (for home screen)
struct Hub {
    std::string title;
    std::string type;
    std::vector<MusicItem> items;
};

// Queue item
struct QueueItem {
    std::string queueItemId;
    std::string uri;
    std::string name;
    std::string artistName;
    std::string albumName;
    std::string imageUrl;
    int duration = 0;
    int trackNumber = 0;
    MediaType mediaType = MediaType::UNKNOWN;
};

// Queue state
struct QueueState {
    std::string queueId;
    std::string currentItemId;
    int currentIndex = 0;
    int elapsed = 0;           // seconds
    int duration = 0;          // seconds
    bool shuffleEnabled = false;
    std::string repeatMode;    // "off", "one", "all"
    std::string state;         // "playing", "paused", "idle"
    std::vector<QueueItem> items;
};

// Player info from MA server
struct PlayerInfo {
    std::string playerId;
    std::string name;
    std::string type;
    bool powered = false;
    bool available = true;
    int volume = 100;
    bool muted = false;
};

} // namespace vita_ma
