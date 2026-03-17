/**
 * Vita Music Assistant - Local HTTP Audio Stream Server
 *
 * Bridges Sendspin WebSocket audio data to MPV via a local HTTP stream.
 * MPV connects to http://127.0.0.1:<port>/audio.<ext> and receives audio
 * data in real-time as it arrives from the Sendspin protocol.
 */

#include "utils/audio_stream_server.hpp"
#include <borealis.hpp>
#include <chrono>
#include <cstring>
#include <sstream>

#ifdef __vita__
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/kernel/threadmgr.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#endif

namespace vita_ma {

AudioStreamServer::AudioStreamServer() = default;

AudioStreamServer::~AudioStreamServer() {
    stop();
}

bool AudioStreamServer::start() {
    if (m_running.load()) return true;

    m_shouldStop.store(false);
    m_streamEnded.store(false);

#ifdef __vita__
    m_listenSocket = sceNetSocket("audio_srv", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
    if (m_listenSocket < 0) {
        brls::Logger::error("AudioStreamServer: socket creation failed");
        return false;
    }

    // Allow port reuse
    int optval = 1;
    sceNetSetsockopt(m_listenSocket, SCE_NET_SOL_SOCKET, SCE_NET_SO_REUSEADDR,
                     &optval, sizeof(optval));

    SceNetSockaddrIn addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_len = sizeof(addr);
    addr.sin_family = SCE_NET_AF_INET;
    addr.sin_port = sceNetHtons(0);  // Random port
    addr.sin_addr.s_addr = sceNetHtonl(SCE_NET_INADDR_LOOPBACK);

    if (sceNetBind(m_listenSocket, (SceNetSockaddr*)&addr, sizeof(addr)) < 0) {
        brls::Logger::error("AudioStreamServer: bind failed");
        sceNetSocketClose(m_listenSocket);
        m_listenSocket = -1;
        return false;
    }

    if (sceNetListen(m_listenSocket, 1) < 0) {
        brls::Logger::error("AudioStreamServer: listen failed");
        sceNetSocketClose(m_listenSocket);
        m_listenSocket = -1;
        return false;
    }

    // Get the assigned port
    SceNetSockaddrIn boundAddr;
    unsigned int addrLen = sizeof(boundAddr);
    sceNetGetsockname(m_listenSocket, (SceNetSockaddr*)&boundAddr, &addrLen);
    m_port = sceNetNtohs(boundAddr.sin_port);

#else
    m_listenSocket = ::socket(AF_INET, SOCK_STREAM, 0);
    if (m_listenSocket < 0) {
        brls::Logger::error("AudioStreamServer: socket creation failed");
        return false;
    }

    int optval = 1;
    setsockopt(m_listenSocket, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(0);  // Random port
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::bind(m_listenSocket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        brls::Logger::error("AudioStreamServer: bind failed");
        ::close(m_listenSocket);
        m_listenSocket = -1;
        return false;
    }

    if (::listen(m_listenSocket, 1) < 0) {
        brls::Logger::error("AudioStreamServer: listen failed");
        ::close(m_listenSocket);
        m_listenSocket = -1;
        return false;
    }

    struct sockaddr_in boundAddr;
    socklen_t addrLen = sizeof(boundAddr);
    getsockname(m_listenSocket, (struct sockaddr*)&boundAddr, &addrLen);
    m_port = ntohs(boundAddr.sin_port);
#endif

    brls::Logger::info("AudioStreamServer: listening on 127.0.0.1:{}", m_port);

    m_running.store(true);
    m_serverThread = std::thread(&AudioStreamServer::serverLoop, this);

    return true;
}

void AudioStreamServer::stop() {
    if (!m_running.load()) return;

    m_shouldStop.store(true);
    m_streamEnded.store(true);
    m_queueCv.notify_all();

    // Close listen socket to unblock accept()
    if (m_listenSocket >= 0) {
#ifdef __vita__
        sceNetSocketClose(m_listenSocket);
#else
        ::close(m_listenSocket);
#endif
        m_listenSocket = -1;
    }

    if (m_serverThread.joinable()) {
        m_serverThread.join();
    }

    m_running.store(false);

    // Clear queue
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_queue.clear();
    }

    brls::Logger::info("AudioStreamServer: stopped");
}

std::string AudioStreamServer::getStreamUrl(const std::string& codec) const {
    std::string ext = "flac";
    if (codec == "pcm" || codec == "wav") ext = "wav";
    else if (codec == "mp3") ext = "mp3";
    else if (codec == "opus") ext = "ogg";

    return "http://127.0.0.1:" + std::to_string(m_port) + "/audio." + ext;
}

void AudioStreamServer::pushAudioData(const uint8_t* data, size_t size) {
    if (size == 0 || m_shouldStop.load()) return;

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_queue.emplace_back(data, data + size);
        m_bufferedBytes += size;
        // Signal that we have enough data for MPV to detect the format
        // (at least 16KB or ~4 chunks of FLAC data)
        if (m_bufferedBytes >= 16384 && !m_hasInitialData.load()) {
            m_hasInitialData.store(true);
        }
    }
    m_queueCv.notify_one();
}

void AudioStreamServer::signalStreamEnd() {
    m_streamEnded.store(true);
    m_queueCv.notify_all();
}

void AudioStreamServer::resetStream() {
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_queue.clear();
        m_bufferedBytes = 0;
    }
    m_streamEnded.store(false);
    m_hasInitialData.store(false);
    m_codecHeader.clear();
}

