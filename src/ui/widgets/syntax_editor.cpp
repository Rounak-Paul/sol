#include "syntax_editor.h"
#include "ui/editor_settings.h"
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>

namespace sol {

SyntaxEditor::SyntaxEditor() {
    // Register for mode change notifications
    EditorSettings::Get().SetOnModeChanged([this](bool vimEnabled) {
        m_InputManager.SetVimEnabled(vimEnabled);
    });
}

float SyntaxEditor::GetCharWidth() const {
    return ImGui::CalcTextSize("M").x;
}

float SyntaxEditor::GetLineNumberWidth(size_t lineCount) const {
    if (!m_ShowLineNumbers) return 0.0f;
    int digits = 1;
    size_t n = lineCount;
    while (n >= 10) { n /= 10; ++digits; }
    digits = std::max(digits, 3); // Minimum 3 digits
    return GetCharWidth() * (digits + 2); // Extra padding
}

bool SyntaxEditor::Render(const char* label, TextBuffer& buffer, const ImVec2& size) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;
    
    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
    const ImGuiID id = window->GetID(label);
    
    const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
    m_CharWidth = GetCharWidth();
    
    // Calculate size
    ImVec2 contentSize = size;
    if (contentSize.x <= 0.0f) contentSize.x = ImGui::GetContentRegionAvail().x;
    if (contentSize.y <= 0.0f) contentSize.y = ImGui::GetContentRegionAvail().y;
    
    const size_t lineCount = buffer.LineCount();
    const float lineNumberWidth = GetLineNumberWidth(lineCount);
    const float textAreaWidth = contentSize.x - lineNumberWidth;
    
    // Begin child region for scrolling
    ImGui::PushStyleColor(ImGuiCol_ChildBg, m_Theme.background);
    ImGui::BeginChild(id, contentSize, false, 
        ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoMove);
    
    m_IsFocused = ImGui::IsWindowFocused();
    
    // Handle scrolling
    m_ScrollX = ImGui::GetScrollX();
    m_ScrollY = ImGui::GetScrollY();
    
    // Calculate visible lines
    const size_t firstVisibleLine = static_cast<size_t>(m_ScrollY / lineHeight);
    const size_t visibleLineCount = static_cast<size_t>(contentSize.y / lineHeight) + 2;
    const size_t lastVisibleLine = std::min(firstVisibleLine + visibleLineCount, lineCount);
    
    // Ensure buffer is parsed
    if (!buffer.IsParsed() && buffer.HasLanguage()) {
        buffer.Parse();
    }
    
    ImVec2 cursorPos = ImGui::GetCursorScreenPos();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    
    // Render line numbers
    if (m_ShowLineNumbers) {
        RenderLineNumbers(buffer, cursorPos, lineHeight, firstVisibleLine, lastVisibleLine);
    }
    
    // Text area position
    ImVec2 textPos = ImVec2(cursorPos.x + lineNumberWidth, cursorPos.y);
    
    // Render selection background
    if (m_HasSelection) {
        RenderSelection(buffer, textPos, lineHeight, firstVisibleLine, lastVisibleLine);
    }
    
    // Render current line highlight
    if (m_IsFocused && !m_HasSelection) {
        auto [cursorLine, cursorCol] = buffer.PosToLineCol(m_CursorPos);
        if (cursorLine >= firstVisibleLine && cursorLine < lastVisibleLine) {
            float y = textPos.y + (cursorLine - firstVisibleLine) * lineHeight;
            drawList->AddRectFilled(
                ImVec2(cursorPos.x, y),
                ImVec2(cursorPos.x + contentSize.x, y + lineHeight),
                m_Theme.currentLine
            );
        }
    }
    
    // Render syntax-highlighted text
    RenderText(buffer, textPos, lineHeight, firstVisibleLine, lastVisibleLine);
    
    // Render cursor
    if (m_IsFocused && !m_ReadOnly) {
        RenderCursor(buffer, textPos, lineHeight, firstVisibleLine);
    }
    
    // Handle mouse input for clicking and dragging
    ImVec2 mousePos = ImGui::GetMousePos();
    
    // Helper lambda to convert mouse position to buffer position
    auto mouseToBufPos = [&](ImVec2 pos) -> size_t {
        float relX = pos.x - textPos.x + m_ScrollX;
        float relY = pos.y - textPos.y;
        
        size_t clickedLine = firstVisibleLine + static_cast<size_t>(std::max(0.0f, relY) / lineHeight);
        clickedLine = std::min(clickedLine, lineCount > 0 ? lineCount - 1 : 0);
        
        size_t clickedCol = static_cast<size_t>(std::max(0.0f, relX) / m_CharWidth);
        std::string line = buffer.Line(clickedLine);
        clickedCol = std::min(clickedCol, line.length());
        
        return buffer.LineColToPos(clickedLine, clickedCol);
    };
    
