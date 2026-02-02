#include "application.h"
#include "core/event_system.h"
#include "core/logger.h"
#include "core/resource_system.h"
#include "core/file_dialog.h"
#include "ui/layers/menu_bar.h"
#include "ui/layers/workspace.h"
#include "ui/layers/explorer.h"
#include "ui/layers/status_bar.h"
#include <imgui.h>

using sol::Logger;
using sol::EventSystem;

namespace sol {

Application::Application() {
}

Application::~Application() {
}

void Application::OnStart() {
    Logger::Info("Application starting...");
    SetupEvents();
    SetupUILayers();
    Logger::Info("Application initialized successfully");
}

void Application::OnUpdate() {
}

void Application::OnUI() {
    m_UISystem.RenderLayers();
}

void Application::SetupEvents() {
    Logger::Info("Setting up events...");
    
    auto exitEvent = std::make_shared<Event>("exit");
    exitEvent->SetHandler([this](const EventData& data) {
        Logger::Info("Exit event triggered");
        Quit();
        return true;
    })
    .SetSuccessCallback([](const EventData& data) {
        Logger::Info("Exit event completed successfully");
    })
    .SetFailureCallback([](const std::string& error) {
        Logger::Error("Exit event failed: " + error);
    });
    EventSystem::Register(exitEvent);
    
    auto toggleWindowEvent = std::make_shared<Event>("toggle_window");
    toggleWindowEvent->SetHandler([this](const EventData& data) {
        auto it = data.find("window_id");
        if (it == data.end()) {
            Logger::Error("toggle_window event missing window_id");
            return false;
        }
        
        std::string windowId = std::any_cast<std::string>(it->second);
        auto layer = m_UISystem.GetLayer(windowId);
        if (layer) {
            layer->SetEnabled(!layer->IsEnabled());
            Logger::Info("Toggled window: " + windowId + " to " + (layer->IsEnabled() ? "enabled" : "disabled"));
            return true;
        }
        
        Logger::Error("Window not found: " + windowId);
        return false;
    });
    EventSystem::Register(toggleWindowEvent);
    
    // File events
    auto openFileEvent = std::make_shared<Event>("open_file_dialog");
    openFileEvent->SetHandler([](const EventData& data) {
        auto path = FileDialog::OpenFile("Open File");
        if (path) {
            ResourceSystem::GetInstance().OpenFile(*path);
            Logger::Info("Opened file: " + path->string());
            return true;
        }
        return false;
    });
    EventSystem::Register(openFileEvent);
    
    auto openFolderEvent = std::make_shared<Event>("open_folder_dialog");
    openFolderEvent->SetHandler([this](const EventData& data) {
        auto path = FileDialog::OpenFolder("Open Folder");
        if (path) {
            ResourceSystem::GetInstance().SetWorkingDirectory(*path);
            Logger::Info("Opened folder: " + path->string());
            
            // Refresh explorer
            auto explorer = std::dynamic_pointer_cast<Explorer>(m_UISystem.GetLayer("Explorer"));
            if (explorer) {
                explorer->Refresh();
            }
            return true;
        }
        return false;
    });
    EventSystem::Register(openFolderEvent);
    
    auto saveFileEvent = std::make_shared<Event>("save_file");
    saveFileEvent->SetHandler([](const EventData& data) {
        auto activeBuffer = ResourceSystem::GetInstance().GetActiveBuffer();
        if (activeBuffer) {
            return ResourceSystem::GetInstance().SaveBuffer(activeBuffer->GetId());
        }
        return false;
    });
    EventSystem::Register(saveFileEvent);
    
    auto saveAllEvent = std::make_shared<Event>("save_all_files");
    saveAllEvent->SetHandler([](const EventData& data) {
        return ResourceSystem::GetInstance().SaveAllBuffers();
    });
    EventSystem::Register(saveAllEvent);
    
    auto closeBufferEvent = std::make_shared<Event>("close_buffer");
    closeBufferEvent->SetHandler([](const EventData& data) {
        auto activeBuffer = ResourceSystem::GetInstance().GetActiveBuffer();
        if (activeBuffer) {
            ResourceSystem::GetInstance().CloseBuffer(activeBuffer->GetId());
            return true;
        }
        return false;
    });
    EventSystem::Register(closeBufferEvent);
}

void Application::SetupUILayers() {
    auto menuBar = std::make_shared<MenuBar>(&m_UISystem);
    m_UISystem.RegisterLayer(menuBar);
    
    auto explorer = std::make_shared<Explorer>();
    m_UISystem.RegisterLayer(explorer);
    
    auto workspace = std::make_shared<Workspace>();
    m_UISystem.RegisterLayer(workspace);
    
    auto statusBar = std::make_shared<StatusBar>();
    m_UISystem.RegisterLayer(statusBar);
}

int Application::GetDockspaceFlags() {
    return ImGuiDockNodeFlags_PassthruCentralNode | ImGuiDockNodeFlags_NoDockingInCentralNode;
}

float Application::GetDockspaceBottomOffset() {
    return UISystem::StatusBarHeight;
}

} // namespace sol
