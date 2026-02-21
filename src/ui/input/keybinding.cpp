#include "keybinding.h"
#include "ui/editor_settings.h"
#include <sstream>
#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace sol {

// Get the configured leader key from settings (default: Space)
ImGuiKey GetLeaderKey() {
    auto& settings = EditorSettings::Get();
    return settings.GetKeybinds().leaderKey;
}

// Key name mappings
static const std::unordered_map<std::string, ImGuiKey> s_KeyNameMap = {
    // Letters
    {"a", ImGuiKey_A}, {"b", ImGuiKey_B}, {"c", ImGuiKey_C}, {"d", ImGuiKey_D},
    {"e", ImGuiKey_E}, {"f", ImGuiKey_F}, {"g", ImGuiKey_G}, {"h", ImGuiKey_H},
    {"i", ImGuiKey_I}, {"j", ImGuiKey_J}, {"k", ImGuiKey_K}, {"l", ImGuiKey_L},
    {"m", ImGuiKey_M}, {"n", ImGuiKey_N}, {"o", ImGuiKey_O}, {"p", ImGuiKey_P},
    {"q", ImGuiKey_Q}, {"r", ImGuiKey_R}, {"s", ImGuiKey_S}, {"t", ImGuiKey_T},
    {"u", ImGuiKey_U}, {"v", ImGuiKey_V}, {"w", ImGuiKey_W}, {"x", ImGuiKey_X},
    {"y", ImGuiKey_Y}, {"z", ImGuiKey_Z},
    
    // Numbers
    {"0", ImGuiKey_0}, {"1", ImGuiKey_1}, {"2", ImGuiKey_2}, {"3", ImGuiKey_3},
    {"4", ImGuiKey_4}, {"5", ImGuiKey_5}, {"6", ImGuiKey_6}, {"7", ImGuiKey_7},
    {"8", ImGuiKey_8}, {"9", ImGuiKey_9},
    
    // Function keys
    {"f1", ImGuiKey_F1}, {"f2", ImGuiKey_F2}, {"f3", ImGuiKey_F3}, {"f4", ImGuiKey_F4},
    {"f5", ImGuiKey_F5}, {"f6", ImGuiKey_F6}, {"f7", ImGuiKey_F7}, {"f8", ImGuiKey_F8},
    {"f9", ImGuiKey_F9}, {"f10", ImGuiKey_F10}, {"f11", ImGuiKey_F11}, {"f12", ImGuiKey_F12},
    
    // Navigation
    {"up", ImGuiKey_UpArrow}, {"down", ImGuiKey_DownArrow},
    {"left", ImGuiKey_LeftArrow}, {"right", ImGuiKey_RightArrow},
    {"home", ImGuiKey_Home}, {"end", ImGuiKey_End},
    {"pageup", ImGuiKey_PageUp}, {"pagedown", ImGuiKey_PageDown},
    {"pgup", ImGuiKey_PageUp}, {"pgdn", ImGuiKey_PageDown},
    
    // Editing
    {"insert", ImGuiKey_Insert}, {"ins", ImGuiKey_Insert},
    {"delete", ImGuiKey_Delete}, {"del", ImGuiKey_Delete},
    {"backspace", ImGuiKey_Backspace}, {"bs", ImGuiKey_Backspace},
    {"enter", ImGuiKey_Enter}, {"return", ImGuiKey_Enter}, {"cr", ImGuiKey_Enter},
    {"tab", ImGuiKey_Tab},
    {"space", ImGuiKey_Space}, {"spc", ImGuiKey_Space},
    {"escape", ImGuiKey_Escape}, {"esc", ImGuiKey_Escape},
    
    // Punctuation
    {"`", ImGuiKey_GraveAccent}, {"grave", ImGuiKey_GraveAccent}, {"backtick", ImGuiKey_GraveAccent},
    {"-", ImGuiKey_Minus}, {"minus", ImGuiKey_Minus},
    {"=", ImGuiKey_Equal}, {"equal", ImGuiKey_Equal}, {"equals", ImGuiKey_Equal},
    {"[", ImGuiKey_LeftBracket}, {"leftbracket", ImGuiKey_LeftBracket},
    {"]", ImGuiKey_RightBracket}, {"rightbracket", ImGuiKey_RightBracket},
    {"\\", ImGuiKey_Backslash}, {"backslash", ImGuiKey_Backslash},
    {";", ImGuiKey_Semicolon}, {"semicolon", ImGuiKey_Semicolon},
    {"'", ImGuiKey_Apostrophe}, {"apostrophe", ImGuiKey_Apostrophe}, {"quote", ImGuiKey_Apostrophe},
    {",", ImGuiKey_Comma}, {"comma", ImGuiKey_Comma},
    {".", ImGuiKey_Period}, {"period", ImGuiKey_Period}, {"dot", ImGuiKey_Period},
    {"/", ImGuiKey_Slash}, {"slash", ImGuiKey_Slash},
    
    // Numpad
    {"numpad0", ImGuiKey_Keypad0}, {"numpad1", ImGuiKey_Keypad1},
    {"numpad2", ImGuiKey_Keypad2}, {"numpad3", ImGuiKey_Keypad3},
    {"numpad4", ImGuiKey_Keypad4}, {"numpad5", ImGuiKey_Keypad5},
    {"numpad6", ImGuiKey_Keypad6}, {"numpad7", ImGuiKey_Keypad7},
    {"numpad8", ImGuiKey_Keypad8}, {"numpad9", ImGuiKey_Keypad9},
    {"numpadmultiply", ImGuiKey_KeypadMultiply}, {"numpad*", ImGuiKey_KeypadMultiply},
    {"numpadadd", ImGuiKey_KeypadAdd}, {"numpad+", ImGuiKey_KeypadAdd},
    {"numpadsubtract", ImGuiKey_KeypadSubtract}, {"numpad-", ImGuiKey_KeypadSubtract},
    {"numpaddecimal", ImGuiKey_KeypadDecimal}, {"numpad.", ImGuiKey_KeypadDecimal},
    {"numpaddivide", ImGuiKey_KeypadDivide}, {"numpad/", ImGuiKey_KeypadDivide},
    {"numpadenter", ImGuiKey_KeypadEnter},
};

