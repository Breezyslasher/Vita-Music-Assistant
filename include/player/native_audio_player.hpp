/**
 * Vita Music Assistant - Native audio player
 *
 * Decodes Sendspin audio (FLAC via dr_flac, or raw PCM) and plays it directly
 * through sceAudioOut, bypassing the local HTTP server + mpv path. Lighter and
 * lower-latency; selected by the "native audio" setting.
 *
 * Threading: Sendspin's receive thread calls pushAudio() to feed encoded bytes
 * into a ring; a single output thread pulls from that ring (blocking), decodes,
 * and writes PCM to sceAudioOut (whose output call blocks for real-time pacing).
 */

#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <cstdint>

namespace vita_ma {

class NativeAudioPlayer {
public:
    static NativeAudioPlayer& instance();

    // Begin a new stream. codec is "flac" or "pcm". codecHeader (the FLAC
    // fLaC+STREAMINFO bytes) is fed to the decoder first so it can initialize.
    void startStream(const std::string& codec, int sampleRate, int channels,
                     int bitDepth, const std::vector<uint8_t>& codecHeader);

    // Push encoded (FLAC) or raw interleaved little-endian S16 (PCM) bytes.
    void pushAudio(const uint8_t* data, size_t len);

    // No more data will arrive; drain what's buffered, then stop.
    void endStream();

    // Hard stop: tear down output immediately.
    void stop();

    bool isPlaying() const { return m_running.load(); }

    // Forwarders for dr_flac's C callbacks (they only get a void* user
    // pointer). Public so the free functions in the .cpp can reach the ring.
    // Seek/tell operate on absolute stream positions so dr_flac can reposition
    // within the still-buffered window during open.
    size_t readEncodedForCallback(void* out, size_t bytes);
    bool seekForCallback(int offset, int origin);   // origin: 0=SET,1=CUR,2=END
    bool tellForCallback(int64_t* cursor);

private:
    NativeAudioPlayer() = default;
    ~NativeAudioPlayer();
    NativeAudioPlayer(const NativeAudioPlayer&) = delete;
    NativeAudioPlayer& operator=(const NativeAudioPlayer&) = delete;

    void outputLoop();
    // Blocking read from the encoded ring; returns bytes copied, or 0 at
    // end-of-stream / stop. Used as dr_flac's read callback and for raw PCM.
    size_t readEncoded(void* out, size_t bytes);

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_endOfStream{false};

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::vector<uint8_t> m_encoded;  // byte queue (append at back, read via m_readPos)
    size_t m_readPos = 0;
    // Bytes dropped from the front by compaction; absolute stream position is
    // m_baseOffset + m_readPos. Lets seek/tell work in absolute coordinates.
    uint64_t m_baseOffset = 0;
    size_t m_headerLen = 0;  // seed header length (to defer open until a frame arrives)

    std::string m_codec;
    int m_sampleRate = 44100;
    int m_channels = 2;
    int m_bitDepth = 16;
};

} // namespace vita_ma
