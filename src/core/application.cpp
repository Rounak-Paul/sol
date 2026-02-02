#include "application.h"
#include "logger.h"
#include "../ui/layers/menu_bar.h"
#include "../ui/layers/workspace.h"
#include "../ui/layers/explorer.h"
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
}

void Application::SetupUILayers() {
    auto menuBar = std::make_shared<MenuBar>(&m_UISystem);
    m_UISystem.RegisterLayer(menuBar);
    
    auto workspace = std::make_shared<Workspace>();
    m_UISystem.RegisterLayer(workspace);

    auto explorer = std::make_shared<Explorer>();
    m_UISystem.RegisterLayer(explorer);
}

int Application::GetDockspaceFlags() {
    return ImGuiDockNodeFlags_PassthruCentralNode | ImGuiDockNodeFlags_NoDockingInCentralNode;
}

} // namespace sol
