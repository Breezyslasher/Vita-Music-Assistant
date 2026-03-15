#pragma once

#include "app.h"
#include <string>

namespace vita_ma {

class Application {
public:
    static Application& instance();

    // Initialize and decide which activity to show
    void start();

    // Navigation
    void showLogin();
    void showMain();
    void showPlayer();

    // Check if we have valid connection settings
    bool hasServerConfig() const;

private:
    Application() = default;
};

} // namespace vita_ma
