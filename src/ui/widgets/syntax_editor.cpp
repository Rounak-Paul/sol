#include "syntax_editor.h"
#include "ui/editor_settings.h"
#include "core/lsp/lsp_manager.h"
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <cctype>

namespace sol {

SyntaxEditor::SyntaxEditor() {
    // Register global handler if we are the first editor? 
    // Ideally this is done by Workspace.
}

void SyntaxEditor::UpdateDiagnostics(const std::string& path, const std::vector<LSPDiagnostic>& diagnostics) {
    m_Diagnostics.clear();
    for (const auto& diag : diagnostics) {
        // Group by line for easier rendering
        m_Diagnostics[diag.range.start.line].push_back(diag);
    }
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
    
    ImGuiWindowFlags childFlags = ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoMove;
    if (m_ShowCompletion) {
        // Prevent buffer scrolling when navigating completion list
        childFlags |= ImGuiWindowFlags_NoNavInputs; 
    }

    ImGui::BeginChild(id, contentSize, false, childFlags);
    
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
    if ((m_IsFocused || m_ShowCompletion) && !m_HasSelection) {
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
    
    // Render diagnostics (squiggles)
    RenderDiagnostics(buffer, textPos, lineHeight, firstVisibleLine, lastVisibleLine);

    // Render cursor (only if cursor line is visible)
    if ((m_IsFocused || m_ShowCompletion) && !m_ReadOnly) {
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
    
    // Handle keyboard input (Navigation and State Control)
    if (m_IsFocused || m_ShowCompletion) {
        bool inputHandled = HandleInput(buffer);
        
        // Update cursor position for status bar
        auto& settings = EditorSettings::Get();
        auto [line, col] = buffer.PosToLineCol(m_CursorPos);
        settings.SetCursorPos(line + 1, col + 1);  // 1-based for display

        // Process Text Input (Typing)
        if (!inputHandled) {
             HandleTextInput(buffer);
        }
    }
    
    // Calculate completion position before ending child (to get correct screen coords)
    // Always calculate this because m_ShowCompletion might have changed during HandleTextInput
    ImVec2 completionPos;
    {
        auto [cursorLine, cursorCol] = buffer.PosToLineCol(m_CursorPos);
        size_t firstVisibleLine = static_cast<size_t>(m_ScrollY / lineHeight);
        
        completionPos.x = textPos.x + cursorCol * m_CharWidth;
        completionPos.y = textPos.y + (cursorLine - firstVisibleLine + 1) * lineHeight;
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();

    // Render completion (needs to be last/on top and outside child to avoid clipping)
    if (m_ShowCompletion) {
        RenderCompletion(buffer, completionPos);
    }
    
    return buffer.IsModified();
}

void SyntaxEditor::HandleTextInput(TextBuffer& buffer) {
     ImGuiIO& io = ImGui::GetIO();
     if (m_ReadOnly) return;
     if (io.InputQueueCharacters.Size == 0) return;
     
     // Build editor state for input mode
    EditorState state;
    state.buffer = &buffer;
    state.undoTree = &m_UndoTree;
    state.cursorPos = m_CursorPos;
    state.selectionStart = m_SelectionStart;
    state.selectionEnd = m_SelectionEnd;
    state.hasSelection = m_HasSelection;

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
             buffer.MarkModified();

            // Auto-trigger and Update Completion
            // We run this even if completion is already shown to update/filter the list
            if (m_CursorPos > 0) {
                char c = buffer.At(m_CursorPos - 1);
                bool isTriggerChar = (c == '.' || c == '>' || c == ':' || c == '/');
                bool isWordChar = std::isalnum(c) || c == '_';
                
                // If the list is open but the user typed a space or something that breaks the word, close it
                if (m_ShowCompletion && !isWordChar && !isTriggerChar) {
                    m_ShowCompletion = false;
                }
                else if (isTriggerChar || isWordChar) {
                    auto [line, col] = buffer.PosToLineCol(m_CursorPos);
                    const auto* langPtr = buffer.GetLanguage();
                    std::string lang = langPtr ? langPtr->name : "";
                    
                    bool hasServerConfig = LSPManager::GetInstance().HasServerFor(lang);
                    bool reqSent = false;
                    
                    // Calculate word start
                    size_t start = m_CursorPos;
                    while (start > 0) {
                        char p = buffer.At(start - 1);
                        if (!std::isalnum(p) && p != '_') break;
                        start--;
                    }

                    if (isWordChar) {
                        // Optimistically show local completions FIRST
                        if (m_CursorPos - start >= 1) {
                            std::string prefix = buffer.Substring(start, m_CursorPos - start);
                            std::vector<std::string> words = buffer.GetWordCompletions(prefix);
                            
                            if (langPtr) {
                                for(const auto& mapping : langPtr->highlightMappings) {
                                    if (mapping.second == HighlightGroup::Keyword || mapping.second == HighlightGroup::Type) {
                                        if (mapping.first.length() > prefix.length() && 
                                            mapping.first.substr(0, prefix.length()) == prefix) {
                                            words.push_back(mapping.first);
                                        }
                                    }
                                }
                            }

                            if (!words.empty()) {
                                m_CompletionItems.clear();
                                for(const auto& w : words) {
                                    LSPCompletionItem item;
                                    item.label = w;
                                    item.kind = 1; // Text
                                    item.insertText = w;
                                    item.detail = "Buffer";
                                    m_CompletionItems.push_back(item);
                                }
                                m_ShowCompletion = true;
                                m_SelectedCompletionIndex = 0;
                            }
                        }
                    }

                    if (hasServerConfig) {
                        // LSP Mode - Attempt to fetch smarter results
                        if (isTriggerChar || (isWordChar && m_CursorPos - start >= 1)) {
                             LSPManager::GetInstance().RequestCompletion(
                                buffer.GetFilePath().string(), 
                                lang, 
                                line, col, 
                                [this](const std::vector<LSPCompletionItem>& items) {
                                    if (!items.empty()) {
                                        m_CompletionItems = items;
                                        m_ShowCompletion = true;
                                        m_SelectedCompletionIndex = 0;
                                    } else {
                                        // If server explicitly returns nothing but we had local items, 
                                        // we might want to keep local items? 
                                        // Usually explicit empty list means "no suggestions".
                                        // But invalid/timeout requests might result in no callback.
                                        // So if this callback runs, let's respect it, mostly.
                                        // But for now, if empty, we might just hide or leave it if previously populated?
                                        // If users explicitly asked for LSP via trigger char, we should show what LSP says.
                                        if (m_CompletionItems.empty()) {
                                             m_ShowCompletion = false;
                                        }
                                    }
                                }
                            );
                        }
                    } 
                    
                    // Cleanup if nothing found at all
                    if (m_ShowCompletion && m_CompletionItems.empty()) {
                        m_ShowCompletion = false;
                    }
                }
            }
        }
    }
}
void SyntaxEditor::RenderLineNumbers(TextBuffer& buffer, const ImVec2& pos, float lineHeight, 
                                      size_t firstLine, size_t lastLine) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    float lineNumWidth = GetLineNumberWidth(buffer.LineCount());
    
    auto [cursorLine, cursorCol] = buffer.PosToLineCol(m_CursorPos);
    
    for (size_t i = firstLine; i < lastLine; ++i) {
        float y = pos.y + (i - firstLine) * lineHeight;
        
        // Highlight current line number
        bool isFocused = (m_IsFocused || m_ShowCompletion);
        ImU32 color = (i == cursorLine && isFocused) ? m_Theme.text : m_Theme.lineNumber;
        
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

    // Use text height instead of line height (which includes spacing)
    // to avoid selection looking "larger" than the text line
    float textHeight = ImGui::GetTextLineHeight();
    
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
            ImVec2(xEnd, y + textHeight),
            m_Theme.selection
        );
    }
}

