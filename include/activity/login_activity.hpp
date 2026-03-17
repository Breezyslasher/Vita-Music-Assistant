/**
 * Vita Music Assistant - Login Activity
 * Handles connection to a Music Assistant server
 * Supports both Music Assistant and Home Assistant authentication
 */

#pragma once

#include <borealis.hpp>
#include "app/ma_types.hpp"

namespace vita_ma {

enum class AuthMode {
    MUSIC_ASSISTANT,   // Direct MA server login (username/password)
    HOME_ASSISTANT,    // HA instance login (HA credentials forwarded to MA)
    REMOTE_ACCESS      // Remote access via Remote ID (MA-XXXX-XXXX)
};

class LoginActivity : public brls::Activity {
public:
    LoginActivity();

    brls::View* createContentView() override;

    void onContentAvailable() override;

private:
    void onLoginPressed();
    void switchAuthMode();
    void updateUIForMode();

    // Auth flows
    void loginWithMA(const std::string& serverUrl,
                     const std::string& username,
                     const std::string& password);
    void loginWithHA(const std::string& serverUrl,
                     const std::string& username,
                     const std::string& password);
    void loginWithRemoteId(const std::string& remoteId,
                           const std::string& authToken);

    // After getting a token, connect the WebSocket
    void connectWithToken(const std::string& serverUrl,
                          const std::string& token,
                          const std::string& username);

    BRLS_BIND(brls::Label, titleLabel, "login/title");
    BRLS_BIND(brls::Label, subtitleLabel, "login/subtitle");
    BRLS_BIND(brls::Box, inputContainer, "login/input_container");
    BRLS_BIND(brls::Label, serverLabel, "login/server_label");
    BRLS_BIND(brls::Label, usernameLabel, "login/username_label");
    BRLS_BIND(brls::Label, passwordLabel, "login/password_label");
    BRLS_BIND(brls::Button, loginButton, "login/login_button");
    BRLS_BIND(brls::Button, modeButton, "login/mode_button");
    BRLS_BIND(brls::Label, statusLabel, "login/status");

    AuthMode m_authMode = AuthMode::MUSIC_ASSISTANT;
    std::string m_serverUrl;
    std::string m_username;
    std::string m_password;
    std::string m_remoteId;    // MA-XXXX-XXXX for remote access
    std::string m_authToken;   // Auth token for remote access
};

} // namespace vita_ma
