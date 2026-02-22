#pragma once

#include "core/terminal/terminal_emulator.h"
#include <imgui.h>
#include <memory>
#include <string>

namespace sol {

// Terminal widget configuration
struct TerminalConfig {
    int defaultRows = 24;
    int defaultCols = 80;
    std::string shell;  // Empty = use default shell
    std::string workingDir;  // Empty = use current directory
    bool showScrollbar = true;
    float fontSize = 0.0f;  // 0 = use default
};

// Terminal widget that renders a TerminalEmulator
class TerminalWidget {
public:
    TerminalWidget();
    ~TerminalWidget();
    
    // Spawn a new terminal session
    bool Spawn(const TerminalConfig& config = {});
    
    // Close the terminal
    void Close();
    
    // Process input from PTY without rendering (for background terminals)
    void ProcessInput();
    
    // Render the terminal widget
    // Returns true if the terminal has output to display
    bool Render(const char* label, const ImVec2& size = ImVec2(0, 0));
    
    // Is the terminal session alive?
    bool IsAlive() const;
    
    // Get exit code (valid when not alive)
    int GetExitCode() const;
    
    // Get/Set focused state
    bool IsFocused() const { return m_IsFocused; }
    void SetFocused(bool focused) { m_IsFocused = focused; }
    
    // Request keyboard focus capture for next frame
    void RequestFocusCapture() { m_WantsFocusCapture = true; }
    
    // Get terminal title
    const std::string& GetTitle() const;
    
    // Get the terminal emulator (for advanced access)
    TerminalEmulator* GetEmulator() { return m_Emulator.get(); }
    const TerminalEmulator* GetEmulator() const { return m_Emulator.get(); }
    
    // Configuration
    void SetShowScrollbar(bool show) { m_ShowScrollbar = show; }

private:
    void HandleInput();
    void RenderContent(const ImVec2& pos, const ImVec2& size);
    void RenderScrollbar(const ImVec2& pos, const ImVec2& size);
    void CalculateDimensions(const ImVec2& size);
    
    std::unique_ptr<TerminalEmulator> m_Emulator;
    std::shared_ptr<Pty> m_Pty;
    
    // Rendering state
    float m_CharWidth = 0.0f;
    float m_CharHeight = 0.0f;
    int m_VisibleRows = 24;
    int m_VisibleCols = 80;
    
    // Scroll state
    int m_ScrollOffset = 0;  // Offset into scrollback (0 = bottom)
    bool m_ShowScrollbar = true;
    bool m_ScrollbarDragging = false;
    float m_ScrollbarDragStartY = 0.0f;
    int m_ScrollbarDragStartOffset = 0;
    
    bool m_WantsFocusCapture = false;  // Request immediate keyboard focus
    
    // Focus state
    bool m_IsFocused = false;
    
    // Cursor blink
    float m_CursorBlinkTime = 0.0f;
    bool m_CursorVisible = true;
    
    // Selection
    bool m_Selecting = false;
    ImVec2 m_SelectStart;
    ImVec2 m_SelectEnd;
    
    // Title fallback
    std::string m_DefaultTitle = "Terminal";
};

} // namespace sol
