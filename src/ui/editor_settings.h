#pragma once

#include <string>
#include <functional>

namespace sol {

// Global editor settings - singleton for UI access
class EditorSettings {
public:
    static EditorSettings& Get() {
        static EditorSettings instance;
        return instance;
    }
    
    // Input mode
    bool IsVimEnabled() const { return m_VimEnabled; }
    void SetVimEnabled(bool enabled) {
        m_VimEnabled = enabled;
        if (m_OnModeChanged) m_OnModeChanged(enabled);
    }
    
    // Mode indicator for status bar
    const char* GetModeIndicator() const { return m_ModeIndicator.c_str(); }
    void SetModeIndicator(const std::string& indicator) { m_ModeIndicator = indicator; }
    
    // Cursor position for status bar
    size_t GetCursorLine() const { return m_CursorLine; }
    size_t GetCursorCol() const { return m_CursorCol; }
    void SetCursorPos(size_t line, size_t col) { m_CursorLine = line; m_CursorCol = col; }
    
    // Callback when mode changes
    void SetOnModeChanged(std::function<void(bool)> callback) { m_OnModeChanged = std::move(callback); }
    
private:
    EditorSettings() = default;
    
    bool m_VimEnabled = false;
    std::string m_ModeIndicator = "STANDARD";
    size_t m_CursorLine = 1;
    size_t m_CursorCol = 1;
    std::function<void(bool)> m_OnModeChanged;
};

} // namespace sol
