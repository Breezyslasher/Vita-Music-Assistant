#pragma once

#include "app.h"
#include <borealis.hpp>
#include <vector>
#include <functional>

namespace vita_ma {

// Data source for the recycling grid
class RecyclingGridDataSource {
public:
    virtual ~RecyclingGridDataSource() = default;
    virtual int getItemCount() = 0;
    virtual brls::View* createCell(int index) = 0;
    virtual void onItemSelected(int index) = 0;
    virtual bool canLoadMore() { return false; }
    virtual void loadMore() {}
};

class RecyclingGrid : public brls::ScrollingFrame {
public:
    RecyclingGrid();
    ~RecyclingGrid() override;

    void setDataSource(RecyclingGridDataSource* source);
    void reloadData();
    void setColumns(int columns) { m_columns = columns; }

    static brls::View* create();

private:
    RecyclingGridDataSource* m_dataSource = nullptr;
    brls::Box* m_contentBox = nullptr;
    int m_columns = 6;

    void buildGrid();
};

} // namespace vita_ma
