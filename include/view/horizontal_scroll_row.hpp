#pragma once

#include <borealis.hpp>

namespace vita_ma {

class HorizontalScrollRow : public brls::ScrollingFrame {
public:
    HorizontalScrollRow();
    ~HorizontalScrollRow() override = default;

    void addCell(brls::View* cell);
    void clearCells();

    static brls::View* create();

private:
    brls::Box* m_content = nullptr;
};

} // namespace vita_ma
