#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <thread>
#include <deque>

namespace vita_ma {

/**
 * Local HTTP server that bridges Sendspin audio data to MPV.
 *
 * Sendspin delivers audio as WebSocket binary frames on the receive thread.
 * MPV needs an HTTP URL to stream from. This server listens on 127.0.0.1,
 * accepts one connection from MPV, and feeds it audio data in real-time.
 *
 * Flow:
 *   Sendspin binary frame -> pushAudioData() -> queue -> HTTP response -> MPV
 */
class AudioStreamServer {
public:
    AudioStreamServer();
    ~AudioStreamServer();

    // Start the server on a random local port.
    // Returns true if the server started successfully.
    bool start();

    // Stop the server and clean up.
    void stop();

    // Get the URL that MPV should load (e.g. "http://127.0.0.1:12345/audio.flac")
    std::string getStreamUrl(const std::string& codec = "flac") const;

    // Push audio data into the queue. Called from the Sendspin receive thread.
    // Thread-safe.
    void pushAudioData(const uint8_t* data, size_t size);

    // Signal that the stream is complete (no more data will be pushed).
    void signalStreamEnd();

    // Reset for a new stream (clears queue, resets EOF flag).
    void resetStream();

    // Set the codec for the current stream (determines Content-Type header)
    void setCodec(const std::string& codec) { m_codec = codec; }

    // Set codec container header to prepend before audio data (e.g. FLAC header)
    void setCodecHeader(const std::vector<uint8_t>& header) { m_codecHeader = header; }

    // Check if the server has buffered enough initial data for MPV to probe
    bool hasInitialData() const { return m_hasInitialData.load(); }

    // Check if server is running
    bool isRunning() const { return m_running.load(); }

    // Get the port the server is listening on
    int getPort() const { return m_port; }

private:
    void serverLoop();
    void handleClient(int clientSocket);
    bool sendAll(int socket, const void* data, size_t len);

    std::thread m_serverThread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_shouldStop{false};
    int m_listenSocket = -1;
    int m_port = 0;

    // Audio data queue (producer-consumer)
    std::deque<std::vector<uint8_t>> m_queue;
    std::mutex m_queueMutex;
    std::condition_variable m_queueCv;
    std::atomic<bool> m_streamEnded{false};
    std::atomic<bool> m_hasInitialData{false};
    size_t m_bufferedBytes = 0;
    std::string m_codec = "flac";
    std::vector<uint8_t> m_codecHeader;  // Container header to prepend (e.g. fLaC + STREAMINFO)
};

} // namespace vita_ma
