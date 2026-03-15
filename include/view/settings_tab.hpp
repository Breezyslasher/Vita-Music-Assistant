#pragma once

#include "app.h"
#include <borealis.hpp>

namespace vita_ma {

class SettingsTab : public brls::Box {
public:
    SettingsTab();
    ~SettingsTab() override = default;

    void onShow();

private:
    void buildSettings();
};

} // namespace vita_ma