bool SyntaxEditor::HandleInput(TextBuffer& buffer) {
    ImGuiIO& io = ImGui::GetIO();

    // Completion navigation
    if (m_ShowCompletion) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            m_ShowCompletion = false;
            ImGui::SetWindowFocus(); // Restore focus to buffer window
            return true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
            m_SelectedCompletionIndex = (m_SelectedCompletionIndex + 1) % m_CompletionItems.size();
            return true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
            m_SelectedCompletionIndex = (m_SelectedCompletionIndex - 1 + m_CompletionItems.size()) % m_CompletionItems.size();
            return true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_Tab)) {
            if (m_SelectedCompletionIndex >= 0 && m_SelectedCompletionIndex < m_CompletionItems.size()) {
                const auto& item = m_CompletionItems[m_SelectedCompletionIndex];
                
                // Find existing word prefix to replace (simple heuristic)
                size_t start = m_CursorPos;
                while (start > 0) {
                    char c = buffer.At(start - 1);
                    if (!std::isalnum(c) && c != '_') break;
                    start--;
                }
                
                if (start < m_CursorPos) {
                    buffer.Delete(start, m_CursorPos - start);
                    m_CursorPos = start;
                }
                
                buffer.Insert(m_CursorPos, item.insertText);
                m_CursorPos += item.insertText.length();
                m_ShowCompletion = false;
                ImGui::SetWindowFocus(); // Restore focus to buffer window
            }
            return true;
        }
    } else {
        // Trigger completion manually
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Space)) {
            auto [line, col] = buffer.PosToLineCol(m_CursorPos);
            std::string lang = buffer.GetLanguage() ? buffer.GetLanguage()->name : "";
            
            bool hasLSP = LSPManager::GetInstance().HasServerFor(lang);
            
            bool reqSent = false;
            
            if (hasLSP) {
                reqSent = LSPManager::GetInstance().RequestCompletion(
                    buffer.GetFilePath().string(), 
                    lang, 
                    line, col, 
                    [this](const std::vector<LSPCompletionItem>& items) {
                        if (!items.empty()) {
                            m_CompletionItems = items;
                            m_ShowCompletion = true;
                            m_SelectedCompletionIndex = 0;
                        }
                    }
                );
            }
            
            if (!reqSent) {
                // Built-in fallback
                // Find current word prefix
                size_t start = m_CursorPos;
                while (start > 0) {
                    char c = buffer.At(start - 1);
                    if (!std::isalnum(c) && c != '_') break;
                    start--;
                }
                std::string prefix = buffer.Substring(start, m_CursorPos - start);
                
                std::vector<std::string> words = buffer.GetWordCompletions(prefix);
                
                if (!words.empty()) {
                    m_CompletionItems.clear();
                    for(const auto& w : words) {
                         LSPCompletionItem item;
                         item.label = w;
                         item.kind = 1; // Text
                         item.insertText = w;
                         item.detail = "Buffer";
                         m_CompletionItems.push_back(item);
                    }
                    m_ShowCompletion = true;
                    m_SelectedCompletionIndex = 0;
                }
            }
        }
    }

    if (m_ReadOnly) return false;
    
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
    // REMOVED DUPLICATE HANDLING - Logic moved to HandleTextInput
    // if (io.InputQueueCharacters.Size > 0) { ... }
    
    if (modified) {
        buffer.MarkModified();
    }
    return result.handled;
}


