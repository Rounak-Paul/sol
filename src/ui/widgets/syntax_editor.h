#pragma once

#include "core/text/text_buffer.h"
#include "core/text/undo_tree.h"
#include "ui/input/input_manager.h"
#include "core/lsp/lsp_types.h"
#include <imgui.h>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <map>
#include <mutex>
#include <optional>

namespace sol {

// Color theme for syntax highlighting
struct SyntaxTheme {
    ImU32 background = IM_COL32(30, 30, 30, 255);
    ImU32 text = IM_COL32(212, 212, 212, 255);
    ImU32 keyword = IM_COL32(86, 156, 214, 255);      // Blue
    ImU32 type = IM_COL32(78, 201, 176, 255);         // Teal
    ImU32 function = IM_COL32(220, 220, 170, 255);    // Yellow
    ImU32 variable = IM_COL32(156, 220, 254, 255);    // Light blue
    ImU32 string = IM_COL32(206, 145, 120, 255);      // Orange
    ImU32 number = IM_COL32(181, 206, 168, 255);      // Light green
    ImU32 comment = IM_COL32(106, 153, 85, 255);      // Green
    ImU32 op = IM_COL32(212, 212, 212, 255);          // White
    ImU32 punctuation = IM_COL32(212, 212, 212, 255); // White
    ImU32 macro = IM_COL32(189, 99, 197, 255);        // Purple
    ImU32 constant = IM_COL32(100, 150, 220, 255);    // Blue
    ImU32 error = IM_COL32(244, 71, 71, 255);         // Red
    ImU32 lineNumber = IM_COL32(100, 100, 100, 255);  // Gray
    
    ImU32 popupBg = 0xFF252526;
    ImU32 popupBorder = 0xFF454545;
    ImU32 popupText = 0xFFCCCCCC;
    ImU32 popupSelected = 0xFF094771;
    ImU32 currentLine = IM_COL32(40, 40, 40, 255);    // Slightly lighter
    ImU32 selection = IM_COL32(38, 79, 120, 255);     // Selection blue
    ImU32 cursor = IM_COL32(255, 255, 255, 255);      // White cursor
    
    ImU32 GetColor(HighlightGroup group) const {
        switch (group) {
            case HighlightGroup::Keyword: return keyword;
            case HighlightGroup::Type: return type;
            case HighlightGroup::Function: return function;
            case HighlightGroup::Variable: return variable;
            case HighlightGroup::String: return string;
            case HighlightGroup::Number: return number;
            case HighlightGroup::Comment: return comment;
            case HighlightGroup::Operator: return op;
            case HighlightGroup::Punctuation: return punctuation;
            case HighlightGroup::Macro: return macro;
            case HighlightGroup::Constant: return constant;
            case HighlightGroup::Error: return error;
            default: return text;
        }
    }
};

class SyntaxEditor {
public:
    SyntaxEditor();
    ~SyntaxEditor() = default;
    
    // Render the editor - returns true if content was modified
    bool Render(const char* label, TextBuffer& buffer, const ImVec2& size = ImVec2(0, 0));
    
    // Configuration
    void SetTheme(const SyntaxTheme& theme) { m_Theme = theme; }
    void SetShowLineNumbers(bool show) { m_ShowLineNumbers = show; }
    void SetTabSize(int size) { m_TabSize = size; }
    void SetReadOnly(bool readOnly) { m_ReadOnly = readOnly; }
    
    // Input mode
    InputManager& GetInputManager() { return m_InputManager; }
    
    // Undo/Redo tree
    UndoTree& GetUndoTree() { return m_UndoTree; }
    
    // State
    size_t GetCursorPos() const { return m_CursorPos; }
    void SetCursorPos(size_t pos) { m_CursorPos = pos; }

    void UpdateDiagnostics(const std::string& path, const std::vector<LSPDiagnostic>& diagnostics);
    
private:
    bool HandleInput(TextBuffer& buffer);
    void HandleTextInput(TextBuffer& buffer);
    void RenderCompletion(TextBuffer& buffer, const ImVec2& cursorScreenPos, const ImVec4& bufferRect, float lineHeight);
    void RenderDiagnostics(TextBuffer& buffer, const ImVec2& textPos, float lineHeight, size_t firstLine, size_t lastLine);

    void RenderLineNumbers(TextBuffer& buffer, const ImVec2& pos, float lineHeight, size_t firstLine, size_t lastLine);
    void RenderText(TextBuffer& buffer, const ImVec2& pos, float lineHeight, size_t firstLine, size_t lastLine);
    void RenderSpan(ImDrawList* drawList, std::string_view lineText, size_t start, size_t end, float& x, float y, ImU32 color);
    void RenderCursor(TextBuffer& buffer, const ImVec2& textPos, float lineHeight, size_t firstLine);
    void RenderSelection(TextBuffer& buffer, const ImVec2& textPos, float lineHeight, size_t firstLine, size_t lastLine);
    void RenderStatusLine(TextBuffer& buffer, const ImVec2& pos, float width);
    
    float GetCharWidth() const;
    float GetLineNumberWidth(size_t lineCount) const;
    
    SyntaxTheme m_Theme;
    bool m_ShowLineNumbers = true;
    int m_TabSize = 4;
    bool m_ReadOnly = false;
    
    // Input handling
    InputManager m_InputManager;
    UndoTree m_UndoTree;
    
    // Cursor and selection state
    size_t m_CursorPos = 0;
    size_t m_SelectionStart = 0;
    size_t m_SelectionEnd = 0;
    bool m_HasSelection = false;
    bool m_IsDragging = false;
    
    // Scroll state
    float m_ScrollX = 0.0f;
    float m_ScrollY = 0.0f;
    bool m_NeedsScrollToCursor = false;  // Only scroll when cursor actually moves
    
    // Cached for current frame
    bool m_IsFocused = false;
    float m_CharWidth = 0.0f;
    
    // Completion state
    bool m_ShowCompletion = false;
    std::vector<LSPCompletionItem> m_CompletionItems;
    int m_SelectedCompletionIndex = 0;
    
    // Thread-safe pending completion from LSP (written from background, read on main)
    std::mutex m_PendingCompletionMutex;
    std::optional<std::vector<LSPCompletionItem>> m_PendingCompletionItems;
    
    // Diagnostics state
    std::string m_LastDiagnosticsPath;
    std::map<size_t, std::vector<LSPDiagnostic>> m_Diagnostics;

    // Blink timer for cursor
    float m_CursorBlinkTimer = 0.0f;
    static constexpr float CURSOR_BLINK_RATE = 0.5f;
};

} // namespace sol
