#pragma once

#include <cstddef>

namespace sol {

// Global editor settings - singleton for UI access
class EditorSettings {
public:
    static EditorSettings& Get() {
        static EditorSettings instance;
        return instance;
    }
    
    // Cursor position for status bar
    size_t GetCursorLine() const { return m_CursorLine; }
    size_t GetCursorCol() const { return m_CursorCol; }
    void SetCursorPos(size_t line, size_t col) { m_CursorLine = line; m_CursorCol = col; }
    
private:
    EditorSettings() = default;
    
    size_t m_CursorLine = 1;
    size_t m_CursorCol = 1;
};

} // namespace sol
