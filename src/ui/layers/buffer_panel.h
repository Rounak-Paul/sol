#pragma once

#include "../ui_system.h"

namespace sol {

class BufferPanel : public UILayer {
public:
    explicit BufferPanel(const Id& id = "buffer_panel");
    ~BufferPanel() override = default;

    void OnUI() override;

private:
    void RenderBufferTabs();
    void RenderBufferContent();
};

} // namespace sol
