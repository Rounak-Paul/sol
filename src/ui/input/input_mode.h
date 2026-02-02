#pragma once

#include <imgui.h>
#include <string>
#include <functional>
#include <optional>

namespace sol {

class TextBuffer;
class UndoTree;

// Result of input handling
struct InputResult {
    bool handled = false;           // Was the input consumed?
    bool textChanged = false;       // Did the text content change?
    bool cursorMoved = false;       // Did the cursor move?
    bool selectionChanged = false;  // Did selection change?
    std::optional<size_t> newCursorPos;
    std::optional<size_t> newSelectionStart;
    std::optional<size_t> newSelectionEnd;
};

// Editor state passed to input modes
struct EditorState {
    TextBuffer* buffer = nullptr;
    UndoTree* undoTree = nullptr;
    
    size_t cursorPos = 0;
    size_t selectionStart = 0;
    size_t selectionEnd = 0;
    bool hasSelection = false;
    
    // View info
    size_t firstVisibleLine = 0;
    size_t lastVisibleLine = 0;
    float charWidth = 0.0f;
    float lineHeight = 0.0f;
};

// Base class for input modes
class InputMode {
public:
    virtual ~InputMode() = default;
    
    // Get mode name for status display
    virtual const char* GetName() const = 0;
    
    // Get mode indicator (e.g., "NORMAL", "INSERT", "VISUAL" for vim)
    virtual const char* GetIndicator() const = 0;
    
    // Handle keyboard input - called every frame when focused
    virtual InputResult HandleKeyboard(EditorState& state) = 0;
    
    // Handle text input (character input)
    virtual InputResult HandleTextInput(EditorState& state, const ImWchar* chars, int count) = 0;
    
    // Handle mouse input
    virtual InputResult HandleMouse(EditorState& state, ImVec2 mousePos, bool clicked, bool dragging, bool released) = 0;
    
    // Called when mode is activated
    virtual void OnActivate(EditorState& state) {}
    
    // Called when mode is deactivated
    virtual void OnDeactivate(EditorState& state) {}
    
    // Render any mode-specific UI (like vim command line)
    virtual void RenderUI(EditorState& state) {}
    
    // Should text input be processed? (e.g., vim normal mode ignores text input)
    virtual bool WantsTextInput() const { return true; }
};

// Common text operations used by input modes
namespace TextOps {
    // Motion helpers
    size_t WordStart(const TextBuffer& buffer, size_t pos);
    size_t WordEnd(const TextBuffer& buffer, size_t pos);
    size_t NextWord(const TextBuffer& buffer, size_t pos);
    size_t PrevWord(const TextBuffer& buffer, size_t pos);
    size_t LineStart(const TextBuffer& buffer, size_t pos);
    size_t LineEnd(const TextBuffer& buffer, size_t pos);
    size_t FirstNonWhitespace(const TextBuffer& buffer, size_t pos);
    size_t ParagraphStart(const TextBuffer& buffer, size_t pos);
    size_t ParagraphEnd(const TextBuffer& buffer, size_t pos);
    size_t MatchingBracket(const TextBuffer& buffer, size_t pos);
    
    // Find operations
    size_t FindChar(const TextBuffer& buffer, size_t pos, char c, bool forward = true);
    size_t FindString(const TextBuffer& buffer, size_t pos, const std::string& str, bool forward = true);
    
    // Text manipulation with undo support
    void Insert(TextBuffer& buffer, UndoTree& undo, size_t pos, const std::string& text, size_t cursorBefore);
    void Delete(TextBuffer& buffer, UndoTree& undo, size_t pos, size_t len, size_t cursorBefore);
    void Replace(TextBuffer& buffer, UndoTree& undo, size_t pos, size_t len, const std::string& text, size_t cursorBefore);
    
    // Undo/redo operations
    std::optional<size_t> Undo(TextBuffer& buffer, UndoTree& undo);
    std::optional<size_t> Redo(TextBuffer& buffer, UndoTree& undo);
}

} // namespace sol