    // Mouse click - start selection or set cursor
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0)) {
        m_CursorPos = mouseToBufPos(mousePos);
        
        // Handle selection with shift
        if (ImGui::GetIO().KeyShift) {
            m_SelectionEnd = m_CursorPos;
            m_HasSelection = m_SelectionStart != m_SelectionEnd;
        } else {
            m_SelectionStart = m_CursorPos;
            m_SelectionEnd = m_CursorPos;
            m_HasSelection = false;
        }
        
        m_IsDragging = true;
        m_CursorBlinkTimer = 0.0f;
    }
    
    // Mouse drag - extend selection
    if (m_IsDragging && ImGui::IsMouseDown(0)) {
        size_t newPos = mouseToBufPos(mousePos);
        if (newPos != m_CursorPos) {
            m_CursorPos = newPos;
            m_SelectionEnd = m_CursorPos;
            m_HasSelection = m_SelectionStart != m_SelectionEnd;
            m_CursorBlinkTimer = 0.0f;
        }
    }
    
    // Mouse release - stop dragging
    if (ImGui::IsMouseReleased(0)) {
        m_IsDragging = false;
    }
    
    // Handle keyboard input
    if (m_IsFocused) {
        HandleInput(buffer);
    }
    
    // Render mode-specific UI (vim command line, etc.)
    if (m_IsFocused) {
        EditorState uiState;
        uiState.buffer = &buffer;
        uiState.cursorPos = m_CursorPos;
        m_InputManager.RenderUI(uiState);
        
        // Update global editor settings for status bar
        auto& settings = EditorSettings::Get();
        settings.SetModeIndicator(m_InputManager.GetModeIndicator());
        auto [line, col] = buffer.PosToLineCol(m_CursorPos);
        settings.SetCursorPos(line + 1, col + 1);  // 1-based for display
    }
    
    // Set content size for scrolling
    float totalHeight = lineCount * lineHeight;
    float maxLineWidth = 0.0f;
    for (size_t i = firstVisibleLine; i < lastVisibleLine; ++i) {
        maxLineWidth = std::max(maxLineWidth, buffer.Line(i).length() * m_CharWidth);
    }
    ImGui::Dummy(ImVec2(maxLineWidth + lineNumberWidth, totalHeight));
    
    ImGui::EndChild();
    ImGui::PopStyleColor();
    
    return buffer.IsModified();
}

void SyntaxEditor::RenderLineNumbers(TextBuffer& buffer, const ImVec2& pos, float lineHeight, 
                                      size_t firstLine, size_t lastLine) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    float lineNumWidth = GetLineNumberWidth(buffer.LineCount());
    
    auto [cursorLine, cursorCol] = buffer.PosToLineCol(m_CursorPos);
    
    for (size_t i = firstLine; i < lastLine; ++i) {
        float y = pos.y + (i - firstLine) * lineHeight;
        
        // Highlight current line number
        ImU32 color = (i == cursorLine && m_IsFocused) ? m_Theme.text : m_Theme.lineNumber;
        
        char lineNumStr[16];
        snprintf(lineNumStr, sizeof(lineNumStr), "%zu", i + 1);
        
        // Right-align line numbers
        float textWidth = ImGui::CalcTextSize(lineNumStr).x;
        float x = pos.x + lineNumWidth - textWidth - m_CharWidth;
        
        drawList->AddText(ImVec2(x, y), color, lineNumStr);
    }
    
    // Draw separator line
    drawList->AddLine(
        ImVec2(pos.x + lineNumWidth - m_CharWidth * 0.5f, pos.y),
        ImVec2(pos.x + lineNumWidth - m_CharWidth * 0.5f, pos.y + (lastLine - firstLine) * lineHeight),
        m_Theme.lineNumber,
        1.0f
    );
}

void SyntaxEditor::RenderText(TextBuffer& buffer, const ImVec2& pos, float lineHeight,
                               size_t firstLine, size_t lastLine) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    
    // Get syntax tokens for visible range
    std::vector<SyntaxToken> tokens = buffer.GetSyntaxTokens(firstLine, lastLine);
    
    // Build a map of byte position -> color
    std::vector<std::pair<size_t, ImU32>> colorChanges;
    for (const auto& token : tokens) {
        colorChanges.emplace_back(token.startByte, m_Theme.GetColor(static_cast<HighlightGroup>(token.highlightId)));
        colorChanges.emplace_back(token.endByte, m_Theme.text);
    }
    std::sort(colorChanges.begin(), colorChanges.end());
    
    // Render each visible line
    for (size_t lineIdx = firstLine; lineIdx < lastLine; ++lineIdx) {
        std::string lineText = buffer.Line(lineIdx);
        float y = pos.y + (lineIdx - firstLine) * lineHeight;
        float x = pos.x - m_ScrollX;
        
        size_t lineStart = buffer.LineStart(lineIdx);
        size_t lineEnd = lineStart + lineText.length();
        
        // Find starting color for this line
        ImU32 currentColor = m_Theme.text;
        for (const auto& token : tokens) {
            if (token.startByte <= lineStart && token.endByte > lineStart) {
                currentColor = m_Theme.GetColor(static_cast<HighlightGroup>(token.highlightId));
                break;
            }
        }
        
        // Render character by character with color changes
        size_t charIdx = 0;
        size_t bytePos = lineStart;
        
        while (charIdx < lineText.length()) {
            // Check for color change at this position
            for (const auto& token : tokens) {
                if (token.startByte == bytePos) {
                    currentColor = m_Theme.GetColor(static_cast<HighlightGroup>(token.highlightId));
                } else if (token.endByte == bytePos) {
                    currentColor = m_Theme.text;
                }
            }
            
            // Handle tabs
            if (lineText[charIdx] == '\t') {
                x += m_CharWidth * m_TabSize;
            } else {
                // Render single character
                char ch[2] = { lineText[charIdx], '\0' };
                drawList->AddText(ImVec2(x, y), currentColor, ch);
                x += m_CharWidth;
            }
            
            ++charIdx;
            ++bytePos;
        }
    }
}

