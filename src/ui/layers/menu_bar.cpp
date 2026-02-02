#include "menu_bar.h"
#include <imgui.h>

using sol::EventSystem;

namespace sol {

MenuBar::MenuBar(UISystem* uiSystem, const Id& id)
    : UILayer(id), m_UISystem(uiSystem) {
    SetupMenuBar();
}

void MenuBar::OnUI() {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("Sol")) {
            if (ImGui::MenuItem("Exit", "Esc")) {
                EventSystem::Execute("exit");
            }
            ImGui::EndMenu();
        }
        
        RenderViewMenu();
        
        ImGui::EndMainMenuBar();
    }
}

void MenuBar::SetupMenuBar() {
    // MenuBar setup if needed in the future
}

void MenuBar::RenderViewMenu() {
    if (ImGui::BeginMenu("View")) {
        auto explorer = m_UISystem->GetLayer("Explorer");
        if (explorer) {
            bool enabled = explorer->IsEnabled();
            if (ImGui::MenuItem("Explorer", nullptr, &enabled)) {
                EventSystem::Execute("toggle_window", {{"window_id", std::string("Explorer")}});
            }
        }
        
        ImGui::EndMenu();
    }
}

} // namespace sol
