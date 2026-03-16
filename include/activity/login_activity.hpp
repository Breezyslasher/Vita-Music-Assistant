/**
 * Vita Music Assistant - Login Activity
 * Handles connection to a Music Assistant server
 */

#pragma once

#include <borealis.hpp>
#include <borealis/core/timer.hpp>
#include "app/ma_types.hpp"

namespace vita_ma {

class LoginActivity : public brls::Activity {
public:
    LoginActivity();

    brls::View* createContentView() override;

    void onContentAvailable() override;

private:
    void onConnectPressed();

    BRLS_BIND(brls::Label, titleLabel, "login/title");
    BRLS_BIND(brls::Box, inputContainer, "login/input_container");
    BRLS_BIND(brls::Label, serverLabel, "login/server_label");
    BRLS_BIND(brls::Label, usernameLabel, "login/username_label");
    BRLS_BIND(brls::Label, passwordLabel, "login/password_label");
    BRLS_BIND(brls::Button, loginButton, "login/login_button");
    BRLS_BIND(brls::Button, pinButton, "login/pin_button");
    BRLS_BIND(brls::Label, statusLabel, "login/status");
    BRLS_BIND(brls::Label, pinCodeLabel, "login/pin_code");

    std::string m_serverUrl;
    std::string m_authToken;
};

} // namespace vita_ma
