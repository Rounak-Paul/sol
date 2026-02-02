#include "buffer_panel.h"
#include "../../core/resource_system.h"
#include <imgui.h>

namespace sol {

BufferPanel::BufferPanel(const Id& id)
    : UILayer(id) {
}

void BufferPanel::OnUI() {
    ImGui::Begin("Open Buffers");
    
    auto& rs = ResourceSystem::GetInstance();
    const auto& buffers = rs.GetBuffers();
    
    if (buffers.empty()) {
        ImGui::TextDisabled("No open files");
    } else {
        for (const auto& buffer : buffers) {
            std::string label = buffer->GetName();
            if (buffer->GetResource()->IsModified()) {
                label += " *";
            }
            if (buffer->IsFloating()) {
                label += " (floating)";
            }
            
            bool isActive = buffer->IsActive();
            if (isActive) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.4f, 1.0f));
            }
            
            if (ImGui::Selectable(label.c_str(), isActive)) {
                rs.SetActiveBuffer(buffer->GetId());
            }
            
            if (isActive) {
                ImGui::PopStyleColor();
            }
            
            // Context menu
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Close")) {
                    rs.CloseBuffer(buffer->GetId());
                }
                ImGui::EndPopup();
            }
        }
    }
    
    ImGui::End();
}

void BufferPanel::RenderBufferTabs() {
}

void BufferPanel::RenderBufferContent() {
}

} // namespace sol
