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

// Callback types
using WebRTCStateCallback = std::function<void(WebRTCState state)>;
using WebRTCMessageCallback = std::function<void(const std::string& message)>;

/**
 * WebRTC Remote Access Client
 *
 * On PS Vita, we implement a simplified WebRTC-like connection:
 * 1. Connect to signaling server via WebSocket
 * 2. Exchange SDP offer/answer with the MA server peer
 * 3. Exchange ICE candidates
 * 4. Establish a data channel for API messages
 *
 * Since PS Vita lacks native WebRTC, we use the signaling server as a relay
 * when direct P2P fails, with the data channel messages tunneled through
 * the signaling WebSocket as a fallback.
 */
class WebRTCClient {
public:
    static WebRTCClient& instance();

    // Connect to a remote MA server using its Remote ID
    // remoteId format: "MA-XXXX-XXXX"
    bool connectRemote(const std::string& remoteId, const std::string& authToken);

    // Disconnect from the remote server
    void disconnect();

    // Send a message through the data channel (same format as WebSocket API)
    bool sendMessage(const std::string& message);

    // State
    WebRTCState getState() const { return m_state.load(); }
    bool isConnected() const { return m_state.load() == WebRTCState::CONNECTED; }
    const std::string& getRemoteId() const { return m_remoteId; }

    // Callbacks
    void setStateCallback(WebRTCStateCallback cb) { m_stateCallback = std::move(cb); }
    void setMessageCallback(WebRTCMessageCallback cb) { m_messageCallback = std::move(cb); }

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
    void handleDataChannelMessage(const std::string& message);

    // State management
    void setState(WebRTCState state);

    // Reconnection
    std::atomic<bool> m_shouldReconnect{false};
    void attemptReconnect();

    // Relay mode: when P2P fails, tunnel messages through signaling server
    bool m_relayMode = false;
    void sendViaRelay(const std::string& message);
};

} // namespace vita_ma
