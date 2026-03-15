#include "app/application.hpp"
#include "activity/login_activity.hpp"
#include "activity/main_activity.hpp"
#include "activity/player_activity.hpp"
#include <borealis.hpp>

namespace vita_ma {

Application& Application::instance() {
    static Application instance;
    return instance;
}

bool Application::hasServerConfig() const {
    auto& app = App::instance();
    return !app.getServerUrl().empty();
}

void Application::start() {
    if (hasServerConfig()) {
        showMain();
    } else {
        showLogin();
    }
}

void Application::showLogin() {
    auto* activity = new LoginActivity();
    brls::Application::pushActivity(activity);
}

void Application::showMain() {
    auto* activity = new MainActivity();
    brls::Application::pushActivity(activity);
}

void Application::showPlayer() {
    auto* activity = new PlayerActivity();
    brls::Application::pushActivity(activity);
}

} // namespace vita_ma
