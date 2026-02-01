#include "workspace.h"
#include <imgui.h>
#include <imgui_internal.h>

namespace sol {

Workspace::Workspace(const Id& id)
    : UILayer(id), m_dockspaceID(0) {
}

void Workspace::OnUI() {
    // Get or create the main dockspace ID
    m_dockspaceID = ImGui::GetID("MainDockSpace");
    
    // Set window class to hide tab bar for this window
    ImGuiWindowClass windowClass;
    windowClass.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_NoTabBar;
    ImGui::SetNextWindowClass(&windowClass);
    
    // Window flags - no decorations, permanent
    ImGuiWindowFlags workspaceFlags = ImGuiWindowFlags_NoTitleBar | 
                                      ImGuiWindowFlags_NoCollapse |
                                      ImGuiWindowFlags_NoScrollbar |
                                      ImGuiWindowFlags_NoScrollWithMouse |
                                      ImGuiWindowFlags_NoBringToFrontOnFocus;
    
    // Always dock to the main dockspace
    ImGui::SetNextWindowDockID(m_dockspaceID, ImGuiCond_Always);
    
    ImGui::Begin("Workspace", nullptr, workspaceFlags);
    
    // Update viewport size from available content region
    ImVec2 size = ImGui::GetContentRegionAvail();
    m_viewportWidth = size.x;
    m_viewportHeight = size.y;
    
    // Render content area (empty for now)
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 windowPos = ImGui::GetWindowPos();
    ImVec2 contentMin = ImGui::GetWindowContentRegionMin();
    ImVec2 contentMax = ImGui::GetWindowContentRegionMax();
    
    ImVec2 clipMin(windowPos.x + contentMin.x, windowPos.y + contentMin.y);
    ImVec2 clipMax(windowPos.x + contentMax.x, windowPos.y + contentMax.y);
    drawList->PushClipRect(clipMin, clipMax, true);
    
    // Draw background
    drawList->AddRectFilled(clipMin, clipMax, IM_COL32(10, 10, 12, 255));
    
    drawList->PopClipRect();
    
    ImGui::End();
}

} // namespace sol
