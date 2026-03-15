#pragma once

#include <borealis.hpp>
#include <functional>
#include <vector>

namespace vita_ma {

class OverlayDialog : public brls::Box {
public:
    OverlayDialog(const std::string& title, const std::string& message = "");
    ~OverlayDialog() override = default;

    void addButton(const std::string& label, std::function<void()> callback);
    void setCustomContent(brls::View* content);
    void show();
    void dismiss();

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override;

private:
    brls::Box* m_container = nullptr;
    brls::Box* m_buttonRow = nullptr;
    brls::Label* m_titleLabel = nullptr;
    brls::Label* m_messageLabel = nullptr;
    float m_backdropAlpha = 0.7f;
    bool m_dismissed = false;
};

} // namespace vita_ma
