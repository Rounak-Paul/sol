#include "application.h"
#include "logger.h"
#include "../ui/layers/menu_bar_layer.h"

namespace sol {

Application::Application() {
}

Application::~Application() {
}

void Application::OnStart() {
    Logger::GetInstance().Info("Application starting...");
    SetupEvents();
    SetupUILayers();
    Logger::GetInstance().Info("Application initialized successfully");
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
    Logger::GetInstance().Info("Setting up events...");
    auto exitEvent = std::make_shared<Event>("exit");
    exitEvent->SetHandler([this](const EventData& data) {
        Logger::GetInstance().Info("Exit event triggered");
        Quit();
        return true;
    })
    .SetSuccessCallback([](const EventData& data) {
        Logger::GetInstance().Info("Exit event completed successfully");
    })
    .SetFailureCallback([](const std::string& error) {
        Logger::GetInstance().Error("Exit event failed: " + error);
    });
    
    EventSystem::GetInstance().RegisterEvent(exitEvent);
}

void Application::SetupUILayers() {
    auto menuBar = std::make_shared<MenuBarLayer>();
    m_UISystem.RegisterLayer(menuBar);
    
    m_MainWindow = std::make_shared<MainWindowLayer>();
    m_UISystem.RegisterLayer(m_MainWindow);
}

} // namespace sol
