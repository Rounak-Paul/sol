#include "status_bar.h"
#include "ui/ui_system.h"
#include "ui/editor_settings.h"
#include "ui/input/command.h"
#include "ui/input/keybinding.h"
#include <imgui.h>
#include <algorithm>

namespace sol {

StatusBar::StatusBar(const Id& id)
    : UILayer(id) {
}

void StatusBar::OnUI() {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    
    // Use a base height that scales with UI
    float scale = ImGui::GetIO().FontGlobalScale;
    const float height = 16.0f * scale;
    
    auto& inputSystem = InputSystem::GetInstance();
    auto& themeColors = EditorSettings::Get().GetTheme().colors;
    
    // Render keybinding hints popup if there's a pending sequence
    if (inputSystem.HasPendingSequence()) {
        RenderKeyHints(viewport, height);
    }
    
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
    
    ImGui::PushStyleColor(ImGuiCol_WindowBg, themeColors.menuBarBg);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
    
    if (ImGui::Begin("##StatusBar", nullptr, windowFlags)) {
        auto& settings = EditorSettings::Get();
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
        
        // After mode badge: Show pending key sequence if any (left-aligned)
        if (inputSystem.HasPendingSequence()) {
            std::string pendingStr = inputSystem.GetPendingSequence().ToString();
            ImVec2 pendingSize = ImGui::CalcTextSize(pendingStr.c_str());
            float pendingX = badgeWidth + badgePadding;  // Left-aligned after badge
            float pendingY = (windowHeight - pendingSize.y) * 0.5f;
            
            // Draw with a highlight color (yellow/gold)
            drawList->AddText(
                ImVec2(barPos.x + pendingX, barPos.y + pendingY),
                IM_COL32(255, 200, 100, 255),
                pendingStr.c_str()
            );
        }
        
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

void StatusBar::RenderKeyHints(ImGuiViewport* viewport, float statusBarHeight) {
    auto& inputSystem = InputSystem::GetInstance();
    auto matches = inputSystem.GetMatchingBindings();
    
    if (matches.empty()) return;
    
    float scale = ImGui::GetIO().FontGlobalScale;
    float lineHeight = ImGui::GetTextLineHeightWithSpacing();
    float padding = 12.0f * scale;
    float itemPadding = 8.0f * scale;
    
    // Calculate item width - each item has key + description
    // We want a grid layout with multiple columns
    float maxKeyWidth = 0.0f;
    float maxDescWidth = 0.0f;
    for (const auto& [key, cmd] : matches) {
        maxKeyWidth = std::max(maxKeyWidth, ImGui::CalcTextSize(key.c_str()).x);
        maxDescWidth = std::max(maxDescWidth, ImGui::CalcTextSize(cmd.c_str()).x);
    }
    
    float itemWidth = maxKeyWidth + maxDescWidth + itemPadding * 3.0f;
    float windowWidth = viewport->WorkSize.x - padding * 2.0f;
    
    // Calculate number of columns that fit
    int numColumns = std::max(1, static_cast<int>(windowWidth / itemWidth));
    itemWidth = windowWidth / numColumns;  // Distribute width evenly
    
    // Calculate number of rows needed
    int numItems = static_cast<int>(matches.size());
    int numRows = (numItems + numColumns - 1) / numColumns;
    
    // Calculate popup height (limit to half of viewport height)
    float maxHeight = viewport->WorkSize.y * 0.5f;
    float contentHeight = lineHeight * numRows + padding * 2.0f;
    float popupHeight = std::min(contentHeight, maxHeight);
    
    // Full width popup
    float popupWidth = viewport->WorkSize.x;
    float popupX = viewport->WorkPos.x;
    float popupY = viewport->WorkPos.y + viewport->WorkSize.y - statusBarHeight - popupHeight;
    
    ImGui::SetNextWindowPos(ImVec2(popupX, popupY));
    ImGui::SetNextWindowSize(ImVec2(popupWidth, popupHeight));
    
    ImGuiWindowFlags popupFlags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav;
    
    // Allow scrolling if content exceeds max height
    if (contentHeight > maxHeight) {
        popupFlags |= ImGuiWindowFlags_AlwaysVerticalScrollbar;
    } else {
        popupFlags |= ImGuiWindowFlags_NoScrollbar;
    }
    
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.12f, 0.12f, 0.15f, 0.98f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.25f, 0.25f, 0.3f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padding, padding));
    
    if (ImGui::Begin("##KeyHints", nullptr, popupFlags)) {
        // Render items in grid layout
        for (int i = 0; i < numItems; ++i) {
            const auto& [key, cmd] = matches[i];
            
            int col = i % numColumns;
            int row = i / numColumns;
            
            // Set cursor position for grid item
            float itemX = col * itemWidth;
            float itemY = row * lineHeight;
            
            ImGui::SetCursorPos(ImVec2(padding + itemX, padding + itemY));
            
            // Key in cyan/bright color with background
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 0.8f, 1.0f, 1.0f));
            ImGui::Text("%s", key.c_str());
            ImGui::PopStyleColor();
            
            // Description after key
            ImGui::SameLine(padding + itemX + maxKeyWidth + itemPadding);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.8f, 0.8f, 1.0f));
            ImGui::Text("%s", cmd.c_str());
            ImGui::PopStyleColor();
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

} // namespace sol
