#pragma once

#include "app.h"
#include <borealis.hpp>

namespace vita_ma {

class QueueTab : public brls::Box {
public:
    QueueTab();
    ~QueueTab() override = default;

    void onShow();
    void refresh();

private:
    void loadQueueItems();

    brls::ScrollingFrame* m_scrollFrame = nullptr;
    brls::Box* m_content = nullptr;
    bool m_loaded = false;
};

} // namespace vita_ma
