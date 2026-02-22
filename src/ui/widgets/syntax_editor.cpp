#include "syntax_editor.h"
#include "ui/editor_settings.h"
#include "ui/icons_nerd.h"
#include "ui/input/command.h"
#include "core/lsp/lsp_manager.h"
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <cctype>

namespace sol {

SyntaxEditor::SyntaxEditor() {
    // Register global handler if we are the first editor? 
    // Ideally this is done by Workspace.
    
    // Initialize default theme colors
    m_Theme.scopeBackground = IM_COL32(50, 50, 60, 100); // 40% opacity
    
    // Rainbow Indents (Yellow, Green, Blue, Purple)
    m_Theme.rainbowIndents = {
        IM_COL32(255, 255, 80, 128),   // Increased alpha
        IM_COL32(100, 255, 100, 128),
        IM_COL32(80, 180, 255, 128),
        IM_COL32(255, 100, 255, 128)
    };
    
    // Rainbow Brackets (Gold, Orchid, SkyBlue)
    m_Theme.rainbowBrackets = {
         IM_COL32(255, 215, 0, 255),
         IM_COL32(218, 112, 214, 255),
         IM_COL32(135, 206, 235, 255)
    };
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
    return GetCharWidth() * (digits + 3); // Extra padding
}

float SyntaxEditor::GetFoldGutterWidth() const {
    if (!m_ShowLineNumbers) return 0.0f;
    return GetCharWidth() * 2.0f; // Width for fold indicator column
}

bool SyntaxEditor::Render(const char* label, TextBuffer& buffer, const ImVec2& size) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;

    // Sync theme from EditorSettings
    {
        const auto& e = EditorSettings::Get().GetTheme().editor;
        m_Theme.background   = ImGui::ColorConvertFloat4ToU32(e.background);
        m_Theme.text         = ImGui::ColorConvertFloat4ToU32(e.text);
        m_Theme.keyword      = ImGui::ColorConvertFloat4ToU32(e.keyword);
        m_Theme.type         = ImGui::ColorConvertFloat4ToU32(e.type);
        m_Theme.function     = ImGui::ColorConvertFloat4ToU32(e.function);
        m_Theme.variable     = ImGui::ColorConvertFloat4ToU32(e.variable);
        m_Theme.string       = ImGui::ColorConvertFloat4ToU32(e.string);
        m_Theme.number       = ImGui::ColorConvertFloat4ToU32(e.number);
        m_Theme.comment      = ImGui::ColorConvertFloat4ToU32(e.comment);
        m_Theme.op           = ImGui::ColorConvertFloat4ToU32(e.op);
        m_Theme.punctuation  = ImGui::ColorConvertFloat4ToU32(e.punctuation);
        m_Theme.macro        = ImGui::ColorConvertFloat4ToU32(e.macro);
        m_Theme.constant     = ImGui::ColorConvertFloat4ToU32(e.constant);
        m_Theme.error        = ImGui::ColorConvertFloat4ToU32(e.error);
        m_Theme.lineNumber   = ImGui::ColorConvertFloat4ToU32(e.lineNumber);
        m_Theme.currentLine  = ImGui::ColorConvertFloat4ToU32(e.currentLine);
        m_Theme.selection    = ImGui::ColorConvertFloat4ToU32(e.selection);
        m_Theme.cursor       = ImGui::ColorConvertFloat4ToU32(e.cursor);
        m_Theme.popupBg      = ImGui::ColorConvertFloat4ToU32(e.popupBg);
        m_Theme.popupBorder  = ImGui::ColorConvertFloat4ToU32(e.popupBorder);
        m_Theme.popupText    = ImGui::ColorConvertFloat4ToU32(e.popupText);
        m_Theme.popupSelected = ImGui::ColorConvertFloat4ToU32(e.popupSelected);
    }
    
    // Process any pending completion items from LSP (thread-safe)
    {
        std::lock_guard<std::mutex> lock(m_PendingCompletionMutex);
        if (m_PendingCompletionItems.has_value()) {
            if (!m_PendingCompletionItems->empty()) {
                m_CompletionItems = std::move(*m_PendingCompletionItems);
                m_ShowCompletion = true;
                m_SelectedCompletionIndex = 0;
            } else if (m_CompletionItems.empty()) {
                m_ShowCompletion = false;
            }
            m_PendingCompletionItems.reset();
        }
    }
    
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
    const float foldGutterWidth = GetFoldGutterWidth();
    const float totalGutterWidth = lineNumberWidth + foldGutterWidth;
    const float textAreaWidth = contentSize.x - totalGutterWidth;
    
    // Begin child region for scrolling
    ImGui::PushStyleColor(ImGuiCol_ChildBg, m_Theme.background);
    
    // Always disable default navigation inputs to prevent auto-scrolling on arrow keys
    // We handle scrolling manually based on cursor position
    ImGuiWindowFlags childFlags = ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNavInputs;
    
    ImGui::BeginChild(id, contentSize, false, childFlags);
    
    m_IsFocused = ImGui::IsWindowFocused();
    
