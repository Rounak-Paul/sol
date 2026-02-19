#pragma once

#include "pty.h"
#include <string>
#include <vector>
#include <deque>
#include <memory>
#include <cstdint>

namespace sol {

// Terminal cell attributes
struct TerminalAttr {
    uint32_t fg = 0xFFCCCCCC;  // Foreground color (ARGB)
    uint32_t bg = 0xFF1E1E1E;  // Background color (ARGB)
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool strikethrough = false;
    bool inverse = false;
    bool dim = false;
    
    bool operator==(const TerminalAttr& other) const {
        return fg == other.fg && bg == other.bg && bold == other.bold &&
               italic == other.italic && underline == other.underline &&
               strikethrough == other.strikethrough && inverse == other.inverse &&
               dim == other.dim;
    }
};

// A single cell in the terminal grid
struct TerminalCell {
    char32_t codepoint = ' ';
    TerminalAttr attr;
    bool dirty = true;  // Needs redraw
};

// A line in the terminal with wrap tracking
struct TerminalLine {
    std::vector<TerminalCell> cells;
    bool wrapped = false;  // True if line continues to next (soft wrap), false if hard newline
    
    // Convenience accessors to maintain compatibility
    size_t size() const { return cells.size(); }
    bool empty() const { return cells.empty(); }
    void resize(size_t n) { cells.resize(n); }
    void clear() { cells.clear(); wrapped = false; }
    TerminalCell& operator[](size_t i) { return cells[i]; }
    const TerminalCell& operator[](size_t i) const { return cells[i]; }
    auto begin() { return cells.begin(); }
    auto end() { return cells.end(); }
    auto begin() const { return cells.begin(); }
    auto end() const { return cells.end(); }
    void push_back(const TerminalCell& c) { cells.push_back(c); }
};

// Terminal color palette (256 color)
class TerminalPalette {
public:
    TerminalPalette();
    
    uint32_t GetColor(int index) const;
    void SetColor(int index, uint32_t color);
    
    // Standard terminal colors
    static constexpr int Black = 0;
    static constexpr int Red = 1;
    static constexpr int Green = 2;
    static constexpr int Yellow = 3;
    static constexpr int Blue = 4;
    static constexpr int Magenta = 5;
    static constexpr int Cyan = 6;
    static constexpr int White = 7;
    
private:
    uint32_t m_Colors[256];
};

// Terminal emulator state machine and buffer
class TerminalEmulator {
public:
    TerminalEmulator(int rows = 24, int cols = 80);
    ~TerminalEmulator();
    
    // Connect to a PTY
    void AttachPty(std::shared_ptr<Pty> pty);
    std::shared_ptr<Pty> GetPty() const { return m_Pty; }
    
    // Process input from PTY
    void ProcessInput();
    
    // Write to PTY (keyboard input)
    void Write(const std::string& data);
    void Write(char c);
    
    // Handle keyboard events
    void HandleKey(int key, int mods);
    void HandleChar(char32_t c);
    
    // Resize the terminal
    void Resize(int rows, int cols);
    
    // Get terminal dimensions
    int GetRows() const { return m_Rows; }
    int GetCols() const { return m_Cols; }
    
    // Get cursor position
    int GetCursorRow() const { return m_CursorRow; }
    int GetCursorCol() const { return m_CursorCol; }
    bool IsCursorVisible() const { return m_CursorVisible; }
    
    // Access screen buffer
    const TerminalLine& GetLine(int row) const;
    const TerminalCell& GetCell(int row, int col) const;
    
    // Scrollback buffer
    size_t GetScrollbackSize() const { return m_Scrollback.size(); }
    const TerminalLine& GetScrollbackLine(size_t index) const;
    
    // Selection
    void SetSelection(int startRow, int startCol, int endRow, int endCol);
    void ClearSelection();
    bool HasSelection() const { return m_HasSelection; }
    std::string GetSelectedText() const;
    
    // Mark all cells as dirty (force full redraw)
    void MarkDirty();
    
    // Clear dirty flags
    void ClearDirty();
    
    // Is the terminal alive?
    bool IsAlive() const;
    
