#include "menu_bar.h"
#include "core/resource_system.h"
#include "ui/editor_settings.h"
#include <imgui.h>

using sol::EventSystem;

namespace sol {

MenuBar::MenuBar(UISystem* uiSystem, const Id& id)
    : UILayer(id), m_UISystem(uiSystem) {
    SetupMenuBar();
}

void MenuBar::OnUI() {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("Sol")) {
            if (ImGui::MenuItem("Settings", "Ctrl+,")) {
                EventSystem::Execute("toggle_window", {{"window_id", std::string("Settings")}});
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Esc")) {
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
        if (ImGui::MenuItem("Open", "Ctrl+O")) {
            EventSystem::Execute("open_file_dialog");
        }
        if (ImGui::MenuItem("Open Folder", "Ctrl+Shift+O")) {
            EventSystem::Execute("open_folder_dialog");
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Save", "Ctrl+S")) {
            EventSystem::Execute("save_file");
        }
        if (ImGui::MenuItem("Save All", "Ctrl+Shift+S")) {
            EventSystem::Execute("save_all_files");
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Close Buffer", "Ctrl+W")) {
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
            if (ImGui::MenuItem("Explorer", nullptr, &enabled)) {
                EventSystem::Execute("toggle_window", {{"window_id", std::string("Explorer")}});
            }
        }

        auto settings = m_UISystem->GetLayer("Settings");
        if (settings) {
            bool enabled = settings->IsEnabled();
            if (ImGui::MenuItem("Settings", "Ctrl+,", &enabled)) {
                EventSystem::Execute("toggle_window", {{"window_id", std::string("Settings")}});
            }
        }
        
        ImGui::EndMenu();
    }
}

} // namespace sol
