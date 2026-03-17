#pragma once

/**
 * Vita Music Assistant - WebRTC Remote Access Client
 *
 * Implements WebRTC-based remote access to Music Assistant servers.
 * Uses the signaling server at wss://signaling.music-assistant.io/ws
 * to establish a peer-to-peer data channel connection through NAT/firewalls.
 *
 * Once connected, the data channel transports the same WebSocket API messages
 * as a local connection, so the rest of the app works identically.
 */

#include "app/websocket_client.hpp"
#include "app/ma_client.hpp"
#include <string>
#include <functional>
#include <mutex>
#include <atomic>
#include <vector>
#include <cstdint>

namespace vita_ma {

// WebRTC connection states
enum class WebRTCState {
    DISCONNECTED,
    CONNECTING_SIGNALING,  // Connecting to signaling server
    SIGNALING_CONNECTED,   // Connected to signaling, waiting for peer
    NEGOTIATING,           // ICE/SDP exchange in progress
    CONNECTED,             // Data channel open, ready for API calls
    RECONNECTING,
    ERROR
};

// Remote access info from the MA server
struct RemoteAccessInfo {
    bool enabled = false;
    bool connected = false;
    std::string remoteId;       // Format: MA-XXXX-XXXX
    std::string signalingUrl;   // wss://signaling.music-assistant.io/ws
};

// ICE candidate
struct IceCandidate {
    std::string candidate;
    std::string sdpMid;
    int sdpMLineIndex = 0;
};

// Channel identifiers for multiplexing over the relay
// The official mobile app uses two WebRTC data channels: "ma-api" and "sendspin".
// Since PS Vita relays everything through the signaling server, we multiplex
// both channels over the same relay WebSocket using a "channel" field.
static constexpr const char* CHANNEL_MA_API = "ma-api";
static constexpr const char* CHANNEL_SENDSPIN = "sendspin";

// Callback types
using WebRTCStateCallback = std::function<void(WebRTCState state)>;
using WebRTCMessageCallback = std::function<void(const std::string& message)>;
using WebRTCBinaryCallback = std::function<void(const uint8_t* data, size_t size)>;

/**
 * WebRTC Remote Access Client
 *
 * On PS Vita, we implement a simplified WebRTC-like connection:
 * 1. Connect to signaling server via WebSocket
 * 2. Exchange SDP offer/answer with the MA server peer
 * 3. Exchange ICE candidates
 * 4. Establish data channels for API messages and Sendspin audio
 *
 * Since PS Vita lacks native WebRTC, we use the signaling server as a relay.
 * Two logical channels are multiplexed over the relay:
 *   - "ma-api":    Text JSON messages for the Music Assistant API
 *   - "sendspin":  Text control messages + binary audio data for Sendspin streaming
 *
 * Binary data (audio) is base64-encoded for relay transport (~33% overhead).
 */
class WebRTCClient {
public:
    static WebRTCClient& instance();

    // Connect to a remote MA server using its Remote ID
    // remoteId format: "MA-XXXX-XXXX"
    bool connectRemote(const std::string& remoteId, const std::string& authToken);

    // Disconnect from the remote server
    void disconnect();

    // Send a message through the ma-api data channel
    bool sendMessage(const std::string& message);

    // Send a text message through the sendspin data channel
    bool sendSendspinMessage(const std::string& message);

    // Send binary data through the sendspin data channel (base64-encoded for relay)
    bool sendSendspinBinary(const uint8_t* data, size_t size);

    // State
    WebRTCState getState() const { return m_state.load(); }
    bool isConnected() const { return m_state.load() == WebRTCState::CONNECTED; }
    bool isSendspinAvailable() const { return m_sendspinChannelOpen.load(); }
    const std::string& getRemoteId() const { return m_remoteId; }

    // Callbacks - ma-api channel
    void setStateCallback(WebRTCStateCallback cb) { m_stateCallback = std::move(cb); }
    void setMessageCallback(WebRTCMessageCallback cb) { m_messageCallback = std::move(cb); }

    // Callbacks - sendspin channel
    void setSendspinMessageCallback(WebRTCMessageCallback cb) { m_sendspinMessageCallback = std::move(cb); }
    void setSendspinBinaryCallback(WebRTCBinaryCallback cb) { m_sendspinBinaryCallback = std::move(cb); }

    // Get remote access info from a locally-connected MA server
    // (used to discover the Remote ID before connecting remotely)
    static void fetchRemoteAccessInfo(MAClient& client,
        std::function<void(bool success, const RemoteAccessInfo& info)> cb);

private:
    WebRTCClient() = default;
    ~WebRTCClient() = default;

    // Signaling WebSocket connection
    WebSocketClient m_signalingWs;
    std::string m_remoteId;
    std::string m_authToken;
    std::string m_signalingUrl = "wss://signaling.music-assistant.io/ws";

    // State
    std::atomic<WebRTCState> m_state{WebRTCState::DISCONNECTED};
    WebRTCStateCallback m_stateCallback;
    WebRTCMessageCallback m_messageCallback;

    // Sendspin channel state and callbacks
    std::atomic<bool> m_sendspinChannelOpen{false};
    WebRTCMessageCallback m_sendspinMessageCallback;
    WebRTCBinaryCallback m_sendspinBinaryCallback;

    // Session
    std::string m_sessionId;
    std::string m_localSdp;
    std::string m_remoteSdp;
    std::vector<IceCandidate> m_localCandidates;
    std::vector<IceCandidate> m_remoteCandidates;

    // Signaling message handlers
    void onSignalingMessage(const std::string& message);
    void onSignalingClose(int code, const std::string& reason);
    void onSignalingError(const std::string& error);

    // WebRTC negotiation
    void sendOffer();
    void handleAnswer(const Json& data);
    void handleIceCandidate(const Json& data);
    void handleDataChannelMessage(const std::string& channel, const std::string& data);
    void handleDataChannelBinary(const std::string& channel, const std::string& base64Data);

    // State management
    void setState(WebRTCState state);

    // Reconnection
    std::atomic<bool> m_shouldReconnect{false};
    void attemptReconnect();

    // Relay mode: when P2P fails, tunnel messages through signaling server
    bool m_relayMode = false;
    void sendViaRelay(const std::string& channel, const std::string& message);
    void sendBinaryViaRelay(const std::string& channel, const uint8_t* data, size_t size);
};

} // namespace vita_ma
