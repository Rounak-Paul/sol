#include "terminal_emulator.h"
#include <algorithm>
#include <cstring>
#include <chrono>

namespace sol {

// Static empty instances for bounds checking
TerminalLine TerminalEmulator::s_EmptyLine;
TerminalCell TerminalEmulator::s_EmptyCell;

// Color palette initialization
TerminalPalette::TerminalPalette() {
    // Standard 16 colors (dark and bright variants)
    // Dark colors
    m_Colors[0] = 0xFF1E1E1E;  // Black
    m_Colors[1] = 0xFFCD3131;  // Red
    m_Colors[2] = 0xFF0DBC79;  // Green
    m_Colors[3] = 0xFFE5E510;  // Yellow
    m_Colors[4] = 0xFF2472C8;  // Blue
    m_Colors[5] = 0xFFBC3FBC;  // Magenta
    m_Colors[6] = 0xFF11A8CD;  // Cyan
    m_Colors[7] = 0xFFE5E5E5;  // White
    
    // Bright colors
    m_Colors[8]  = 0xFF666666;  // Bright Black
    m_Colors[9]  = 0xFFF14C4C;  // Bright Red
    m_Colors[10] = 0xFF23D18B;  // Bright Green
    m_Colors[11] = 0xFFF5F543;  // Bright Yellow
    m_Colors[12] = 0xFF3B8EEA;  // Bright Blue
    m_Colors[13] = 0xFFD670D6;  // Bright Magenta
    m_Colors[14] = 0xFF29B8DB;  // Bright Cyan
    m_Colors[15] = 0xFFFFFFFF;  // Bright White
    
    // 216 color cube (6x6x6)
    for (int r = 0; r < 6; r++) {
        for (int g = 0; g < 6; g++) {
            for (int b = 0; b < 6; b++) {
                int index = 16 + r * 36 + g * 6 + b;
                int rVal = r ? (r * 40 + 55) : 0;
                int gVal = g ? (g * 40 + 55) : 0;
                int bVal = b ? (b * 40 + 55) : 0;
                m_Colors[index] = 0xFF000000 | (rVal << 16) | (gVal << 8) | bVal;
            }
        }
    }
    
    // 24 grayscale colors
    for (int i = 0; i < 24; i++) {
        int gray = i * 10 + 8;
        m_Colors[232 + i] = 0xFF000000 | (gray << 16) | (gray << 8) | gray;
    }
}

uint32_t TerminalPalette::GetColor(int index) const {
    if (index < 0 || index >= 256) return m_Colors[7];  // Default to white
    return m_Colors[index];
}

void TerminalPalette::SetColor(int index, uint32_t color) {
    if (index >= 0 && index < 256) {
        m_Colors[index] = color;
    }
}

// Terminal Emulator implementation
TerminalEmulator::TerminalEmulator(int rows, int cols)
    : m_Rows(rows), m_Cols(cols) {
    
    m_DefaultAttr.fg = m_Palette.GetColor(7);   // Default white
    m_DefaultAttr.bg = m_Palette.GetColor(0);   // Default black
    m_CurrentAttr = m_DefaultAttr;
    
    m_ScrollTop = 0;
    m_ScrollBottom = m_Rows - 1;
    
    // Initialize screen buffer
    m_Screen.resize(static_cast<size_t>(rows));
    for (auto& line : m_Screen) {
        line.resize(static_cast<size_t>(cols));
        for (auto& cell : line) {
            cell.attr = m_DefaultAttr;
        }
    }
    
    // Initialize empty line for bounds checking
    s_EmptyLine.resize(static_cast<size_t>(cols));
}

TerminalEmulator::~TerminalEmulator() = default;

void TerminalEmulator::AttachPty(std::shared_ptr<Pty> pty) {
    m_Pty = pty;
    if (m_Pty) {
        m_Pty->Resize(m_Rows, m_Cols);
    }
}

void TerminalEmulator::ProcessInput() {
    if (!m_Pty) return;
    
    char buffer[4096];
    
    // Time-based limiting: spend max 2ms processing per frame
    // This prevents UI freeze while still being responsive
    auto startTime = std::chrono::steady_clock::now();
    const auto maxDuration = std::chrono::microseconds(2000);
    
    int n;
    while ((n = m_Pty->Read(buffer, sizeof(buffer))) > 0) {
        for (int i = 0; i < n; i++) {
            ProcessByte(static_cast<uint8_t>(buffer[i]));
        }
        
        // Check if we've exceeded our time budget
        auto elapsed = std::chrono::steady_clock::now() - startTime;
        if (elapsed > maxDuration) {
            break;
        }
    }
}

void TerminalEmulator::Write(const std::string& data) {
    if (m_Pty) {
        m_Pty->Write(data);
    }
}

void TerminalEmulator::Write(char c) {
    if (m_Pty) {
        m_Pty->Write(&c, 1);
    }
}

void TerminalEmulator::HandleKey(int key, int mods) {
    // Convert key codes to escape sequences
    // This maps GLFW/ImGui key codes to terminal escape sequences
    
    std::string seq;
    bool shift = (mods & 1) != 0;
    bool ctrl = (mods & 2) != 0;
    bool alt = (mods & 4) != 0;
    
    // Arrow keys
    const int KEY_UP = 265;
    const int KEY_DOWN = 264;
    const int KEY_LEFT = 263;
    const int KEY_RIGHT = 262;
    const int KEY_HOME = 268;
    const int KEY_END = 269;
    const int KEY_PAGE_UP = 266;
    const int KEY_PAGE_DOWN = 267;
    const int KEY_INSERT = 260;
    const int KEY_DELETE = 261;
    const int KEY_BACKSPACE = 259;
    const int KEY_ENTER = 257;
    const int KEY_TAB = 258;
    const int KEY_ESCAPE = 256;
    
    switch (key) {
        case KEY_UP:
            seq = m_ApplicationCursor ? "\033OA" : "\033[A";
            break;
        case KEY_DOWN:
            seq = m_ApplicationCursor ? "\033OB" : "\033[B";
            break;
        case KEY_RIGHT:
            seq = m_ApplicationCursor ? "\033OC" : "\033[C";
            break;
        case KEY_LEFT:
            seq = m_ApplicationCursor ? "\033OD" : "\033[D";
            break;
        case KEY_HOME:
            seq = "\033[H";
            break;
        case KEY_END:
            seq = "\033[F";
            break;
        case KEY_PAGE_UP:
            seq = "\033[5~";
            break;
        case KEY_PAGE_DOWN:
            seq = "\033[6~";
            break;
        case KEY_INSERT:
            seq = "\033[2~";
            break;
        case KEY_DELETE:
            seq = "\033[3~";
            break;
        case KEY_BACKSPACE:
            seq = ctrl ? "\x7f" : "\x08";
            break;
        case KEY_ENTER:
            seq = "\r";
            break;
        case KEY_TAB:
            seq = shift ? "\033[Z" : "\t";
            break;
        case KEY_ESCAPE:
            seq = "\033";
            break;
        default:
            // F1-F12 keys (GLFW key codes 290-301)
            if (key >= 290 && key <= 301) {
                int f = key - 290 + 1;
                if (f <= 4) {
                    seq = "\033O" + std::string(1, static_cast<char>('P' + f - 1));
                } else if (f == 5) {
                    seq = "\033[15~";
                } else if (f == 6) {
                    seq = "\033[17~";
                } else if (f == 7) {
                    seq = "\033[18~";
                } else if (f == 8) {
                    seq = "\033[19~";
                } else if (f == 9) {
                    seq = "\033[20~";
                } else if (f == 10) {
                    seq = "\033[21~";
                } else if (f == 11) {
                    seq = "\033[23~";
                } else if (f == 12) {
                    seq = "\033[24~";
                }
            }
            return;
    }
    
    if (!seq.empty()) {
        Write(seq);
    }
}

void TerminalEmulator::HandleChar(char32_t c) {
    // Convert character to UTF-8 and send to PTY
    std::string utf8;
    
    if (c < 0x80) {
        utf8 = static_cast<char>(c);
    } else if (c < 0x800) {
        utf8 += static_cast<char>(0xC0 | (c >> 6));
        utf8 += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        utf8 += static_cast<char>(0xE0 | (c >> 12));
        utf8 += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        utf8 += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        utf8 += static_cast<char>(0xF0 | (c >> 18));
        utf8 += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        utf8 += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        utf8 += static_cast<char>(0x80 | (c & 0x3F));
    }
    
    Write(utf8);
}

void TerminalEmulator::Resize(int rows, int cols) {
    if (rows == m_Rows && cols == m_Cols) return;
    
    // Resize screen buffer
    m_Screen.resize(static_cast<size_t>(rows));
    for (auto& line : m_Screen) {
        line.resize(static_cast<size_t>(cols));
        for (auto& cell : line) {
            if (cell.codepoint == 0) {
                cell.codepoint = ' ';
                cell.attr = m_DefaultAttr;
                cell.dirty = true;
            }
        }
    }
    
    // Adjust cursor
    m_CursorRow = std::min(m_CursorRow, rows - 1);
    m_CursorCol = std::min(m_CursorCol, cols - 1);
    
    // Adjust scroll region
    m_ScrollTop = 0;
    m_ScrollBottom = rows - 1;
    
    m_Rows = rows;
    m_Cols = cols;
    
    // Resize empty line
    s_EmptyLine.resize(static_cast<size_t>(cols));
    
    // Notify PTY
    if (m_Pty) {
        m_Pty->Resize(rows, cols);
    }
    
    MarkDirty();
}

const TerminalLine& TerminalEmulator::GetLine(int row) const {
    if (row < 0 || row >= m_Rows) return s_EmptyLine;
    return m_Screen[static_cast<size_t>(row)];
}

const TerminalCell& TerminalEmulator::GetCell(int row, int col) const {
    if (row < 0 || row >= m_Rows || col < 0 || col >= m_Cols) return s_EmptyCell;
    return m_Screen[static_cast<size_t>(row)][static_cast<size_t>(col)];
}

const TerminalLine& TerminalEmulator::GetScrollbackLine(size_t index) const {
    if (index >= m_Scrollback.size()) return s_EmptyLine;
    return m_Scrollback[index];
}

void TerminalEmulator::SetSelection(int startRow, int startCol, int endRow, int endCol) {
    m_HasSelection = true;
    m_SelectionStartRow = startRow;
    m_SelectionStartCol = startCol;
    m_SelectionEndRow = endRow;
    m_SelectionEndCol = endCol;
    MarkDirty();
}

void TerminalEmulator::ClearSelection() {
    m_HasSelection = false;
    MarkDirty();
}

std::string TerminalEmulator::GetSelectedText() const {
    if (!m_HasSelection) return "";
    
    std::string result;
    int sr = m_SelectionStartRow, sc = m_SelectionStartCol;
    int er = m_SelectionEndRow, ec = m_SelectionEndCol;
    
    if (sr > er || (sr == er && sc > ec)) {
        std::swap(sr, er);
        std::swap(sc, ec);
    }
    
    for (int row = sr; row <= er; row++) {
        int colStart = (row == sr) ? sc : 0;
        int colEnd = (row == er) ? ec : m_Cols - 1;
        
        const auto& line = GetLine(row);
        for (int col = colStart; col <= colEnd && col < static_cast<int>(line.size()); col++) {
            char32_t c = line[static_cast<size_t>(col)].codepoint;
            if (c < 0x80) {
                result += static_cast<char>(c);
            }
            // TODO: Handle UTF-8 encoding for non-ASCII
        }
        if (row < er) result += '\n';
    }
    
    return result;
}

void TerminalEmulator::MarkDirty() {
    for (auto& line : m_Screen) {
        for (auto& cell : line) {
            cell.dirty = true;
        }
    }
}

void TerminalEmulator::ClearDirty() {
    for (auto& line : m_Screen) {
        for (auto& cell : line) {
            cell.dirty = false;
        }
    }
}

bool TerminalEmulator::IsAlive() const {
    return m_Pty && m_Pty->IsAlive();
}

int TerminalEmulator::GetExitCode() const {
    return m_Pty ? m_Pty->GetExitCode() : 0;
}

// Escape sequence processing
void TerminalEmulator::ProcessByte(uint8_t byte) {
    // UTF-8 continuation
    if (m_UTF8Remaining > 0) {
        if ((byte & 0xC0) == 0x80) {
            m_UTF8Char = (m_UTF8Char << 6) | (byte & 0x3F);
            m_UTF8Remaining--;
            if (m_UTF8Remaining == 0) {
                ProcessNormalChar(m_UTF8Char);
            }
            return;
        } else {
            // Invalid UTF-8, reset
            m_UTF8Remaining = 0;
        }
    }
    
    switch (m_ParseState) {
        case ParseState::Normal:
            if (byte == 0x1B) {  // ESC
                m_ParseState = ParseState::Escape;
            } else if (byte < 0x20) {
                ProcessControlChar(byte);
            } else if (byte < 0x80) {
                ProcessNormalChar(static_cast<char32_t>(byte));
            } else {
                // UTF-8 start byte
                if ((byte & 0xE0) == 0xC0) {
                    m_UTF8Char = byte & 0x1F;
                    m_UTF8Remaining = 1;
                } else if ((byte & 0xF0) == 0xE0) {
                    m_UTF8Char = byte & 0x0F;
                    m_UTF8Remaining = 2;
                } else if ((byte & 0xF8) == 0xF0) {
                    m_UTF8Char = byte & 0x07;
                    m_UTF8Remaining = 3;
                }
            }
            break;
            
        case ParseState::Escape:
            ProcessEscapeSequence(byte);
            break;
            
        case ParseState::CSI:
            ProcessCSI(byte);
            break;
            
        case ParseState::OSC:
            ProcessOSC(byte);
            break;
            
        case ParseState::CharsetG0:
        case ParseState::CharsetG1:
            // Ignore charset selection for now
            m_ParseState = ParseState::Normal;
            break;
            
        default:
            m_ParseState = ParseState::Normal;
            break;
    }
}

void TerminalEmulator::ProcessNormalChar(char32_t c) {
    PutChar(c);
}

void TerminalEmulator::ProcessControlChar(uint8_t c) {
    switch (c) {
        case 0x00: break;  // NUL - ignore
        case 0x07: Bell(); break;
        case 0x08: Backspace(); break;
        case 0x09: Tab(); break;
        case 0x0A: // LF
        case 0x0B: // VT
        case 0x0C: // FF
            Newline();
            break;
        case 0x0D: CarriageReturn(); break;
        default: break;
    }
}

void TerminalEmulator::ProcessEscapeSequence(uint8_t c) {
    switch (c) {
        case '[':  // CSI
            m_ParseState = ParseState::CSI;
            m_CSIParams.clear();
            m_CSIIntermediate.clear();
            m_CurrentParam = 0;
            m_HasParam = false;
            break;
            
        case ']':  // OSC
            m_ParseState = ParseState::OSC;
            m_OSCString.clear();
            break;
            
        case '(':  // Charset G0
            m_ParseState = ParseState::CharsetG0;
            break;
            
        case ')':  // Charset G1
            m_ParseState = ParseState::CharsetG1;
            break;
            
        case '7':  // DECSC - Save cursor
            SaveCursor();
            m_ParseState = ParseState::Normal;
            break;
            
        case '8':  // DECRC - Restore cursor
            RestoreCursor();
            m_ParseState = ParseState::Normal;
            break;
            
        case 'c':  // RIS - Reset
            Reset();
            m_ParseState = ParseState::Normal;
            break;
            
        case 'D':  // IND - Index (cursor down)
            MoveCursorDown();
            m_ParseState = ParseState::Normal;
            break;
            
        case 'E':  // NEL - Next line
            CarriageReturn();
            Newline();
            m_ParseState = ParseState::Normal;
            break;
            
        case 'M':  // RI - Reverse index (cursor up)
            if (m_CursorRow == m_ScrollTop) {
                ScrollDown();
            } else {
                MoveCursorUp();
            }
            m_ParseState = ParseState::Normal;
            break;
            
        default:
            m_ParseState = ParseState::Normal;
            break;
    }
}

void TerminalEmulator::ProcessCSI(uint8_t c) {
    if (c >= '0' && c <= '9') {
        m_CurrentParam = m_CurrentParam * 10 + (c - '0');
        m_HasParam = true;
    } else if (c == ';') {
        m_CSIParams.push_back(m_HasParam ? m_CurrentParam : 0);
        m_CurrentParam = 0;
        m_HasParam = false;
    } else if (c >= 0x20 && c < 0x40) {
        // Intermediate bytes (includes '?' for DEC private modes)
        m_CSIIntermediate += static_cast<char>(c);
    } else if (c >= 0x40) {
        // Final byte - execute the sequence
        if (m_HasParam || !m_CSIParams.empty()) {
            m_CSIParams.push_back(m_CurrentParam);
        }
        ExecuteCSI(static_cast<char>(c));
        m_ParseState = ParseState::Normal;
    }
}

void TerminalEmulator::ProcessOSC(uint8_t c) {
    if (c == 0x07 || c == 0x9C) {  // BEL or ST
        ExecuteOSC();
        m_ParseState = ParseState::Normal;
    } else if (c == 0x1B) {
        // Might be ESC \ (ST)
        // For simplicity, just execute OSC
        ExecuteOSC();
        m_ParseState = ParseState::Escape;
    } else {
        m_OSCString += static_cast<char>(c);
    }
}

void TerminalEmulator::ExecuteCSI(char finalByte) {
    auto getParam = [this](size_t index, int defaultVal = 0) -> int {
        if (index < m_CSIParams.size()) {
            return m_CSIParams[index] ? m_CSIParams[index] : defaultVal;
        }
        return defaultVal;
    };
    
    int param0 = getParam(0, 1);
    int param1 = getParam(1, 1);
    bool isPrivate = !m_CSIIntermediate.empty() && m_CSIIntermediate[0] == '?';
    
    // Handle DEC private modes
    if (isPrivate) {
        int mode = getParam(0, 0);
        switch (finalByte) {
            case 'h':  // Set mode
                switch (mode) {
                    case 1: m_ApplicationCursor = true; break;
                    case 12: break;  // Start blinking cursor (ignore)
                    case 25: SetCursorVisibility(true); break;
                    case 1049:  // Save cursor and switch to alternate screen buffer
                        SaveCursor();
                        m_AlternateScreen = true;
                        EraseInDisplay(2);
                        break;
                    case 2004: break;  // Enable bracketed paste mode (ignore for now)
                }
                break;
            case 'l':  // Reset mode
                switch (mode) {
                    case 1: m_ApplicationCursor = false; break;
                    case 12: break;  // Stop blinking cursor (ignore)
                    case 25: SetCursorVisibility(false); break;
                    case 1049:  // Restore cursor and switch back from alternate screen
                        m_AlternateScreen = false;
                        RestoreCursor();
                        break;
                    case 2004: break;  // Disable bracketed paste mode (ignore)
                }
                break;
        }
        return;
    }
    
    // Standard CSI sequences
    switch (finalByte) {
        case 'A':  // CUU - Cursor Up
            MoveCursorUp(param0);
            break;
            
        case 'B':  // CUD - Cursor Down
            MoveCursorDown(param0);
            break;
            
        case 'C':  // CUF - Cursor Forward
            MoveCursorRight(param0);
            break;
            
        case 'D':  // CUB - Cursor Back
            MoveCursorLeft(param0);
            break;
            
        case 'E':  // CNL - Cursor Next Line
            MoveCursorDown(param0);
            m_CursorCol = 0;
            break;
            
        case 'F':  // CPL - Cursor Previous Line
            MoveCursorUp(param0);
            m_CursorCol = 0;
            break;
            
        case 'G':  // CHA - Cursor Horizontal Absolute
            m_CursorCol = std::clamp(param0 - 1, 0, m_Cols - 1);
            break;
            
        case 'H':  // CUP - Cursor Position
        case 'f':  // HVP - Horizontal and Vertical Position
            MoveCursor(getParam(0, 1) - 1, getParam(1, 1) - 1);
            break;
            
        case 'J':  // ED - Erase in Display
            EraseInDisplay(getParam(0, 0));
            break;
            
        case 'K':  // EL - Erase in Line
            EraseInLine(getParam(0, 0));
            break;
            
        case 'L':  // IL - Insert Lines
            InsertLines(param0);
            break;
            
        case 'M':  // DL - Delete Lines
            DeleteLines(param0);
            break;
            
        case 'P':  // DCH - Delete Characters
            DeleteChars(param0);
            break;
            
        case 'S':  // SU - Scroll Up
            ScrollUp(param0);
            break;
            
        case 'T':  // SD - Scroll Down
            ScrollDown(param0);
            break;
            
        case 'X':  // ECH - Erase Characters
            for (int i = 0; i < param0 && m_CursorCol + i < m_Cols; i++) {
                auto& cell = m_Screen[static_cast<size_t>(m_CursorRow)][static_cast<size_t>(m_CursorCol + i)];
                cell.codepoint = ' ';
                cell.attr = m_CurrentAttr;
                cell.dirty = true;
            }
            break;
            
        case '@':  // ICH - Insert Characters
            InsertChars(param0);
            break;
            
        case 'd':  // VPA - Vertical Position Absolute
            m_CursorRow = std::clamp(param0 - 1, 0, m_Rows - 1);
            break;
            
        case 'h':  // SM - Set Mode
            // Standard modes (not DEC private)
            if (getParam(0, 0) == 4) m_InsertMode = true;
            break;
            
        case 'l':  // RM - Reset Mode
            if (getParam(0, 0) == 4) m_InsertMode = false;
            break;
            
        case 'm':  // SGR - Select Graphic Rendition
            SetGraphicsRendition();
            break;
            
        case 'n':  // DSR - Device Status Report
            if (param0 == 6) {
                // Report cursor position
                std::string response = "\033[" + std::to_string(m_CursorRow + 1) + ";" + std::to_string(m_CursorCol + 1) + "R";
                Write(response);
            }
            break;
            
        case 'r':  // DECSTBM - Set Top and Bottom Margins
            SetScrollRegion(getParam(0, 1) - 1, getParam(1, m_Rows) - 1);
            break;
            
        case 's':  // SCP - Save Cursor Position
            SaveCursor();
            break;
            
        case 'u':  // RCP - Restore Cursor Position
            RestoreCursor();
            break;
            
        case 't':  // Window manipulation (mostly ignored)
            break;
            
        case 'c':  // DA - Device Attributes
            // Report as VT100 with no options
            Write("\033[?1;0c");
            break;
    }
}

void TerminalEmulator::ExecuteOSC() {
    // Parse OSC command
    size_t semicolon = m_OSCString.find(';');
    if (semicolon == std::string::npos) return;
    
    int cmd = 0;
    try {
        cmd = std::stoi(m_OSCString.substr(0, semicolon));
    } catch (...) {
        return;
    }
    
    std::string arg = m_OSCString.substr(semicolon + 1);
    
    switch (cmd) {
        case 0:  // Set icon name and window title
        case 1:  // Set icon name
        case 2:  // Set window title
            m_Title = arg;
            break;
    }
}

void TerminalEmulator::PutChar(char32_t c) {
    if (m_CursorCol >= m_Cols) {
        if (m_AutoWrap) {
            CarriageReturn();
            Newline();
        } else {
            m_CursorCol = m_Cols - 1;
        }
    }
    
    auto& cell = m_Screen[static_cast<size_t>(m_CursorRow)][static_cast<size_t>(m_CursorCol)];
    cell.codepoint = c;
    cell.attr = m_CurrentAttr;
    cell.dirty = true;
    
    m_CursorCol++;
}

void TerminalEmulator::Newline() {
    if (m_CursorRow == m_ScrollBottom) {
        ScrollUp();
    } else if (m_CursorRow < m_Rows - 1) {
        m_CursorRow++;
    }
}

void TerminalEmulator::CarriageReturn() {
    m_CursorCol = 0;
}

void TerminalEmulator::Tab() {
    int nextTab = ((m_CursorCol / 8) + 1) * 8;
    m_CursorCol = std::min(nextTab, m_Cols - 1);
}

void TerminalEmulator::Backspace() {
    if (m_CursorCol > 0) {
        m_CursorCol--;
    }
}

void TerminalEmulator::Delete() {
    // DEL character - typically ignored
}

void TerminalEmulator::Bell() {
    // Could trigger a visual bell or sound
}

void TerminalEmulator::MoveCursor(int row, int col) {
    m_CursorRow = std::clamp(row, 0, m_Rows - 1);
    m_CursorCol = std::clamp(col, 0, m_Cols - 1);
}

void TerminalEmulator::MoveCursorUp(int n) {
    m_CursorRow = std::max(m_CursorRow - n, m_ScrollTop);
}

void TerminalEmulator::MoveCursorDown(int n) {
    m_CursorRow = std::min(m_CursorRow + n, m_ScrollBottom);
}

void TerminalEmulator::MoveCursorLeft(int n) {
    m_CursorCol = std::max(m_CursorCol - n, 0);
}

void TerminalEmulator::MoveCursorRight(int n) {
    m_CursorCol = std::min(m_CursorCol + n, m_Cols - 1);
}

void TerminalEmulator::ScrollUp(int n) {
    for (int i = 0; i < n; i++) {
        // Move top line to scrollback
        if (m_ScrollTop == 0) {
            m_Scrollback.push_back(std::move(m_Screen[static_cast<size_t>(m_ScrollTop)]));
            if (m_Scrollback.size() > MaxScrollback) {
                m_Scrollback.pop_front();
            }
        }
        
        // Scroll region up
        for (int row = m_ScrollTop; row < m_ScrollBottom; row++) {
            m_Screen[static_cast<size_t>(row)] = std::move(m_Screen[static_cast<size_t>(row + 1)]);
        }
        
        // Clear bottom line
        m_Screen[static_cast<size_t>(m_ScrollBottom)].clear();
        m_Screen[static_cast<size_t>(m_ScrollBottom)].resize(static_cast<size_t>(m_Cols));
        for (auto& cell : m_Screen[static_cast<size_t>(m_ScrollBottom)]) {
            cell.attr = m_DefaultAttr;
            cell.dirty = true;
        }
    }
    
    MarkDirty();
}

void TerminalEmulator::ScrollDown(int n) {
    for (int i = 0; i < n; i++) {
        // Scroll region down
        for (int row = m_ScrollBottom; row > m_ScrollTop; row--) {
            m_Screen[static_cast<size_t>(row)] = std::move(m_Screen[static_cast<size_t>(row - 1)]);
        }
        
        // Clear top line
        m_Screen[static_cast<size_t>(m_ScrollTop)].clear();
        m_Screen[static_cast<size_t>(m_ScrollTop)].resize(static_cast<size_t>(m_Cols));
        for (auto& cell : m_Screen[static_cast<size_t>(m_ScrollTop)]) {
            cell.attr = m_DefaultAttr;
            cell.dirty = true;
        }
    }
    
    MarkDirty();
}

void TerminalEmulator::EraseInDisplay(int mode) {
    switch (mode) {
        case 0:  // Erase from cursor to end of display
            EraseInLine(0);
            for (int row = m_CursorRow + 1; row < m_Rows; row++) {
                for (auto& cell : m_Screen[static_cast<size_t>(row)]) {
                    cell.codepoint = ' ';
                    cell.attr = m_CurrentAttr;
                    cell.dirty = true;
                }
            }
            break;
            
        case 1:  // Erase from start of display to cursor
            for (int row = 0; row < m_CursorRow; row++) {
                for (auto& cell : m_Screen[static_cast<size_t>(row)]) {
                    cell.codepoint = ' ';
                    cell.attr = m_CurrentAttr;
                    cell.dirty = true;
                }
            }
            EraseInLine(1);
            break;
            
        case 2:  // Erase entire display
        case 3:  // Erase entire display with scrollback
            for (auto& line : m_Screen) {
                for (auto& cell : line) {
                    cell.codepoint = ' ';
                    cell.attr = m_CurrentAttr;
                    cell.dirty = true;
                }
            }
            if (mode == 3) {
                m_Scrollback.clear();
            }
            break;
    }
}

void TerminalEmulator::EraseInLine(int mode) {
    auto& line = m_Screen[static_cast<size_t>(m_CursorRow)];
    
    int start = 0, end = m_Cols;
    switch (mode) {
        case 0: start = m_CursorCol; break;  // From cursor to end
        case 1: end = m_CursorCol + 1; break;  // From start to cursor
        case 2: break;  // Entire line
    }
    
    for (int col = start; col < end; col++) {
        auto& cell = line[static_cast<size_t>(col)];
        cell.codepoint = ' ';
        cell.attr = m_CurrentAttr;
        cell.dirty = true;
    }
}

void TerminalEmulator::DeleteChars(int n) {
    auto& line = m_Screen[static_cast<size_t>(m_CursorRow)];
    
    for (int col = m_CursorCol; col < m_Cols - n; col++) {
        line[static_cast<size_t>(col)] = line[static_cast<size_t>(col + n)];
        line[static_cast<size_t>(col)].dirty = true;
    }
    
    for (int col = m_Cols - n; col < m_Cols; col++) {
        auto& cell = line[static_cast<size_t>(col)];
        cell.codepoint = ' ';
        cell.attr = m_CurrentAttr;
        cell.dirty = true;
    }
}

void TerminalEmulator::InsertChars(int n) {
    auto& line = m_Screen[static_cast<size_t>(m_CursorRow)];
    
    for (int col = m_Cols - 1; col >= m_CursorCol + n; col--) {
        line[static_cast<size_t>(col)] = line[static_cast<size_t>(col - n)];
        line[static_cast<size_t>(col)].dirty = true;
    }
    
    for (int col = m_CursorCol; col < m_CursorCol + n && col < m_Cols; col++) {
        auto& cell = line[static_cast<size_t>(col)];
        cell.codepoint = ' ';
        cell.attr = m_CurrentAttr;
        cell.dirty = true;
    }
}

void TerminalEmulator::DeleteLines(int n) {
    for (int i = 0; i < n; i++) {
        for (int row = m_CursorRow; row < m_ScrollBottom; row++) {
            m_Screen[static_cast<size_t>(row)] = std::move(m_Screen[static_cast<size_t>(row + 1)]);
        }
        
        m_Screen[static_cast<size_t>(m_ScrollBottom)].clear();
        m_Screen[static_cast<size_t>(m_ScrollBottom)].resize(static_cast<size_t>(m_Cols));
        for (auto& cell : m_Screen[static_cast<size_t>(m_ScrollBottom)]) {
            cell.attr = m_DefaultAttr;
            cell.dirty = true;
        }
    }
    MarkDirty();
}

void TerminalEmulator::InsertLines(int n) {
    for (int i = 0; i < n; i++) {
        for (int row = m_ScrollBottom; row > m_CursorRow; row--) {
            m_Screen[static_cast<size_t>(row)] = std::move(m_Screen[static_cast<size_t>(row - 1)]);
        }
        
        m_Screen[static_cast<size_t>(m_CursorRow)].clear();
        m_Screen[static_cast<size_t>(m_CursorRow)].resize(static_cast<size_t>(m_Cols));
        for (auto& cell : m_Screen[static_cast<size_t>(m_CursorRow)]) {
            cell.attr = m_DefaultAttr;
            cell.dirty = true;
        }
    }
    MarkDirty();
}

void TerminalEmulator::SetScrollRegion(int top, int bottom) {
    m_ScrollTop = std::clamp(top, 0, m_Rows - 1);
    m_ScrollBottom = std::clamp(bottom, m_ScrollTop, m_Rows - 1);
    MoveCursor(m_OriginMode ? m_ScrollTop : 0, 0);
}

void TerminalEmulator::ResetScrollRegion() {
    m_ScrollTop = 0;
    m_ScrollBottom = m_Rows - 1;
}

void TerminalEmulator::SetGraphicsRendition() {
    for (size_t i = 0; i < m_CSIParams.size(); i++) {
        int param = m_CSIParams[i];
        
        switch (param) {
            case 0:  // Reset
                m_CurrentAttr = m_DefaultAttr;
                break;
            case 1:  // Bold
                m_CurrentAttr.bold = true;
                break;
            case 2:  // Dim
                m_CurrentAttr.dim = true;
                break;
            case 3:  // Italic
                m_CurrentAttr.italic = true;
                break;
            case 4:  // Underline
                m_CurrentAttr.underline = true;
                break;
            case 7:  // Inverse
                m_CurrentAttr.inverse = true;
                break;
            case 9:  // Strikethrough
                m_CurrentAttr.strikethrough = true;
                break;
            case 22:  // Normal intensity
                m_CurrentAttr.bold = false;
                m_CurrentAttr.dim = false;
                break;
            case 23:  // Not italic
                m_CurrentAttr.italic = false;
                break;
            case 24:  // Not underlined
                m_CurrentAttr.underline = false;
                break;
            case 27:  // Not inverse
                m_CurrentAttr.inverse = false;
                break;
            case 29:  // Not strikethrough
                m_CurrentAttr.strikethrough = false;
                break;
                
            // Foreground colors (30-37)
            case 30: case 31: case 32: case 33:
            case 34: case 35: case 36: case 37:
                m_CurrentAttr.fg = m_Palette.GetColor(param - 30);
                break;
                
            // Default foreground
            case 39:
                m_CurrentAttr.fg = m_DefaultAttr.fg;
                break;
                
            // Background colors (40-47)
            case 40: case 41: case 42: case 43:
            case 44: case 45: case 46: case 47:
                m_CurrentAttr.bg = m_Palette.GetColor(param - 40);
                break;
                
            // Default background
            case 49:
                m_CurrentAttr.bg = m_DefaultAttr.bg;
                break;
                
            // Bright foreground (90-97)
            case 90: case 91: case 92: case 93:
            case 94: case 95: case 96: case 97:
                m_CurrentAttr.fg = m_Palette.GetColor(param - 90 + 8);
                break;
                
            // Bright background (100-107)
            case 100: case 101: case 102: case 103:
            case 104: case 105: case 106: case 107:
                m_CurrentAttr.bg = m_Palette.GetColor(param - 100 + 8);
                break;
                
            // Extended colors (38, 48)
            case 38:  // Foreground
            case 48:  // Background
                if (i + 1 < m_CSIParams.size()) {
                    int mode = m_CSIParams[++i];
                    uint32_t color = m_DefaultAttr.fg;
                    
                    if (mode == 5 && i + 1 < m_CSIParams.size()) {
                        // 256 color mode
                        int index = m_CSIParams[++i];
                        color = m_Palette.GetColor(index);
                    } else if (mode == 2 && i + 3 < m_CSIParams.size()) {
                        // True color mode
                        int r = m_CSIParams[++i];
                        int g = m_CSIParams[++i];
                        int b = m_CSIParams[++i];
                        color = 0xFF000000 | (r << 16) | (g << 8) | b;
                    }
                    
                    if (param == 38) {
                        m_CurrentAttr.fg = color;
                    } else {
                        m_CurrentAttr.bg = color;
                    }
                }
                break;
        }
    }
}

void TerminalEmulator::SetCursorVisibility(bool visible) {
    m_CursorVisible = visible;
}

void TerminalEmulator::SaveCursor() {
    m_SavedCursorRow = m_CursorRow;
    m_SavedCursorCol = m_CursorCol;
    m_SavedAttr = m_CurrentAttr;
}

void TerminalEmulator::RestoreCursor() {
    m_CursorRow = m_SavedCursorRow;
    m_CursorCol = m_SavedCursorCol;
    m_CurrentAttr = m_SavedAttr;
}

void TerminalEmulator::Reset() {
    m_CursorRow = 0;
    m_CursorCol = 0;
    m_CursorVisible = true;
    m_CurrentAttr = m_DefaultAttr;
    m_ScrollTop = 0;
    m_ScrollBottom = m_Rows - 1;
    m_AutoWrap = true;
    m_OriginMode = false;
    m_InsertMode = false;
    m_ApplicationCursor = false;
    m_ApplicationKeypad = false;
    m_Title.clear();
    
    for (auto& line : m_Screen) {
        for (auto& cell : line) {
            cell.codepoint = ' ';
            cell.attr = m_DefaultAttr;
            cell.dirty = true;
        }
    }
}

uint32_t TerminalEmulator::AnsiToColor(int ansi, bool bright) {
    int index = ansi + (bright ? 8 : 0);
    return m_Palette.GetColor(index);
}

} // namespace sol
