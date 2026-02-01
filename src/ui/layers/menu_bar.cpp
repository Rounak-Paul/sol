#include "menu_bar.h"
#include <imgui.h>

using sol::EventSystem;

namespace sol {

MenuBar::MenuBar(const Id& id)
    : UILayer(id) {
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
        ImGui::EndMainMenuBar();
    }
}

void MenuBar::SetupMenuBar() {
    // MenuBar setup if needed in the future
}

} // namespace sol
