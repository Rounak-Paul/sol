#include "workspace.h"
#include <imgui.h>
#include <imgui_internal.h>

namespace sol {

Workspace::Workspace(const Id& id)
    : UILayer(id), m_dockspaceID(0) {
}

void Workspace::OnUI() {
    if (ImGui::Begin("Workspace")) {
        ImVec2 size = ImGui::GetContentRegionAvail();
        m_viewportWidth = size.x;
        m_viewportHeight = size.y;
        
        ImGui::Text("Workspace Area %fx%f", m_viewportWidth, m_viewportHeight);
    }
    ImGui::End();
}

} // namespace sol