// Reverse map for key to string
static std::string KeyToString(ImGuiKey key) {
    for (const auto& [name, k] : s_KeyNameMap) {
        if (k == key) {
            // Prefer shorter, common names
            if (name.length() == 1 || name == "up" || name == "down" || 
                name == "left" || name == "right" || name == "enter" ||
                name == "escape" || name == "tab" || name == "space" ||
                name == "backspace" || name == "delete" || name == "home" ||
                name == "end" || name == "pageup" || name == "pagedown" ||
                name.substr(0, 1) == "f") {
                std::string result = name;
                if (!result.empty()) {
                    result[0] = static_cast<char>(std::toupper(result[0]));
                }
                return result;
            }
        }
    }
    return "Unknown";
}

bool KeyChord::IsPressed() const {
    if (key == ImGuiKey_None) return false;
    
    // Check modifiers match
    Modifier currentMods = GetCurrentModifiers();
    if (currentMods != mods) return false;
    
    // Check key is pressed
    return ImGui::IsKeyPressed(key, false);
}

std::string KeyChord::ToString() const {
    std::string result;
    
    if (HasModifier(mods, Modifier::Ctrl)) result += "Ctrl+";
    if (HasModifier(mods, Modifier::Shift)) result += "Shift+";
    if (HasModifier(mods, Modifier::Alt)) result += "Alt+";
    if (HasModifier(mods, Modifier::Super)) {
#ifdef __APPLE__
        result += "Cmd+";
#else
        result += "Super+";
#endif
    }
    
    result += KeyToString(key);
    return result;
}

std::optional<KeyChord> KeyChord::Parse(const std::string& str) {
    KeyChord chord;
    std::string remaining = str;
    
    // Convert to lowercase for matching
    std::string lower;
    lower.reserve(str.size());
    for (char c : str) {
        lower += static_cast<char>(std::tolower(c));
    }
    
    // Trim whitespace
    while (!lower.empty() && std::isspace(lower.front())) lower.erase(0, 1);
    while (!lower.empty() && std::isspace(lower.back())) lower.pop_back();
    
    if (lower.empty()) return std::nullopt;
    
    // Check if this is just "Leader" - returns the configured leader key
    if (lower == "leader") {
        chord.key = GetLeaderKey();
        chord.mods = Modifier::None;
        return chord;
    }
    
    // Parse modifiers (only Ctrl, Shift, Alt, Super - not Leader)
    size_t pos = 0;
    while (true) {
        size_t plus = lower.find('+', pos);
        if (plus == std::string::npos) break;
        
        std::string mod = lower.substr(pos, plus - pos);
        if (mod == "ctrl" || mod == "c") {
            chord.mods = chord.mods | Modifier::Ctrl;
        } else if (mod == "shift" || mod == "s") {
            chord.mods = chord.mods | Modifier::Shift;
        } else if (mod == "alt" || mod == "a" || mod == "meta" || mod == "m") {
            chord.mods = chord.mods | Modifier::Alt;
        } else if (mod == "super" || mod == "cmd" || mod == "win" || mod == "d") {
            chord.mods = chord.mods | Modifier::Super;
        } else {
            // Not a modifier, must be part of key name
            break;
        }
        pos = plus + 1;
    }
    
    // Parse key
    std::string keyName = lower.substr(pos);
    
    // Trim whitespace
    while (!keyName.empty() && std::isspace(keyName.front())) keyName.erase(0, 1);
    while (!keyName.empty() && std::isspace(keyName.back())) keyName.pop_back();
    
    if (keyName.empty()) return std::nullopt;
    
    auto it = s_KeyNameMap.find(keyName);
    if (it == s_KeyNameMap.end()) {
        return std::nullopt;
    }
    
    chord.key = it->second;
    return chord;
}

