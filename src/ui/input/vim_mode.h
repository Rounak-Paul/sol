#pragma once

#include "input_mode.h"
#include <string>
#include <optional>
#include <functional>

namespace sol {

// Vim mode states
enum class VimState {
    Normal,     // Default mode for navigation
    Insert,     // Text insertion mode
    Visual,     // Character-wise visual selection
    VisualLine, // Line-wise visual selection
    VisualBlock,// Block visual selection (not fully implemented)
    Command,    // : command mode
    Search,     // / or ? search mode
    Replace,    // r replace single char
    Operator    // Waiting for motion after operator (d, c, y, etc.)
};

// Vim operator types
enum class VimOperator {
    None,
    Delete,     // d
    Change,     // c
    Yank,       // y
    Indent,     // >
    Unindent,   // <
    Format,     // gq
    Uppercase,  // gU
    Lowercase,  // gu
};

class VimMode : public InputMode {
public:
    VimMode();
    
    const char* GetName() const override { return "Vim"; }
    const char* GetIndicator() const override;
    
    InputResult HandleKeyboard(EditorState& state) override;
    InputResult HandleTextInput(EditorState& state, const ImWchar* chars, int count) override;
    InputResult HandleMouse(EditorState& state, ImVec2 mousePos, bool clicked, bool dragging, bool released) override;
    
    void OnActivate(EditorState& state) override;
    void OnDeactivate(EditorState& state) override;
    void RenderUI(EditorState& state) override;
    
    bool WantsTextInput() const override { return m_State == VimState::Insert || m_State == VimState::Command || m_State == VimState::Search; }
    
    // Mode state
    VimState GetState() const { return m_State; }
    void SetState(VimState state) { m_State = state; }
    
private:
    // Mode handlers
    InputResult HandleNormalMode(EditorState& state);
    InputResult HandleInsertMode(EditorState& state);
    InputResult HandleVisualMode(EditorState& state);
    InputResult HandleCommandMode(EditorState& state);
    InputResult HandleSearchMode(EditorState& state);
    InputResult HandleOperatorPending(EditorState& state);
    
    // Motion helpers - return new cursor position
    std::optional<size_t> ExecuteMotion(EditorState& state, char motion, int count = 1);
    std::optional<std::pair<size_t, size_t>> GetMotionRange(EditorState& state, char motion, int count = 1);
    
    // Text object helpers - return range (start, end)
    std::optional<std::pair<size_t, size_t>> GetTextObject(EditorState& state, char obj, bool inner);
    
    // Operator execution
    void ExecuteOperator(EditorState& state, size_t start, size_t end);
    
    // State
    VimState m_State = VimState::Normal;
    VimOperator m_PendingOperator = VimOperator::None;
    
    // Count for repetition (e.g., 5j moves down 5 lines)
    int m_Count = 0;
    bool m_HasCount = false;
    
    // Pending keys for multi-key commands
    std::string m_PendingKeys;
    
    // Command/search buffer
    std::string m_CommandBuffer;
    std::string m_LastSearch;
    bool m_SearchForward = true;
    
    // Register for yanked text
    std::string m_Register;
    bool m_RegisterIsLine = false;  // Was yanked as whole line(s)?
    
    // Visual mode anchor
    size_t m_VisualAnchor = 0;
    
    // Last edit for dot repeat
    struct LastEdit {
        VimOperator op;
        char motion;
        int count;
        std::string insertedText;
    };
    std::optional<LastEdit> m_LastEdit;
    
    // Mark positions
    std::unordered_map<char, size_t> m_Marks;
    
    // Jump list for Ctrl-O/Ctrl-I
    std::vector<size_t> m_JumpList;
    size_t m_JumpIndex = 0;
    
    // Helper methods
    void EnterInsertMode(EditorState& state, bool afterCursor = false);
    void EnterVisualMode(EditorState& state, VimState visualType);
    void ExitToNormal(EditorState& state);
    void AddCount(int digit);
    int GetCount() const { return m_HasCount ? m_Count : 1; }
    void ResetCount() { m_Count = 0; m_HasCount = false; }
    
    void PushJump(size_t pos);
    std::optional<size_t> JumpBack();
    std::optional<size_t> JumpForward();
};

} // namespace sol
