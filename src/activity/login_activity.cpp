/**
 * Vita Music Assistant - Login Activity implementation
 * Connects to a Music Assistant server via WebSocket
 */

#include "activity/login_activity.hpp"
#include "app/application.hpp"
#include "app/ma_client.hpp"
#include "app/ma_types.hpp"
#include "view/progress_dialog.hpp"
#include "utils/async.hpp"

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

    // Set initial values
    if (titleLabel) {
        titleLabel->setText("Vita Music Assistant");
    }

    if (statusLabel) {
        statusLabel->setText("Enter your Music Assistant server URL");
    }

    // Hide PIN-related UI (not used with Music Assistant)
    if (pinCodeLabel) {
        pinCodeLabel->setVisibility(brls::Visibility::GONE);
    }
    if (pinButton) {
        pinButton->setVisibility(brls::Visibility::GONE);
    }

    // Hide username/password (MA uses token auth, not credentials)
    if (usernameLabel) {
        usernameLabel->setVisibility(brls::Visibility::GONE);
    }
    if (passwordLabel) {
        passwordLabel->setVisibility(brls::Visibility::GONE);
    }

    // Server URL input
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

    // Connect button
    if (loginButton) {
        loginButton->setText("Connect");
        loginButton->registerClickAction([this](brls::View* view) {
            onConnectPressed();
            return true;
        });
    }
}

void LoginActivity::onConnectPressed() {
    if (m_serverUrl.empty()) {
        if (statusLabel) statusLabel->setText("Please enter a server URL");
        return;
    }

    if (statusLabel) statusLabel->setText("Connecting...");

    // Show progress dialog
    auto* progressDialog = new ProgressDialog("Connecting");
    progressDialog->setStatus("Connecting to " + m_serverUrl + "...");
    progressDialog->show();

    auto cancelled = std::make_shared<bool>(false);
    progressDialog->setCancelCallback([cancelled]() {
        *cancelled = true;
    });

    std::string serverUrl = m_serverUrl;
    std::string authToken = m_authToken;

    asyncRun([this, serverUrl, authToken, progressDialog, cancelled]() {
        if (*cancelled) return;

        MAClient& client = MAClient::instance();

        if (client.connect(serverUrl, authToken)) {
            brls::sync([this, progressDialog, serverUrl]() {
                progressDialog->setStatus("Connected!");
                progressDialog->setProgress(1.0f);

                Application::getInstance().setServerUrl(serverUrl);
                Application::getInstance().saveSettings();
                if (statusLabel) statusLabel->setText("Connected to server");

                brls::delay(500, [this, progressDialog]() {
                    progressDialog->dismiss();
                    Application::getInstance().pushMainActivity();
                });
            });
        } else {
            brls::sync([this, progressDialog, serverUrl]() {
                progressDialog->setStatus("Connection failed");
                if (statusLabel) statusLabel->setText("Failed to connect to " + serverUrl);
                brls::Logger::error("Failed to connect to {}", serverUrl);

                brls::delay(2000, [progressDialog]() {
                    progressDialog->dismiss();
                });
            });
        }
    });
}

} // namespace vita_ma
