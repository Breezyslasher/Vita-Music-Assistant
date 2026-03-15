#pragma once

#include "app.h"
#include <borealis.hpp>

namespace vita_ma {

class MediaItemCell : public brls::Box {
public:
    MediaItemCell();
    ~MediaItemCell() override = default;

    void setData(const MediaItem& item);
    void setTitle(const std::string& title);
    void setSubtitle(const std::string& subtitle);
    void setImage(const std::string& url);

    MediaItem getItem() const { return m_item; }

    static brls::View* create();

private:
    MediaItem m_item;
    brls::Image* m_image = nullptr;
    brls::Label* m_title = nullptr;
    brls::Label* m_subtitle = nullptr;
};

} // namespace vita_ma
