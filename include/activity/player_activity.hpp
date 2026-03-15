#pragma once

#include <borealis.hpp>

namespace vita_ma {

class PlayerActivity : public brls::Activity {
public:
    PlayerActivity();

    brls::View* createContentView() override;

private:
    void updateUI();
};

} // namespace vita_ma
