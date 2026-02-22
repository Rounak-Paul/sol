#include "menu_bar.h"
#include "core/resource_system.h"
#include "ui/editor_settings.h"
#include <imgui.h>

using sol::EventSystem;

namespace sol {

// Helper to get shortcut for menu display
static const char* GetShortcut(const std::string& eventId) {
    static std::string shortcutCache;
    shortcutCache = EditorSettings::Get().GetShortcutForEvent(eventId);
    return shortcutCache.empty() ? nullptr : shortcutCache.c_str();
}

MenuBar::MenuBar(UISystem* uiSystem, const Id& id)
    : UILayer(id), m_UISystem(uiSystem) {
    SetupMenuBar();
}

void MenuBar::OnUI() {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("Sol")) {
            if (ImGui::MenuItem("Settings", GetShortcut("toggle_settings"))) {
                EventSystem::Execute("toggle_window", {{"window_id", std::string("Settings")}});
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", GetShortcut("exit"))) {
                EventSystem::Execute("exit");
            }
            ImGui::EndMenu();
        }
        
        RenderFileMenu();
        RenderViewMenu();
        
        ImGui::EndMainMenuBar();
    }
}

void MenuBar::SetupMenuBar() {
    // MenuBar setup if needed in the future
}

void MenuBar::RenderFileMenu() {
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open", GetShortcut("open_file_dialog"))) {
            EventSystem::Execute("open_file_dialog");
        }
        if (ImGui::MenuItem("Open Folder", GetShortcut("open_folder_dialog"))) {
            EventSystem::Execute("open_folder_dialog");
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Save", GetShortcut("save_file"))) {
            EventSystem::Execute("save_file");
        }
        if (ImGui::MenuItem("Save All", GetShortcut("save_all_files"))) {
            EventSystem::Execute("save_all_files");
        }
        ImGui::Separator();
        if (ImGui::MenuItem("New Buffer", GetShortcut("new_buffer"))) {
            EventSystem::Execute("new_buffer");
        }
        if (ImGui::MenuItem("Close Buffer", GetShortcut("close_buffer"))) {
            EventSystem::Execute("close_buffer");
        }
        ImGui::EndMenu();
    }
}

void MenuBar::RenderViewMenu() {
    if (ImGui::BeginMenu("View")) {
        auto explorer = m_UISystem->GetLayer("Explorer");
        if (explorer) {
            bool enabled = explorer->IsEnabled();
            if (ImGui::MenuItem("Explorer", GetShortcut("toggle_explorer"), &enabled)) {
                EventSystem::Execute("toggle_window", {{"window_id", std::string("Explorer")}});
            }
        }

        auto terminalPanel = m_UISystem->GetLayer("terminal_panel");
        if (terminalPanel) {
            bool enabled = terminalPanel->IsEnabled();
            if (ImGui::MenuItem("Terminal", GetShortcut("toggle_terminal"), &enabled)) {
                EventSystem::Execute("toggle_terminal");
            }
        }

        auto settings = m_UISystem->GetLayer("Settings");
        if (settings) {
            bool enabled = settings->IsEnabled();
            if (ImGui::MenuItem("Settings", GetShortcut("toggle_settings"), &enabled)) {
                EventSystem::Execute("toggle_window", {{"window_id", std::string("Settings")}});
            }
        }
        
        ImGui::EndMenu();
    }
}

} // namespace sol
