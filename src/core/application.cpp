#include "application.h"
#include "logger.h"
#include "../ui/layers/menu_bar.h"
#include "../ui/layers/workspace.h"
#include "../ui/layers/panel.h"

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
}

void Application::SetupUILayers() {
    auto menuBar = std::make_shared<MenuBar>();
    m_UISystem.RegisterLayer(menuBar);
    
    auto workspace = std::make_shared<Workspace>();
    m_UISystem.RegisterLayer(workspace);

    auto pannel = std::make_shared<Panel>();
    m_UISystem.RegisterLayer(pannel);
}

} // namespace sol
