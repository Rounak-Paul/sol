#include "workspace.h"
#include "core/resource_system.h"
#include "core/text/text_buffer.h"
#include <imgui.h>
#include <imgui_internal.h>

namespace sol {

Workspace::Workspace(const Id& id)
    : UILayer(id), m_DockspaceID(0), m_PendingFloatBuffer(std::nullopt) {
}

void Workspace::OnUI() {
    // Process any pending actions from last frame
    ProcessPendingActions();
    
    // Get or create the main dockspace ID
    m_DockspaceID = ImGui::GetID(UISystem::MainDockSpaceId);
    
    // Set window class to hide tab bar for this window (it's always there)
    ImGuiWindowClass windowClass;
    windowClass.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_NoTabBar;
    ImGui::SetNextWindowClass(&windowClass);
    
    // Window flags - no decorations, permanent
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | 
                             ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoScrollWithMouse |
                             ImGuiWindowFlags_NoBringToFrontOnFocus;
    
    // Always dock to the main dockspace central node
    ImGui::SetNextWindowDockID(m_DockspaceID, ImGuiCond_Always);
    
    ImGui::Begin("##Workspace", nullptr, flags);
    
    RenderTabBar();
    RenderBufferContent();
    
    ImGui::End();
    
    // Render any floating buffer windows
    RenderFloatingBuffers();
}

void Workspace::RenderTabBar() {
    auto& rs = ResourceSystem::GetInstance();
    const auto& buffers = rs.GetBuffers();
    
    if (buffers.empty()) {
        return;
    }
    
    ImGuiTabBarFlags tabBarFlags = ImGuiTabBarFlags_Reorderable | 
                                    ImGuiTabBarFlags_AutoSelectNewTabs |
                                    ImGuiTabBarFlags_FittingPolicyScroll;
    
    if (ImGui::BeginTabBar(UISystem::EditorTabBarId, tabBarFlags)) {
        for (const auto& buffer : buffers) {
            // Skip floating buffers
            if (m_FloatingBufferIds.count(buffer->GetId())) {
                continue;
            }
            
            std::string label = buffer->GetName();
            if (buffer->GetResource()->IsModified()) {
                label += " *";
            }
            // Unique ID per buffer to avoid label conflicts
            label += "###buf_" + std::to_string(buffer->GetId());
            
            bool open = true;
            
            if (ImGui::BeginTabItem(label.c_str(), &open)) {
                // This tab is selected, update active buffer
                if (!buffer->IsActive()) {
                    rs.SetActiveBuffer(buffer->GetId());
                }
                ImGui::EndTabItem();
            }
            
            // Context menu for undocking
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Float Window")) {
                    m_PendingFloatBuffer = buffer->GetId();
                }
                if (ImGui::MenuItem("Close")) {
                    m_PendingCloseBuffers.push_back(buffer->GetId());
                }
                ImGui::EndPopup();
            }
            
            if (!open) {
                m_PendingCloseBuffers.push_back(buffer->GetId());
            }
        }
        ImGui::EndTabBar();
    }
}

void Workspace::RenderBufferContent() {
    auto& rs = ResourceSystem::GetInstance();
    auto activeBuffer = rs.GetActiveBuffer();
    
    if (!activeBuffer || activeBuffer->IsFloating()) {
        // Find first non-floating buffer to display
        for (const auto& buffer : rs.GetBuffers()) {
            if (!buffer->IsFloating()) {
                rs.SetActiveBuffer(buffer->GetId());
                activeBuffer = buffer;
                break;
            }
        }
    }
    
    if (!activeBuffer || activeBuffer->IsFloating()) {
        ImVec2 center = ImGui::GetContentRegionAvail();
        ImGui::SetCursorPos(ImVec2(center.x * 0.5f - 50, center.y * 0.5f));
        ImGui::TextDisabled("No buffer open");
        return;
    }
    
    auto resource = activeBuffer->GetResource();
    if (resource->GetType() == ResourceType::Text) {
        auto textResource = std::dynamic_pointer_cast<TextResource>(resource);
        if (textResource) {
            TextBuffer& buffer = textResource->GetBuffer();
            
            TextResource::EditState editState{textResource.get()};
            
            ImVec2 available = ImGui::GetContentRegionAvail();
            ImGui::InputTextMultiline("##editor_content", 
                buffer.Data(), 
                buffer.Capacity() + 1,
                available,
                ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_CallbackEdit | ImGuiInputTextFlags_AllowTabInput,
                TextResource::InputTextCallback,
                &editState);
        }
    } else {
        ImGui::TextDisabled("Unsupported file type");
    }
}

void Workspace::RenderFloatingBuffers() {
    auto& rs = ResourceSystem::GetInstance();
    
    std::vector<size_t> toRemove;
    
    for (auto bufferId : m_FloatingBufferIds) {
        auto buffer = rs.GetBuffer(bufferId);
        if (!buffer) {
            toRemove.push_back(bufferId);
            continue;
        }
        
        std::string windowName = buffer->GetName();
        if (buffer->GetResource()->IsModified()) {
            windowName += " *";
        }
        windowName += "###float_" + std::to_string(bufferId);
        
        bool open = true;
        ImGui::SetNextWindowSize(ImVec2(600, 400), ImGuiCond_FirstUseEver);
        
        if (ImGui::Begin(windowName.c_str(), &open)) {
            // Context menu to dock back
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Dock to Editor")) {
                    toRemove.push_back(bufferId);
                    buffer->SetFloating(false);
                    rs.SetActiveBuffer(bufferId);
                }
                ImGui::EndPopup();
            }
            
            auto resource = buffer->GetResource();
            if (resource->GetType() == ResourceType::Text) {
                auto textResource = std::dynamic_pointer_cast<TextResource>(resource);
                if (textResource) {
                    TextBuffer& textBuf = textResource->GetBuffer();
                    
                    TextResource::EditState editState{textResource.get()};
                    
                    ImVec2 available = ImGui::GetContentRegionAvail();
                    std::string id = "##float_content_" + std::to_string(bufferId);
                    ImGui::InputTextMultiline(id.c_str(), 
                        textBuf.Data(), 
                        textBuf.Capacity() + 1,
                        available,
                        ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_CallbackEdit | ImGuiInputTextFlags_AllowTabInput,
                        TextResource::InputTextCallback,
                        &editState);
                }
            }
        }
        ImGui::End();
        
        if (!open) {
            toRemove.push_back(bufferId);
            buffer->SetFloating(false);
        }
    }
    
    for (auto id : toRemove) {
        m_FloatingBufferIds.erase(id);
    }
}

void Workspace::ProcessPendingActions() {
    auto& rs = ResourceSystem::GetInstance();
    
    // Process pending closes first
    for (auto bufferId : m_PendingCloseBuffers) {
        m_FloatingBufferIds.erase(bufferId);
        rs.CloseBuffer(bufferId);
    }
    m_PendingCloseBuffers.clear();
    
    // Process pending float
    if (m_PendingFloatBuffer.has_value()) {
        auto bufferId = m_PendingFloatBuffer.value();
        m_PendingFloatBuffer = std::nullopt;
        
        auto buffer = rs.GetBuffer(bufferId);
        if (buffer) {
            m_FloatingBufferIds.insert(bufferId);
            buffer->SetFloating(true);
            
            // Set a different buffer as active if this was active
            if (buffer->IsActive()) {
                for (const auto& b : rs.GetBuffers()) {
                    if (!b->IsFloating() && b->GetId() != bufferId) {
                        rs.SetActiveBuffer(b->GetId());
                        break;
                    }
                }
            }
        }
    }
}

} // namespace sol
