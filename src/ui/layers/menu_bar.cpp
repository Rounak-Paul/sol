#include "menu_bar.h"
#include "ui/ui_system.h"
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

MenuBar::MenuBar(UISystem* uiSystem)
    : m_UISystem(uiSystem) {}

void MenuBar::Render() {
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
        if (ImGui::MenuItem("Explorer", GetShortcut("toggle_explorer"))) {
            EventSystem::Execute("toggle_explorer");
        }

        if (ImGui::MenuItem("Terminal", GetShortcut("toggle_terminal"))) {
            EventSystem::Execute("toggle_terminal");
        }

        if (ImGui::MenuItem("Split Vertical", GetShortcut("split_vertical"))) {
            EventSystem::Execute("split_vertical");
        }
        if (ImGui::MenuItem("Split Horizontal", GetShortcut("split_horizontal"))) {
            EventSystem::Execute("split_horizontal");
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
