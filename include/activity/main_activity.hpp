/**
 * Vita Music Assistant - Main Activity
 * Main navigation with tabs for Home, Library, Search, Settings
 */

#pragma once

#include <borealis.hpp>

namespace vita_ma {

class MainActivity : public brls::Activity {
public:
    MainActivity();

    brls::View* createContentView() override;

    void onContentAvailable() override;

private:
    void loadLibrariesToSidebar();

    BRLS_BIND(brls::TabFrame, tabFrame, "main/tab_frame");
};

} // namespace vita_ma
