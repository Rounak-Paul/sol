#include "status_bar.h"
#include "ui/ui_system.h"
#include "ui/editor_settings.h"
#include <imgui.h>
#include <cstring>

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
        
        // Left side: Mode indicator
        const char* modeIndicator = settings.GetModeIndicator();
        bool isVim = settings.IsVimEnabled();
        
        // Ensure we have valid text for the button
        const char* displayMode = (modeIndicator && modeIndicator[0] != '\0') ? modeIndicator : (isVim ? "NORMAL" : "STANDARD");
        
        // Mode badge with color
        if (isVim) {
            // Vim mode colors based on state
            ImVec4 badgeColor = ImVec4(0.2f, 0.6f, 0.2f, 1.0f);  // Green for normal
            if (strcmp(displayMode, "INSERT") == 0) {
                badgeColor = ImVec4(0.6f, 0.4f, 0.2f, 1.0f);  // Orange for insert
            } else if (strncmp(displayMode, "VISUAL", 6) == 0 || strncmp(displayMode, "V-", 2) == 0) {
                badgeColor = ImVec4(0.5f, 0.3f, 0.6f, 1.0f);  // Purple for visual
            } else if (strcmp(displayMode, ":") == 0 || strcmp(displayMode, "/") == 0 || strcmp(displayMode, "?") == 0) {
                badgeColor = ImVec4(0.3f, 0.5f, 0.7f, 1.0f);  // Blue for command/search
            }
            
            ImGui::PushStyleColor(ImGuiCol_Button, badgeColor);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, badgeColor);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, badgeColor);
            ImGui::SmallButton(displayMode);
            ImGui::PopStyleColor(3);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.3f, 0.5f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.3f, 0.5f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.3f, 0.3f, 0.5f, 1.0f));
            ImGui::SmallButton("STANDARD");
            ImGui::PopStyleColor(3);
        }
        
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "|");
        ImGui::SameLine();
        
        // Right side: Cursor position (pushed to right)
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
