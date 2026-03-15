#include "view/overlay_dialog.hpp"

namespace vita_ma {

OverlayDialog::OverlayDialog(const std::string& title, const std::string& message) {
    this->setAxis(brls::Axis::COLUMN);
    this->setJustifyContent(brls::JustifyContent::CENTER);
    this->setAlignItems(brls::AlignItems::CENTER);
    this->setGrow(1.0f);

    // Container
    m_container = new brls::Box();
    m_container->setAxis(brls::Axis::COLUMN);
    m_container->setPadding(25);
    m_container->setCornerRadius(12);
    m_container->setWidth(500);

    // Title
    m_titleLabel = new brls::Label();
    m_titleLabel->setText(title);
    m_titleLabel->setFontSize(22);
    m_titleLabel->setMarginBottom(16);
    m_container->addView(m_titleLabel);

    // Message
    if (!message.empty()) {
        m_messageLabel = new brls::Label();
        m_messageLabel->setText(message);
        m_messageLabel->setFontSize(16);
        m_messageLabel->setMarginBottom(16);
        m_container->addView(m_messageLabel);
    }

    // Button row
    m_buttonRow = new brls::Box();
    m_buttonRow->setAxis(brls::Axis::ROW);
    m_buttonRow->setJustifyContent(brls::JustifyContent::FLEX_END);
    m_buttonRow->setGap(8);
    m_container->addView(m_buttonRow);

    this->addView(m_container);

    // Circle button to dismiss
    this->registerAction("Back", brls::ControllerButton::BUTTON_B, [this](...) {
        dismiss();
        return true;
    });
}

void OverlayDialog::addButton(const std::string& label, std::function<void()> callback) {
    auto* btn = new brls::Label();
    btn->setText(label);
    btn->setFontSize(16);
    btn->setFocusable(true);
    btn->setPadding(8, 16, 8, 16);
    btn->setCornerRadius(8);

    btn->registerClickAction([this, callback](...) {
        if (callback) callback();
        dismiss();
        return true;
    });

    m_buttonRow->addView(btn);
}

void OverlayDialog::setCustomContent(brls::View* content) {
    if (m_messageLabel) {
        m_container->removeView(m_messageLabel);
        m_messageLabel = nullptr;
    }
    // Insert before button row
    m_container->addView(content, m_container->getChildren().size() - 1);
}

void OverlayDialog::show() {
    brls::Application::pushActivity(new brls::Activity(this));
}

void OverlayDialog::dismiss() {
    if (m_dismissed) return;
    m_dismissed = true;
    brls::Application::popActivity();
}

void OverlayDialog::draw(NVGcontext* vg, float x, float y, float width, float height,
                          brls::Style style, brls::FrameContext* ctx) {
    // Draw semi-transparent backdrop
    nvgBeginPath(vg);
    nvgRect(vg, x, y, width, height);
    nvgFillColor(vg, nvgRGBAf(0, 0, 0, m_backdropAlpha));
    nvgFill(vg);

    // Draw container background
    brls::Box::draw(vg, x, y, width, height, style, ctx);
}

} // namespace vita_ma
