#include "application.h"
#include <imgui.h>

namespace sol {

Application::Application() {
}

Application::~Application() {
}

void Application::OnUpdate() {
    m_Counter += DeltaTime();
}

void Application::OnUI() {
    ImGui::Begin("Sol Application");
    ImGui::Text("Hello from Sol!");
    ImGui::Text("Delta Time: %.4f ms", DeltaTime() * 1000.0f);
    ImGui::Text("Counter: %.2f", m_Counter);
    
    if (ImGui::Button("Quit")) {
        Quit();
    }
    
    ImGui::End();
}

} // namespace sol
