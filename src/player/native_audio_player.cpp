/**
 * Vita Music Assistant - Native audio player implementation
 */

#include "player/native_audio_player.hpp"
#include <borealis.hpp>
#include <cstring>
#include <cstdio>

// dr_flac: single-header FLAC decoder (public domain / MIT-0). We drive it from
// callbacks over the pushed byte stream; no file IO or Ogg needed.
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_OGG
#include "dr_flac.h"

#ifdef __vita__
#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#endif

namespace vita_ma {

// sceAudioOut plays a fixed number of frames per output call ("grain"). 1024
// frames @ 44100 Hz is ~23ms of audio - small enough for responsiveness, large
// enough that the blocking output call paces the decoder smoothly.
static constexpr int GRAIN = 1024;
// Bail out if the encoded backlog explodes (output stalled / port dead) rather
// than growing memory without bound on a 256 MB device.
static constexpr size_t MAX_BACKLOG = 8 * 1024 * 1024;

NativeAudioPlayer& NativeAudioPlayer::instance() {
    static NativeAudioPlayer inst;
    return inst;
}

NativeAudioPlayer::~NativeAudioPlayer() {
    stop();
}

void NativeAudioPlayer::startStream(const std::string& codec, int sampleRate,
                                    int channels, int bitDepth,
                                    const std::vector<uint8_t>& codecHeader) {
    stop();  // tear down any previous stream

    m_codec = codec;
    m_sampleRate = sampleRate > 0 ? sampleRate : 44100;
    m_channels = (channels == 1 || channels == 2) ? channels : 2;
    m_bitDepth = bitDepth > 0 ? bitDepth : 16;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_encoded.clear();
        m_readPos = 0;
        m_baseOffset = 0;
        // Seed the decoder with the container header (FLAC needs fLaC+STREAMINFO).
        std::vector<uint8_t> hdr = codecHeader;
        if (m_codec == "flac" && !hdr.empty()) {
            // dr_flac's native open requires the "fLaC" marker. Some servers send
            // only the STREAMINFO metadata block; prepend the magic if missing.
            bool hasMagic = hdr.size() >= 4 && hdr[0] == 'f' && hdr[1] == 'L' &&
                            hdr[2] == 'a' && hdr[3] == 'C';
            if (!hasMagic) {
                brls::Logger::info("NativeAudio: FLAC header missing fLaC magic, prepending");
                hdr.insert(hdr.begin(), {'f', 'L', 'a', 'C'});
            }
            // Log the first bytes for diagnosis.
            char hex[64] = {0};
            for (size_t i = 0; i < hdr.size() && i < 12; i++)
                snprintf(hex + i * 3, 4, "%02x ", hdr[i]);
            brls::Logger::info("NativeAudio: FLAC header[{}]: {}", hdr.size(), hex);
        }
        if (!hdr.empty()) {
            m_encoded.insert(m_encoded.end(), hdr.begin(), hdr.end());
        }
        m_headerLen = m_encoded.size();
    }
    m_endOfStream.store(false);
    m_running.store(true);

    brls::Logger::info("NativeAudio: starting {} {}Hz {}ch {}bit",
                       m_codec, m_sampleRate, m_channels, m_bitDepth);
    m_thread = std::thread([this]() { outputLoop(); });
}

void NativeAudioPlayer::pushAudio(const uint8_t* data, size_t len) {
    if (!m_running.load() || len == 0) return;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // Compact the consumed prefix occasionally so the vector doesn't grow
        // forever while streaming.
        if (m_readPos > 1024 * 1024 && m_readPos * 2 > m_encoded.size()) {
            m_baseOffset += m_readPos;
            m_encoded.erase(m_encoded.begin(), m_encoded.begin() + m_readPos);
            m_readPos = 0;
        }
        if (m_encoded.size() - m_readPos > MAX_BACKLOG) {
            brls::Logger::warning("NativeAudio: backlog over cap, stopping");
            m_running.store(false);
        } else {
            m_encoded.insert(m_encoded.end(), data, data + len);
        }
    }
    m_cv.notify_one();
}

void NativeAudioPlayer::endStream() {
    m_endOfStream.store(true);
    m_cv.notify_one();
    if (m_thread.joinable()) m_thread.join();
    m_running.store(false);
}

void NativeAudioPlayer::stop() {
    m_running.store(false);
    m_endOfStream.store(true);
    m_cv.notify_one();
    if (m_thread.joinable()) m_thread.join();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_encoded.clear();
        m_readPos = 0;
    }
    m_endOfStream.store(false);
}

size_t NativeAudioPlayer::readEncoded(void* out, size_t bytes) {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_cv.wait(lock, [this]() {
        return (m_encoded.size() - m_readPos) > 0 || m_endOfStream.load() ||
               !m_running.load();
    });
    if (!m_running.load()) return 0;
    size_t avail = m_encoded.size() - m_readPos;
    if (avail == 0) return 0;  // end of stream, drained
    size_t n = bytes < avail ? bytes : avail;
    std::memcpy(out, m_encoded.data() + m_readPos, n);
    m_readPos += n;
    return n;
}

