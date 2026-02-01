#pragma once

#include "../ui_system.h"
#include "../../core/event_system.h"

namespace sol {

class MenuBarLayer : public UILayer {
public:
    explicit MenuBarLayer(const Id& id = "menu_bar");
    ~MenuBarLayer() override = default;

    void OnUI() override;

private:
    void SetupMenuBar();
};

} // namespace sol
