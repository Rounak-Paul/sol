#include "status_bar.h"
#include "../ui_system.h"
#include <imgui.h>

namespace sol {

StatusBar::StatusBar(const Id& id)
    : UILayer(id) {
}

void StatusBar::OnUI() {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    
    const float height = UISystem::StatusBarHeight;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x, viewport->WorkPos.y + viewport->WorkSize.y - height));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, height));
    ImGui::SetNextWindowViewport(viewport->ID);
    
    ImGuiWindowFlags windowFlags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoDocking;
    
    if (ImGui::Begin("##StatusBar", nullptr, windowFlags)) {
        // Empty for now
    }
    ImGui::End();
}

} // namespace sol
