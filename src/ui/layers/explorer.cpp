#include "explorer.h"
#include <imgui.h>
namespace sol
{
Explorer::Explorer(const Id& id)
    : UILayer(id)
{
    SetupExplorer();
}

void Explorer::OnUI()
{
    ImGui::Begin("Explorer");
    ImGui::Text("This is File Explorer.");
    ImGui::End();
}

void Explorer::SetupExplorer()
{
    // Pannel setup if needed in the future
}
} // namespace sol