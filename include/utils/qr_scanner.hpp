#pragma once

/**
 * QR Code Scanner for PS Vita
 *
 * Uses the Vita's front camera (sceCamera API) to capture frames
 * and the quirc library to decode QR codes from them.
 *
 * Usage:
 *   QRScanner scanner;
 *   scanner.start([](const std::string& data) {
 *       // Handle decoded QR data
 *   });
 *   // ... later ...
 *   scanner.stop();
 *
 * The QR code is expected to contain a JSON payload from the
 * Music Assistant companion setup, e.g.:
 *   {"server_url": "http://192.168.1.28:8095", "token": "abc123"}
 * or a remote access URI:
 *   ma-remote://MA-XXXX-XXXX/token
 */

#include <string>
#include <functional>
#include <atomic>
#include <thread>
#include <cstdint>

namespace vita_ma {

// QR scan result types
enum class QRResultType {
    UNKNOWN,
    SERVER_LOGIN,       // {"server_url": "...", "token": "..."}
    REMOTE_ACCESS,      // ma-remote://MA-XXXX-XXXX/token
    PLAIN_TEXT          // Any other QR content
};

struct QRScanResult {
    QRResultType type = QRResultType::UNKNOWN;
    std::string rawData;

    // Parsed fields (populated based on type)
    std::string serverUrl;
    std::string authToken;
    std::string remoteId;
    std::string username;
};

using QRScanCallback = std::function<void(const QRScanResult& result)>;

class QRScanner {
public:
    QRScanner();
    ~QRScanner();

    // Start scanning for QR codes using the front camera.
    // The callback is invoked on the main thread when a QR code is decoded.
    // Returns false if the camera could not be opened.
    bool start(QRScanCallback callback);

    // Stop scanning and release the camera.
    void stop();

    // Check if the scanner is currently active.
    bool isRunning() const { return m_running.load(); }

    // Parse QR data into a structured result
    static QRScanResult parseQRData(const std::string& data);

private:
    void scanLoop();

    // Convert camera frame (typically ARGB or YUV) to grayscale for quirc
    void frameToGrayscale(const uint8_t* src, uint8_t* dst, int width, int height, int srcStride);

    std::thread m_scanThread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_shouldStop{false};
    QRScanCallback m_callback;

#ifdef __vita__
    int m_cameraDevice = -1;
#endif
};

} // namespace vita_ma
