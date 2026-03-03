#pragma once

#include "ui/ui_system.h"
#include "ui/window_tree.h"
#include "ui/widgets/explorer.h"
#include <cstdint>
#include <vector>
#include <mutex>

namespace sol {

class Workspace : public UILayer {
public:
    explicit Workspace(const Id& id = "workspace");
    ~Workspace() override = default;

    void OnUI() override;

    // LSP diagnostics (thread-safe)
    void UpdateDiagnostics(const std::string& path, const std::vector<LSPDiagnostic>& diagnostics);

    bool IsFocused() const { return m_IsFocused; }
    void Focus();

    // Undo/Redo on active window's buffer
    bool Undo();
    bool Redo();

    // Window management
    WindowTree& GetWindowTree() { return m_WindowTree; }
    Window* SplitVertical();
    Window* SplitHorizontal();
    void FocusNextWindow();
    void FocusPrevWindow();

    bool IsExplorerFocused() const { return m_ExplorerOpen && m_Explorer.IsFocused(); }

    // Terminal management (integrated into window tree)
    void ToggleTerminal();
    void NewTerminal();
    void CloseTerminal();

    // Explorer sidebar (left-side toggle like NvimTree)
    void ToggleExplorer();
    bool IsExplorerOpen() const { return m_ExplorerOpen; }
    ExplorerWidget& GetExplorer() { return m_Explorer; }

private:
    void RenderTabBar();
    void ProcessPendingCloses();
    void ProcessPendingDiagnostics();
    void SyncActiveBuffer();

    uint32_t m_DockspaceID = 0;
    WindowTree m_WindowTree;
    size_t m_LastActiveBufferId = 0;
    std::vector<size_t> m_PendingCloseBuffers;

    // Explorer sidebar
    ExplorerWidget m_Explorer;
    bool m_ExplorerOpen = false;
    bool m_ExplorerWantsFocus = false;
    float m_ExplorerWidth = 220.0f;
    static constexpr float EXPLORER_MIN_WIDTH = 120.0f;
    static constexpr float EXPLORER_MAX_WIDTH = 500.0f;
    static constexpr float DIVIDER_SIZE = 3.0f;

    // Thread-safe pending diagnostics from LSP
    std::mutex m_DiagnosticsMutex;
    std::vector<std::pair<std::string, std::vector<LSPDiagnostic>>> m_PendingDiagnostics;

    bool m_IsFocused = false;
    bool m_WantsFocus = false;
};

} // namespace sol
