#pragma once

#include <borealis.hpp>

namespace vita_ma {

class LoginActivity : public brls::Activity {
public:
    LoginActivity();

    brls::View* createContentView() override;

private:
    void tryConnect(const std::string& serverUrl, const std::string& authToken);
};

} // namespace vita_ma
