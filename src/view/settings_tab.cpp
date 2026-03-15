#include "view/settings_tab.hpp"
#include "app/ma_client.hpp"
#include "app/application.hpp"
#include <borealis.hpp>

namespace vita_ma {

SettingsTab::SettingsTab() {
    this->setAxis(brls::Axis::COLUMN);
    this->setGrow(1.0f);
    buildSettings();
}

void SettingsTab::onShow() {}

void SettingsTab::buildSettings() {
    auto* scrollFrame = new brls::ScrollingFrame();
    scrollFrame->setGrow(1.0f);

    auto* content = new brls::Box();
    content->setAxis(brls::Axis::COLUMN);
    content->setPadding(16);

    auto* title = new brls::Label();
    title->setText("Settings");
    title->setFontSize(24);
    title->setMarginBottom(24);
    content->addView(title);

    auto& app = App::instance();

    // --- Server Connection ---
    auto* serverHeader = new brls::Label();
    serverHeader->setText("Server Connection");
    serverHeader->setFontSize(18);
    serverHeader->setMarginBottom(8);
    content->addView(serverHeader);

    // Server URL
    auto* serverRow = new brls::Box();
    serverRow->setAxis(brls::Axis::ROW);
    serverRow->setAlignItems(brls::AlignItems::CENTER);
    serverRow->setHeight(48);
    serverRow->setFocusable(true);
    serverRow->setPadding(8);

    auto* serverLabel = new brls::Label();
    serverLabel->setText("Server URL: " + (app.getServerUrl().empty() ? "(not set)" : app.getServerUrl()));
    serverLabel->setFontSize(14);
    serverLabel->setGrow(1.0f);
    serverRow->addView(serverLabel);

    serverRow->registerClickAction([serverLabel](...) {
        brls::Swkbd::openForText([serverLabel](const std::string& text) {
            if (!text.empty()) {
                App::instance().setServerUrl(text);
                App::instance().saveSettings();
                serverLabel->setText("Server URL: " + text);
            }
        }, "Server URL", "http://192.168.1.100:8095", 256,
           App::instance().getServerUrl());
        return true;
    });
    content->addView(serverRow);

    // Auth Token
    auto* tokenRow = new brls::Box();
    tokenRow->setAxis(brls::Axis::ROW);
    tokenRow->setAlignItems(brls::AlignItems::CENTER);
    tokenRow->setHeight(48);
    tokenRow->setFocusable(true);
    tokenRow->setPadding(8);

    auto* tokenLabel = new brls::Label();
    tokenLabel->setText("Auth Token: " + (app.getAuthToken().empty() ? "(not set)" : "****"));
    tokenLabel->setFontSize(14);
    tokenLabel->setGrow(1.0f);
    tokenRow->addView(tokenLabel);

    tokenRow->registerClickAction([tokenLabel](...) {
        brls::Swkbd::openForText([tokenLabel](const std::string& text) {
            App::instance().setAuthToken(text);
            App::instance().saveSettings();
            tokenLabel->setText("Auth Token: " + (text.empty() ? "(not set)" : "****"));
        }, "Auth Token", "Enter authentication token (optional)", 256, "");
        return true;
    });
    content->addView(tokenRow);

    // Connect/Reconnect button
    auto* connectBtn = new brls::Label();
    connectBtn->setText("Connect to Server");
    connectBtn->setFontSize(16);
    connectBtn->setFocusable(true);
    connectBtn->setPadding(12, 16, 12, 16);
    connectBtn->setCornerRadius(8);
    connectBtn->setMarginTop(8);
    connectBtn->setMarginBottom(16);

    connectBtn->registerClickAction([](...) {
        auto& a = App::instance();
        if (a.getServerUrl().empty()) {
            brls::Application::notify("Please set server URL first");
            return true;
        }
        MAClient::instance().disconnect();
        MAClient::instance().connect(a.getServerUrl(), a.getAuthToken());
        brls::Application::notify("Connecting...");
        return true;
    });
    content->addView(connectBtn);

    // --- Playback ---
    auto* playbackHeader = new brls::Label();
    playbackHeader->setText("Playback");
    playbackHeader->setFontSize(18);
    playbackHeader->setMarginBottom(8);
    content->addView(playbackHeader);

    // Background playback toggle
    auto* bgRow = new brls::Box();
    bgRow->setAxis(brls::Axis::ROW);
    bgRow->setAlignItems(brls::AlignItems::CENTER);
    bgRow->setHeight(48);
    bgRow->setFocusable(true);
    bgRow->setPadding(8);

    auto* bgLabel = new brls::Label();
    bgLabel->setText("Background Playback: " +
                     std::string(app.isBackgroundPlaybackEnabled() ? "ON" : "OFF"));
    bgLabel->setFontSize(14);
    bgLabel->setGrow(1.0f);
    bgRow->addView(bgLabel);

    bgRow->registerClickAction([bgLabel](...) {
        auto& a = App::instance();
        a.setBackgroundPlaybackEnabled(!a.isBackgroundPlaybackEnabled());
        a.saveSettings();
        bgLabel->setText("Background Playback: " +
                         std::string(a.isBackgroundPlaybackEnabled() ? "ON" : "OFF"));
        return true;
    });
    content->addView(bgRow);

    // Audio quality
    auto* qualityRow = new brls::Box();
    qualityRow->setAxis(brls::Axis::ROW);
    qualityRow->setAlignItems(brls::AlignItems::CENTER);
    qualityRow->setHeight(48);
    qualityRow->setFocusable(true);
    qualityRow->setPadding(8);

    auto* qualityLabel = new brls::Label();
    qualityLabel->setText("Audio Quality: " + app.getAudioQuality());
    qualityLabel->setFontSize(14);
    qualityLabel->setGrow(1.0f);
    qualityRow->addView(qualityLabel);

    qualityRow->registerClickAction([qualityLabel](...) {
        auto& a = App::instance();
        // Cycle through quality options
        std::string current = a.getAudioQuality();
        if (current == "flac") current = "mp3_320";
        else if (current == "mp3_320") current = "mp3_192";
        else if (current == "mp3_192") current = "mp3_128";
        else current = "flac";

        a.setAudioQuality(current);
        a.saveSettings();
        qualityLabel->setText("Audio Quality: " + current);
        return true;
    });
    content->addView(qualityRow);

    // --- About ---
    auto* aboutHeader = new brls::Label();
    aboutHeader->setText("About");
    aboutHeader->setFontSize(18);
    aboutHeader->setMarginTop(16);
    aboutHeader->setMarginBottom(8);
    content->addView(aboutHeader);

    auto* versionLabel = new brls::Label();
    versionLabel->setText("Vita Music Assistant v1.0.0");
    versionLabel->setFontSize(14);
    versionLabel->setPadding(8);
    content->addView(versionLabel);

    auto* descLabel = new brls::Label();
    descLabel->setText("A Music Assistant client for PS Vita");
    descLabel->setFontSize(12);
    descLabel->setTextColor(nvgRGBA(180, 180, 180, 255));
    descLabel->setPadding(0, 8, 8, 8);
    content->addView(descLabel);

    // Server info if connected
    if (app.isConnected()) {
        auto info = app.getServerInfo();
        auto* serverInfo = new brls::Label();
        serverInfo->setText("Connected to: " + info.server_name + " v" + info.server_version);
        serverInfo->setFontSize(12);
        serverInfo->setTextColor(nvgRGBA(100, 200, 100, 255));
        serverInfo->setPadding(0, 8, 8, 8);
        content->addView(serverInfo);
    }

    // Logout / disconnect
    auto* logoutBtn = new brls::Label();
    logoutBtn->setText("Disconnect & Reset");
    logoutBtn->setFontSize(16);
    logoutBtn->setFocusable(true);
    logoutBtn->setPadding(12, 16, 12, 16);
    logoutBtn->setCornerRadius(8);
    logoutBtn->setMarginTop(16);
    logoutBtn->setTextColor(nvgRGBA(255, 100, 100, 255));

    logoutBtn->registerClickAction([](...) {
        MAClient::instance().disconnect();
        App::instance().setServerUrl("");
        App::instance().setAuthToken("");
        App::instance().setConnected(false);
        App::instance().saveSettings();
        Application::instance().showLogin();
        return true;
    });
    content->addView(logoutBtn);

    scrollFrame->setContentView(content);
    this->addView(scrollFrame);
}

} // namespace vita_ma
