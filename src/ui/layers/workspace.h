#pragma once

#include "../ui_system.h"

namespace sol {

class Workspace : public UILayer {
public:
    explicit Workspace(const Id& id = "workspace");
    ~Workspace() override = default;

    void OnUI() override;
    
    float GetViewportWidth() const { return m_viewportWidth; }
    float GetViewportHeight() const { return m_viewportHeight; }

private:
    uint32_t m_dockspaceID;
    float m_viewportWidth = 800.0f;
    float m_viewportHeight = 600.0f;
};

} // namespace sol
