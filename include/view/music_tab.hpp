#pragma once

#include "app.h"
#include <borealis.hpp>

namespace vita_ma {

class MusicTab : public brls::Box {
public:
    MusicTab();
    ~MusicTab() override = default;

    void onShow();

private:
    void loadBrowsePath(const std::string& path);
    void navigateBack();

    brls::ScrollingFrame* m_scrollFrame = nullptr;
    brls::Box* m_content = nullptr;
    std::vector<std::string> m_pathHistory;
    bool m_loaded = false;
};

} // namespace vita_ma
