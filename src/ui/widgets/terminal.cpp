#include "terminal.h"
#include "ui/input/command.h"
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <unistd.h>

namespace sol {

TerminalWidget::TerminalWidget() = default;

TerminalWidget::~TerminalWidget() {
    Close();
}

bool TerminalWidget::Spawn(const TerminalConfig& config) {
    Close();
    
    m_Pty = std::make_shared<Pty>();
    
    std::string workingDir = config.workingDir;
    if (workingDir.empty()) {
        char cwd[1024];
        if (getcwd(cwd, sizeof(cwd))) {
            workingDir = cwd;
        }
    }
    
    if (!m_Pty->Spawn(config.shell, config.defaultRows, config.defaultCols, workingDir)) {
        m_Pty.reset();
        return false;
    }
    
    m_Emulator = std::make_unique<TerminalEmulator>(config.defaultRows, config.defaultCols);
    m_Emulator->AttachPty(m_Pty);
    
    m_VisibleRows = config.defaultRows;
    m_VisibleCols = config.defaultCols;
    m_ShowScrollbar = config.showScrollbar;
    m_ScrollOffset = 0;
    
    return true;
}

void TerminalWidget::Close() {
    if (m_Emulator) {
        m_Emulator.reset();
    }
    if (m_Pty) {
        m_Pty->Close();
        m_Pty.reset();
    }
}

bool TerminalWidget::IsAlive() const {
    return m_Emulator && m_Emulator->IsAlive();
}

int TerminalWidget::GetExitCode() const {
    return m_Emulator ? m_Emulator->GetExitCode() : 0;
}

const std::string& TerminalWidget::GetTitle() const {
    if (m_Emulator && !m_Emulator->GetTitle().empty()) {
        return m_Emulator->GetTitle();
    }
    return m_DefaultTitle;
}

void TerminalWidget::ProcessInput() {
    if (m_Emulator) {
        m_Emulator->ProcessInput();
    }
}

void TerminalWidget::CalculateDimensions(const ImVec2& size) {
    // Calculate character dimensions using the current font
    ImFont* font = ImGui::GetFont();
    m_CharWidth = ImGui::CalcTextSize("M").x;
    m_CharHeight = ImGui::GetTextLineHeight();
    
    // Account for scrollbar width
    float scrollbarWidth = m_ShowScrollbar ? ImGui::GetStyle().ScrollbarSize : 0.0f;
    float contentWidth = size.x - scrollbarWidth;
    
    // Calculate visible rows and columns
    int newCols = std::max(1, static_cast<int>(contentWidth / m_CharWidth));
    int newRows = std::max(1, static_cast<int>(size.y / m_CharHeight));
    
    // Resize if dimensions changed
    if (m_Emulator && (newRows != m_VisibleRows || newCols != m_VisibleCols)) {
        m_VisibleRows = newRows;
        m_VisibleCols = newCols;
        m_Emulator->Resize(newRows, newCols);
    }
}

bool TerminalWidget::Render(const char* label, const ImVec2& size) {
    if (!m_Emulator) return false;
    
    // Process PTY input
    m_Emulator->ProcessInput();
    
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;
    
    const ImGuiID id = window->GetID(label);
    
    // Calculate actual size
    ImVec2 contentSize = size;
    if (contentSize.x <= 0.0f) contentSize.x = ImGui::GetContentRegionAvail().x;
    if (contentSize.y <= 0.0f) contentSize.y = ImGui::GetContentRegionAvail().y;
    
    // Calculate character dimensions
    CalculateDimensions(contentSize);
    
    // Terminal background color from emulator's default attributes
    const auto& palette = m_Emulator->GetPalette();
    ImU32 bgColor = palette.GetColor(0);  // Black background
    
    // Begin child region with NoNav to prevent Tab from navigating UI elements
    ImGui::PushStyleColor(ImGuiCol_ChildBg, bgColor);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(2, 2));
    
