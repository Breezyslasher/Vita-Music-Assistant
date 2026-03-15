#pragma once

#include "app.h"
#include <borealis.hpp>

namespace vita_ma {

class SearchTab : public brls::Box {
public:
    SearchTab();
    ~SearchTab() override = default;

    void onShow();

private:
    void performSearch(const std::string& query);
    void showSearchInput();

    brls::ScrollingFrame* m_scrollFrame = nullptr;
    brls::Box* m_content = nullptr;
    brls::Box* m_resultsBox = nullptr;
    std::string m_lastQuery;
};

} // namespace vita_ma
