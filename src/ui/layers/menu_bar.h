#pragma once

#include "../ui_system.h"
#include "../../core/event_system.h"

namespace sol {

class MenuBar : public UILayer {
public:
    explicit MenuBar(const Id& id = "menu_bar");
    ~MenuBar() override = default;

    void OnUI() override;

private:
    void SetupMenuBar();
};

} // namespace sol
