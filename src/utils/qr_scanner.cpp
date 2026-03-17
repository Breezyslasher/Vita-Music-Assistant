#include "utils/qr_scanner.hpp"
#include "app/ma_client.hpp"
#include <borealis.hpp>
#include <cstring>
#include <chrono>
#include <thread>

// quirc QR decoder
extern "C" {
#include "quirc.h"
}

#ifdef __vita__
#include <psp2/camera.h>
#include <psp2/kernel/threadmgr.h>

// Camera resolution for QR scanning (lower res = faster decode)
static constexpr int CAM_WIDTH = 640;
static constexpr int CAM_HEIGHT = 480;
static constexpr int CAM_FPS = 15;
#endif

namespace vita_ma {

QRScanner::QRScanner() {
}

QRScanner::~QRScanner() {
    stop();
}

bool QRScanner::start(QRScanCallback callback) {
    if (m_running.load()) {
        stop();
    }

    m_callback = std::move(callback);
    m_shouldStop.store(false);

#ifdef __vita__
    // Initialize camera
    SceCameraInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    info.priority = SCE_CAMERA_PRIORITY_SHARE;
    info.format = SCE_CAMERA_FORMAT_ARGB;
    info.resolution = SCE_CAMERA_RESOLUTION_640_480;
    info.framerate = CAM_FPS;
    info.width = CAM_WIDTH;
    info.height = CAM_HEIGHT;
    info.sizeIBase = CAM_WIDTH * CAM_HEIGHT * 4;  // ARGB = 4 bytes per pixel
    info.pIBase = new uint8_t[info.sizeIBase];

    int ret = sceCameraOpen(SCE_CAMERA_DEVICE_FRONT, &info);
    if (ret < 0) {
        brls::Logger::error("QRScanner: failed to open camera ({:#x})", ret);
        delete[] static_cast<uint8_t*>(info.pIBase);
        return false;
    }

    m_cameraDevice = SCE_CAMERA_DEVICE_FRONT;

    ret = sceCameraStart(m_cameraDevice);
    if (ret < 0) {
        brls::Logger::error("QRScanner: failed to start camera ({:#x})", ret);
        sceCameraClose(m_cameraDevice);
        delete[] static_cast<uint8_t*>(info.pIBase);
        m_cameraDevice = -1;
        return false;
    }

    brls::Logger::info("QRScanner: camera opened ({}x{} @ {}fps)", CAM_WIDTH, CAM_HEIGHT, CAM_FPS);
#else
    brls::Logger::info("QRScanner: no camera support on this platform (stub mode)");
#endif

    m_running.store(true);
    m_scanThread = std::thread(&QRScanner::scanLoop, this);

    return true;
}

void QRScanner::stop() {
    if (!m_running.load()) return;

    m_shouldStop.store(true);

    if (m_scanThread.joinable()) {
        m_scanThread.join();
    }

#ifdef __vita__
    if (m_cameraDevice >= 0) {
        sceCameraStop(m_cameraDevice);
        sceCameraClose(m_cameraDevice);
        m_cameraDevice = -1;
        brls::Logger::info("QRScanner: camera closed");
    }
#endif

    m_running.store(false);
}

void QRScanner::scanLoop() {
#ifdef __vita__
    struct quirc* qr = quirc_new();
    if (!qr) {
        brls::Logger::error("QRScanner: failed to create quirc context");
        m_running.store(false);
        return;
    }

    if (quirc_resize(qr, CAM_WIDTH, CAM_HEIGHT) < 0) {
        brls::Logger::error("QRScanner: failed to resize quirc buffer");
        quirc_destroy(qr);
        m_running.store(false);
        return;
    }

    brls::Logger::info("QRScanner: scan loop started");

    SceCameraRead read;
    memset(&read, 0, sizeof(read));
    read.size = sizeof(read);

    while (!m_shouldStop.load()) {
        // Read a frame from the camera
        int ret = sceCameraRead(m_cameraDevice, &read);
        if (ret < 0 || read.status != SCE_CAMERA_STATUS_IS_ACTIVE) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        // Get quirc's image buffer and fill with grayscale data
        int w, h;
        uint8_t* image = quirc_begin(qr, &w, &h);

        // Convert ARGB camera frame to grayscale for quirc
        const uint8_t* src = static_cast<const uint8_t*>(read.pIBase);
        frameToGrayscale(src, image, CAM_WIDTH, CAM_HEIGHT, CAM_WIDTH * 4);

        // Process the image
        quirc_end(qr);

        // Check for decoded QR codes
        int count = quirc_count(qr);
        for (int i = 0; i < count; i++) {
            struct quirc_code code;
            struct quirc_data data;

            quirc_extract(qr, i, &code);
            quirc_decode_error_t err = quirc_decode(&code, &data);

            if (err == QUIRC_SUCCESS) {
                std::string qrText(reinterpret_cast<const char*>(data.payload), data.payload_len);
                brls::Logger::info("QRScanner: decoded QR code ({} bytes)", qrText.size());

                QRScanResult result = parseQRData(qrText);

                // Deliver result on main thread
                if (m_callback) {
                    brls::sync([this, result]() {
                        if (m_callback) {
                            m_callback(result);
                        }
                    });
                }

                // Stop scanning after first successful decode
                m_shouldStop.store(true);
                break;
            }
        }

        // Don't hog the CPU - scan at a reasonable rate
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    quirc_destroy(qr);
    brls::Logger::info("QRScanner: scan loop ended");
    m_running.store(false);

#else
    // Non-Vita stub: just wait until stopped
    brls::Logger::info("QRScanner: stub mode - no camera available");
    while (!m_shouldStop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    m_running.store(false);
#endif
}

void QRScanner::frameToGrayscale(const uint8_t* src, uint8_t* dst,
                                   int width, int height, int srcStride) {
    // Convert ARGB to grayscale using luminance formula:
    // Y = 0.299*R + 0.587*G + 0.114*B
    // Using integer math: Y = (77*R + 150*G + 29*B) >> 8
    for (int y = 0; y < height; y++) {
        const uint8_t* row = src + y * srcStride;
        uint8_t* out = dst + y * width;
        for (int x = 0; x < width; x++) {
            // ARGB layout: A, R, G, B
            uint8_t r = row[x * 4 + 1];
            uint8_t g = row[x * 4 + 2];
            uint8_t b = row[x * 4 + 3];
            out[x] = static_cast<uint8_t>((77 * r + 150 * g + 29 * b) >> 8);
        }
    }
}

QRScanResult QRScanner::parseQRData(const std::string& data) {
    QRScanResult result;
    result.rawData = data;

    // Check for ma-remote:// URI scheme
    // Format: ma-remote://MA-XXXX-XXXX/authtoken
    if (data.find("ma-remote://") == 0) {
        result.type = QRResultType::REMOTE_ACCESS;
        std::string rest = data.substr(12); // skip "ma-remote://"

        size_t slashPos = rest.find('/');
        if (slashPos != std::string::npos) {
            result.remoteId = rest.substr(0, slashPos);
            result.authToken = rest.substr(slashPos + 1);
        } else {
            result.remoteId = rest;
        }
        return result;
    }

    // Check for JSON payload
    // Format: {"server_url": "http://...", "token": "...", "username": "..."}
    if (!data.empty() && data[0] == '{') {
        Json json = Json::parse(data);

        if (json.has("server_url") || json.has("serverUrl")) {
            result.type = QRResultType::SERVER_LOGIN;
            result.serverUrl = json.has("server_url") ? json["server_url"].str() : json["serverUrl"].str();

            if (json.has("token")) result.authToken = json["token"].str();
            if (json.has("auth_token")) result.authToken = json["auth_token"].str();
            if (json.has("username")) result.username = json["username"].str();
            return result;
        }

        // Check for remote access JSON format
        if (json.has("remote_id") || json.has("remoteId")) {
            result.type = QRResultType::REMOTE_ACCESS;
            result.remoteId = json.has("remote_id") ? json["remote_id"].str() : json["remoteId"].str();
            if (json.has("token")) result.authToken = json["token"].str();
            if (json.has("auth_token")) result.authToken = json["auth_token"].str();
            return result;
        }
    }

    // Plain URL - treat as server URL
    if (data.find("http://") == 0 || data.find("https://") == 0) {
        result.type = QRResultType::SERVER_LOGIN;
        result.serverUrl = data;
        return result;
    }

    // Unknown format
    result.type = QRResultType::PLAIN_TEXT;
    return result;
}

} // namespace vita_ma
