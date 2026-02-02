#pragma once

#include "ui/ui_system.h"
#include "core/event_system.h"

namespace sol {

class MenuBar : public UILayer {
public:
    explicit MenuBar(UISystem* uiSystem, const Id& id = "menu_bar");
    ~MenuBar() override = default;

    void OnUI() override;

private:
    void SetupMenuBar();
    void RenderFileMenu();
    void RenderViewMenu();
    void RenderInputMenu();
    
    UISystem* m_UISystem;
};

} // namespace sol
