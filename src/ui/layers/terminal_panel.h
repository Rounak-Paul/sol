#pragma once

#include "ui/ui_system.h"
#include "ui/widgets/terminal.h"
#include <vector>
#include <memory>
#include <string>

namespace sol {

// Terminal panel that manages multiple terminal instances
// Similar to neovim/nvchad terminal functionality:
// - Toggle terminal visibility
// - Multiple terminal tabs
// - Horizontal/vertical splits possible
class TerminalPanel : public UILayer {
public:
    explicit TerminalPanel(const Id& id = "terminal_panel");
    ~TerminalPanel() override = default;

    void OnUI() override;
    
    // Create a new terminal
    size_t CreateTerminal(const TerminalConfig& config = {});
    
    // Close a terminal
    void CloseTerminal(size_t index);
    
    // Get terminal count
    size_t GetTerminalCount() const { return m_Terminals.size(); }
    
    // Get active terminal index
    size_t GetActiveTerminal() const { return m_ActiveTerminal; }
    
    // Set active terminal
    void SetActiveTerminal(size_t index);
    
    // Navigate between terminals
    void NextTerminal();
    void PrevTerminal();
    
    // Toggle panel visibility (like :ToggleTerm in nvchad)
    void Toggle();
    
    // Set the panel height as a ratio of the window (0.0 - 1.0)
    void SetHeightRatio(float ratio) { m_HeightRatio = std::clamp(ratio, 0.1f, 0.9f); }
    float GetHeightRatio() const { return m_HeightRatio; }
    
    // Focus the terminal (for keyboard input)
    void Focus();
    bool IsFocused() const { return m_IsFocused; }
    
    // Get the working directory for new terminals
    void SetWorkingDirectory(const std::string& dir) { m_WorkingDirectory = dir; }
    const std::string& GetWorkingDirectory() const { return m_WorkingDirectory; }

private:
    struct TerminalInstance {
        std::unique_ptr<TerminalWidget> widget;
        std::string name;
        bool alive = true;
    };
    
    void RenderTabBar();
    void RenderTerminalContent();
    void CleanupDeadTerminals();
    
    std::vector<TerminalInstance> m_Terminals;
    size_t m_ActiveTerminal = 0;
    float m_HeightRatio = 0.3f;  // Default: 30% of window height
    std::string m_WorkingDirectory;
    bool m_WantsFocus = false;
    bool m_IsFocused = false;
    bool m_ForceSelectTab = false;  // Force tab selection on next frame
};

} // namespace sol