    // Get exit code
    int GetExitCode() const;
    
    // Get the title set by the terminal application
    const std::string& GetTitle() const { return m_Title; }
    
    // Color palette
    TerminalPalette& GetPalette() { return m_Palette; }
    const TerminalPalette& GetPalette() const { return m_Palette; }

private:
    // Escape sequence parsing
    enum class ParseState {
        Normal,
        Escape,         // Got ESC
        CSI,            // Got ESC [
        OSC,            // Got ESC ]
        DCS,            // Got ESC P
        CharsetG0,      // Got ESC (
        CharsetG1,      // Got ESC )
    };
    
    void ProcessByte(uint8_t byte);
    void ProcessNormalChar(char32_t c);
    void ProcessControlChar(uint8_t c);
    void ProcessEscapeSequence(uint8_t c);
    void ProcessCSI(uint8_t c);
    void ProcessOSC(uint8_t c);
    void ExecuteCSI(char finalByte);
    void ExecuteOSC();
    
    // Terminal operations
    void PutChar(char32_t c);
    void Newline();
    void CarriageReturn();
    void Tab();
    void Backspace();
    void Delete();
    void Bell();
    
    void MoveCursor(int row, int col);
    void MoveCursorUp(int n = 1);
    void MoveCursorDown(int n = 1);
    void MoveCursorLeft(int n = 1);
    void MoveCursorRight(int n = 1);
    
    void ScrollUp(int n = 1);
    void ScrollDown(int n = 1);
    
    void EraseInDisplay(int mode);
    void EraseInLine(int mode);
    void DeleteChars(int n);
    void InsertChars(int n);
    void DeleteLines(int n);
    void InsertLines(int n);
    
    void SetScrollRegion(int top, int bottom);
    void ResetScrollRegion();
    
    void SetGraphicsRendition();  // SGR - process m_CSIParams
    void SetCursorVisibility(bool visible);
    
    void SaveCursor();
    void RestoreCursor();
    
    void Reset();
    
    // Helper to convert ANSI color index to ARGB
    uint32_t AnsiToColor(int ansi, bool bright);
    
    // Screen buffer
    std::vector<TerminalLine> m_Screen;
    std::deque<TerminalLine> m_Scrollback;
    static constexpr size_t MaxScrollback = 10000;
    
    // Dimensions
    int m_Rows;
    int m_Cols;
    
    // Cursor state
    int m_CursorRow = 0;
    int m_CursorCol = 0;
    bool m_CursorVisible = true;
    
    // Saved cursor state
    int m_SavedCursorRow = 0;
    int m_SavedCursorCol = 0;
    TerminalAttr m_SavedAttr;
    
    // Scroll region
    int m_ScrollTop = 0;
    int m_ScrollBottom = 0;
    
    // Current attributes
    TerminalAttr m_CurrentAttr;
    TerminalAttr m_DefaultAttr;
    
    // Parse state
    ParseState m_ParseState = ParseState::Normal;
    std::vector<int> m_CSIParams;
    std::string m_CSIIntermediate;
    std::string m_OSCString;
    int m_CurrentParam = 0;
    bool m_HasParam = false;
    
    // UTF-8 decoding state
    char32_t m_UTF8Char = 0;
    int m_UTF8Remaining = 0;
    
    // Selection
    bool m_HasSelection = false;
    int m_SelectionStartRow = 0;
    int m_SelectionStartCol = 0;
    int m_SelectionEndRow = 0;
    int m_SelectionEndCol = 0;
    
    // Title
    std::string m_Title;
    
    // PTY
    std::shared_ptr<Pty> m_Pty;
    
    // Color palette
    TerminalPalette m_Palette;
    
    // Mode flags
    bool m_AlternateScreen = false;
    bool m_ApplicationCursor = false;
    bool m_ApplicationKeypad = false;
    bool m_AutoWrap = true;
    bool m_OriginMode = false;
    bool m_InsertMode = false;
    
    // Empty line for bounds checking
    static TerminalLine s_EmptyLine;
    static TerminalCell s_EmptyCell;
};

} // namespace sol
