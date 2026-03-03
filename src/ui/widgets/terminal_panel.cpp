#include "terminal_panel.h"
#include "core/resource_system.h"
#include "core/event_system.h"
#include "ui/input/command.h"
#include "ui/icons_nerd.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>

namespace sol {

void TerminalPanel::Render(const ImVec2& size) {
    // Auto-create first tab if panel is shown with no tabs
    if (m_Tabs.empty()) {
        NewTab();
    }

    // Clamp active tab
    m_ActiveTab = std::clamp(m_ActiveTab, 0, std::max(0, (int)m_Tabs.size() - 1));

    // Apply pending focus
    if (m_WantsFocus) {
        ImGui::SetWindowFocus();
        m_WantsFocus = false;
    }

    m_IsFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);

    // Tab bar
    ImGuiTabBarFlags tabFlags = ImGuiTabBarFlags_AutoSelectNewTabs |
                                ImGuiTabBarFlags_FittingPolicyScroll |
                                ImGuiTabBarFlags_NoCloseWithMiddleMouseButton;

    if (ImGui::BeginTabBar("##terminal_tabs", tabFlags)) {
        for (int i = 0; i < (int)m_Tabs.size(); ++i) {
            auto& tab = m_Tabs[i];
            std::string label;
            if (tab.terminal && tab.terminal->IsAlive()) {
                label = tab.terminal->GetTitle();
                if (label.empty()) label = "Terminal";
            } else {
                label = "Terminal (dead)";
            }
            label += "###term_" + std::to_string(i);

            bool open = true;
            ImGuiTabItemFlags itemFlags = 0;
            if (m_TabChanged && i == m_ActiveTab)
                itemFlags |= ImGuiTabItemFlags_SetSelected;

            if (ImGui::BeginTabItem(label.c_str(), &open, itemFlags)) {
                // Only let ImGui drive tab selection when no programmatic change pending
                if (!m_TabChanged)
                    m_ActiveTab = i;
                ImGui::EndTabItem();
            }

            if (!open) {
                // Close this tab
                m_Tabs.erase(m_Tabs.begin() + i);
                if (m_ActiveTab >= (int)m_Tabs.size())
                    m_ActiveTab = std::max(0, (int)m_Tabs.size() - 1);
                --i;
            }
        }
        ImGui::EndTabBar();
    }
    m_TabChanged = false;

    // Render active terminal
    if (m_ActiveTab >= 0 && m_ActiveTab < (int)m_Tabs.size()) {
        auto& tab = m_Tabs[m_ActiveTab];
        if (tab.terminal) {
            tab.terminal->SetWindowActive(m_IsWindowActive);
            if (m_WantsFocus) {
                tab.terminal->SetFocused(true);
                tab.terminal->RequestFocusCapture();
            }
            ImVec2 avail = ImGui::GetContentRegionAvail();
            tab.terminal->Render("##term_content", avail);
        }
    }
}

void TerminalPanel::NewTab() {
    Tab tab;
    tab.terminal = std::make_unique<TerminalWidget>();

    TerminalConfig cfg;
    auto& rs = ResourceSystem::GetInstance();
    if (rs.HasWorkingDirectory())
        cfg.workingDir = rs.GetWorkingDirectory().string();

    tab.terminal->Spawn(cfg);
    tab.title = "Terminal";

    m_Tabs.push_back(std::move(tab));
    m_ActiveTab = (int)m_Tabs.size() - 1;
}

void TerminalPanel::CloseActiveTab() {
    if (m_Tabs.empty()) return;
    m_Tabs.erase(m_Tabs.begin() + m_ActiveTab);
    if (m_ActiveTab >= (int)m_Tabs.size())
        m_ActiveTab = std::max(0, (int)m_Tabs.size() - 1);
}

void TerminalPanel::NextTab() {
    if (m_Tabs.size() <= 1) return;
    m_ActiveTab = (m_ActiveTab + 1) % (int)m_Tabs.size();
    m_TabChanged = true;
}

void TerminalPanel::PrevTab() {
    if (m_Tabs.size() <= 1) return;
    m_ActiveTab = (m_ActiveTab == 0) ? (int)m_Tabs.size() - 1 : m_ActiveTab - 1;
    m_TabChanged = true;
}

void TerminalPanel::ProcessAllInputs() {
    for (auto& tab : m_Tabs) {
        if (tab.terminal)
            tab.terminal->ProcessInput();
    }
}

} // namespace sol
