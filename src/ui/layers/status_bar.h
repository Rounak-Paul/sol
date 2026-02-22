#pragma once

#include "ui/ui_system.h"

struct ImGuiViewport;

namespace sol {

class StatusBar : public UILayer {
public:
    explicit StatusBar(const Id& id = "status_bar");
    ~StatusBar() override = default;

    void OnUI() override;
    
private:
    void RenderKeyHints(ImGuiViewport* viewport, float statusBarHeight);
};

} // namespace sol