void SyntaxEditor::RenderCursor(TextBuffer& buffer, const ImVec2& textPos, float lineHeight, size_t firstLine) {
    // Update blink timer
    m_CursorBlinkTimer += ImGui::GetIO().DeltaTime;
    if (m_CursorBlinkTimer > CURSOR_BLINK_RATE * 2) {
        m_CursorBlinkTimer = 0.0f;
    }
    
    // Only show cursor half the time (blinking)
    if (m_CursorBlinkTimer > CURSOR_BLINK_RATE) {
        return;
    }
    
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    
    // Get cursor line/col from buffer
    auto [cursorLine, cursorCol] = buffer.PosToLineCol(m_CursorPos);
    
    // Calculate cursor screen position
    float x = textPos.x + cursorCol * m_CharWidth - m_ScrollX;
    float y = textPos.y + (cursorLine - firstLine) * lineHeight;
    
    // Get line height without spacing for cursor
    float cursorHeight = ImGui::GetTextLineHeight();
    
    // Draw cursor line (thin vertical bar)
    drawList->AddRectFilled(
        ImVec2(x, y + 2),
        ImVec2(x + 2, y + cursorHeight - 2),
        m_Theme.cursor
    );
}

void SyntaxEditor::RenderSelection(TextBuffer& buffer, const ImVec2& textPos, float lineHeight,
                                    size_t firstLine, size_t lastLine) {
    if (!m_HasSelection) return;
    
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    
    size_t selStart = std::min(m_SelectionStart, m_SelectionEnd);
    size_t selEnd = std::max(m_SelectionStart, m_SelectionEnd);
    
    auto [startLine, startCol] = buffer.PosToLineCol(selStart);
    auto [endLine, endCol] = buffer.PosToLineCol(selEnd);
    
    for (size_t line = startLine; line <= endLine && line < lastLine; ++line) {
        if (line < firstLine) continue;
        
        std::string lineText = buffer.Line(line);
        float y = textPos.y + (line - firstLine) * lineHeight;
        
        size_t colStart = (line == startLine) ? startCol : 0;
        size_t colEnd = (line == endLine) ? endCol : lineText.length();
        
        float xStart = textPos.x + colStart * m_CharWidth - m_ScrollX;
        float xEnd = textPos.x + colEnd * m_CharWidth - m_ScrollX;
        
        drawList->AddRectFilled(
            ImVec2(xStart, y),
            ImVec2(xEnd, y + lineHeight),
            m_Theme.selection
        );
    }
}

void SyntaxEditor::HandleInput(TextBuffer& buffer) {
    if (m_ReadOnly) return;
    
    ImGuiIO& io = ImGui::GetIO();
    
    // Build editor state for input mode
    EditorState state;
    state.buffer = &buffer;
    state.undoTree = &m_UndoTree;
    state.cursorPos = m_CursorPos;
    state.selectionStart = m_SelectionStart;
    state.selectionEnd = m_SelectionEnd;
    state.hasSelection = m_HasSelection;
    
    bool modified = false;
    
    // Handle keyboard input through input manager
    InputResult result = m_InputManager.HandleKeyboard(state);
    
    if (result.handled) {
        if (result.cursorMoved || result.textChanged) {
            if (result.newCursorPos) {
                m_CursorPos = *result.newCursorPos;
            }
            m_CursorBlinkTimer = 0.0f;
        }
        if (result.selectionChanged) {
            if (result.newSelectionStart) m_SelectionStart = *result.newSelectionStart;
            if (result.newSelectionEnd) m_SelectionEnd = *result.newSelectionEnd;
            m_HasSelection = m_SelectionStart != m_SelectionEnd;
        }
        if (result.textChanged) {
            modified = true;
        }
    }
    
    // Handle text input through input manager
    if (io.InputQueueCharacters.Size > 0) {
        InputResult textResult = m_InputManager.HandleTextInput(
            state, 
            io.InputQueueCharacters.Data, 
            io.InputQueueCharacters.Size
        );
        
        if (textResult.handled) {
            if (textResult.cursorMoved || textResult.textChanged) {
                if (textResult.newCursorPos) {
                    m_CursorPos = *textResult.newCursorPos;
                }
                m_CursorBlinkTimer = 0.0f;
            }
            if (textResult.textChanged) {
                modified = true;
            }
        }
    }
    
    if (modified) {
        buffer.MarkModified();
    }
}

} // namespace sol
