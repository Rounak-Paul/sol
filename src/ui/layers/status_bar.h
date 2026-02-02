#pragma once

#include "ui/ui_system.h"

namespace sol {

class StatusBar : public UILayer {
public:
    explicit StatusBar(const Id& id = "status_bar");
    ~StatusBar() override = default;

    void OnUI() override;
};

} // namespace sol