void AudioStreamServer::serverLoop() {
    while (!m_shouldStop.load()) {
        // Check if listen socket is still valid (stop() may have closed it)
        if (m_listenSocket < 0) break;

        // Accept a client connection (blocking)
#ifdef __vita__
        SceNetSockaddrIn clientAddr;
        unsigned int clientLen = sizeof(clientAddr);
        int clientSocket = sceNetAccept(m_listenSocket,
            (SceNetSockaddr*)&clientAddr, &clientLen);
#else
        struct sockaddr_in clientAddr;
        socklen_t clientLen = sizeof(clientAddr);
        int clientSocket = ::accept(m_listenSocket,
            (struct sockaddr*)&clientAddr, &clientLen);
#endif

        if (clientSocket < 0) {
            if (m_shouldStop.load() || m_listenSocket < 0) break;
            brls::Logger::error("AudioStreamServer: accept failed");
            // Backoff to avoid tight spin loop on persistent errors
#ifdef __vita__
            sceKernelDelayThread(100 * 1000);  // 100ms
#else
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
#endif
            continue;
        }

        brls::Logger::info("AudioStreamServer: client connected");
        handleClient(clientSocket);

#ifdef __vita__
        sceNetSocketClose(clientSocket);
#else
        ::close(clientSocket);
#endif

        brls::Logger::info("AudioStreamServer: client disconnected");
    }
}

void AudioStreamServer::handleClient(int clientSocket) {
    // Read the HTTP request (we don't really care about the content,
    // just consume it so the client doesn't block)
    char reqBuf[2048];
#ifdef __vita__
    sceNetRecv(clientSocket, reqBuf, sizeof(reqBuf), 0);
#else
    recv(clientSocket, reqBuf, sizeof(reqBuf), 0);
#endif

    // Determine Content-Type from codec
    std::string contentType = "audio/flac";
    if (m_codec == "pcm" || m_codec == "wav") contentType = "audio/wav";
    else if (m_codec == "mp3") contentType = "audio/mpeg";
    else if (m_codec == "opus") contentType = "audio/ogg";

    // Send HTTP response headers for streaming audio.
    // Use HTTP/1.0-style streaming: no Content-Length, Connection: close.
    // The body is raw audio bytes streamed until connection closes.
    // This avoids chunked transfer encoding which Vita's MPV may not handle.
    std::string headers =
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: " + contentType + "\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-cache\r\n"
        "Accept-Ranges: none\r\n"
        "\r\n";

    if (!sendAll(clientSocket, headers.c_str(), headers.size())) {
        brls::Logger::error("AudioStreamServer: failed to send headers");
        return;
    }

    brls::Logger::debug("AudioStreamServer: sent headers ({})", contentType);

    // Send codec container header first (e.g. fLaC + STREAMINFO for FLAC).
    // MPV needs this at the start of the stream for format detection.
    if (!m_codecHeader.empty()) {
        brls::Logger::info("AudioStreamServer: sending codec header ({} bytes)", m_codecHeader.size());
        if (!sendAll(clientSocket, m_codecHeader.data(), m_codecHeader.size())) {
            brls::Logger::error("AudioStreamServer: failed to send codec header");
            return;
        }
    }

    // Stream raw audio bytes until stream ends or connection breaks
    size_t totalBytesSent = 0;
    size_t chunkCount = 0;
    while (!m_shouldStop.load()) {
        std::vector<uint8_t> chunk;

        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_queueCv.wait(lock, [this]() {
                return !m_queue.empty() || m_streamEnded.load() || m_shouldStop.load();
            });

            if (m_shouldStop.load()) break;

            if (!m_queue.empty()) {
                chunk = std::move(m_queue.front());
                m_queue.pop_front();
            } else if (m_streamEnded.load()) {
                // No more data - close connection
                brls::Logger::info("AudioStreamServer: stream ended, sent {} chunks ({} bytes total)",
                    chunkCount, totalBytesSent);
                break;
            }
        }

        if (!chunk.empty()) {
            if (!sendAll(clientSocket, chunk.data(), chunk.size())) {
                brls::Logger::error("AudioStreamServer: send failed after {} chunks ({} bytes)",
                    chunkCount, totalBytesSent);
                break;
            }
            totalBytesSent += chunk.size();
            chunkCount++;
            // Log first few chunks and then periodically
            if (chunkCount <= 3 || (chunkCount % 100 == 0)) {
                brls::Logger::debug("AudioStreamServer: sent chunk #{} ({} bytes, total {})",
                    chunkCount, chunk.size(), totalBytesSent);
            }
        }
    }
}

bool AudioStreamServer::sendAll(int socket, const void* data, size_t len) {
    const uint8_t* ptr = static_cast<const uint8_t*>(data);
    size_t remaining = len;

    while (remaining > 0) {
#ifdef __vita__
        int sent = sceNetSend(socket, ptr, remaining, 0);
#else
        int sent = ::send(socket, ptr, remaining, 0);
#endif

        if (sent <= 0) return false;
        ptr += sent;
        remaining -= sent;
    }

    return true;
}

} // namespace vita_ma
