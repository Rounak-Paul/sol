#include "status_bar.h"
#include "ui/ui_system.h"
#include "ui/editor_settings.h"
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
    
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.15f, 0.15f, 0.15f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 1.0f));
    
    if (ImGui::Begin("##StatusBar", nullptr, windowFlags)) {
        auto& settings = EditorSettings::Get();
        
        // Right side: Cursor position
        size_t line = settings.GetCursorLine();
        size_t col = settings.GetCursorCol();
        
        char posStr[64];
        snprintf(posStr, sizeof(posStr), "Ln %zu, Col %zu", line, col);
        
        float posWidth = ImGui::CalcTextSize(posStr).x + 20.0f;
        float availWidth = ImGui::GetContentRegionAvail().x;
        
        if (availWidth > posWidth) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + availWidth - posWidth);
        }
        
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", posStr);
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

} // namespace sol