    ImGuiWindowFlags childFlags = ImGuiWindowFlags_NoScrollbar | 
                                   ImGuiWindowFlags_NoScrollWithMouse | 
                                   ImGuiWindowFlags_NoNavInputs |  // Prevent Tab navigation pollution
                                   ImGuiWindowFlags_NoNavFocus;    // Don't participate in nav focus
    
    bool wasOpen = ImGui::BeginChild(id, contentSize, false, childFlags);
    
    if (wasOpen) {
        // Handle focus capture request
        if (m_WantsFocusCapture) {
            ImGui::SetWindowFocus();
            // Also make the parent window focused
            if (ImGui::GetCurrentWindow()->ParentWindow) {
                ImGui::FocusWindow(ImGui::GetCurrentWindow()->ParentWindow);
            }
            m_WantsFocusCapture = false;
        }
        
        m_IsFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        
        ImVec2 contentPos = ImGui::GetCursorScreenPos();
        ImVec2 contentAvail = ImGui::GetContentRegionAvail();
        
        // Handle keyboard input when focused
        if (m_IsFocused) {
            HandleInput();
        }
        
        // Render terminal content
        RenderContent(contentPos, contentAvail);
        
        // Handle mouse input
        ImVec2 mousePos = ImGui::GetMousePos();
        bool hovered = ImGui::IsWindowHovered();
        
        if (hovered && ImGui::IsMouseClicked(0)) {
            ImGui::SetWindowFocus();
            // Click resets scroll to bottom
            m_ScrollOffset = 0;
        }
        
        // Handle mouse wheel scrolling (same direction as text editor)
        if (hovered) {
            float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f) {
                int scrollLines = static_cast<int>(wheel * 3);  // 3 lines per notch
                int maxScroll = static_cast<int>(m_Emulator->GetScrollbackSize());
                
                // Positive wheel = scroll up = see older content = increase offset
                m_ScrollOffset += scrollLines;
                m_ScrollOffset = std::max(0, std::min(m_ScrollOffset, maxScroll));
            }
        }
        
        // Render scrollbar if there's scrollback
        if (m_ShowScrollbar) {
            RenderScrollbar(contentPos, contentAvail);
        }
        
        // Update cursor blink
        m_CursorBlinkTime += ImGui::GetIO().DeltaTime;
        if (m_CursorBlinkTime >= 0.5f) {
            m_CursorBlinkTime = 0.0f;
            m_CursorVisible = !m_CursorVisible;
        }
    }
    
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    
    return m_Emulator->IsAlive();
}