    // Track active state for cursor visibility
    // - Set active when focused
    // - Clear only when focus moves to another window (not just temporarily lost)
    if (m_IsFocused) {
        m_IsActive = true;
    } else if (m_IsActive) {
        // Check if another window/item has taken focus
        // This distinguishes between temporary unfocus (Escape) and actual focus movement (terminal open)
        bool anotherWindowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow) && !ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if (anotherWindowFocused) {
            m_IsActive = false;
        }
    }
    
    // Ensure buffer is parsed first (needed for fold ranges)
    if (!buffer.IsParsed() && buffer.HasLanguage()) {
        buffer.Parse();
        m_FoldRangesDirty = true;
    }
    
    // Update fold ranges from tree-sitter (must be before any fold-related calculations)
    UpdateFoldRanges(buffer);
    
    // Calculate total visible lines (total - hidden from folds)
    const size_t hiddenLineCount = GetHiddenLineCount();
    const size_t visibleTotalLines = lineCount > hiddenLineCount ? lineCount - hiddenLineCount : lineCount;

    // Auto-scroll to cursor only when cursor moved (not every frame)
    if (m_NeedsScrollToCursor && m_IsFocused) {
        m_NeedsScrollToCursor = false;
        
        auto [cursorLine, cursorCol] = buffer.PosToLineCol(m_CursorPos);
        
        ImVec2 region = ImGui::GetContentRegionAvail();

        // Vertical scrolling - use screen line (accounting for folds)
        // If cursor is hidden, scroll to the fold start line instead
        size_t targetLine = cursorLine;
        if (IsLineHidden(cursorLine)) {
            // Find the fold containing this line
            for (const auto& [foldStart, foldEnd] : m_FoldEndLines) {
                if (cursorLine > foldStart && cursorLine <= foldEnd && m_FoldedLines.count(foldStart)) {
                    targetLine = foldStart;
                    break;
                }
            }
        }
        
        size_t screenLine = BufferLineToScreenLine(targetLine);
        float cursorY = screenLine * lineHeight;
        float currentScrollY = ImGui::GetScrollY();
        float displayHeight = region.y;
        
        if (cursorY < currentScrollY) {
            ImGui::SetScrollY(cursorY);
        } else if (cursorY + lineHeight > currentScrollY + displayHeight) {
            ImGui::SetScrollY(cursorY + lineHeight - displayHeight);
        }
        
        // Horizontal scrolling
        float cursorX = cursorCol * m_CharWidth;
        float currentScrollX = ImGui::GetScrollX();
        float displayWidth = region.x - totalGutterWidth;
        float scrollMargin = std::max(displayWidth * 0.1f, m_CharWidth * 4.0f); 

        if (cursorX < currentScrollX + scrollMargin) {
            ImGui::SetScrollX(std::max(0.0f, cursorX - scrollMargin));
        } else if (cursorX > currentScrollX + displayWidth - scrollMargin) {
            ImGui::SetScrollX(cursorX - displayWidth + scrollMargin);
        }
    }
    
    // Handle scrolling
    m_ScrollX = ImGui::GetScrollX();
    m_ScrollY = ImGui::GetScrollY();
    
    // Calculate visible lines based on screen rows (not buffer lines)
    const size_t firstScreenLine = static_cast<size_t>(m_ScrollY / lineHeight);
    const size_t screenLineCount = static_cast<size_t>(contentSize.y / lineHeight) + 2;
    
    // Convert screen lines to buffer lines
    const size_t firstVisibleLine = ScreenLineToBufferLine(firstScreenLine, lineCount);
    
    // Calculate last visible buffer line by counting visible screen rows
    size_t lastVisibleLine = firstVisibleLine;
    size_t visibleCount = 0;
    while (lastVisibleLine < lineCount && visibleCount < screenLineCount) {
        if (!IsLineHidden(lastVisibleLine)) {
            visibleCount++;
        }
        lastVisibleLine++;
    }
    lastVisibleLine = std::min(lastVisibleLine, lineCount);
    
    // Prepare visible range for large file optimizations
    buffer.PrepareVisibleRange(firstVisibleLine, lastVisibleLine);
    
    // Calculate max line width for horizontal scrollbar (use cached line lengths)
    float maxLineWidth = 0.0f;
    for (size_t i = firstVisibleLine; i < lastVisibleLine; ++i) {
        if (IsLineHidden(i)) continue;
        size_t lineLen = buffer.LineEnd(i) - buffer.LineStart(i);
        maxLineWidth = std::max(maxLineWidth, lineLen * m_CharWidth);
    }
    
    // Set total content size for scrolling (using visible line count)
    float totalHeight = visibleTotalLines * lineHeight;
    ImGui::Dummy(ImVec2(maxLineWidth + totalGutterWidth, totalHeight));
    
    // Get window position for drawing (not affected by scroll, we handle scroll ourselves)
    ImVec2 windowPos = ImGui::GetWindowPos();
    ImVec2 windowPadding = ImGui::GetStyle().WindowPadding;
    
    // Calculate the offset within the first visible line due to partial scroll
    float scrollOffset = m_ScrollY - (firstScreenLine * lineHeight);
    
    // Base position for drawing: window position + padding - partial line offset
    ImVec2 cursorPos = ImVec2(windowPos.x + windowPadding.x - m_ScrollX, 
                               windowPos.y + windowPadding.y - scrollOffset);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    
    // Fold gutter position (between line numbers and text)
    ImVec2 foldGutterPos = ImVec2(cursorPos.x + lineNumberWidth, cursorPos.y);
    
    // Text area position (after line numbers and fold gutter)
    ImVec2 textPos = ImVec2(cursorPos.x + totalGutterWidth, cursorPos.y);
    
    // Render Scope Highlight 
    RenderScope(buffer, textPos, lineHeight, firstVisibleLine, lastVisibleLine);

    // Render current line highlight
    if ((m_IsFocused || m_IsActive || m_ShowCompletion) && !m_HasSelection) {
        auto [cursorLine, cursorCol] = buffer.PosToLineCol(m_CursorPos);
        if (cursorLine >= firstVisibleLine && cursorLine < lastVisibleLine && !IsLineHidden(cursorLine)) {
            // Calculate screen row for cursor line
            size_t screenRow = 0;
            for (size_t i = firstVisibleLine; i < cursorLine; ++i) {
                if (!IsLineHidden(i)) screenRow++;
            }
            float y = textPos.y + screenRow * lineHeight;
            float textHeight = ImGui::GetTextLineHeight();
            drawList->AddRectFilled(
                ImVec2(cursorPos.x, y),
                ImVec2(cursorPos.x + contentSize.x, y + textHeight),
                m_Theme.currentLine
            );
        }
    }

    // Render line numbers and fold indicators
    bool foldClickConsumed = false;
    if (m_ShowLineNumbers) {
        RenderLineNumbers(buffer, cursorPos, lineHeight, firstVisibleLine, lastVisibleLine);
        foldClickConsumed = RenderFoldIndicators(buffer, foldGutterPos, lineHeight, firstVisibleLine, lastVisibleLine);
    }
    
    // Render selection background
    if (m_HasSelection) {
        RenderSelection(buffer, textPos, lineHeight, firstVisibleLine, lastVisibleLine);
    }
    
    // Render indent guides (Rainbow Indents)
    RenderIndentGuides(buffer, textPos, lineHeight, firstVisibleLine, lastVisibleLine);

    // Render syntax-highlighted text
    RenderText(buffer, textPos, lineHeight, firstVisibleLine, lastVisibleLine);
    
    // Render matching bracket highlight
    if (m_IsFocused || m_IsActive || m_ShowCompletion) {
        RenderMatchingBracket(buffer, textPos, lineHeight, firstVisibleLine);
    }

    // Render diagnostics (squiggles)
    RenderDiagnostics(buffer, textPos, lineHeight, firstVisibleLine, lastVisibleLine);

    // Render cursor (only if cursor line is visible)
    // Show cursor when focused, active (clicked but focus temporarily lost), or showing completion
    if ((m_IsFocused || m_IsActive || m_ShowCompletion) && !m_ReadOnly) {
        auto [cursorLine, cursorCol] = buffer.PosToLineCol(m_CursorPos);
        if (cursorLine >= firstVisibleLine && cursorLine < lastVisibleLine) {
            RenderCursor(buffer, textPos, lineHeight, firstVisibleLine);
        }
    }
    
    // Handle mouse input for clicking and dragging
    ImVec2 mousePos = ImGui::GetMousePos();
    
    // Helper lambda to convert mouse position to buffer position (accounting for folds)
    auto mouseToBufPos = [&](ImVec2 pos) -> size_t {
        float relX = pos.x - textPos.x;
        float relY = pos.y - textPos.y;
        
        // Convert screen row to buffer line (account for folded lines)
        size_t targetScreenRow = static_cast<size_t>(std::max(0.0f, relY) / lineHeight);
        size_t clickedLine = firstVisibleLine;
        size_t screenRow = 0;
        
        while (clickedLine < lineCount && screenRow < targetScreenRow) {
            if (!IsLineHidden(clickedLine)) {
                screenRow++;
            }
            clickedLine++;
        }
        
        // Skip any hidden lines at the click position
        while (clickedLine < lineCount && IsLineHidden(clickedLine)) {
            clickedLine++;
        }
        
        clickedLine = std::min(clickedLine, lineCount > 0 ? lineCount - 1 : 0);
        
        size_t clickedCol = static_cast<size_t>(std::max(0.0f, relX) / m_CharWidth);
        size_t lineLen = buffer.LineEnd(clickedLine) - buffer.LineStart(clickedLine);
        clickedCol = std::min(clickedCol, lineLen);
        
        return buffer.LineColToPos(clickedLine, clickedCol);
    };
    
    // Mouse click - start selection or set cursor (skip if fold gutter consumed the click)
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0) && !foldClickConsumed) {
        m_CursorPos = mouseToBufPos(mousePos);
        m_NeedsScrollToCursor = true;
        m_IsActive = true;  // User clicked in editor, mark as active
        
        // Close completion on mouse click navigation
        m_ShowCompletion = false;
        
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
    // Clear active state if user clicks outside this editor
    else if (ImGui::IsMouseClicked(0) && !ImGui::IsWindowHovered()) {
        m_IsActive = false;
    }
    
    // Mouse drag - extend selection
    if (m_IsDragging && ImGui::IsMouseDown(0)) {
        size_t newPos = mouseToBufPos(mousePos);
        if (newPos != m_CursorPos) {
            m_CursorPos = newPos;
            m_SelectionEnd = m_CursorPos;
            m_HasSelection = m_SelectionStart != m_SelectionEnd;
            m_CursorBlinkTimer = 0.0f;
            m_NeedsScrollToCursor = true;
        }
    }
    
    // Mouse release - stop dragging
    if (ImGui::IsMouseReleased(0)) {
        m_IsDragging = false;
    }
    
    // Handle keyboard input (Navigation and State Control)
    // Use m_IsActive in addition to m_IsFocused to handle keyboard after mode switches
    if (m_IsFocused || m_IsActive || m_ShowCompletion) {
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
    
    // Calculate completion position and buffer bounds before ending child
    ImVec2 cursorScreenPos;
    ImVec4 bufferRect; // x, y, x+w, y+h
    {
        auto [cursorLine, cursorCol] = buffer.PosToLineCol(m_CursorPos);
        size_t firstVisibleLine = static_cast<size_t>(m_ScrollY / lineHeight);
        
        // Calculate screen row for cursor (accounting for folds)
        size_t screenRow = 0;
        for (size_t i = firstVisibleLine; i < cursorLine; ++i) {
            if (!IsLineHidden(i)) screenRow++;
        }
        
        cursorScreenPos.x = textPos.x + cursorCol * m_CharWidth;
        cursorScreenPos.y = textPos.y + screenRow * lineHeight;
        
        // Get buffer region bounds (child window bounds)
        ImVec2 childMin = ImGui::GetWindowPos();
        ImVec2 childSize = ImGui::GetWindowSize();
        bufferRect = ImVec4(childMin.x, childMin.y, childMin.x + childSize.x, childMin.y + childSize.y);
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();

    // Render completion (needs to be last/on top and outside child to avoid clipping)
    if (m_ShowCompletion) {
        RenderCompletion(buffer, cursorScreenPos, bufferRect, lineHeight);
    }
    
    return buffer.IsModified();
}

void SyntaxEditor::HandleTextInput(TextBuffer& buffer) {
     ImGuiIO& io = ImGui::GetIO();
     if (m_ReadOnly) return;
     if (io.InputQueueCharacters.Size == 0) return;
     
     // Block text input in Command mode - only allow in Insert mode
     if (InputSystem::GetInstance().GetInputMode() != EditorInputMode::Insert) {
         // Clear the input queue to prevent buffering
         io.InputQueueCharacters.resize(0);
         return;
     }
     
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
            m_NeedsScrollToCursor = true;
        }
        if (textResult.textChanged) {
             buffer.MarkModified();
             m_FoldRangesDirty = true;  // Recalculate fold ranges after edit

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
                                    // Thread-safe: queue items for main thread processing
                                    std::lock_guard<std::mutex> lock(m_PendingCompletionMutex);
                                    m_PendingCompletionItems = items;
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
    
    size_t screenRow = 0;
    for (size_t i = firstLine; i < lastLine; ++i) {
        // Skip hidden (folded) lines
        if (IsLineHidden(i)) continue;
        
        float y = pos.y + screenRow * lineHeight;
        
        // Highlight current line number
        bool isFocused = (m_IsFocused || m_ShowCompletion);
        ImU32 color = (i == cursorLine && isFocused) ? m_Theme.text : m_Theme.lineNumber;
        
        char lineNumStr[16];
        snprintf(lineNumStr, sizeof(lineNumStr), "%zu", i + 1);
        
        // Right-align line numbers
        float textWidth = ImGui::CalcTextSize(lineNumStr).x;
        float x = pos.x + lineNumWidth - textWidth - m_CharWidth;
        
        drawList->AddText(ImVec2(x, y), color, lineNumStr);
        
        // If this line is folded, show "..." indicator after line number
        if (IsLineFolded(i)) {
            auto it = m_FoldEndLines.find(i);
            if (it != m_FoldEndLines.end()) {
                size_t foldedCount = it->second - i;
                char foldInfo[32];
                snprintf(foldInfo, sizeof(foldInfo), "... %zu lines", foldedCount);
                // This will be drawn in the text area by RenderText if needed
            }
        }
        
        screenRow++;
    }
    
    // Draw separator line
    drawList->AddLine(
        ImVec2(pos.x + lineNumWidth - m_CharWidth * 0.5f, pos.y),
        ImVec2(pos.x + lineNumWidth - m_CharWidth * 0.5f, pos.y + screenRow * lineHeight),
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
        
        if (token.depth > 0 && !m_Theme.rainbowBrackets.empty()) {
             std::string_view type = token.type;
             if (type == "{" || type == "}" || type == "(" || type == ")" || type == "[" || type == "]") {
                 color = m_Theme.rainbowBrackets[(token.depth - 1) % m_Theme.rainbowBrackets.size()];
             }
        }
        
        colorChanges.push_back({token.startByte, color, true});
        colorChanges.push_back({token.endByte, m_Theme.text, false});
    }
    
    std::sort(colorChanges.begin(), colorChanges.end(), [](const ColorChange& a, const ColorChange& b) {
        if (a.pos != b.pos) return a.pos < b.pos;
        // Ends come before Starts at same position
        return a.isStart < b.isStart;
    });
    
    // Render each visible line
    size_t screenRow = 0;
    for (size_t lineIdx = firstLine; lineIdx < lastLine; ++lineIdx) {
        // Skip hidden (folded) lines
        if (IsLineHidden(lineIdx)) continue;
        
        std::string_view lineText = buffer.LineView(lineIdx);
        
        // Strip trailing newlines/CR to prevent whitespace rendering issues
        while (!lineText.empty() && (lineText.back() == '\n' || lineText.back() == '\r')) {
            lineText.remove_suffix(1);
        }
        
        float y = pos.y + screenRow * lineHeight;
        float x = pos.x;
        
        // Draw fold ellipsis if this line is folded
        if (IsLineFolded(lineIdx)) {
            auto it = m_FoldEndLines.find(lineIdx);
            if (it != m_FoldEndLines.end()) {
                // Render the first line content, then append "..."
                // First render the line normally, then add ellipsis
            }
        }
        
        if (lineText.empty()) {
            screenRow++;
            continue;
        }
        
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
                // Check if this token should have rainbow color (re-apply logic for initial state)
                 if (token.depth > 0 && !m_Theme.rainbowBrackets.empty()) {
                    std::string_view type = token.type;
                    if (type == "{" || type == "}" || type == "(" || type == ")" || type == "[" || type == "]") {
                        currentColor = m_Theme.rainbowBrackets[(token.depth - 1) % m_Theme.rainbowBrackets.size()];
                    }
                }
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
                if (charIdx > spanStart) {
                    RenderSpan(drawList, lineText, spanStart, charIdx, x, y, spanColor);
                }
                spanStart = charIdx;

                if (it->isStart) {
                    spanColor = it->color;
                } else {
                    // Revert to text
                    spanColor = m_Theme.text;
                }
                ++it;
            }
            
            ++charIdx;
            
            // Advance iterator past positions we've passed (should not be needed if step-by-step logic is correct)
            while (it != colorChanges.end() && it->pos < lineStart + charIdx) {
                 if (it->isStart) {
                    spanColor = it->color;
                } else {
                    spanColor = m_Theme.text;
                }
                ++it;
            }
        }
        
        // Render final span
        if (lineText.length() > spanStart) {
            RenderSpan(drawList, lineText, spanStart, lineText.length(), x, y, spanColor);
        }
        
        // Draw fold indicator "..." after the line if folded
        if (IsLineFolded(lineIdx)) {
            auto foldIt = m_FoldEndLines.find(lineIdx);
            if (foldIt != m_FoldEndLines.end()) {
                size_t foldedLines = foldIt->second - lineIdx;
                char foldText[32];
                snprintf(foldText, sizeof(foldText), " ... %zu lines", foldedLines);
                drawList->AddText(ImVec2(x + 4.0f, y), IM_COL32(128, 128, 128, 255), foldText);
            }
        }
        
        screenRow++;
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
    
    // Calculate screen row for cursor (accounting for folds)
    size_t screenRow = 0;
    for (size_t i = firstLine; i < cursorLine; ++i) {
        if (!IsLineHidden(i)) screenRow++;
    }
    
    // Calculate cursor screen position
    float x = textPos.x + cursorCol * m_CharWidth;
    float y = textPos.y + screenRow * lineHeight;
    
    // Draw cursor based on input mode
    // Insert mode: thin vertical bar | Command mode: horizontal underscore
    bool isInsertMode = InputSystem::GetInstance().GetInputMode() == EditorInputMode::Insert;
    float textHeight = ImGui::GetTextLineHeight();
    
    if (isInsertMode) {
        // Thin vertical bar cursor for Insert mode
        drawList->AddRectFilled(
            ImVec2(x, y),
            ImVec2(x + 2, y + textHeight),
            m_Theme.cursor
        );
    } else {
        // Horizontal underscore cursor for Command mode
        float underscoreHeight = 2.0f;
        drawList->AddRectFilled(
            ImVec2(x, y + textHeight - underscoreHeight),
            ImVec2(x + m_CharWidth, y + textHeight),
            m_Theme.cursor
        );
    }
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
    
    // Build screen row mapping
    size_t screenRow = 0;
    for (size_t line = firstLine; line <= endLine && line < lastLine; ++line) {
        if (IsLineHidden(line)) continue;
        
        if (line >= startLine) {
            size_t lineLen = buffer.LineEnd(line) - buffer.LineStart(line);
            float y = textPos.y + screenRow * lineHeight;
            
            size_t colStart = (line == startLine) ? startCol : 0;
            size_t colEnd = (line == endLine) ? endCol : lineLen;
            
            float xStart = textPos.x + colStart * m_CharWidth;
            float xEnd = textPos.x + colEnd * m_CharWidth;
            
            drawList->AddRectFilled(
                ImVec2(xStart, y),
                ImVec2(xEnd, y + textHeight),
                m_Theme.selection
            );
        }
        
        screenRow++;
    }
}

void SyntaxEditor::RenderScope(TextBuffer& buffer, const ImVec2& textPos, float lineHeight, size_t firstLine, size_t lastLine) {
    auto [start, end] = buffer.GetScopeRange(m_CursorPos);
    if (start == end) return;
    
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    auto [startLine, startCol] = buffer.PosToLineCol(start);
    auto [endLine, endCol] = buffer.PosToLineCol(end);
    
    if (endLine < firstLine || startLine >= lastLine) return;
    
    size_t drawStartLine = std::max(startLine, firstLine);
    size_t drawEndLine = std::min(endLine, lastLine - 1);
    
    // Build screen row mapping for the render range
    size_t screenRow = 0;
    for (size_t i = firstLine; i <= drawEndLine; ++i) {
        if (IsLineHidden(i)) continue;
        
        if (i >= drawStartLine) {
            float y = textPos.y + screenRow * lineHeight;
            
            float xStart = textPos.x;
            float xEnd = textPos.x + ImGui::GetContentRegionAvail().x;
            
            // Only constrain width on first and last lines for block effect
            if (i == startLine) xStart += startCol * m_CharWidth;
            if (i == endLine) xEnd = textPos.x + endCol * m_CharWidth;
            
            // Ensure min width for empty scope or cursor
            if (xEnd <= xStart) xEnd = xStart + m_CharWidth;
            
            drawList->AddRectFilled(ImVec2(xStart, y), ImVec2(xEnd, y + lineHeight), m_Theme.scopeBackground);
        }
        
        screenRow++;
    }
}

void SyntaxEditor::RenderIndentGuides(TextBuffer& buffer, const ImVec2& textPos, float lineHeight, size_t firstLine, size_t lastLine) {
    if (m_Theme.rainbowIndents.empty()) return;
    
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    
    // Offset x starting position by line number width if shown
    // textPos already includes line number width from Render()
    
    size_t screenRow = 0;
    for (size_t i = firstLine; i < lastLine; ++i) {
        // Skip hidden (folded) lines
        if (IsLineHidden(i)) continue;
        
        std::string_view line = buffer.LineView(i);
        size_t indentLevel = 0;
        size_t spaces = 0;
        
        for (char c : line) {
            if (c == ' ') {
                spaces++;
                if (spaces % m_TabSize == 0) {
                     indentLevel++;
                }
            } else if (c == '\t') {
                spaces = 0;
                indentLevel++;
            } else {
                break;
            }
        }
        
        // Draw guide lines
        // We draw up to indentLevel
        // e.g. if indentLevel is 2, we draw lines at indent 0 and 1? 
        // Or indent 1 and 2?
        // Usually guides are drawn AT the indent columns.
        
        for (size_t j = 0; j < indentLevel; ++j) {
            float x = textPos.x + (j * m_TabSize) * m_CharWidth;
            float y = textPos.y + screenRow * lineHeight;
            
            ImU32 color = m_Theme.rainbowIndents[j % m_Theme.rainbowIndents.size()];
            drawList->AddLine(ImVec2(x, y), ImVec2(x, y + lineHeight), color, 1.0f);
        }
        
        screenRow++;
    }
}

void SyntaxEditor::RenderMatchingBracket(TextBuffer& buffer, const ImVec2& textPos, float lineHeight, size_t firstLine) {
    size_t bracketPos = m_CursorPos;
    size_t matchPos = buffer.GetMatchingBracket(bracketPos);
    
    // Check previous character if current is not a bracket
    if (matchPos == SIZE_MAX && m_CursorPos > 0) {
        bracketPos = m_CursorPos - 1;
        matchPos = buffer.GetMatchingBracket(bracketPos);
    }
    
    if (matchPos != SIZE_MAX) {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        float textHeight = ImGui::GetTextLineHeight();
        
        auto drawBox = [&](size_t pos) {
            auto [line, col] = buffer.PosToLineCol(pos);
            if (line < firstLine || IsLineHidden(line)) return;
            
            // Calculate screen row (accounting for folds)
            size_t screenRow = 0;
            for (size_t i = firstLine; i < line; ++i) {
                if (!IsLineHidden(i)) screenRow++;
            }
            
            float x = textPos.x + col * m_CharWidth;
            float y = textPos.y + screenRow * lineHeight;
            
            drawList->AddRect(
                ImVec2(x, y), 
                ImVec2(x + m_CharWidth, y + textHeight), 
                IM_COL32(255, 200, 0, 200) // Gold color for matching bracket box
            );
        };
        
        drawBox(bracketPos);
        drawBox(matchPos);
    }
}

// Code Folding Implementation

void SyntaxEditor::UpdateFoldRanges(TextBuffer& buffer) {
    if (!m_FoldRangesDirty) return;
    
    m_FoldRanges = buffer.GetFoldRanges();
    m_FoldEndLines.clear();
    
    for (const auto& range : m_FoldRanges) {
        m_FoldEndLines[range.startLine] = range.endLine;
    }
    
    m_FoldRangesDirty = false;
}

bool SyntaxEditor::IsLineFolded(size_t line) const {
    return m_FoldedLines.count(line) > 0;
}

bool SyntaxEditor::IsLineHidden(size_t line) const {
    // A line is hidden if it's inside any actively folded region (but not the start line)
    for (size_t foldStart : m_FoldedLines) {
        auto it = m_FoldEndLines.find(foldStart);
        if (it != m_FoldEndLines.end()) {
            // Hidden if: after fold start AND at or before fold end
            if (line > foldStart && line <= it->second) {
                return true;
            }
        }
    }
    return false;
}

size_t SyntaxEditor::ScreenLineToBufferLine(size_t screenLine, size_t maxLine) const {
    if (m_FoldedLines.empty()) {
        return screenLine; // No folds, direct mapping
    }
    
    size_t bufferLine = 0;
    size_t visibleCount = 0;
    
    while (bufferLine < maxLine && visibleCount < screenLine) {
        if (!IsLineHidden(bufferLine)) {
            visibleCount++;
        }
        bufferLine++;
    }
    
    // Find the next visible line at this screen position
    while (bufferLine < maxLine && IsLineHidden(bufferLine)) {
        bufferLine++;
    }
    
    return std::min(bufferLine, maxLine > 0 ? maxLine - 1 : 0);
}

size_t SyntaxEditor::BufferLineToScreenLine(size_t bufferLine) const {
    if (m_FoldedLines.empty()) {
        return bufferLine; // No folds, direct mapping
    }
    
    size_t screenLine = 0;
    for (size_t i = 0; i < bufferLine; ++i) {
        if (!IsLineHidden(i)) {
            screenLine++;
        }
    }
    return screenLine;
}

size_t SyntaxEditor::GetHiddenLineCount() const {
    if (m_FoldedLines.empty()) {
        return 0;
    }
    
    size_t hidden = 0;
    for (size_t foldStart : m_FoldedLines) {
        auto it = m_FoldEndLines.find(foldStart);
        if (it != m_FoldEndLines.end()) {
            // Count lines from (foldStart+1) to foldEnd inclusive
            hidden += (it->second - foldStart);
        }
    }
    return hidden;
}

bool SyntaxEditor::RenderFoldIndicators(TextBuffer& buffer, const ImVec2& pos, float lineHeight, 
                                         size_t firstLine, size_t lastLine) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImGuiIO& io = ImGui::GetIO();
    ImFont* font = ImGui::GetFont();
    
    float foldGutterWidth = GetFoldGutterWidth();
    float fontSize = ImGui::GetFontSize() * 0.75f; // Slightly smaller for low profile
    
    bool clickConsumed = false;
    size_t screenRow = 0;
    for (size_t i = firstLine; i < lastLine; ++i) {
        // Skip hidden lines
        if (IsLineHidden(i)) continue;
        
        // Check if this line starts a foldable region
        auto it = m_FoldEndLines.find(i);
        if (it != m_FoldEndLines.end()) {
            float y = pos.y + screenRow * lineHeight;
            
            bool isFolded = IsLineFolded(i);
            bool isHovered = false;
            
            // Hit test for click - entire fold gutter column
            ImVec2 hitMin(pos.x, y);
            ImVec2 hitMax(pos.x + foldGutterWidth, y + lineHeight);
            
            ImVec2 mousePos = io.MousePos;
            if (mousePos.x >= hitMin.x && mousePos.x <= hitMax.x &&
                mousePos.y >= hitMin.y && mousePos.y <= hitMax.y) {
                isHovered = true;
                
                // Handle click to toggle fold
                if (ImGui::IsMouseClicked(0)) {
                    if (isFolded) {
                        m_FoldedLines.erase(i);
                    } else {
                        m_FoldedLines.insert(i);
                    }
                    clickConsumed = true;  // Prevent text area from also handling this click
                }
            }
            
            // Draw the fold indicator using font icon
            ImU32 color = isHovered ? IM_COL32(255, 255, 255, 255) : m_Theme.lineNumber;
            const char* icon = isFolded ? ICON_NF_CHEVRON_RIGHT : ICON_NF_CHEVRON_DOWN;
            
            // Center the icon in the gutter (adjusted up for visual alignment)
            ImVec2 iconSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, icon);
            float iconX = pos.x + (foldGutterWidth - iconSize.x) * 0.5f;
            float iconY = y + (lineHeight - iconSize.y) * 0.4f;
            
            drawList->AddText(font, fontSize, ImVec2(iconX, iconY), color, icon);
        }
        
        screenRow++;
    }
    
    return clickConsumed;
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
                m_NeedsScrollToCursor = true;
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
            m_NeedsScrollToCursor = true;
            
            // Close completion on cursor navigation (but not from text input)
            if (result.cursorMoved && !result.textChanged && m_ShowCompletion) {
                m_ShowCompletion = false;
            }
        }
        if (result.selectionChanged) {
            if (result.newSelectionStart) m_SelectionStart = *result.newSelectionStart;
            if (result.newSelectionEnd) m_SelectionEnd = *result.newSelectionEnd;
            m_HasSelection = m_SelectionStart != m_SelectionEnd;
        }
        if (result.textChanged) {
            modified = true;
            
            // Update completion when text changed (e.g., backspace)
            if (m_ShowCompletion && m_CursorPos > 0) {
                // Find current word prefix at cursor
                size_t start = m_CursorPos;
                while (start > 0) {
                    char c = buffer.At(start - 1);
                    if (!std::isalnum(c) && c != '_') break;
                    start--;
                }
                
                if (m_CursorPos > start) {
                    // Still in a word, update suggestions
                    std::string prefix = buffer.Substring(start, m_CursorPos - start);
                    std::vector<std::string> words = buffer.GetWordCompletions(prefix);
                    
                    const auto* langPtr = buffer.GetLanguage();
                    if (langPtr) {
                        for (const auto& mapping : langPtr->highlightMappings) {
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
                        for (const auto& w : words) {
                            LSPCompletionItem item;
                            item.label = w;
                            item.kind = 1;
                            item.insertText = w;
                            item.detail = "Buffer";
                            m_CompletionItems.push_back(item);
                        }
                        m_SelectedCompletionIndex = 0;
                    } else {
                        m_ShowCompletion = false;
                    }
                } else {
                    // No longer in a word, close completion
                    m_ShowCompletion = false;
                }
            }
        }
    }
    
    // Handle text input through input manager
    // REMOVED DUPLICATE HANDLING - Logic moved to HandleTextInput
    // if (io.InputQueueCharacters.Size > 0) { ... }
    
    if (modified) {
        buffer.MarkModified();
        m_FoldRangesDirty = true;  // Recalculate fold ranges after edit
    }
    return result.handled;
}


