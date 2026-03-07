#pragma once

#include "core/event_system.h"

namespace sol {

class UISystem;

class MenuBar {
public:
    explicit MenuBar(UISystem* uiSystem);

    // Render menu items — call from inside an active menu bar context (OnMenuBar)
    void Render();

private:
    void RenderFileMenu();
    void RenderViewMenu();

    UISystem* m_UISystem;
};

} // namespace sol
