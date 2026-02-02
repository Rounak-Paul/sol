#pragma once

#include "ui/ui_system.h"
#include "ui/widgets/syntax_editor.h"
#include <set>
#include <cstdint>
#include <optional>
#include <vector>
#include <memory>

namespace sol {

class Workspace : public UILayer {
public:
    explicit Workspace(const Id& id = "workspace");
    ~Workspace() override = default;

    void OnUI() override;

private:
    void RenderTabBar();
    void RenderBufferContent();
    void RenderFloatingBuffers();
    void ProcessPendingActions();
    
    uint32_t m_DockspaceID;
    std::set<size_t> m_FloatingBufferIds;
    std::optional<size_t> m_PendingFloatBuffer;
    std::vector<size_t> m_PendingCloseBuffers;
    std::unique_ptr<SyntaxEditor> m_Editor;
};

} // namespace sol
