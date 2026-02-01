#include "menu_bar_layer.h"
#include <imgui.h>

using sol::EventSystem;

namespace sol {

MenuBarLayer::MenuBarLayer(const Id& id)
    : UILayer(id) {
    SetupMenuBar();
}

void MenuBarLayer::OnUI() {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("Sol")) {
            if (ImGui::MenuItem("Exit", "Esc")) {
                EventSystem::Execute("exit");
            }
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
}

void MenuBarLayer::SetupMenuBar() {
    // MenuBar setup if needed in the future
}

} // namespace sol
