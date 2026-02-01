#include "panel.h"
#include <imgui.h>
namespace sol
{
Panel::Panel(const Id& id)
    : UILayer(id)
{
    SetupPannel();
}

void Panel::OnUI()
{
    ImGui::Begin("Pannel");
    ImGui::Text("This is a pannel.");
    ImGui::End();
}

void Panel::SetupPannel()
{
    // Pannel setup if needed in the future
}
} // namespace sol