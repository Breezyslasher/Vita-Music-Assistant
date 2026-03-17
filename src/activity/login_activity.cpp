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
#include "app/webrtc_client.hpp"
#include "app/ma_types.hpp"
#include "utils/http_client.hpp"
#include "utils/async.hpp"
#include "view/progress_dialog.hpp"

#include <memory>

namespace vita_ma {

LoginActivity::LoginActivity() {
    brls::Logger::debug("LoginActivity created");
}

LoginActivity::~LoginActivity() {
    if (m_qrScanner && m_qrScanner->isRunning()) {
        m_qrScanner->stop();
    }
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

    // Restore saved remote ID if available
    m_remoteId = app.getSettings().remoteId;

    // Server URL / Remote ID input (click to edit via IME)
    if (serverLabel) {
        serverLabel->setText(std::string("Server: ") + (m_serverUrl.empty() ? "Not set" : m_serverUrl));
        serverLabel->registerClickAction([this](brls::View* view) {
            if (m_authMode == AuthMode::REMOTE_ACCESS) {
                brls::Application::getImeManager()->openForText([this](std::string text) {
                    m_remoteId = text;
                    serverLabel->setText(std::string("Remote ID: ") + text);
                }, "Enter Remote ID", "MA-XXXX-XXXX", 32, m_remoteId);
            } else {
                brls::Application::getImeManager()->openForText([this](std::string text) {
                    m_serverUrl = text;
                    serverLabel->setText(std::string("Server: ") + text);
                }, "Enter Server URL", "http://your-server:8095", 256, m_serverUrl);
            }
            return true;
        });
        serverLabel->addGestureRecognizer(new brls::TapGestureRecognizer(serverLabel));
    }

    // Username / Auth Token input
    if (usernameLabel) {
        usernameLabel->setText(std::string("Username: ") + (m_username.empty() ? "Not set" : m_username));
        usernameLabel->registerClickAction([this](brls::View* view) {
            if (m_authMode == AuthMode::REMOTE_ACCESS) {
                brls::Application::getImeManager()->openForText([this](std::string text) {
                    m_authToken = text;
                    usernameLabel->setText(std::string("Auth Token: ") + (text.empty() ? "Not set" : "(set)"));
                }, "Enter Auth Token", "", 256, m_authToken);
            } else {
                brls::Application::getImeManager()->openForText([this](std::string text) {
                    m_username = text;
                    usernameLabel->setText(std::string("Username: ") + text);
                }, "Enter Username", "admin", 128, m_username);
            }
            return true;
        });
        usernameLabel->addGestureRecognizer(new brls::TapGestureRecognizer(usernameLabel));
    }

    // Password input (hidden in remote access mode)
    if (passwordLabel) {
        passwordLabel->setText("Password: Not set");
        passwordLabel->registerClickAction([this](brls::View* view) {
            brls::Application::getImeManager()->openForText([this](std::string text) {
                m_password = text;
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

    // QR scan button - scans a QR code using the Vita's front camera
    if (qrButton) {
        qrButton->setText("Scan QR");
        qrButton->registerClickAction([this](brls::View* view) {
            startQRScan();
            return true;
        });
    }
}

void LoginActivity::updateUIForMode() {
    bool isRemote = (m_authMode == AuthMode::REMOTE_ACCESS);

    if (subtitleLabel) {
        if (m_authMode == AuthMode::MUSIC_ASSISTANT) {
            subtitleLabel->setText("Music Assistant Login");
        } else if (m_authMode == AuthMode::HOME_ASSISTANT) {
            subtitleLabel->setText("Home Assistant Login");
        } else {
            subtitleLabel->setText("Remote Access Login");
        }
    }

    if (modeButton) {
        if (m_authMode == AuthMode::MUSIC_ASSISTANT) {
            modeButton->setText("Use HA Login");
        } else if (m_authMode == AuthMode::HOME_ASSISTANT) {
            modeButton->setText("Use Remote ID");
        } else {
            modeButton->setText("Use MA Login");
        }
    }

    if (statusLabel) {
        if (m_authMode == AuthMode::MUSIC_ASSISTANT) {
            statusLabel->setText("Enter your Music Assistant server credentials");
        } else if (m_authMode == AuthMode::HOME_ASSISTANT) {
            statusLabel->setText("Enter your Home Assistant credentials");
        } else {
            statusLabel->setText("Enter your Remote ID (MA-XXXX-XXXX) and auth token");
        }
    }

    // In remote access mode, repurpose the input fields:
    // serverLabel -> Remote ID input
    // usernameLabel -> Auth Token input
    // passwordLabel -> hidden
    if (isRemote) {
        if (serverLabel) {
            serverLabel->setText(std::string("Remote ID: ") + (m_remoteId.empty() ? "Not set" : m_remoteId));
        }
        if (usernameLabel) {
            usernameLabel->setText(std::string("Auth Token: ") + (m_authToken.empty() ? "Not set" : "(set)"));
        }
        if (passwordLabel) {
            passwordLabel->setVisibility(brls::Visibility::GONE);
        }
    } else {
        if (serverLabel) {
            serverLabel->setText(std::string("Server: ") + (m_serverUrl.empty() ? "Not set" : m_serverUrl));
        }
        if (usernameLabel) {
            usernameLabel->setText(std::string("Username: ") + (m_username.empty() ? "Not set" : m_username));
        }
        if (passwordLabel) {
            passwordLabel->setVisibility(brls::Visibility::VISIBLE);
            passwordLabel->setText("Password: Not set");
        }
    }
}

void LoginActivity::switchAuthMode() {
    if (m_authMode == AuthMode::MUSIC_ASSISTANT) {
        m_authMode = AuthMode::HOME_ASSISTANT;
    } else if (m_authMode == AuthMode::HOME_ASSISTANT) {
        m_authMode = AuthMode::REMOTE_ACCESS;
    } else {
        m_authMode = AuthMode::MUSIC_ASSISTANT;
    }
    updateUIForMode();
}

void LoginActivity::onLoginPressed() {
    if (m_authMode == AuthMode::REMOTE_ACCESS) {
        if (m_remoteId.empty()) {
            if (statusLabel) statusLabel->setText("Please enter a Remote ID (MA-XXXX-XXXX)");
            return;
        }
        if (m_authToken.empty()) {
            if (statusLabel) statusLabel->setText("Please enter an auth token");
            return;
        }
        loginWithRemoteId(m_remoteId, m_authToken);
        return;
    }

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

void LoginActivity::startQRScan() {
    if (m_qrScanner && m_qrScanner->isRunning()) {
        brls::Logger::info("QR scan already in progress");
        return;
    }

    if (statusLabel) statusLabel->setText("Point camera at QR code...");

    m_qrScanner = std::make_unique<QRScanner>();
    bool started = m_qrScanner->start([this](const QRScanResult& result) {
        onQRScanned(result);
    });

    if (!started) {
        if (statusLabel) statusLabel->setText("Failed to open camera for QR scanning");
        m_qrScanner.reset();
    }
}

void LoginActivity::onQRScanned(const QRScanResult& result) {
    brls::Logger::info("QR scanned: type={}, raw={}", static_cast<int>(result.type), result.rawData);

    // Stop the scanner
    if (m_qrScanner) {
        m_qrScanner->stop();
    }

    switch (result.type) {
        case QRResultType::SERVER_LOGIN:
            if (!result.serverUrl.empty()) {
                m_serverUrl = result.serverUrl;
                if (serverLabel) serverLabel->setText(std::string("Server: ") + m_serverUrl);

                if (!result.authToken.empty()) {
                    // QR has a token - connect directly
                    m_authToken = result.authToken;
                    if (!result.username.empty()) {
                        m_username = result.username;
                        if (usernameLabel) usernameLabel->setText(std::string("Username: ") + m_username);
                    }
                    if (statusLabel) statusLabel->setText("QR scanned! Connecting...");
                    connectWithToken(m_serverUrl, m_authToken, m_username);

                    // Check connection and proceed
                    if (MAClient::instance().isConnected()) {
                        Application::getInstance().connectSendspin();
                        Application::getInstance().pushMainActivity();
                    } else {
                        if (statusLabel) statusLabel->setText("QR scanned - server set. Enter credentials to login.");
                    }
                } else {
                    if (statusLabel) statusLabel->setText("QR scanned! Server URL set. Enter credentials.");
                }
            }
            break;

        case QRResultType::REMOTE_ACCESS:
            if (!result.remoteId.empty()) {
                m_remoteId = result.remoteId;
                m_authToken = result.authToken;

                // Switch to remote access mode
                m_authMode = AuthMode::REMOTE_ACCESS;
                updateUIForMode();

                if (!m_authToken.empty()) {
                    // QR has both remote ID and token - connect immediately
                    if (statusLabel) statusLabel->setText("QR scanned! Connecting via remote access...");
                    loginWithRemoteId(m_remoteId, m_authToken);
                } else {
                    if (statusLabel) statusLabel->setText("QR scanned! Remote ID set. Enter auth token.");
                }
            }
            break;

        case QRResultType::PLAIN_TEXT:
        default:
            if (statusLabel) statusLabel->setText("QR scanned: " + result.rawData.substr(0, 64));
            break;
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

void LoginActivity::loginWithRemoteId(const std::string& remoteId,
                                       const std::string& authToken) {
    auto* progressDialog = new ProgressDialog("Connecting");
    progressDialog->setStatus("Connecting via Remote ID...");
    progressDialog->show();

    auto cancelled = std::make_shared<bool>(false);
    progressDialog->setCancelCallback([cancelled]() {
        *cancelled = true;
    });

    asyncRun([this, remoteId, authToken, progressDialog, cancelled]() {
        if (*cancelled) return;

        brls::Logger::info("Login: connecting via WebRTC remote ID {}", remoteId);

        MAClient& client = MAClient::instance();
        bool connected = client.connectViaRemoteId(remoteId, authToken);

        if (*cancelled) return;

        if (!connected) {
            brls::sync([this, progressDialog]() {
                progressDialog->setStatus("Failed to connect to signaling server");
                if (statusLabel) statusLabel->setText("Connection failed - check Remote ID");
                brls::delay(2000, [progressDialog]() {
                    progressDialog->dismiss();
                });
            });
            return;
        }

        // Wait for the WebRTC connection to establish (with timeout)
        int waitMs = 0;
        while (waitMs < 15000 && !*cancelled) {
            if (client.isConnected()) break;
            auto state = WebRTCClient::instance().getState();
            if (state == WebRTCState::ERROR) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            waitMs += 200;
        }

        if (*cancelled) return;

        brls::sync([this, progressDialog, remoteId, authToken]() {
            MAClient& client = MAClient::instance();
            if (client.isConnected()) {
                progressDialog->setStatus("Connected via Remote Access!");
                progressDialog->setProgress(1.0f);

                // Save remote access settings
                auto& app = Application::getInstance();
                app.getSettings().remoteId = remoteId;
                app.getSettings().remoteAccessEnabled = true;
                app.setAuthToken(authToken);
                app.saveSettings();

                // Connect Sendspin over WebRTC relay for remote audio playback
                Application::getInstance().connectSendspin();
                brls::Logger::info("Login: WebRTC connected - Sendspin via relay");

                brls::delay(500, [this, progressDialog]() {
                    progressDialog->dismiss();
                    Application::getInstance().pushMainActivity();
                });
            } else {
                auto state = WebRTCClient::instance().getState();
                std::string errMsg = "Remote connection failed";
                if (state == WebRTCState::ERROR) {
                    errMsg = "Remote ID not found or server offline";
                }
                progressDialog->setStatus(errMsg);
                if (statusLabel) statusLabel->setText(errMsg);
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

        // Fetch and save remote access info for future remote connections
        WebRTCClient::fetchRemoteAccessInfo(client, [](bool success, const RemoteAccessInfo& info) {
            if (success && info.enabled && !info.remoteId.empty()) {
                auto& app = Application::getInstance();
                app.getSettings().remoteId = info.remoteId;
                app.getSettings().remoteAccessEnabled = true;
                app.saveSettings();
                brls::Logger::info("Login: saved remote ID {} for future use", info.remoteId);
            }
        });
    } else {
        brls::Logger::error("Login: WebSocket connection failed");
    }
}

} // namespace vita_ma