Modifier KeyChord::GetCurrentModifiers() {
    Modifier mods = Modifier::None;
    ImGuiIO& io = ImGui::GetIO();
    
    if (io.KeyCtrl) mods = mods | Modifier::Ctrl;
    if (io.KeyShift) mods = mods | Modifier::Shift;
    if (io.KeyAlt) mods = mods | Modifier::Alt;
    if (io.KeySuper) mods = mods | Modifier::Super;
    
    return mods;
}

bool KeySequence::IsPrefixOf(const KeySequence& other) const {
    if (m_Chords.size() > other.m_Chords.size()) return false;
    
    for (size_t i = 0; i < m_Chords.size(); i++) {
        if (m_Chords[i] != other.m_Chords[i]) return false;
    }
    return true;
}

std::string KeySequence::ToString() const {
    std::string result;
    for (size_t i = 0; i < m_Chords.size(); i++) {
        if (i > 0) result += " ";
        result += m_Chords[i].ToString();
    }
    return result;
}

std::optional<KeySequence> KeySequence::Parse(const std::string& str) {
    KeySequence seq;
    
    // Split by space to get individual chords (e.g., "Leader Q" becomes two chords)
    std::istringstream iss(str);
    std::string token;
    
    while (iss >> token) {
        auto chord = KeyChord::Parse(token);
        if (!chord) return std::nullopt;
        seq.Add(*chord);
    }
    
    if (seq.Empty()) return std::nullopt;
    return seq;
}

const char* InputContextToString(InputContext ctx) {
    switch (ctx) {
        case InputContext::Global: return "Global";
        case InputContext::Editor: return "Editor";
        case InputContext::Terminal: return "Terminal";
        case InputContext::Explorer: return "Explorer";
        case InputContext::CommandPalette: return "CommandPalette";
        case InputContext::Search: return "Search";
        case InputContext::Menu: return "Menu";
    }
    return "Unknown";
}

void KeySequenceMatcher::Reset() {
    m_CurrentSequence.Clear();
    m_LastChordTime = 0.0;
}

KeySequenceMatcher::MatchResult KeySequenceMatcher::ProcessChord(const KeyChord& chord) {
    // Check timeout
    if (HasTimedOut()) {
        Reset();
    }
    
    // Add chord to current sequence
    m_CurrentSequence.Add(chord);
    m_LastChordTime = ImGui::GetTime();
    
    // Check for matches
    bool hasPartialMatch = false;
    bool hasFullMatch = false;
    
    for (const auto& binding : m_Bindings) {
        if (m_CurrentSequence == binding) {
            hasFullMatch = true;
            break;
        }
        if (m_CurrentSequence.IsPrefixOf(binding)) {
            hasPartialMatch = true;
        }
    }
    
    if (hasFullMatch) {
        return MatchResult::FullMatch;
    }
    
    if (hasPartialMatch) {
        return MatchResult::PartialMatch;
    }
    
    // No match - reset
    Reset();
    return MatchResult::NoMatch;
}

void KeySequenceMatcher::SetBindings(const std::vector<KeySequence>& bindings) {
    m_Bindings = bindings;
}

bool KeySequenceMatcher::HasTimedOut() const {
    if (m_CurrentSequence.Empty()) return false;
    double elapsed = (ImGui::GetTime() - m_LastChordTime) * 1000.0;
    return elapsed > SequenceTimeoutMs;
}

float KeySequenceMatcher::GetTimeSinceLastChord() const {
    if (m_LastChordTime == 0.0) return 0.0f;
    return static_cast<float>(ImGui::GetTime() - m_LastChordTime);
}

} // namespace sol
