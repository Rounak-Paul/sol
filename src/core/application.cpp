#include "application.h"
#include "logger.h"
#include "../ui/layers/menu_bar_layer.h"

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
    m_Counter += DeltaTime();
    if (m_MainWindow) {
        m_MainWindow->SetCounter(m_Counter);
        m_MainWindow->SetDeltaTime(DeltaTime());
    }
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
    auto menuBar = std::make_shared<MenuBarLayer>();
    m_UISystem.RegisterLayer(menuBar);
    
    m_MainWindow = std::make_shared<MainWindowLayer>();
    m_UISystem.RegisterLayer(m_MainWindow);
}

} // namespace sol