void TerminalWidget::HandleInput() {
    ImGuiIO& io = ImGui::GetIO();
    
    // Block all input in Command mode - terminal behaves like any other buffer
    if (InputSystem::GetInstance().GetInputMode() != EditorInputMode::Insert) {
        io.InputQueueCharacters.resize(0);  // Clear input queue
        return;
    }
    
    // Handle text input
    if (io.InputQueueCharacters.Size > 0) {
        for (int i = 0; i < io.InputQueueCharacters.Size; i++) {
            ImWchar c = io.InputQueueCharacters[i];
            
            // Filter out control characters that we handle separately
            if (c >= 32 || c == '\t' || c == '\r' || c == '\n') {
                m_Emulator->HandleChar(static_cast<char32_t>(c));
            }
        }
        io.InputQueueCharacters.resize(0);
        
        // Reset cursor blink on input and scroll to bottom
        m_CursorBlinkTime = 0.0f;
        m_CursorVisible = true;
        m_ScrollOffset = 0;
    }
    
    // Handle special keys
    auto checkKey = [&](int imguiKey, int keyCode, bool needsModifier = false) -> bool {
        if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(imguiKey))) {
            int mods = 0;
            if (io.KeyShift) mods |= 1;
            if (io.KeyCtrl) mods |= 2;
            if (io.KeyAlt) mods |= 4;
            m_Emulator->HandleKey(keyCode, mods);
            m_CursorBlinkTime = 0.0f;
            m_CursorVisible = true;
            m_ScrollOffset = 0;  // Reset scroll on key input
            return true;
        }
        return false;
    };
    
    // Arrow keys
    checkKey(ImGuiKey_UpArrow, 265);
    checkKey(ImGuiKey_DownArrow, 264);
    checkKey(ImGuiKey_LeftArrow, 263);
    checkKey(ImGuiKey_RightArrow, 262);
    
    // Navigation keys
    checkKey(ImGuiKey_Home, 268);
    checkKey(ImGuiKey_End, 269);
    checkKey(ImGuiKey_PageUp, 266);
    checkKey(ImGuiKey_PageDown, 267);
    checkKey(ImGuiKey_Insert, 260);
    checkKey(ImGuiKey_Delete, 261);
    
    // Control keys
    checkKey(ImGuiKey_Backspace, 259);
    checkKey(ImGuiKey_Enter, 257);
    checkKey(ImGuiKey_Tab, 258);
    checkKey(ImGuiKey_Escape, 256);
    
    // Function keys
    for (int i = 0; i < 12; i++) {
        checkKey(ImGuiKey_F1 + i, 290 + i);
    }
    
    // Ctrl+C for SIGINT
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C)) {
        m_Emulator->Write("\x03");  // ETX
    }
    
    // Ctrl+Z for SIGTSTP
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z)) {
        m_Emulator->Write("\x1a");  // SUB
    }
    
    // Ctrl+D for EOF
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D)) {
        m_Emulator->Write("\x04");  // EOT
    }
    
    // Ctrl+L for clear screen
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_L)) {
        m_Emulator->Write("\x0c");  // FF
    }
}

