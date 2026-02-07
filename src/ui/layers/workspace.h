#pragma once

#include "ui/ui_system.h"
#include "ui/widgets/syntax_editor.h"
#include <set>
#include <map>
#include <cstdint>
#include <optional>
#include <vector>
#include <memory>
#include <mutex>

namespace sol {

class Workspace : public UILayer {
public:
    explicit Workspace(const Id& id = "workspace");
    ~Workspace() override = default;

    void OnUI() override;
    
    // Updates diagnostic markers (LSP errors/warnings) for a specific file
    // Thread-safe: can be called from any thread
    void UpdateDiagnostics(const std::string& path, const std::vector<LSPDiagnostic>& diagnostics);

private:
    void RenderTabBar();
    void RenderBufferContent();
    void RenderFloatingBuffers();
    void ProcessPendingActions();
    void ProcessPendingDiagnostics();
    
    uint32_t m_DockspaceID;
    std::set<size_t> m_FloatingBufferIds;
    std::optional<size_t> m_PendingFloatBuffer;
    std::vector<size_t> m_PendingCloseBuffers;
    std::map<size_t, std::unique_ptr<SyntaxEditor>> m_Editors;
    size_t m_LastActiveBufferId = 0;
    
    // Thread-safe pending diagnostics from LSP
    std::mutex m_DiagnosticsMutex;
    std::vector<std::pair<std::string, std::vector<LSPDiagnostic>>> m_PendingDiagnostics;

    SyntaxEditor* GetOrCreateEditor(size_t bufferId);
};

} // namespace sol
