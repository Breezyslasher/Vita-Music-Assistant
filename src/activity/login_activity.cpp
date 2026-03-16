/**
 * Vita Music Assistant - Login Activity implementation
 * Supports Music Assistant (direct) and Home Assistant authentication
 *
 * Auth flow:
 *   1. User enters server URL, username, password
 *   2. POST /auth/login with credentials → receive token
 *   3. Connect WebSocket and send auth command with token
 *   4. On success, save settings and proceed to main activity
 */

#include "activity/login_activity.hpp"
#include "app/application.hpp"
#include "app/ma_client.hpp"
#include "app/ma_types.hpp"
#include "utils/http_client.hpp"
#include "utils/async.hpp"
#include "view/progress_dialog.hpp"

#include <memory>

namespace vita_ma {

LoginActivity::LoginActivity() {
    brls::Logger::debug("LoginActivity created");
}

brls::View* LoginActivity::createContentView() {
    return brls::View::createFromXMLResource("activity/login.xml");
}

void LoginActivity::onContentAvailable() {
    brls::Logger::debug("LoginActivity content available");

    // Restore saved values
    auto& app = Application::getInstance();
    m_serverUrl = app.getServerUrl();
    m_username = app.getUsername();

    // Set title
    if (titleLabel) {
        titleLabel->setText("Vita Music Assistant");
    }

    updateUIForMode();

    // Server URL input (click to edit via IME)
    if (serverLabel) {
        serverLabel->setText(std::string("Server: ") + (m_serverUrl.empty() ? "Not set" : m_serverUrl));
        serverLabel->registerClickAction([this](brls::View* view) {
            brls::Application::getImeManager()->openForText([this](std::string text) {
                m_serverUrl = text;
                serverLabel->setText(std::string("Server: ") + text);
            }, "Enter Server URL", "http://your-server:8095", 256, m_serverUrl);
            return true;
        });
        serverLabel->addGestureRecognizer(new brls::TapGestureRecognizer(serverLabel));
    }

    // Username input
    if (usernameLabel) {
        usernameLabel->setText(std::string("Username: ") + (m_username.empty() ? "Not set" : m_username));
        usernameLabel->registerClickAction([this](brls::View* view) {
            brls::Application::getImeManager()->openForText([this](std::string text) {
                m_username = text;
                usernameLabel->setText(std::string("Username: ") + text);
            }, "Enter Username", "admin", 128, m_username);
            return true;
        });
        usernameLabel->addGestureRecognizer(new brls::TapGestureRecognizer(usernameLabel));
    }

    // Password input
    if (passwordLabel) {
        passwordLabel->setText("Password: Not set");
        passwordLabel->registerClickAction([this](brls::View* view) {
            brls::Application::getImeManager()->openForText([this](std::string text) {
                m_password = text;
                // Show masked password
                std::string masked(text.length(), '*');
                passwordLabel->setText(std::string("Password: ") + (text.empty() ? "Not set" : masked));
            }, "Enter Password", "", 128, "");
            return true;
        });
        passwordLabel->addGestureRecognizer(new brls::TapGestureRecognizer(passwordLabel));
    }

    // Login button
    if (loginButton) {
        loginButton->setText("Login");
        loginButton->registerClickAction([this](brls::View* view) {
            onLoginPressed();
            return true;
        });
    }

    // Mode switch button
    if (modeButton) {
        modeButton->registerClickAction([this](brls::View* view) {
            switchAuthMode();
            return true;
        });
    }
}

void LoginActivity::updateUIForMode() {
    if (subtitleLabel) {
        if (m_authMode == AuthMode::MUSIC_ASSISTANT) {
            subtitleLabel->setText("Music Assistant Login");
        } else {
            subtitleLabel->setText("Home Assistant Login");
        }
    }

    if (modeButton) {
        if (m_authMode == AuthMode::MUSIC_ASSISTANT) {
            modeButton->setText("Use HA Login");
        } else {
            modeButton->setText("Use MA Login");
        }
    }

    if (statusLabel) {
        if (m_authMode == AuthMode::MUSIC_ASSISTANT) {
            statusLabel->setText("Enter your Music Assistant server credentials");
        } else {
            statusLabel->setText("Enter your Home Assistant credentials");
        }
    }
}

void LoginActivity::switchAuthMode() {
    if (m_authMode == AuthMode::MUSIC_ASSISTANT) {
        m_authMode = AuthMode::HOME_ASSISTANT;
    } else {
        m_authMode = AuthMode::MUSIC_ASSISTANT;
    }
    updateUIForMode();
}

void LoginActivity::onLoginPressed() {
    if (m_serverUrl.empty()) {
        if (statusLabel) statusLabel->setText("Please enter a server URL");
        return;
    }

    if (m_username.empty()) {
        if (statusLabel) statusLabel->setText("Please enter a username");
        return;
    }

    if (m_password.empty()) {
        if (statusLabel) statusLabel->setText("Please enter a password");
        return;
    }

    if (m_authMode == AuthMode::MUSIC_ASSISTANT) {
        loginWithMA(m_serverUrl, m_username, m_password);
    } else {
        loginWithHA(m_serverUrl, m_username, m_password);
    }
}

void LoginActivity::loginWithMA(const std::string& serverUrl,
                                 const std::string& username,
                                 const std::string& password) {
    auto* progressDialog = new ProgressDialog("Logging in");
    progressDialog->setStatus("Authenticating with Music Assistant...");
    progressDialog->show();

    auto cancelled = std::make_shared<bool>(false);
    progressDialog->setCancelCallback([cancelled]() {
        *cancelled = true;
    });

    asyncRun([this, serverUrl, username, password, progressDialog, cancelled]() {
        if (*cancelled) return;

        // Build the auth URL: POST /auth/login
        std::string authUrl = serverUrl;
        if (!authUrl.empty() && authUrl.back() == '/') authUrl.pop_back();
        authUrl += "/auth/login";

        // Build JSON body
        std::string body = "{\"credentials\":{\"username\":\"" +
            username + "\",\"password\":\"" + password + "\"}}";

        brls::Logger::info("Login: POST {}", authUrl);

        HttpClient http;
        http.setTimeout(15);
        HttpResponse resp = http.post(authUrl, body, "application/json");

        if (*cancelled) return;

        if (!resp.success || resp.statusCode != 200) {
            brls::sync([this, progressDialog, resp]() {
                std::string errMsg = "Login failed";
                if (!resp.error.empty()) {
                    errMsg += ": " + resp.error;
                } else if (resp.statusCode == 401) {
                    errMsg = "Invalid username or password";
                } else if (resp.statusCode == 403) {
                    errMsg = "Access denied";
                } else if (resp.statusCode > 0) {
                    errMsg += " (HTTP " + std::to_string(resp.statusCode) + ")";
                } else {
                    errMsg += ": Could not reach server";
                }
                progressDialog->setStatus(errMsg);
                if (statusLabel) statusLabel->setText(errMsg);
                brls::delay(2000, [progressDialog]() {
                    progressDialog->dismiss();
                });
            });
            return;
        }

        // Parse response to extract token
        Json result = Json::parse(resp.body);
        std::string token;

        if (result.has("token")) {
            token = result["token"].str();
        }

        if (token.empty()) {
            brls::sync([this, progressDialog]() {
                progressDialog->setStatus("Login failed: no token received");
                if (statusLabel) statusLabel->setText("Login failed: no token received");
                brls::delay(2000, [progressDialog]() {
                    progressDialog->dismiss();
                });
            });
            return;
        }

        brls::Logger::info("Login: got token, connecting WebSocket...");

        brls::sync([this, progressDialog, serverUrl, token, username]() {
            progressDialog->setStatus("Connecting to server...");
        });

        connectWithToken(serverUrl, token, username);

        brls::sync([this, progressDialog, serverUrl]() {
            if (MAClient::instance().isConnected()) {
                progressDialog->setStatus("Connected!");
                progressDialog->setProgress(1.0f);

                // Connect Sendspin for audio streaming
                Application::getInstance().connectSendspin();

                brls::delay(500, [this, progressDialog]() {
                    progressDialog->dismiss();
                    Application::getInstance().pushMainActivity();
                });
            } else {
                progressDialog->setStatus("Connected to server but WebSocket auth failed");
                if (statusLabel) statusLabel->setText("WebSocket connection failed");
                brls::delay(2000, [progressDialog]() {
                    progressDialog->dismiss();
                });
            }
        });
    });
}

void LoginActivity::loginWithHA(const std::string& serverUrl,
                                 const std::string& username,
                                 const std::string& password) {
    auto* progressDialog = new ProgressDialog("Logging in");
    progressDialog->setStatus("Authenticating with Home Assistant...");
    progressDialog->show();

    auto cancelled = std::make_shared<bool>(false);
    progressDialog->setCancelCallback([cancelled]() {
        *cancelled = true;
    });

    asyncRun([this, serverUrl, username, password, progressDialog, cancelled]() {
        if (*cancelled) return;

        // Build the auth URL with HA provider
        std::string authUrl = serverUrl;
        if (!authUrl.empty() && authUrl.back() == '/') authUrl.pop_back();
        authUrl += "/auth/login";

        // Build JSON body with HA provider
        std::string body = "{\"provider_id\":\"homeassistant\","
            "\"credentials\":{\"username\":\"" +
            username + "\",\"password\":\"" + password + "\"}}";

        brls::Logger::info("Login: POST {} (HA provider)", authUrl);

        HttpClient http;
        http.setTimeout(15);
        HttpResponse resp = http.post(authUrl, body, "application/json");

        if (*cancelled) return;

        if (!resp.success || resp.statusCode != 200) {
            brls::sync([this, progressDialog, resp]() {
                std::string errMsg = "HA Login failed";
                if (!resp.error.empty()) {
                    errMsg += ": " + resp.error;
                } else if (resp.statusCode == 401) {
                    errMsg = "Invalid HA credentials";
                } else if (resp.statusCode == 403) {
                    errMsg = "Access denied - check HA permissions";
                } else if (resp.statusCode > 0) {
                    errMsg += " (HTTP " + std::to_string(resp.statusCode) + ")";
                } else {
                    errMsg += ": Could not reach server";
                }
                progressDialog->setStatus(errMsg);
                if (statusLabel) statusLabel->setText(errMsg);
                brls::delay(2000, [progressDialog]() {
                    progressDialog->dismiss();
                });
            });
            return;
        }

        // Parse response to extract token
        Json result = Json::parse(resp.body);
        std::string token;

        if (result.has("token")) {
            token = result["token"].str();
        }

        if (token.empty()) {
            brls::sync([this, progressDialog]() {
                progressDialog->setStatus("HA Login failed: no token received");
                if (statusLabel) statusLabel->setText("HA Login failed: no token received");
                brls::delay(2000, [progressDialog]() {
                    progressDialog->dismiss();
                });
            });
            return;
        }

        brls::Logger::info("Login: got HA token, connecting WebSocket...");

        brls::sync([this, progressDialog]() {
            progressDialog->setStatus("Connecting to server...");
        });

        connectWithToken(serverUrl, token, username);

        brls::sync([this, progressDialog, serverUrl]() {
            if (MAClient::instance().isConnected()) {
                progressDialog->setStatus("Connected!");
                progressDialog->setProgress(1.0f);

                // Connect Sendspin for audio streaming
                Application::getInstance().connectSendspin();

                brls::delay(500, [this, progressDialog]() {
                    progressDialog->dismiss();
                    Application::getInstance().pushMainActivity();
                });
            } else {
                progressDialog->setStatus("WebSocket connection failed");
                if (statusLabel) statusLabel->setText("WebSocket connection failed");
                brls::delay(2000, [progressDialog]() {
                    progressDialog->dismiss();
                });
            }
        });
    });
}

void LoginActivity::connectWithToken(const std::string& serverUrl,
                                      const std::string& token,
                                      const std::string& username) {
    // Connect the WebSocket with the obtained token
    MAClient& client = MAClient::instance();

    if (client.connect(serverUrl, token)) {
        brls::Logger::info("Login: WebSocket connected and authenticated");

        // Save credentials
        auto& app = Application::getInstance();
        app.setServerUrl(serverUrl);
        app.setAuthToken(token);
        app.setUsername(username);
        app.saveSettings();
    } else {
        brls::Logger::error("Login: WebSocket connection failed");
    }
}

} // namespace vita_ma