void TerminalWidget::RenderContent(const ImVec2& pos, const ImVec2& size) {
    if (!m_Emulator) return;
    
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    
    int rows = m_Emulator->GetRows();
    int cols = m_Emulator->GetCols();
    int scrollbackSize = static_cast<int>(m_Emulator->GetScrollbackSize());
    
    // When new output arrives, auto-scroll to bottom if already at bottom
    if (m_ScrollOffset == 0) {
        // Already at bottom, will show latest
    }
    
    // Render each visible row
    for (int visibleRow = 0; visibleRow < rows; visibleRow++) {
        const TerminalLine* line = nullptr;
        
        // Calculate which line to render based on scroll offset
        // scrollOffset = 0 means show current screen
        // scrollOffset > 0 means we're scrolled up into scrollback
        int lineIndex = visibleRow - m_ScrollOffset;
        
        if (lineIndex < 0) {
            // This row is in the scrollback buffer
            int scrollbackIndex = scrollbackSize + lineIndex;
            if (scrollbackIndex >= 0 && scrollbackIndex < scrollbackSize) {
                line = &m_Emulator->GetScrollbackLine(static_cast<size_t>(scrollbackIndex));
            }
        } else if (lineIndex < rows) {
            // This row is in the current screen
            line = &m_Emulator->GetLine(lineIndex);
        }
        
        if (!line) continue;
        
        for (int col = 0; col < cols && col < static_cast<int>(line->size()); col++) {
            const auto& cell = (*line)[static_cast<size_t>(col)];
            
            float x = pos.x + col * m_CharWidth;
            float y = pos.y + visibleRow * m_CharHeight;
            
            // Determine colors (handle inverse)
            uint32_t fg = cell.attr.fg;
            uint32_t bg = cell.attr.bg;
            
            if (cell.attr.inverse) {
                std::swap(fg, bg);
            }
            
            if (cell.attr.dim) {
                // Reduce brightness
                int r = (fg >> 16) & 0xFF;
                int g = (fg >> 8) & 0xFF;
                int b = fg & 0xFF;
                r = r * 2 / 3;
                g = g * 2 / 3;
                b = b * 2 / 3;
                fg = (fg & 0xFF000000) | (r << 16) | (g << 8) | b;
            }
            
            // Draw background if not default
            ImU32 defaultBg = m_Emulator->GetPalette().GetColor(0);
            if (bg != defaultBg) {
                drawList->AddRectFilled(
                    ImVec2(x, y),
                    ImVec2(x + m_CharWidth, y + m_CharHeight),
                    bg
                );
            }
            
            // Draw character
            if (cell.codepoint > 32) {
                char utf8[5] = {0};
                char32_t cp = cell.codepoint;
                
                if (cp < 0x80) {
                    utf8[0] = static_cast<char>(cp);
                } else if (cp < 0x800) {
                    utf8[0] = static_cast<char>(0xC0 | (cp >> 6));
                    utf8[1] = static_cast<char>(0x80 | (cp & 0x3F));
                } else if (cp < 0x10000) {
                    utf8[0] = static_cast<char>(0xE0 | (cp >> 12));
                    utf8[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    utf8[2] = static_cast<char>(0x80 | (cp & 0x3F));
                } else {
                    utf8[0] = static_cast<char>(0xF0 | (cp >> 18));
                    utf8[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                    utf8[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    utf8[3] = static_cast<char>(0x80 | (cp & 0x3F));
                }
                
                ImU32 textColor = fg;
                if (cell.attr.bold) {
                    // Make color brighter for bold
                    int r = std::min(255, static_cast<int>(((fg >> 16) & 0xFF) * 5 / 4));
                    int g = std::min(255, static_cast<int>(((fg >> 8) & 0xFF) * 5 / 4));
                    int b = std::min(255, static_cast<int>((fg & 0xFF) * 5 / 4));
                    textColor = (fg & 0xFF000000) | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
                }
                
                drawList->AddText(ImVec2(x, y), textColor, utf8);
                
                // Draw underline
                if (cell.attr.underline) {
                    drawList->AddLine(
                        ImVec2(x, y + m_CharHeight - 1),
                        ImVec2(x + m_CharWidth, y + m_CharHeight - 1),
                        textColor
                    );
                }
                
                // Draw strikethrough
                if (cell.attr.strikethrough) {
                    drawList->AddLine(
                        ImVec2(x, y + m_CharHeight / 2),
                        ImVec2(x + m_CharWidth, y + m_CharHeight / 2),
                        textColor
                    );
                }
            }
        }
    }
    
    // Draw cursor only when at bottom (not scrolled)
    if (m_ScrollOffset == 0 && m_IsFocused && m_Emulator->IsCursorVisible() && m_CursorVisible) {
        int cursorRow = m_Emulator->GetCursorRow();
        int cursorCol = m_Emulator->GetCursorCol();
        
        float cursorX = pos.x + cursorCol * m_CharWidth;
        float cursorY = pos.y + cursorRow * m_CharHeight;
        
        ImU32 cursorColor = IM_COL32(255, 255, 255, 180);
        bool isInsertMode = InputSystem::GetInstance().GetInputMode() == EditorInputMode::Insert;
        
        if (isInsertMode) {
            // Thin vertical bar cursor for Insert mode
            drawList->AddRectFilled(
                ImVec2(cursorX, cursorY),
                ImVec2(cursorX + 2, cursorY + m_CharHeight),
                cursorColor
            );
        } else {
            // Horizontal underscore cursor for Command mode
            float underscoreHeight = 2.0f;
            drawList->AddRectFilled(
                ImVec2(cursorX, cursorY + m_CharHeight - underscoreHeight),
                ImVec2(cursorX + m_CharWidth, cursorY + m_CharHeight),
                cursorColor
            );
        }
        
        // Draw character under cursor in reverse (only for block-style cursors)
        if (!isInsertMode) {
            const auto& cursorCell = m_Emulator->GetCell(cursorRow, cursorCol);
            if (cursorCell.codepoint > 32) {
                char utf8[5] = {0};
                utf8[0] = static_cast<char>(cursorCell.codepoint);
                drawList->AddText(ImVec2(cursorX, cursorY), cursorCell.attr.bg, utf8);
            }
        }
    }
}

void TerminalWidget::RenderScrollbar(const ImVec2& pos, const ImVec2& size) {
    if (!m_Emulator) return;
    
    int scrollbackSize = static_cast<int>(m_Emulator->GetScrollbackSize());
    if (scrollbackSize == 0) return;  // No scrollback, no scrollbar needed
    
    int totalLines = scrollbackSize + m_Emulator->GetRows();
    int visibleLines = m_Emulator->GetRows();
    
    if (totalLines <= visibleLines) return;  // Everything fits
    
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    float scrollbarWidth = ImGui::GetStyle().ScrollbarSize;
    
    // Scrollbar background
    float scrollbarX = pos.x + size.x - scrollbarWidth;
    ImVec2 scrollbarPos(scrollbarX, pos.y);
    ImVec2 scrollbarEnd(scrollbarX + scrollbarWidth, pos.y + size.y);
    
    drawList->AddRectFilled(scrollbarPos, scrollbarEnd, IM_COL32(30, 30, 30, 200));
    
    // Calculate thumb size and position
    float thumbRatio = static_cast<float>(visibleLines) / static_cast<float>(totalLines);
    float thumbHeight = std::max(20.0f, size.y * thumbRatio);
    
    // Position: scrollOffset 0 = at bottom, scrollOffset max = at top
    float scrollRatio = 1.0f - (static_cast<float>(m_ScrollOffset) / static_cast<float>(scrollbackSize));
    float thumbY = pos.y + (size.y - thumbHeight) * scrollRatio;
    
    ImVec2 thumbPos(scrollbarX + 2, thumbY);
    ImVec2 thumbEnd(scrollbarX + scrollbarWidth - 2, thumbY + thumbHeight);
    
    // Check if mouse is over scrollbar for drag handling
    ImVec2 mousePos = ImGui::GetIO().MousePos;
    bool hovered = mousePos.x >= scrollbarX && mousePos.x <= scrollbarEnd.x &&
                   mousePos.y >= pos.y && mousePos.y <= pos.y + size.y;
    
    bool thumbHovered = mousePos.x >= thumbPos.x && mousePos.x <= thumbEnd.x &&
                        mousePos.y >= thumbPos.y && mousePos.y <= thumbEnd.y;
    
    // Handle scrollbar dragging
    if (thumbHovered && ImGui::IsMouseClicked(0)) {
        m_ScrollbarDragging = true;
        m_ScrollbarDragStartY = mousePos.y;
        m_ScrollbarDragStartOffset = m_ScrollOffset;
    }
    
    if (m_ScrollbarDragging) {
        if (ImGui::IsMouseDown(0)) {
            float deltaY = mousePos.y - m_ScrollbarDragStartY;
            float deltaRatio = deltaY / (size.y - thumbHeight);
            int deltaOffset = static_cast<int>(deltaRatio * scrollbackSize);
            m_ScrollOffset = std::max(0, std::min(scrollbackSize, m_ScrollbarDragStartOffset - deltaOffset));
        } else {
            m_ScrollbarDragging = false;
        }
    }
    
    // Click on track to jump
    if (hovered && !thumbHovered && !m_ScrollbarDragging && ImGui::IsMouseClicked(0)) {
        float clickRatio = (mousePos.y - pos.y) / size.y;
        m_ScrollOffset = static_cast<int>((1.0f - clickRatio) * scrollbackSize);
        m_ScrollOffset = std::max(0, std::min(m_ScrollOffset, scrollbackSize));
    }
    
    // Draw thumb
    ImU32 thumbColor = m_ScrollbarDragging ? IM_COL32(120, 120, 120, 255) :
                       thumbHovered ? IM_COL32(100, 100, 100, 255) :
                                      IM_COL32(80, 80, 80, 255);
    drawList->AddRectFilled(thumbPos, thumbEnd, thumbColor, 3.0f);
}

} // namespace sol
