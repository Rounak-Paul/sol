#pragma once

#include "input_mode.h"
#include <memory>
#include <vector>
#include <string>
#include <unordered_map>

namespace sol {

class StandardMode;
class VimMode;

class InputManager {
public:
    InputManager();
    ~InputManager();
    
    // Mode management
    void RegisterMode(const std::string& name, std::unique_ptr<InputMode> mode);
    bool SetActiveMode(const std::string& name);
    const std::string& GetActiveModeName() const { return m_ActiveModeName; }
    InputMode* GetActiveMode() const { return m_ActiveMode; }
    
    // Get specific modes
    StandardMode* GetStandardMode() const;
    VimMode* GetVimMode() const;
    
    // Enable/disable vim mode (convenience)
    void SetVimEnabled(bool enabled);
    bool IsVimEnabled() const { return m_VimEnabled; }
    
    // Input handling - delegates to active mode
    InputResult HandleKeyboard(EditorState& state);
    InputResult HandleTextInput(EditorState& state, const ImWchar* chars, int count);
    InputResult HandleMouse(EditorState& state, ImVec2 mousePos, bool clicked, bool dragging, bool released);
    
    // Mode indicator for status bar
    const char* GetModeIndicator() const;
    
    // Render any mode-specific UI (like vim command line)
    void RenderUI(EditorState& state);
    
    // Get list of available modes
    std::vector<std::string> GetModeNames() const;
    
private:
    std::unordered_map<std::string, std::unique_ptr<InputMode>> m_Modes;
    std::string m_ActiveModeName;
    InputMode* m_ActiveMode = nullptr;
    bool m_VimEnabled = false;
};

} // namespace sol