#ifdef __vita__
// dr_flac callbacks: pull from / reposition within the encoded ring.
static size_t drflacReadCb(void* pUserData, void* pBufferOut, size_t bytesToRead) {
    return static_cast<NativeAudioPlayer*>(pUserData)->readEncodedForCallback(pBufferOut, bytesToRead);
}
static drflac_bool32 drflacSeekCb(void* pUserData, int offset, drflac_seek_origin origin) {
    return static_cast<NativeAudioPlayer*>(pUserData)->seekForCallback(offset, (int)origin)
               ? DRFLAC_TRUE : DRFLAC_FALSE;
}
static drflac_bool32 drflacTellCb(void* pUserData, drflac_int64* pCursor) {
    int64_t c = 0;
    bool ok = static_cast<NativeAudioPlayer*>(pUserData)->tellForCallback(&c);
    if (pCursor) *pCursor = (drflac_int64)c;
    return ok ? DRFLAC_TRUE : DRFLAC_FALSE;
}
#endif

void NativeAudioPlayer::outputLoop() {
#ifdef __vita__
    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, GRAIN, m_sampleRate,
                                   SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0) {
        brls::Logger::error("NativeAudio: sceAudioOutOpenPort failed: 0x{:08x}",
                            (unsigned)port);
        m_running.store(false);
        return;
    }

    // Interleaved stereo S16 output buffer for one grain.
    std::vector<int16_t> outBuf(GRAIN * 2, 0);

    if (m_codec == "flac") {
        // Wait for the first audio frame to arrive before opening, so dr_flac's
        // header init has header + frame data to read/validate and never sees a
        // premature short/EOF read.
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this]() {
                return m_encoded.size() > m_headerLen || m_endOfStream.load() ||
                       !m_running.load();
            });
        }
        if (!m_running.load()) { sceAudioOutReleasePort(port); return; }

        drflac* flac = drflac_open(drflacReadCb, drflacSeekCb, drflacTellCb, this, nullptr);
        if (!flac) {
            brls::Logger::error("NativeAudio: drflac_open failed");
            sceAudioOutReleasePort(port);
            m_running.store(false);
            return;
        }
        brls::Logger::info("NativeAudio: FLAC opened {}ch {}Hz",
                           flac->channels, flac->sampleRate);
        // Temp decode buffer sized to the stream's channel count.
        std::vector<int16_t> dec(GRAIN * flac->channels, 0);
        while (m_running.load()) {
            drflac_uint64 got = drflac_read_pcm_frames_s16(flac, GRAIN, dec.data());
            if (got == 0) break;  // end of stream
            // Map to interleaved stereo (upmix mono, passthrough stereo, take
            // first two channels otherwise).
            for (int i = 0; i < GRAIN; i++) {
                if (i < (int)got) {
                    if (flac->channels == 1) {
                        outBuf[i * 2] = outBuf[i * 2 + 1] = dec[i];
                    } else {
                        outBuf[i * 2]     = dec[i * flac->channels];
                        outBuf[i * 2 + 1] = dec[i * flac->channels + 1];
                    }
                } else {
                    outBuf[i * 2] = outBuf[i * 2 + 1] = 0;  // zero-pad last grain
                }
            }
            sceAudioOutOutput(port, outBuf.data());
        }
        drflac_close(flac);
    } else {
        // Raw interleaved S16 PCM. Read one grain worth of bytes at a time.
        const size_t grainBytes = (size_t)GRAIN * m_channels * sizeof(int16_t);
        std::vector<uint8_t> raw(grainBytes, 0);
        while (m_running.load()) {
            size_t filled = 0;
            while (filled < grainBytes && m_running.load()) {
                size_t n = readEncoded(raw.data() + filled, grainBytes - filled);
                if (n == 0) break;  // end of stream
                filled += n;
            }
            if (filled == 0) break;
            std::memset(raw.data() + filled, 0, grainBytes - filled);  // zero-pad
            const int16_t* src = reinterpret_cast<const int16_t*>(raw.data());
            for (int i = 0; i < GRAIN; i++) {
                if (m_channels == 1) {
                    outBuf[i * 2] = outBuf[i * 2 + 1] = src[i];
                } else {
                    outBuf[i * 2]     = src[i * m_channels];
                    outBuf[i * 2 + 1] = src[i * m_channels + 1];
                }
            }
            sceAudioOutOutput(port, outBuf.data());
        }
    }

    sceAudioOutReleasePort(port);
    m_running.store(false);
    brls::Logger::info("NativeAudio: output stopped");
#else
    m_running.store(false);
#endif
}

// Thin forwarders so the free C callbacks can reach the private ring.
size_t NativeAudioPlayer::readEncodedForCallback(void* out, size_t bytes) {
    return readEncoded(out, bytes);
}

bool NativeAudioPlayer::tellForCallback(int64_t* cursor) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (cursor) *cursor = (int64_t)(m_baseOffset + m_readPos);
    return true;
}

bool NativeAudioPlayer::seekForCallback(int offset, int origin) {
    std::lock_guard<std::mutex> lock(m_mutex);
    // origin: 0 = SET (absolute), 1 = CUR (relative). END is unsupported on a
    // live stream.
    int64_t cur = (int64_t)(m_baseOffset + m_readPos);
    int64_t target = (origin == 1) ? cur + offset : offset;
    int64_t low = (int64_t)m_baseOffset;
    int64_t high = (int64_t)(m_baseOffset + m_encoded.size());
    if (target < low || target > high) return false;  // outside the buffered window
    m_readPos = (size_t)(target - low);
    return true;
}

} // namespace vita_ma
