#include "view/horizontal_scroll_row.hpp"

namespace vita_ma {

HorizontalScrollRow::HorizontalScrollRow() {
    this->setScrollingAxis(brls::Axis::ROW);

    m_content = new brls::Box();
    m_content->setAxis(brls::Axis::ROW);
    m_content->setJustifyContent(brls::JustifyContent::FLEX_START);
    m_content->setAlignItems(brls::AlignItems::FLEX_START);
    m_content->setGap(12);
    m_content->setPaddingRight(16);

    this->setContentView(m_content);
}

void HorizontalScrollRow::addCell(brls::View* cell) {
    if (m_content && cell) {
        m_content->addView(cell);
    }
}

void HorizontalScrollRow::clearCells() {
    if (m_content) {
        m_content->clearViews();
    }
}

brls::View* HorizontalScrollRow::create() {
    return new HorizontalScrollRow();
}

} // namespace vita_ma
