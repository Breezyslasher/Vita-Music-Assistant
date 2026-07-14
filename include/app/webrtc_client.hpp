#pragma once

/**
 * Vita Music Assistant - Music Assistant Remote Access client
 *
 * How MA remote access actually works (verified against
 * music-assistant/signaling-server and the server's
 * controllers/webserver/remote_access/gateway.py):
 *
 *  1. The MA server registers at wss://signaling.music-assistant.io/ws with
 *     {"type":"register-server","remoteId":"MA-XXXX-XXXX","iceServers":[...]}.
 *  2. A client connects to the same signaling server and sends
 *     {"type":"connect-request","remoteId":"MA-XXXX-XXXX"}; the server replies
 *     {"type":"connected","remoteId","sessionId"} and the gateway follows with
 *     {"type":"session-ready","sessionId","iceServers":[...]}.
 *  3. The client then opens a REAL WebRTC peer connection (ICE via the given
 *     STUN servers -> DTLS -> SCTP) and exchanges {"type":"offer"/"answer"/
 *     "ice-candidate","sessionId","data":{...}} through the signaling socket.
 *  4. Over the established connection the client creates two data channels:
 *       "ma-api"   -> bridged by the gateway to the local WS API (/ws)
 *       "sendspin" -> bridged to the local Sendspin server (:8927)
 *     HTTP assets (imageproxy etc.) are tunneled as
 *     {"type":"http-proxy-request",...} / {"type":"http-proxy-response",...}
 *     messages over the ma-api channel.
 *
 * The signaling server offers NO relay/fallback data path - after signaling,
 * all traffic is peer-to-peer. That means a client without a genuine WebRTC
 * stack (ICE + DTLS + SCTP data channels) cannot transport any data.
 *
 * Status on PS Vita: no native WebRTC stack is available. A port of
 * libdatachannel (libjuice + usrsctp + mbedTLS backend) is believed feasible -
 * usrsctp has previously been built for the Vita in this repo's history - but
 * it is a substantial effort that needs on-device iteration. Until that
 * exists, this class implements what IS possible today:
 *   - fetchRemoteAccessInfo(): read remote-access status/Remote ID from a
 *     locally connected server.
 *   - checkRemoteOnline(): ask the signaling server whether a Remote ID is
 *     currently registered (GET /api/check/:remoteId).
 *   - connectRemote(): performs the real signaling handshake, then fails with
 *     a clear error at the point where the P2P transport would be needed.
 *
 * For remote playback today, use a publicly reachable server URL instead
 * (https:// server URL -> API over wss://, Sendspin over wss://host/sendspin,
 * cover art over https - all already supported by this app).
 */

#include "app/websocket_client.hpp"
#include "app/ma_client.hpp"
#include <string>
#include <functional>
#include <atomic>

namespace vita_ma {

// Remote access connection states
enum class WebRTCState {
    DISCONNECTED,
    CONNECTING_SIGNALING,  // Connecting to signaling server
    SIGNALING_CONNECTED,   // connect-request sent, waiting for session
    SESSION_READY,         // Gateway answered; P2P negotiation would start here
    ERROR
};

// Remote access info from the MA server (remote_access/info command)
struct RemoteAccessInfo {
    bool enabled = false;
    bool connected = false;
    std::string remoteId;       // Format: MA-XXXX-XXXX
    std::string signalingUrl;   // wss://signaling.music-assistant.io/ws
};

using WebRTCStateCallback = std::function<void(WebRTCState state, const std::string& detail)>;

class WebRTCClient {
public:
    static WebRTCClient& instance();

    // Perform the real signaling handshake against the MA signaling server.
    // On Vita this cannot complete (no P2P transport); it will reach
    // SESSION_READY at best and then fail with a descriptive error. Kept so the
    // signaling layer is correct and testable for a future native transport.
    bool connectRemote(const std::string& remoteId);

    void disconnect();

    WebRTCState getState() const { return m_state.load(); }

    void setStateCallback(WebRTCStateCallback cb) { m_stateCallback = std::move(cb); }

    // Ask the signaling server whether a Remote ID is currently registered.
    // Uses GET https://signaling.music-assistant.io/api/check/<remoteId>,
    // returning the "online" field. Runs asynchronously; cb on the UI thread.
    static void checkRemoteOnline(const std::string& remoteId,
                                  std::function<void(bool reachable, bool online)> cb);

    // Get remote access info from a locally-connected MA server
    static void fetchRemoteAccessInfo(MAClient& client,
        std::function<void(bool success, const RemoteAccessInfo& info)> cb);

private:
    WebRTCClient() = default;
    ~WebRTCClient() = default;

    WebSocketClient m_signalingWs;
    std::string m_remoteId;
    std::string m_sessionId;
    std::string m_signalingUrl = "wss://signaling.music-assistant.io/ws";

    std::atomic<WebRTCState> m_state{WebRTCState::DISCONNECTED};
    WebRTCStateCallback m_stateCallback;

    void onSignalingMessage(const std::string& message);
    void setState(WebRTCState state, const std::string& detail = "");
};

} // namespace vita_ma