void SyntaxEditor::RenderDiagnostics(TextBuffer& buffer, const ImVec2& textPos, float lineHeight, size_t firstLine, size_t lastLine) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 mousePos = ImGui::GetMousePos();
    bool tooltipShown = false;
    
    // Simple squiggly line rendering
    // A real implementation would use a sine wave or texture
    
    for (size_t line = firstLine; line < lastLine; ++line) {
        // Since diagnostics are mapped by 0-based line index in syntax_editor
        if (m_Diagnostics.count(line) == 0) continue;
        
        float y = textPos.y + (line - firstLine) * lineHeight + lineHeight; // Bottom of line
        
        for (const auto& diag : m_Diagnostics[line]) {
            // Calculate X start/end
            size_t startCol = diag.range.start.character;
            size_t endCol = diag.range.end.character;
            
            float x1 = textPos.x + startCol * m_CharWidth;
            float x2 = textPos.x + endCol * m_CharWidth;
            
            ImU32 color = m_Theme.error; // Default error
            if (diag.severity == 2) color = IM_COL32(255, 180, 0, 255); // Warning
            else if (diag.severity > 2) color = IM_COL32(0, 200, 255, 255); // Info/Hint
            
            // Draw zigzag
            float x = x1;
            float zigLen = 3.0f;
            bool up = true;
            while (x < x2) {
                float nextX = std::min(x + zigLen, x2);
                float yOffset = up ? -2.0f : 0.0f;
                drawList->AddLine(ImVec2(x, y + (up ? 0 : -2)), ImVec2(nextX, y + (up ? -2 : 0)), color, 1.0f);
                x = nextX;
                up = !up;
            }

            // Hover check
            if (ImGui::IsWindowHovered() && 
                mousePos.x >= x1 && mousePos.x <= x2 &&
                mousePos.y >= y - lineHeight && mousePos.y <= y) {
                
                if (!tooltipShown) {
                    ImGui::BeginTooltip();
                    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
                    tooltipShown = true;
                } else {
                    ImGui::Separator();
                }

                ImVec4 titleColor = ImVec4(1, 1, 1, 1);
                const char* title = "Diagnostic";
                
                if (diag.severity == 1) {
                    title = "Error";
                    titleColor = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
                } else if (diag.severity == 2) {
                    title = "Warning";
                    titleColor = ImVec4(1.0f, 0.8f, 0.2f, 1.0f);
                } else if (diag.severity == 3) {
                    title = "Info";
                    titleColor = ImVec4(0.4f, 0.8f, 1.0f, 1.0f);
                } else {
                    title = "Hint";
                    titleColor = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
                }
                
                ImGui::TextColored(titleColor, "%s", title);
                ImGui::TextWrapped("%s", diag.message.c_str());
            }
        }
    }

    if (tooltipShown) {
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void SyntaxEditor::RenderCompletion(TextBuffer& buffer, const ImVec2& popupPos) {
    if (m_CompletionItems.empty()) return;

    ImGuiIO& io = ImGui::GetIO();
    float lineHeight = ImGui::GetTextLineHeightWithSpacing();
    
    // Calculate smart size and position
    float width = 300.0f;
    float padding = ImGui::GetStyle().WindowPadding.y * 2;
    // Estimate content height based on items (approximate)
    float contentHeight = m_CompletionItems.size() * lineHeight; 
    float maxHeight = 200.0f;
    float height = std::min(contentHeight + padding, maxHeight);
    
    ImVec2 pos = popupPos;
    
    // Horizontal Boundary Check (Keep inside right edge)
    if (pos.x + width > io.DisplaySize.x) {
        pos.x = std::max(0.0f, io.DisplaySize.x - width);
    }
    
    // Vertical Boundary Check (Flip if it hits bottom)
    // popupPos is the bottom-left of the cursor.
    if (pos.y + height > io.DisplaySize.y) {
        // Place ABOVE the cursor line
        // We subtract the popup height AND the cursor line height (since pos is bottom of cursor)
        pos.y -= (height + lineHeight);
    }

    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
    
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | 
                             ImGuiWindowFlags_NoResize | 
                             ImGuiWindowFlags_NoMove | 
                             ImGuiWindowFlags_HorizontalScrollbar | 
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_Tooltip |
                             ImGuiWindowFlags_NoFocusOnAppearing; 
                             
    ImGui::PushStyleColor(ImGuiCol_WindowBg, m_Theme.popupBg);
    ImGui::PushStyleColor(ImGuiCol_Border, m_Theme.popupBorder);
    ImGui::PushStyleColor(ImGuiCol_Text, m_Theme.popupText);
    
    if (ImGui::Begin("##completion_popup", nullptr, flags)) {
        for (int i = 0; i < m_CompletionItems.size(); ++i) {
            bool selected = (i == m_SelectedCompletionIndex);
            
            if (selected) {
                ImGui::PushStyleColor(ImGuiCol_Header, m_Theme.popupSelected);
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, m_Theme.popupSelected);
            }
            
            // Explicitly use std::string because of weird name lookup issue
            std::string label = m_CompletionItems[i].label; 
            if (ImGui::Selectable(label.c_str(), selected)) {
                // Clicked
                m_SelectedCompletionIndex = i;
                // Double click? Or just single click to select? 
                // Usually click inserts.
                // For now just select, let Enter insert.
            }
            
            if (selected && m_ShowCompletion) {
                ImGui::SetScrollHereY();
                
                // Show detail tooltip
                if (!m_CompletionItems[i].detail.empty() || !m_CompletionItems[i].documentation.empty()) {
                    ImGui::BeginTooltip();
                    if (!m_CompletionItems[i].detail.empty())
                        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%s", m_CompletionItems[i].detail.c_str());
                    if (!m_CompletionItems[i].documentation.empty())
                        ImGui::TextWrapped("%s", m_CompletionItems[i].documentation.c_str());
                    ImGui::EndTooltip();
                }
            }
            
            if (selected) {
                 ImGui::PopStyleColor(2);
            }
        }
    }
    ImGui::End();
    
    ImGui::PopStyleColor(3);
}

} // namespace sol
