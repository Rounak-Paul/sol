#include "terminal_panel.h"
#include "core/resource_system.h"
#include "ui/icons_nerd.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>

namespace sol {

TerminalPanel::TerminalPanel(const Id& id)
    : UILayer(id) {
}

void TerminalPanel::OnUI() {
    if (!IsEnabled()) return;
    
    // Clean up dead terminals first
    CleanupDeadTerminals();
    
    // If no terminals and panel just opened, create one
    if (m_Terminals.empty()) {
        CreateTerminal();
        if (m_Terminals.empty()) {
            // Failed to create terminal, hide panel
            SetEnabled(false);
            return;
        }
    }
    
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 4));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.1f, 0.1f, 0.1f, 1.0f));
    
    // Process input for ALL terminals (not just active) to prevent buffer bloat
    for (auto& term : m_Terminals) {
        if (term.widget) {
            term.widget->ProcessInput();
            term.alive = term.widget->IsAlive();
        }
    }
    
    // Set initial size only on first appearance
    ImGui::SetNextWindowSize(ImVec2(600, 300), ImGuiCond_FirstUseEver);
    
    bool isOpen = true;
    // Use NoNavInputs to prevent Tab key from cycling through UI elements
    ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoNavInputs;
    if (ImGui::Begin("Terminal", &isOpen, windowFlags)) {
        // Tab bar
        RenderTabBar();
        
        // Terminal content
        RenderTerminalContent();
        
        // Handle focus request
        if (m_WantsFocus) {
            ImGui::SetWindowFocus();
            m_WantsFocus = false;
        }
    }
    ImGui::End();
    
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    
    // If user closed the window via X button
    if (!isOpen) {
        SetEnabled(false);
    }
}

void TerminalPanel::RenderTabBar() {
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_Tab, ImVec4(0.15f, 0.15f, 0.15f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_TabActive, ImVec4(0.2f, 0.2f, 0.2f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_TabHovered, ImVec4(0.25f, 0.25f, 0.25f, 1.0f));
    
    if (ImGui::BeginTabBar("##TerminalTabs", ImGuiTabBarFlags_AutoSelectNewTabs | ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_TabListPopupButton)) {
        // Terminal tabs
        for (size_t i = 0; i < m_Terminals.size(); i++) {
            auto& term = m_Terminals[i];
            
            ImGuiTabItemFlags flags = ImGuiTabItemFlags_None;
            if (m_ForceSelectTab && i == m_ActiveTerminal) {
                flags |= ImGuiTabItemFlags_SetSelected;
            }
            
            // Tab label with terminal number and title
            std::string label = term.name.empty() ? 
                ("Terminal " + std::to_string(i + 1)) : 
                term.name;
            
            // Add status indicator
            if (!term.widget || !term.widget->IsAlive()) {
                label = "[X] " + label;
            }
            
            bool open = true;
            if (ImGui::BeginTabItem((label + "##" + std::to_string(i)).c_str(), &open, flags)) {
                m_ActiveTerminal = i;
                ImGui::EndTabItem();
            }
            
            // Handle close
            if (!open) {
                CloseTerminal(i);
                break;
            }
        }
        
        m_ForceSelectTab = false;  // Clear after processing
        
        // New terminal button
        if (ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip)) {
            CreateTerminal();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("New Terminal");
        }
        
        ImGui::EndTabBar();
    }
    
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();
}

void TerminalPanel::RenderTerminalContent() {
    if (m_ActiveTerminal >= m_Terminals.size()) return;
    
    auto& term = m_Terminals[m_ActiveTerminal];
    if (!term.widget) return;
    
    ImVec2 contentSize = ImGui::GetContentRegionAvail();
    
    term.widget->Render("##TerminalContent", contentSize);
    
    // Update alive status
    term.alive = term.widget->IsAlive();
    
    // Update tab name from terminal title
    const std::string& title = term.widget->GetTitle();
    if (!title.empty() && term.name != title) {
        term.name = title;
    }
}

size_t TerminalPanel::CreateTerminal(const TerminalConfig& config) {
    TerminalInstance instance;
    instance.widget = std::make_unique<TerminalWidget>();
    
    TerminalConfig cfg = config;
    
    // Use panel's working directory if not specified
    if (cfg.workingDir.empty() && !m_WorkingDirectory.empty()) {
        cfg.workingDir = m_WorkingDirectory;
    }
    
    // Try to use resource system's working directory
    if (cfg.workingDir.empty()) {
        auto& rs = ResourceSystem::GetInstance();
        const auto& wd = rs.GetWorkingDirectory();
        if (!wd.empty()) {
            cfg.workingDir = wd.string();
        }
    }
    
    if (instance.widget->Spawn(cfg)) {
        instance.name = "Terminal " + std::to_string(m_Terminals.size() + 1);
        instance.alive = true;
        m_Terminals.push_back(std::move(instance));
        m_ActiveTerminal = m_Terminals.size() - 1;
        m_ForceSelectTab = true;
        m_WantsFocus = true;
        // Request immediate keyboard focus for the terminal
        m_Terminals[m_ActiveTerminal].widget->RequestFocusCapture();
        return m_ActiveTerminal;
    }
    
    return static_cast<size_t>(-1);
}

void TerminalPanel::CloseTerminal(size_t index) {
    if (index >= m_Terminals.size()) return;
    
    m_Terminals.erase(m_Terminals.begin() + static_cast<ptrdiff_t>(index));
    
    // If no terminals left, hide the panel
    if (m_Terminals.empty()) {
        SetEnabled(false);
        return;
    }
    
    if (m_ActiveTerminal >= m_Terminals.size()) {
        m_ActiveTerminal = m_Terminals.size() - 1;
    }
}

void TerminalPanel::SetActiveTerminal(size_t index) {
    if (index < m_Terminals.size()) {
        m_ActiveTerminal = index;
        m_ForceSelectTab = true;
        m_Terminals[m_ActiveTerminal].widget->RequestFocusCapture();
    }
}

void TerminalPanel::NextTerminal() {
    if (m_Terminals.empty()) return;
    size_t next = (m_ActiveTerminal + 1) % m_Terminals.size();
    SetActiveTerminal(next);
}

void TerminalPanel::PrevTerminal() {
    if (m_Terminals.empty()) return;
    size_t prev = m_ActiveTerminal == 0 ? m_Terminals.size() - 1 : m_ActiveTerminal - 1;
    SetActiveTerminal(prev);
}

void TerminalPanel::Toggle() {
    SetEnabled(!IsEnabled());
    if (IsEnabled()) {
        Focus();
    }
}

void TerminalPanel::Focus() {
    m_WantsFocus = true;
    if (m_ActiveTerminal < m_Terminals.size()) {
        m_Terminals[m_ActiveTerminal].widget->SetFocused(true);
        m_Terminals[m_ActiveTerminal].widget->RequestFocusCapture();
    }
}

void TerminalPanel::CleanupDeadTerminals() {
    // Don't auto-remove dead terminals, just mark them
    // User may want to see the exit status
}

} // namespace sol
