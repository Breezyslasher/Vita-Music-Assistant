/**
 * Vita Music Assistant - Main Activity implementation
 */

#include "activity/main_activity.hpp"
#include "view/home_tab.hpp"
#include "view/library_tab.hpp"
#include "view/library_section_tab.hpp"
#include "view/search_tab.hpp"
#include "view/settings_tab.hpp"
#include "view/debug_tab.hpp"
#include "app/application.hpp"
#include "app/ma_types.hpp"
#include "app/music_queue.hpp"
#include "activity/player_activity.hpp"
#include "utils/async.hpp"

#include <algorithm>

namespace vita_ma {

// Helper to calculate text width (approximate based on character count)
static int calculateTextWidth(const std::string& text) {
    const int charWidth = 12;
    const int padding = 50;
    return static_cast<int>(text.length()) * charWidth + padding;
}

MainActivity::MainActivity() {
    brls::Logger::debug("MainActivity created");
}

brls::View* MainActivity::createContentView() {
    return brls::View::createFromXMLResource("activity/main.xml");
}

void MainActivity::onContentAvailable() {
    brls::Logger::debug("MainActivity content available");

    if (tabFrame) {
        AppSettings& settings = Application::getInstance().getSettings();

        // Calculate dynamic sidebar width based on content
        int sidebarWidth = 200;

        std::vector<std::string> standardTabs = {"Home", "Library", "Music", "Search", "Settings"};
        for (const auto& tab : standardTabs) {
            sidebarWidth = std::max(sidebarWidth, calculateTextWidth(tab));
        }

        // Apply sidebar width (with reasonable bounds)
        sidebarWidth = std::min(sidebarWidth, 350);
        brls::View* sidebar = tabFrame->getView("brls/tab_frame/sidebar");
        if (sidebar) {
            if (settings.collapseSidebar) {
                sidebar->setWidth(160);
                brls::Logger::debug("MainActivity: Collapsed sidebar to 160px");
            } else {
                sidebar->setWidth(sidebarWidth);
                brls::Logger::debug("MainActivity: Dynamic sidebar width: {}px", sidebarWidth);
            }
        }

        {
            // Add tabs based on sidebar order setting
            std::string sidebarOrder = settings.sidebarOrder;

            std::vector<std::string> order;
            if (!sidebarOrder.empty()) {
                std::string orderStr = sidebarOrder;
                size_t pos = 0;
                while ((pos = orderStr.find(',')) != std::string::npos) {
                    order.push_back(orderStr.substr(0, pos));
                    orderStr.erase(0, pos + 1);
                }
                if (!orderStr.empty()) {
                    order.push_back(orderStr);
                }
            } else {
                // Default order for music-only app
                order = {"home", "library", "search"};
            }

            // Add tabs in specified order
            for (const std::string& item : order) {
                if (item == "home") {
                    tabFrame->addTab("Home", []() { return new HomeTab(); });
                } else if (item == "library") {
                    // One sidebar entry per library category (no wrapper tab).
                    tabFrame->addTab("Artists",    []() { return new LibraryTab(MusicCategory::ARTISTS, false); });
                    tabFrame->addTab("Albums",     []() { return new LibraryTab(MusicCategory::ALBUMS, false); });
                    tabFrame->addTab("Tracks",     []() { return new LibraryTab(MusicCategory::TRACKS, false); });
                    tabFrame->addTab("Playlists",  []() { return new LibraryTab(MusicCategory::PLAYLISTS, false); });
                    tabFrame->addTab("Podcasts",   []() { return new LibraryTab(MusicCategory::PODCASTS, false); });
                    tabFrame->addTab("Audiobooks", []() { return new LibraryTab(MusicCategory::AUDIOBOOKS, false); });
                    tabFrame->addTab("Radio",      []() { return new LibraryTab(MusicCategory::RADIOS, false); });
                } else if (item == "search") {
                    tabFrame->addTab("Search", []() { return new SearchTab(); });
                }
            }
        }

        // Debug and Settings always at the bottom
        tabFrame->addSeparator();
        if (settings.showDebugTab) {
            tabFrame->addTab("Debug", []() { return new DebugTab(); });
        }
        tabFrame->addTab("Settings", []() { return new SettingsTab(); });

        // Focus first tab
        tabFrame->focusTab(0);

        // Register BUTTON_B on the root content view to open/return to player
        brls::View* rootBox = tabFrame->getParent();
        if (rootBox) {
            rootBox->registerAction("", brls::ControllerButton::BUTTON_B, [](brls::View* view) {
                auto* playerActivity = PlayerActivity::createResumeQueue();
                brls::Application::pushActivity(playerActivity);
                return true;
            });
        }
    }
}

} // namespace vita_ma
