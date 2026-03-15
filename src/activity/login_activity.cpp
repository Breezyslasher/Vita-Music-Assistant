#include "activity/login_activity.hpp"
#include "app.h"
#include "app/ma_client.hpp"
#include "app/application.hpp"
#include <borealis.hpp>

namespace vita_ma {

LoginActivity::LoginActivity() {}

brls::View* LoginActivity::createContentView() {
    auto* root = new brls::Box();
    root->setAxis(brls::Axis::COLUMN);
    root->setJustifyContent(brls::JustifyContent::CENTER);
    root->setAlignItems(brls::AlignItems::CENTER);
    root->setGrow(1.0f);
    root->setPadding(32);

    // App title
    auto* appTitle = new brls::Label();
    appTitle->setText("Vita Music Assistant");
    appTitle->setFontSize(28);
    appTitle->setMarginBottom(8);
    root->addView(appTitle);

    auto* appSubtitle = new brls::Label();
    appSubtitle->setText("Music Assistant Client for PS Vita");
    appSubtitle->setFontSize(14);
    appSubtitle->setTextColor(nvgRGBA(180, 180, 180, 255));
    appSubtitle->setMarginBottom(48);
    root->addView(appSubtitle);

    // Server URL input
    auto* serverLabel = new brls::Label();
    serverLabel->setText("Server URL");
    serverLabel->setFontSize(16);
    serverLabel->setMarginBottom(8);
    root->addView(serverLabel);

    auto* serverBtn = new brls::Label();
    serverBtn->setText("Tap to enter server URL...");
    serverBtn->setFontSize(16);
    serverBtn->setFocusable(true);
    serverBtn->setPadding(12, 24, 12, 24);
    serverBtn->setCornerRadius(8);
    serverBtn->setWidth(400);
    serverBtn->setMarginBottom(16);

    std::string serverUrl;
    std::string authToken;

    serverBtn->registerClickAction([serverBtn, &serverUrl](...) {
        brls::Swkbd::openForText([serverBtn](const std::string& text) {
            if (!text.empty()) {
                serverBtn->setText(text);
            }
        }, "Server URL", "e.g. http://192.168.1.100:8095", 256, "");
        return true;
    });
    root->addView(serverBtn);

    // Auth token input (optional)
    auto* tokenLabel = new brls::Label();
    tokenLabel->setText("Authentication Token (optional)");
    tokenLabel->setFontSize(16);
    tokenLabel->setMarginBottom(8);
    root->addView(tokenLabel);

    auto* tokenBtn = new brls::Label();
    tokenBtn->setText("Tap to enter token...");
    tokenBtn->setFontSize(16);
    tokenBtn->setFocusable(true);
    tokenBtn->setPadding(12, 24, 12, 24);
    tokenBtn->setCornerRadius(8);
    tokenBtn->setWidth(400);
    tokenBtn->setMarginBottom(32);

    tokenBtn->registerClickAction([tokenBtn](...) {
        brls::Swkbd::openForText([tokenBtn](const std::string& text) {
            tokenBtn->setText(text.empty() ? "Tap to enter token..." : "Token set");
        }, "Auth Token", "Enter token if required", 256, "");
        return true;
    });
    root->addView(tokenBtn);

    // Connect button
    auto* connectBtn = new brls::Label();
    connectBtn->setText("Connect");
    connectBtn->setFontSize(20);
    connectBtn->setFocusable(true);
    connectBtn->setPadding(14, 48, 14, 48);
    connectBtn->setCornerRadius(12);
    connectBtn->setHorizontalAlign(brls::HorizontalAlign::CENTER);

    connectBtn->registerClickAction([this, serverBtn, tokenBtn](...) {
        std::string url = serverBtn->getFullText();
        std::string token = tokenBtn->getFullText();

        // Clean up placeholder text
        if (url == "Tap to enter server URL..." || url.empty()) {
            brls::Application::notify("Please enter a server URL");
            return true;
        }
        if (token == "Tap to enter token...") {
            token = "";
        }

        tryConnect(url, token);
        return true;
    });
    root->addView(connectBtn);

    // Status label
    auto* statusLabel = new brls::Label();
    statusLabel->setText("");
    statusLabel->setFontSize(14);
    statusLabel->setMarginTop(16);
    statusLabel->setTextColor(nvgRGBA(180, 180, 180, 255));
    root->addView(statusLabel);

    return root;
}

void LoginActivity::tryConnect(const std::string& serverUrl, const std::string& authToken) {
    brls::Application::notify("Connecting to " + serverUrl + "...");

    auto& app = App::instance();
    app.setServerUrl(serverUrl);
    app.setAuthToken(authToken);

    // Set up event callback for connection result
    MAClient::instance().setEventCallback([this](MAEvent event, const Json& data) {
        if (event == MAEvent::CONNECTED) {
            auto& app = App::instance();
            app.setConnected(true);
            app.setServerInfo(MAClient::instance().getServerInfo());
            app.saveSettings();

            brls::Application::notify("Connected!");

            // Navigate to main
            brls::sync([]() {
                Application::instance().showMain();
            });
        } else if (event == MAEvent::DISCONNECTED) {
            brls::Application::notify("Connection failed. Check URL and try again.");
        }
    });

    // Attempt connection
    if (!MAClient::instance().connect(serverUrl, authToken)) {
        brls::Application::notify("Failed to connect. Check network.");
    }
}

} // namespace vita_ma
