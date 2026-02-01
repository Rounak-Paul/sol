#include "main_window_layer.h"
#include "../../core/event_system.h"
#include <imgui.h>

using sol::EventSystem;

namespace sol {

MainWindowLayer::MainWindowLayer(const Id& id)
    : UILayer(id) {
}

void MainWindowLayer::OnUI() {
    ImGui::Begin("Sol Application");
    ImGui::Text("Hello from Sol!");
    ImGui::Text("Delta Time: %.4f ms", m_DeltaTime * 1000.0f);
    ImGui::Text("Counter: %.2f", m_Counter);
    
    if (ImGui::Button("Quit")) {
        EventSystem::Execute("exit");
    }
    
    ImGui::End();
}

} // namespace sol
