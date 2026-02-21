#include "status_bar.h"
#include "ui/ui_system.h"
#include "ui/editor_settings.h"
#include "ui/input/command.h"
#include <imgui.h>

namespace sol {

StatusBar::StatusBar(const Id& id)
    : UILayer(id) {
}

void StatusBar::OnUI() {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    
    // Use a base height that scales with UI
    float scale = ImGui::GetIO().FontGlobalScale;
    const float height = 16.0f * scale;
    
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
    
    auto& themeColors = EditorSettings::Get().GetTheme().colors;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, themeColors.menuBarBg);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
    
    if (ImGui::Begin("##StatusBar", nullptr, windowFlags)) {
        auto& settings = EditorSettings::Get();
        auto& inputSystem = InputSystem::GetInstance();
        auto& colors = themeColors;
        
        // Use the actual set position and dimensions, not ImGui's calculated ones
        ImVec2 barPos = ImVec2(viewport->WorkPos.x, viewport->WorkPos.y + viewport->WorkSize.y - height);
        float windowWidth = viewport->WorkSize.x;
        float windowHeight = height;
        
        // Calculate positions first
        size_t line = settings.GetCursorLine();
        size_t col = settings.GetCursorCol();
        
        char posStr[64];
        snprintf(posStr, sizeof(posStr), "Ln %zu, Col %zu", line, col);
        float posWidth = ImGui::CalcTextSize(posStr).x;
        
        // Left side: Input mode indicator with solid background
        bool isCommandMode = inputSystem.GetInputMode() == EditorInputMode::Command;
        const char* modeText = isCommandMode ? "COMMAND" : "INSERT";
        
        // Use fixed width for consistency (COMMAND is longer)
        float badgePadding = 8.0f * scale;
        float badgeWidth = ImGui::CalcTextSize("COMMAND").x + badgePadding * 2.0f;
        
        // Colors: blue for command, green for insert (these stay distinct for visibility)
        ImVec4 bgColor = isCommandMode ? ImVec4(0.2f, 0.4f, 0.7f, 1.0f) : ImVec4(0.2f, 0.5f, 0.3f, 1.0f);
        ImVec4 textColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        
        // Draw mode badge background - flush left, full height, no rounding
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(
            barPos,
            ImVec2(barPos.x + badgeWidth, barPos.y + windowHeight),
            ImGui::ColorConvertFloat4ToU32(bgColor),
            0.0f  // No rounding
        );
        
        // Draw mode text centered in badge (use actual text metrics)
        ImVec2 modeTextSize = ImGui::CalcTextSize(modeText);
        float modeTextX = (badgeWidth - modeTextSize.x) * 0.5f;
        float modeTextY = (windowHeight - modeTextSize.y) * 0.5f;
        drawList->AddText(
            ImVec2(barPos.x + modeTextX, barPos.y + modeTextY),
            ImGui::ColorConvertFloat4ToU32(textColor),
            modeText
        );
        
        // Right side: Cursor position - vertically centered, use theme text color
        float rightMargin = 8.0f * scale;
        float rightX = windowWidth - posWidth - rightMargin;
        ImVec2 posTextSize = ImGui::CalcTextSize(posStr);
        float posTextY = (windowHeight - posTextSize.y) * 0.5f;
        
        drawList->AddText(
            ImVec2(barPos.x + rightX, barPos.y + posTextY),
            ImGui::ColorConvertFloat4ToU32(colors.textDisabled),
            posStr
        );
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
}

} // namespace sol
