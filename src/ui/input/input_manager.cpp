#include "input_manager.h"
#include "standard_mode.h"

namespace sol {

InputManager::InputManager() {
    // Register standard mode
    RegisterMode("standard", std::make_unique<StandardMode>());
    SetActiveMode("standard");
}

InputManager::~InputManager() = default;

void InputManager::RegisterMode(const std::string& name, std::unique_ptr<InputMode> mode) {
    m_Modes[name] = std::move(mode);
}

bool InputManager::SetActiveMode(const std::string& name) {
    auto it = m_Modes.find(name);
    if (it == m_Modes.end()) {
        return false;
    }
    
    // Deactivate current mode
    if (m_ActiveMode) {
        EditorState dummyState;
        m_ActiveMode->OnDeactivate(dummyState);
    }
    
    m_ActiveModeName = name;
    m_ActiveMode = it->second.get();
    
    // Activate new mode
    EditorState dummyState;
    m_ActiveMode->OnActivate(dummyState);
    
    return true;
}

StandardMode* InputManager::GetStandardMode() const {
    auto it = m_Modes.find("standard");
    if (it != m_Modes.end()) {
        return static_cast<StandardMode*>(it->second.get());
    }
    return nullptr;
}

InputResult InputManager::HandleKeyboard(EditorState& state) {
    if (!m_ActiveMode) {
        return {};
    }
    return m_ActiveMode->HandleKeyboard(state);
}

InputResult InputManager::HandleTextInput(EditorState& state, const ImWchar* chars, int count) {
    if (!m_ActiveMode) {
        return {};
    }
    return m_ActiveMode->HandleTextInput(state, chars, count);
}

InputResult InputManager::HandleMouse(EditorState& state, ImVec2 mousePos, bool clicked, bool dragging, bool released) {
    if (!m_ActiveMode) {
        return {};
    }
    return m_ActiveMode->HandleMouse(state, mousePos, clicked, dragging, released);
}

const char* InputManager::GetModeIndicator() const {
    if (!m_ActiveMode) {
        return "";
    }
    return m_ActiveMode->GetIndicator();
}

void InputManager::RenderUI(EditorState& state) {
    if (m_ActiveMode) {
        m_ActiveMode->RenderUI(state);
    }
}

std::vector<std::string> InputManager::GetModeNames() const {
    std::vector<std::string> names;
    names.reserve(m_Modes.size());
    for (const auto& [name, mode] : m_Modes) {
        names.push_back(name);
    }
    return names;
}

} // namespace sol
