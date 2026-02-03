#pragma once

#include "ui/ui_system.h"
#include "ui/widgets/syntax_editor.h"
#include <set>
#include <map>
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
    
    // Updates diagnostic markers (LSP errors/warnings) for a specific file
    void UpdateDiagnostics(const std::string& path, const std::vector<LSPDiagnostic>& diagnostics);

private:
    void RenderTabBar();
    void RenderBufferContent();
    void RenderFloatingBuffers();
    void ProcessPendingActions();
    
    uint32_t m_DockspaceID;
    std::set<size_t> m_FloatingBufferIds;
    std::optional<size_t> m_PendingFloatBuffer;
    std::vector<size_t> m_PendingCloseBuffers;
    std::map<size_t, std::unique_ptr<SyntaxEditor>> m_Editors;

    SyntaxEditor* GetOrCreateEditor(size_t bufferId);
};

} // namespace sol