void SyntaxEditor::RenderDiagnostics(TextBuffer& buffer, const ImVec2& textPos, float lineHeight, size_t firstLine, size_t lastLine) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 mousePos = ImGui::GetMousePos();
    bool tooltipShown = false;
    
    // Simple squiggly line rendering
    // A real implementation would use a sine wave or texture
    
    size_t screenRow = 0;
    for (size_t line = firstLine; line < lastLine; ++line) {
        // Skip hidden (folded) lines
        if (IsLineHidden(line)) continue;
        
        // Since diagnostics are mapped by 0-based line index in syntax_editor
        if (m_Diagnostics.count(line) > 0) {
            float y = textPos.y + screenRow * lineHeight + lineHeight; // Bottom of line
            
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
                        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 16.0f);
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
        
        screenRow++;
    }

    if (tooltipShown) {
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void SyntaxEditor::RenderCompletion(TextBuffer& buffer, const ImVec2& cursorScreenPos, const ImVec4& bufferRect, float lineHeight) {
    if (m_CompletionItems.empty()) return;

    float itemLineHeight = ImGui::GetTextLineHeightWithSpacing();
    
    // Calculate popup size
    float width = 300.0f;
    float padding = ImGui::GetStyle().WindowPadding.y * 2;
    float contentHeight = m_CompletionItems.size() * itemLineHeight; 
    float maxHeight = 200.0f;
    float height = std::min(contentHeight + padding, maxHeight);
    
    // Buffer bounds
    float bufLeft = bufferRect.x;
    float bufTop = bufferRect.y;
    float bufRight = bufferRect.z;
    float bufBottom = bufferRect.w;
    
    ImVec2 pos;
    
    // Horizontal positioning: prefer right of cursor, but stay in buffer
    pos.x = cursorScreenPos.x;
    if (pos.x + width > bufRight) {
        // Try left of cursor
        pos.x = cursorScreenPos.x - width;
        if (pos.x < bufLeft) {
            // Clamp to buffer left edge
            pos.x = bufLeft;
        }
    }
    
    // Vertical positioning: prefer below cursor, flip above if no space
    float spaceBelow = bufBottom - (cursorScreenPos.y + lineHeight);
    float spaceAbove = cursorScreenPos.y - bufTop;
    
    if (spaceBelow >= height) {
        // Place below cursor
        pos.y = cursorScreenPos.y + lineHeight;
    } else if (spaceAbove >= height) {
        // Place above cursor
        pos.y = cursorScreenPos.y - height;
    } else {
        // Not enough space either way, pick the side with more room
        if (spaceBelow >= spaceAbove) {
            pos.y = cursorScreenPos.y + lineHeight;
        } else {
            pos.y = cursorScreenPos.y - height;
        }
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
