#pragma once

#include <borealis.hpp>

namespace vita_ma {

class ProgressDialog : public brls::Box {
public:
    ProgressDialog(const std::string& title);
    ~ProgressDialog() override = default;

    void setProgress(float progress); // 0.0 - 1.0
    void setMessage(const std::string& message);
    void show();
    void dismiss();

private:
    brls::Label* m_titleLabel = nullptr;
    brls::Label* m_messageLabel = nullptr;
    brls::Label* m_progressLabel = nullptr;
    float m_progress = 0.0f;
    bool m_dismissed = false;
};

} // namespace vita_ma
