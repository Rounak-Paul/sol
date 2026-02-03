#include "syntax_editor.h"
#include "ui/editor_settings.h"
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>

namespace sol {

SyntaxEditor::SyntaxEditor() {
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
    
    // Calculate max line width for horizontal scrollbar (use cached line lengths)
    float maxLineWidth = 0.0f;
    for (size_t i = firstVisibleLine; i < lastVisibleLine; ++i) {
        size_t lineLen = buffer.LineEnd(i) - buffer.LineStart(i);
        maxLineWidth = std::max(maxLineWidth, lineLen * m_CharWidth);
    }
    
    // Set total content size for scrolling
    float totalHeight = lineCount * lineHeight;
    ImGui::Dummy(ImVec2(maxLineWidth + lineNumberWidth, totalHeight));
    
    // Get window position for drawing (not affected by scroll, we handle scroll ourselves)
    ImVec2 windowPos = ImGui::GetWindowPos();
    ImVec2 windowPadding = ImGui::GetStyle().WindowPadding;
    
    // Calculate the offset within the first visible line due to partial scroll
    float scrollOffset = m_ScrollY - (firstVisibleLine * lineHeight);
    
    // Base position for drawing: window position + padding - partial line offset
    ImVec2 cursorPos = ImVec2(windowPos.x + windowPadding.x - m_ScrollX, 
                               windowPos.y + windowPadding.y - scrollOffset);
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
    
    // Render cursor (only if cursor line is visible)
    if (m_IsFocused && !m_ReadOnly) {
        auto [cursorLine, cursorCol] = buffer.PosToLineCol(m_CursorPos);
        if (cursorLine >= firstVisibleLine && cursorLine < lastVisibleLine) {
            RenderCursor(buffer, textPos, lineHeight, firstVisibleLine);
        }
    }
    
    // Handle mouse input for clicking and dragging
    ImVec2 mousePos = ImGui::GetMousePos();
    
    // Helper lambda to convert mouse position to buffer position
    auto mouseToBufPos = [&](ImVec2 pos) -> size_t {
        float relX = pos.x - textPos.x;
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
        
        // Update cursor position for status bar
        auto& settings = EditorSettings::Get();
        auto [line, col] = buffer.PosToLineCol(m_CursorPos);
        settings.SetCursorPos(line + 1, col + 1);  // 1-based for display
    }
    
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
    
    // Build a flat list of color changes sorted by position for O(log n) lookup
    struct ColorChange {
        size_t pos;
        ImU32 color;
        bool isStart;  // true = token start, false = token end
    };
    std::vector<ColorChange> colorChanges;
    colorChanges.reserve(tokens.size() * 2);
    
    for (const auto& token : tokens) {
        ImU32 color = m_Theme.GetColor(static_cast<HighlightGroup>(token.highlightId));
        colorChanges.push_back({token.startByte, color, true});
        colorChanges.push_back({token.endByte, m_Theme.text, false});
    }
    
    std::sort(colorChanges.begin(), colorChanges.end(), [](const ColorChange& a, const ColorChange& b) {
        if (a.pos != b.pos) return a.pos < b.pos;
        // Starts come before ends at same position
        return a.isStart > b.isStart;
    });
    
    // Render each visible line
    for (size_t lineIdx = firstLine; lineIdx < lastLine; ++lineIdx) {
        std::string_view lineText = buffer.LineView(lineIdx);
        float y = pos.y + (lineIdx - firstLine) * lineHeight;
        float x = pos.x;
        
        if (lineText.empty()) continue;
        
        size_t lineStart = buffer.LineStart(lineIdx);
        size_t lineEnd = lineStart + lineText.length();
        
        // Binary search to find first color change at or before lineStart
        auto it = std::upper_bound(colorChanges.begin(), colorChanges.end(), lineStart,
            [](size_t pos, const ColorChange& cc) { return pos < cc.pos; });
        
        // Find the active color at lineStart by scanning backwards
        ImU32 currentColor = m_Theme.text;
        for (const auto& token : tokens) {
            if (token.startByte <= lineStart && token.endByte > lineStart) {
                currentColor = m_Theme.GetColor(static_cast<HighlightGroup>(token.highlightId));
                break;
            }
        }
        
        // Process color changes within this line
        size_t charIdx = 0;
        size_t spanStart = 0;
        ImU32 spanColor = currentColor;
        
        while (charIdx < lineText.length()) {
            size_t bytePos = lineStart + charIdx;
            
            // Check if there's a color change at this position
            while (it != colorChanges.end() && it->pos == bytePos) {
                if (it->isStart) {
                    // Start of a new token
                    if (charIdx > spanStart) {
                        // Render previous span
                        RenderSpan(drawList, lineText, spanStart, charIdx, x, y, spanColor);
                    }
                    spanStart = charIdx;
                    spanColor = it->color;
                }
                ++it;
            }
            
            // Check for token end
            while (it != colorChanges.end() && it->pos == bytePos && !it->isStart) {
                ++it;
            }
            
            ++charIdx;
            
            // Advance iterator past positions we've passed
            while (it != colorChanges.end() && it->pos < lineStart + charIdx) {
                if (it->isStart) {
                    spanColor = it->color;
                } else {
                    // Token ended, check if another starts
                    auto nextIt = it + 1;
                    if (nextIt != colorChanges.end() && nextIt->pos == it->pos && nextIt->isStart) {
                        spanColor = nextIt->color;
                    } else {
                        spanColor = m_Theme.text;
                    }
                }
                ++it;
            }
        }
        
        // Render final span
        if (lineText.length() > spanStart) {
            RenderSpan(drawList, lineText, spanStart, lineText.length(), x, y, spanColor);
        }
    }
}

void SyntaxEditor::RenderSpan(ImDrawList* drawList, std::string_view lineText, 
                               size_t start, size_t end, float& x, float y, ImU32 color) {
    // Get clip rect to skip rendering off-screen content
    const ImVec4& clipRect = drawList->_ClipRectStack.back();
    float clipLeft = clipRect.x;
    float clipRight = clipRect.z;
    
    size_t i = start;
    while (i < end) {
        // Handle tabs
        if (lineText[i] == '\t') {
            x += m_CharWidth * m_TabSize;
            ++i;
            continue;
        }
        
        // Find contiguous run of non-tab characters
        size_t runStart = i;
        while (i < end && lineText[i] != '\t') {
            ++i;
        }
        
        if (i > runStart) {
            float runWidth = (i - runStart) * m_CharWidth;
            
            // Skip if entirely to the left of clip rect
            if (x + runWidth < clipLeft) {
                x += runWidth;
                continue;
            }
            
            // Stop if entirely to the right of clip rect
            if (x > clipRight) {
                x += runWidth;
                continue;
            }
            
            // Render the visible portion
            drawList->AddText(ImVec2(x, y), color, 
                              lineText.data() + runStart, 
                              lineText.data() + i);
            x += runWidth;
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
    float x = textPos.x + cursorCol * m_CharWidth;
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
        
        float xStart = textPos.x + colStart * m_CharWidth;
        float xEnd = textPos.x + colEnd * m_CharWidth;
        
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
