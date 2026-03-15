#include "view/progress_dialog.hpp"
#include <cstdio>

namespace vita_ma {

ProgressDialog::ProgressDialog(const std::string& title) {
    this->setAxis(brls::Axis::COLUMN);
    this->setJustifyContent(brls::JustifyContent::CENTER);
    this->setAlignItems(brls::AlignItems::CENTER);
    this->setGrow(1.0f);

    auto* container = new brls::Box();
    container->setAxis(brls::Axis::COLUMN);
    container->setAlignItems(brls::AlignItems::CENTER);
    container->setPadding(25);
    container->setCornerRadius(12);
    container->setWidth(400);

    m_titleLabel = new brls::Label();
    m_titleLabel->setText(title);
    m_titleLabel->setFontSize(20);
    m_titleLabel->setMarginBottom(12);
    container->addView(m_titleLabel);

    m_messageLabel = new brls::Label();
    m_messageLabel->setFontSize(14);
    m_messageLabel->setTextColor(nvgRGBA(180, 180, 180, 255));
    m_messageLabel->setMarginBottom(8);
    container->addView(m_messageLabel);

    m_progressLabel = new brls::Label();
    m_progressLabel->setText("0%");
    m_progressLabel->setFontSize(16);
    container->addView(m_progressLabel);

    this->addView(container);
}

void ProgressDialog::setProgress(float progress) {
    m_progress = progress;
    char buf[16];
    snprintf(buf, sizeof(buf), "%.0f%%", progress * 100);
    m_progressLabel->setText(buf);
}

void ProgressDialog::setMessage(const std::string& message) {
    m_messageLabel->setText(message);
}

void ProgressDialog::show() {
    brls::Application::pushActivity(new brls::Activity(this));
}

void ProgressDialog::dismiss() {
    if (m_dismissed) return;
    m_dismissed = true;
    brls::Application::popActivity();
}

} // namespace vita_ma
