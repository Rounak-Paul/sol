#pragma once

#include "input_mode.h"

namespace sol {

// Standard editor mode (like VS Code without vim)
class StandardMode : public InputMode {
public:
    const char* GetName() const override { return "Standard"; }
    const char* GetIndicator() const override { return ""; }
    
    InputResult HandleKeyboard(EditorState& state) override;
    InputResult HandleTextInput(EditorState& state, const ImWchar* chars, int count) override;
    InputResult HandleMouse(EditorState& state, ImVec2 mousePos, bool clicked, bool dragging, bool released) override;
    
    bool WantsTextInput() const override { return true; }
    
private:
    // Helper methods
    InputResult HandleNavigation(EditorState& state, ImGuiKey key, bool shift, bool ctrl);
    InputResult HandleEditing(EditorState& state, ImGuiKey key, bool ctrl);
    
    void DeleteSelection(EditorState& state);
    void InsertText(EditorState& state, const std::string& text);
    
    // For mouse dragging
    bool m_IsDragging = false;
};

} // namespace sol
