#include "view/recycling_grid.hpp"
#include <borealis.hpp>

namespace vita_ma {

RecyclingGrid::RecyclingGrid() {
    m_contentBox = new brls::Box();
    m_contentBox->setAxis(brls::Axis::ROW);
    m_contentBox->setJustifyContent(brls::JustifyContent::FLEX_START);
    m_contentBox->setAlignItems(brls::AlignItems::FLEX_START);
    m_contentBox->setWrap(true);
    m_contentBox->setPadding(8);

    this->setContentView(m_contentBox);
}

RecyclingGrid::~RecyclingGrid() {
    delete m_dataSource;
}

void RecyclingGrid::setDataSource(RecyclingGridDataSource* source) {
    delete m_dataSource;
    m_dataSource = source;
    reloadData();
}

void RecyclingGrid::reloadData() {
    if (!m_dataSource || !m_contentBox) return;
    buildGrid();
}

void RecyclingGrid::buildGrid() {
    m_contentBox->clearViews();

    if (!m_dataSource) return;

    int count = m_dataSource->getItemCount();
    for (int i = 0; i < count; i++) {
        brls::View* cell = m_dataSource->createCell(i);
        if (cell) {
            int index = i;
            cell->registerClickAction([this, index](...) {
                if (m_dataSource) m_dataSource->onItemSelected(index);
                return true;
            });
            m_contentBox->addView(cell);
        }
    }

    // Check if more data can be loaded
    if (m_dataSource->canLoadMore()) {
        auto* loadMore = new brls::Label();
        loadMore->setText("Loading more...");
        loadMore->setFontSize(14);
        loadMore->setMarginTop(16);
        loadMore->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        m_contentBox->addView(loadMore);
        m_dataSource->loadMore();
    }
}

brls::View* RecyclingGrid::create() {
    return new RecyclingGrid();
}

} // namespace vita_ma
