#pragma once

#include "app.h"
#include "view/horizontal_scroll_row.hpp"
#include <borealis.hpp>

namespace vita_ma {

class HomeTab : public brls::Box {
public:
    HomeTab();
    ~HomeTab() override = default;

    void onShow();

private:
    void loadRecentlyPlayed();
    void loadRecommendations();

    brls::ScrollingFrame* m_scrollFrame = nullptr;
    brls::Box* m_content = nullptr;
    HorizontalScrollRow* m_recentRow = nullptr;
    HorizontalScrollRow* m_recommendRow = nullptr;
    bool m_loaded = false;
};

} // namespace vita_ma
