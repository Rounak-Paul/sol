#include "editor_settings.h"
#include "core/utils/json.h"
#include "core/logger.h"
#include <imgui.h>
#include <fstream>
#include <cstdlib>
#include <unordered_map>

namespace sol {

const char* EditorInputModeToString(EditorInputMode mode) {
    switch (mode) {
        case EditorInputMode::Insert:  return "Insert";
        case EditorInputMode::Command: return "Command";
        default:                       return "Insert";
    }
}

// Key name to ImGuiKey mapping
static const std::unordered_map<std::string, ImGuiKey> s_KeyNameToImGui = {
    {"space", ImGuiKey_Space}, {"escape", ImGuiKey_Escape}, {"esc", ImGuiKey_Escape},
    {"enter", ImGuiKey_Enter}, {"return", ImGuiKey_Enter}, {"tab", ImGuiKey_Tab},
    {"backspace", ImGuiKey_Backspace}, {"delete", ImGuiKey_Delete},
    {"insert", ImGuiKey_Insert}, {"home", ImGuiKey_Home}, {"end", ImGuiKey_End},
    {"pageup", ImGuiKey_PageUp}, {"pagedown", ImGuiKey_PageDown},
    {"left", ImGuiKey_LeftArrow}, {"right", ImGuiKey_RightArrow},
    {"up", ImGuiKey_UpArrow}, {"down", ImGuiKey_DownArrow},
    {"a", ImGuiKey_A}, {"b", ImGuiKey_B}, {"c", ImGuiKey_C}, {"d", ImGuiKey_D},
    {"e", ImGuiKey_E}, {"f", ImGuiKey_F}, {"g", ImGuiKey_G}, {"h", ImGuiKey_H},
    {"i", ImGuiKey_I}, {"j", ImGuiKey_J}, {"k", ImGuiKey_K}, {"l", ImGuiKey_L},
    {"m", ImGuiKey_M}, {"n", ImGuiKey_N}, {"o", ImGuiKey_O}, {"p", ImGuiKey_P},
    {"q", ImGuiKey_Q}, {"r", ImGuiKey_R}, {"s", ImGuiKey_S}, {"t", ImGuiKey_T},
    {"u", ImGuiKey_U}, {"v", ImGuiKey_V}, {"w", ImGuiKey_W}, {"x", ImGuiKey_X},
    {"y", ImGuiKey_Y}, {"z", ImGuiKey_Z},
    {"0", ImGuiKey_0}, {"1", ImGuiKey_1}, {"2", ImGuiKey_2}, {"3", ImGuiKey_3},
    {"4", ImGuiKey_4}, {"5", ImGuiKey_5}, {"6", ImGuiKey_6}, {"7", ImGuiKey_7},
    {"8", ImGuiKey_8}, {"9", ImGuiKey_9},
    {"f1", ImGuiKey_F1}, {"f2", ImGuiKey_F2}, {"f3", ImGuiKey_F3}, {"f4", ImGuiKey_F4},
    {"f5", ImGuiKey_F5}, {"f6", ImGuiKey_F6}, {"f7", ImGuiKey_F7}, {"f8", ImGuiKey_F8},
    {"f9", ImGuiKey_F9}, {"f10", ImGuiKey_F10}, {"f11", ImGuiKey_F11}, {"f12", ImGuiKey_F12},
    {"`", ImGuiKey_GraveAccent}, {"grave", ImGuiKey_GraveAccent},
    {"-", ImGuiKey_Minus}, {"=", ImGuiKey_Equal},
    {"[", ImGuiKey_LeftBracket}, {"]", ImGuiKey_RightBracket},
    {"\\", ImGuiKey_Backslash}, {";", ImGuiKey_Semicolon},
    {"'", ImGuiKey_Apostrophe}, {",", ImGuiKey_Comma},
    {".", ImGuiKey_Period}, {"/", ImGuiKey_Slash},
};

std::string ImGuiKeyToString(ImGuiKey key) {
    for (const auto& [name, k] : s_KeyNameToImGui) {
        if (k == key) {
            // Prefer readable names
            if (name == "space" || name == "escape" || name == "enter" ||
                name == "tab" || name == "backspace" || name == "delete" ||
                name.length() == 1 || name.substr(0, 1) == "f") {
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

ImGuiKey ImGuiKeyFromString(const std::string& str) {
    std::string lower;
    for (char c : str) {
        lower += static_cast<char>(std::tolower(c));
    }
    auto it = s_KeyNameToImGui.find(lower);
    if (it != s_KeyNameToImGui.end()) {
        return it->second;
    }
    return ImGuiKey_None;
}

std::vector<KeybindEntry> GetDefaultKeybindings() {
    // Format: "Leader X" means press leader key, then X
    // Format: "Ctrl+S" means traditional modifier+key
    return {
        {"Leader Q", "exit", "Global"},
        {"Leader S", "save_file", "Global"},
        {"Leader Shift+S", "save_all_files", "Global"},
        {"Leader O", "open_file_dialog", "Global"},
        {"Leader W", "close_buffer", "Global"},
        {"Leader `", "toggle_terminal", "Global"},
    };
}

static Theme MakeSpaceBlack() {
    Theme t;
    t.name = "Space Black";
    // defaults are already space black
    // editor defaults are VS Code dark-style, good for space black
    return t;
}

static Theme MakeDark() {
    Theme t;
    t.name = "Dark";
    auto& c = t.colors;
    c.windowBg          = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    c.childBg           = ImVec4(0.11f, 0.11f, 0.11f, 1.00f);
    c.popupBg           = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    c.border            = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
    c.frameBg           = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
    c.frameBgHovered    = ImVec4(0.22f, 0.22f, 0.22f, 1.00f);
    c.frameBgActive     = ImVec4(0.28f, 0.28f, 0.28f, 1.00f);
    c.titleBg           = ImVec4(0.08f, 0.08f, 0.08f, 1.00f);
    c.titleBgActive     = ImVec4(0.12f, 0.12f, 0.12f, 1.00f);
    c.menuBarBg         = ImVec4(0.12f, 0.12f, 0.12f, 1.00f);
    c.scrollbarBg       = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    c.scrollbarGrab     = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    c.scrollbarGrabHovered = ImVec4(0.40f, 0.40f, 0.40f, 1.00f);
    c.scrollbarGrabActive  = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
    c.checkMark         = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    c.sliderGrab        = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    c.sliderGrabActive  = ImVec4(0.36f, 0.69f, 1.00f, 1.00f);
    c.button            = ImVec4(0.18f, 0.18f, 0.18f, 1.00f);
    c.buttonHovered     = ImVec4(0.26f, 0.26f, 0.26f, 1.00f);
    c.buttonActive      = ImVec4(0.34f, 0.34f, 0.34f, 1.00f);
    c.header            = ImVec4(0.18f, 0.18f, 0.18f, 1.00f);
    c.headerHovered     = ImVec4(0.26f, 0.26f, 0.26f, 1.00f);
    c.headerActive      = ImVec4(0.34f, 0.34f, 0.34f, 1.00f);
    c.separator         = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
    c.separatorHovered  = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    c.tab               = ImVec4(0.12f, 0.12f, 0.12f, 1.00f);
    c.tabHovered        = ImVec4(0.26f, 0.59f, 0.98f, 0.80f);
    c.tabActive         = ImVec4(0.20f, 0.41f, 0.68f, 1.00f);
    c.tabUnfocused      = ImVec4(0.08f, 0.08f, 0.08f, 1.00f);
    c.tabUnfocusedActive = ImVec4(0.14f, 0.26f, 0.42f, 1.00f);
    c.dockingPreview    = ImVec4(0.26f, 0.59f, 0.98f, 0.70f);
    c.text              = ImVec4(0.92f, 0.92f, 0.92f, 1.00f);
    c.textDisabled      = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
    c.textSelectedBg    = ImVec4(0.26f, 0.59f, 0.98f, 0.35f);

    auto& e = t.editor;
    e.background   = ImVec4(0.12f, 0.12f, 0.12f, 1.00f);
    e.text         = ImVec4(0.85f, 0.85f, 0.85f, 1.00f);
    e.keyword      = ImVec4(0.337f, 0.612f, 0.839f, 1.00f);
    e.type         = ImVec4(0.306f, 0.788f, 0.690f, 1.00f);
    e.function     = ImVec4(0.863f, 0.863f, 0.667f, 1.00f);
    e.variable     = ImVec4(0.612f, 0.863f, 0.996f, 1.00f);
    e.string       = ImVec4(0.808f, 0.569f, 0.471f, 1.00f);
    e.number       = ImVec4(0.710f, 0.808f, 0.659f, 1.00f);
    e.comment      = ImVec4(0.416f, 0.600f, 0.333f, 1.00f);
    e.op           = ImVec4(0.85f, 0.85f, 0.85f, 1.00f);
    e.punctuation  = ImVec4(0.85f, 0.85f, 0.85f, 1.00f);
    e.macro        = ImVec4(0.741f, 0.388f, 0.773f, 1.00f);
    e.constant     = ImVec4(0.392f, 0.588f, 0.863f, 1.00f);
    e.error        = ImVec4(0.957f, 0.278f, 0.278f, 1.00f);
    e.lineNumber   = ImVec4(0.40f, 0.40f, 0.40f, 1.00f);
    e.currentLine  = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
    e.selection    = ImVec4(0.17f, 0.35f, 0.56f, 1.00f);
    e.cursor       = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    e.popupBg      = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
    e.popupBorder  = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    e.popupText    = ImVec4(0.85f, 0.85f, 0.85f, 1.00f);
    e.popupSelected = ImVec4(0.17f, 0.35f, 0.56f, 1.00f);

    return t;
}

static Theme MakeLight() {
    Theme t;
    t.name = "Light";
    auto& c = t.colors;
    c.windowBg          = ImVec4(0.94f, 0.94f, 0.94f, 1.00f);
    c.childBg           = ImVec4(0.94f, 0.94f, 0.94f, 1.00f);
    c.popupBg           = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    c.border            = ImVec4(0.70f, 0.70f, 0.70f, 1.00f);
    c.frameBg           = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    c.frameBgHovered    = ImVec4(0.26f, 0.59f, 0.98f, 0.40f);
    c.frameBgActive     = ImVec4(0.26f, 0.59f, 0.98f, 0.67f);
    c.titleBg           = ImVec4(0.85f, 0.85f, 0.85f, 1.00f);
    c.titleBgActive     = ImVec4(0.82f, 0.82f, 0.82f, 1.00f);
    c.menuBarBg         = ImVec4(0.86f, 0.86f, 0.86f, 1.00f);
    c.scrollbarBg       = ImVec4(0.98f, 0.98f, 0.98f, 0.53f);
    c.scrollbarGrab     = ImVec4(0.69f, 0.69f, 0.69f, 0.80f);
    c.scrollbarGrabHovered = ImVec4(0.49f, 0.49f, 0.49f, 0.80f);
    c.scrollbarGrabActive  = ImVec4(0.49f, 0.49f, 0.49f, 1.00f);
    c.checkMark         = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    c.sliderGrab        = ImVec4(0.26f, 0.59f, 0.98f, 0.78f);
    c.sliderGrabActive  = ImVec4(0.46f, 0.54f, 0.80f, 0.60f);
    c.button            = ImVec4(0.26f, 0.59f, 0.98f, 0.40f);
    c.buttonHovered     = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    c.buttonActive      = ImVec4(0.06f, 0.53f, 0.98f, 1.00f);
    c.header            = ImVec4(0.26f, 0.59f, 0.98f, 0.31f);
    c.headerHovered     = ImVec4(0.26f, 0.59f, 0.98f, 0.80f);
    c.headerActive      = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    c.separator         = ImVec4(0.39f, 0.39f, 0.39f, 0.62f);
    c.separatorHovered  = ImVec4(0.14f, 0.44f, 0.80f, 0.78f);
    c.tab               = ImVec4(0.76f, 0.80f, 0.84f, 0.93f);
    c.tabHovered        = ImVec4(0.26f, 0.59f, 0.98f, 0.80f);
    c.tabActive         = ImVec4(0.60f, 0.73f, 0.88f, 1.00f);
    c.tabUnfocused      = ImVec4(0.92f, 0.93f, 0.94f, 0.99f);
    c.tabUnfocusedActive = ImVec4(0.74f, 0.82f, 0.91f, 1.00f);
    c.dockingPreview    = ImVec4(0.26f, 0.59f, 0.98f, 0.22f);
    c.text              = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
    c.textDisabled      = ImVec4(0.60f, 0.60f, 0.60f, 1.00f);
    c.textSelectedBg    = ImVec4(0.26f, 0.59f, 0.98f, 0.35f);

    auto& e = t.editor;
    e.background   = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    e.text         = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
    e.keyword      = ImVec4(0.00f, 0.00f, 1.00f, 1.00f);
    e.type         = ImVec4(0.16f, 0.56f, 0.47f, 1.00f);
    e.function     = ImVec4(0.47f, 0.31f, 0.09f, 1.00f);
    e.variable     = ImVec4(0.00f, 0.06f, 0.52f, 1.00f);
    e.string       = ImVec4(0.64f, 0.08f, 0.08f, 1.00f);
    e.number       = ImVec4(0.05f, 0.55f, 0.15f, 1.00f);
    e.comment      = ImVec4(0.00f, 0.50f, 0.00f, 1.00f);
    e.op           = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
    e.punctuation  = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
    e.macro        = ImVec4(0.54f, 0.17f, 0.89f, 1.00f);
    e.constant     = ImVec4(0.00f, 0.44f, 0.75f, 1.00f);
    e.error        = ImVec4(1.00f, 0.00f, 0.00f, 1.00f);
    e.lineNumber   = ImVec4(0.55f, 0.55f, 0.55f, 1.00f);
    e.currentLine  = ImVec4(0.93f, 0.93f, 0.93f, 1.00f);
    e.selection    = ImVec4(0.68f, 0.82f, 1.00f, 1.00f);
    e.cursor       = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
    e.popupBg      = ImVec4(0.96f, 0.96f, 0.96f, 1.00f);
    e.popupBorder  = ImVec4(0.75f, 0.75f, 0.75f, 1.00f);
    e.popupText    = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
    e.popupSelected = ImVec4(0.80f, 0.87f, 0.97f, 1.00f);

    return t;
}

static Theme MakeNord() {
    Theme t;
    t.name = "Nord";
    auto& c = t.colors;
    c.windowBg          = ImVec4(0.18f, 0.20f, 0.25f, 1.00f);
    c.childBg           = ImVec4(0.18f, 0.20f, 0.25f, 1.00f);
    c.popupBg           = ImVec4(0.16f, 0.18f, 0.22f, 1.00f);
    c.border            = ImVec4(0.24f, 0.27f, 0.33f, 1.00f);
    c.frameBg           = ImVec4(0.22f, 0.25f, 0.30f, 1.00f);
    c.frameBgHovered    = ImVec4(0.26f, 0.30f, 0.36f, 1.00f);
    c.frameBgActive     = ImVec4(0.30f, 0.34f, 0.42f, 1.00f);
    c.titleBg           = ImVec4(0.15f, 0.17f, 0.21f, 1.00f);
    c.titleBgActive     = ImVec4(0.18f, 0.20f, 0.25f, 1.00f);
    c.menuBarBg         = ImVec4(0.18f, 0.20f, 0.25f, 1.00f);
    c.scrollbarBg       = ImVec4(0.18f, 0.20f, 0.25f, 1.00f);
    c.scrollbarGrab     = ImVec4(0.30f, 0.34f, 0.42f, 1.00f);
    c.scrollbarGrabHovered = ImVec4(0.36f, 0.40f, 0.48f, 1.00f);
    c.scrollbarGrabActive  = ImVec4(0.42f, 0.46f, 0.54f, 1.00f);
    c.checkMark         = ImVec4(0.53f, 0.75f, 0.82f, 1.00f);
    c.sliderGrab        = ImVec4(0.53f, 0.75f, 0.82f, 1.00f);
    c.sliderGrabActive  = ImVec4(0.63f, 0.85f, 0.92f, 1.00f);
    c.button            = ImVec4(0.24f, 0.27f, 0.33f, 1.00f);
    c.buttonHovered     = ImVec4(0.30f, 0.34f, 0.42f, 1.00f);
    c.buttonActive      = ImVec4(0.36f, 0.40f, 0.48f, 1.00f);
    c.header            = ImVec4(0.24f, 0.27f, 0.33f, 1.00f);
    c.headerHovered     = ImVec4(0.30f, 0.34f, 0.42f, 1.00f);
    c.headerActive      = ImVec4(0.36f, 0.40f, 0.48f, 1.00f);
    c.separator         = ImVec4(0.24f, 0.27f, 0.33f, 1.00f);
    c.separatorHovered  = ImVec4(0.53f, 0.75f, 0.82f, 1.00f);
    c.tab               = ImVec4(0.15f, 0.17f, 0.21f, 1.00f);
    c.tabHovered        = ImVec4(0.53f, 0.75f, 0.82f, 0.80f);
    c.tabActive         = ImVec4(0.22f, 0.25f, 0.30f, 1.00f);
    c.tabUnfocused      = ImVec4(0.15f, 0.17f, 0.21f, 1.00f);
    c.tabUnfocusedActive = ImVec4(0.18f, 0.20f, 0.25f, 1.00f);
    c.dockingPreview    = ImVec4(0.53f, 0.75f, 0.82f, 0.70f);
    c.text              = ImVec4(0.85f, 0.87f, 0.91f, 1.00f);
    c.textDisabled      = ImVec4(0.50f, 0.53f, 0.58f, 1.00f);
    c.textSelectedBg    = ImVec4(0.53f, 0.75f, 0.82f, 0.35f);

    // Nord editor colors (based on Nord palette)
    auto& e = t.editor;
    e.background   = ImVec4(0.18f, 0.20f, 0.26f, 1.00f);  // Nord0
    e.text         = ImVec4(0.85f, 0.87f, 0.91f, 1.00f);  // Nord4
    e.keyword      = ImVec4(0.51f, 0.63f, 0.76f, 1.00f);  // Nord9
    e.type         = ImVec4(0.53f, 0.75f, 0.82f, 1.00f);  // Nord8
    e.function     = ImVec4(0.53f, 0.75f, 0.82f, 1.00f);  // Nord8
    e.variable     = ImVec4(0.85f, 0.87f, 0.91f, 1.00f);  // Nord4
    e.string       = ImVec4(0.64f, 0.75f, 0.55f, 1.00f);  // Nord14
    e.number       = ImVec4(0.71f, 0.56f, 0.68f, 1.00f);  // Nord15
    e.comment      = ImVec4(0.40f, 0.45f, 0.53f, 1.00f);  // Nord3 lighter
    e.op           = ImVec4(0.51f, 0.63f, 0.76f, 1.00f);  // Nord9
    e.punctuation  = ImVec4(0.76f, 0.79f, 0.84f, 1.00f);  // Nord4 dim
    e.macro        = ImVec4(0.71f, 0.56f, 0.68f, 1.00f);  // Nord15
    e.constant     = ImVec4(0.51f, 0.63f, 0.76f, 1.00f);  // Nord9
    e.error        = ImVec4(0.75f, 0.38f, 0.42f, 1.00f);  // Nord11
    e.lineNumber   = ImVec4(0.40f, 0.45f, 0.53f, 1.00f);  // Nord3
    e.currentLine  = ImVec4(0.22f, 0.25f, 0.30f, 1.00f);  // Nord1
    e.selection    = ImVec4(0.26f, 0.30f, 0.36f, 1.00f);  // Nord2
    e.cursor       = ImVec4(0.85f, 0.87f, 0.91f, 1.00f);  // Nord4
    e.popupBg      = ImVec4(0.16f, 0.18f, 0.22f, 1.00f);
    e.popupBorder  = ImVec4(0.24f, 0.27f, 0.33f, 1.00f);
    e.popupText    = ImVec4(0.85f, 0.87f, 0.91f, 1.00f);
    e.popupSelected = ImVec4(0.26f, 0.30f, 0.36f, 1.00f);

    return t;
}

void EditorSettings::SetPreset(const std::string& name) {
    Theme oldTheme = m_Theme;
    
    if (name == "Space Black")    m_Theme = MakeSpaceBlack();
    else if (name == "Dark")      m_Theme = MakeDark();
    else if (name == "Light")     m_Theme = MakeLight();
    else if (name == "Nord")      m_Theme = MakeNord();
    else return;

    // Preserve font settings when switching color presets
    m_Theme.font = oldTheme.font;
    ApplyTheme();
}

void EditorSettings::ApplyTheme() {
    ImGuiStyle& style = ImGui::GetStyle();
    const auto& s = m_Theme.style;
    const auto& c = m_Theme.colors;

    // Rounding
    style.WindowRounding    = s.windowRounding;
    style.ChildRounding     = s.childRounding;
    style.FrameRounding     = s.frameRounding;
    style.PopupRounding     = s.popupRounding;
    style.ScrollbarRounding = s.scrollbarRounding;
    style.GrabRounding      = s.grabRounding;
    style.TabRounding       = s.tabRounding;

    // Borders
    style.WindowBorderSize  = s.windowBorderSize;
    style.ChildBorderSize   = s.childBorderSize;
    style.PopupBorderSize   = s.popupBorderSize;
    style.FrameBorderSize   = s.frameBorderSize;
    style.TabBorderSize     = s.tabBorderSize;

    // Spacing
    style.WindowPadding     = s.windowPadding;
    style.FramePadding      = s.framePadding;
    style.CellPadding       = s.cellPadding;
    style.ItemSpacing       = s.itemSpacing;
    style.ItemInnerSpacing  = s.itemInnerSpacing;
    style.IndentSpacing     = s.indentSpacing;
    style.ScrollbarSize     = s.scrollbarSize;
    style.GrabMinSize       = s.grabMinSize;

    // Font scale
    ImGui::GetIO().FontGlobalScale = m_Theme.font.fontScale;

    // Colors
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg]           = c.windowBg;
    colors[ImGuiCol_ChildBg]            = c.childBg;
    colors[ImGuiCol_PopupBg]            = c.popupBg;
    colors[ImGuiCol_Border]             = c.border;
    colors[ImGuiCol_BorderShadow]       = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_FrameBg]            = c.frameBg;
    colors[ImGuiCol_FrameBgHovered]     = c.frameBgHovered;
    colors[ImGuiCol_FrameBgActive]      = c.frameBgActive;
    colors[ImGuiCol_TitleBg]            = c.titleBg;
    colors[ImGuiCol_TitleBgActive]      = c.titleBgActive;
    colors[ImGuiCol_TitleBgCollapsed]   = c.titleBg;
    colors[ImGuiCol_MenuBarBg]          = c.menuBarBg;
    colors[ImGuiCol_ScrollbarBg]        = c.scrollbarBg;
    colors[ImGuiCol_ScrollbarGrab]      = c.scrollbarGrab;
    colors[ImGuiCol_ScrollbarGrabHovered] = c.scrollbarGrabHovered;
    colors[ImGuiCol_ScrollbarGrabActive]  = c.scrollbarGrabActive;
    colors[ImGuiCol_CheckMark]          = c.checkMark;
    colors[ImGuiCol_SliderGrab]         = c.sliderGrab;
    colors[ImGuiCol_SliderGrabActive]   = c.sliderGrabActive;
    colors[ImGuiCol_Button]             = c.button;
    colors[ImGuiCol_ButtonHovered]      = c.buttonHovered;
    colors[ImGuiCol_ButtonActive]       = c.buttonActive;
    colors[ImGuiCol_Header]             = c.header;
    colors[ImGuiCol_HeaderHovered]      = c.headerHovered;
    colors[ImGuiCol_HeaderActive]       = c.headerActive;
    colors[ImGuiCol_Separator]          = c.separator;
    colors[ImGuiCol_SeparatorHovered]   = c.separatorHovered;
    colors[ImGuiCol_SeparatorActive]    = c.separatorHovered;
    colors[ImGuiCol_ResizeGrip]         = c.separator;
    colors[ImGuiCol_ResizeGripHovered]  = c.separatorHovered;
    colors[ImGuiCol_ResizeGripActive]   = c.separatorHovered;
    colors[ImGuiCol_Tab]                = c.tab;
    colors[ImGuiCol_TabHovered]         = c.tabHovered;
    colors[ImGuiCol_TabActive]          = c.tabActive;
    colors[ImGuiCol_TabUnfocused]       = c.tabUnfocused;
    colors[ImGuiCol_TabUnfocusedActive] = c.tabUnfocusedActive;
    colors[ImGuiCol_DockingPreview]     = c.dockingPreview;
    colors[ImGuiCol_DockingEmptyBg]     = c.windowBg;
    colors[ImGuiCol_PlotLines]          = c.text;
    colors[ImGuiCol_PlotLinesHovered]   = c.checkMark;
    colors[ImGuiCol_PlotHistogram]      = c.checkMark;
    colors[ImGuiCol_PlotHistogramHovered] = c.sliderGrabActive;
    colors[ImGuiCol_TableHeaderBg]      = c.frameBg;
    colors[ImGuiCol_TableBorderStrong]  = c.border;
    colors[ImGuiCol_TableBorderLight]   = c.frameBg;
    colors[ImGuiCol_TableRowBg]         = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_TableRowBgAlt]      = ImVec4(1, 1, 1, 0.03f);
    colors[ImGuiCol_Text]               = c.text;
    colors[ImGuiCol_TextDisabled]       = c.textDisabled;
    colors[ImGuiCol_TextSelectedBg]     = c.textSelectedBg;
    colors[ImGuiCol_DragDropTarget]     = c.checkMark;
    colors[ImGuiCol_NavHighlight]       = c.checkMark;
    colors[ImGuiCol_NavWindowingHighlight] = ImVec4(0.70f, 0.70f, 0.75f, 0.70f);
    colors[ImGuiCol_NavWindowingDimBg]  = ImVec4(0.15f, 0.15f, 0.17f, 0.20f);
    colors[ImGuiCol_ModalWindowDimBg]   = ImVec4(0.00f, 0.00f, 0.00f, 0.70f);
}

// --- Config Persistence ---

std::filesystem::path EditorSettings::GetConfigDir() {
    const char* home = std::getenv("HOME");
    if (!home) home = "~";
    return std::filesystem::path(home) / ".sol";
}

std::filesystem::path EditorSettings::GetConfigPath() {
    return GetConfigDir() / "config.json";
}

std::filesystem::path EditorSettings::GetKeybindsPath() {
    return GetConfigDir() / "keybinds.json";
}

// Helper to serialize ImVec4 to JSON array
static JsonValue Vec4ToJson(const ImVec4& v) {
    JsonArray arr;
    arr.push_back(JsonValue(static_cast<double>(v.x)));
    arr.push_back(JsonValue(static_cast<double>(v.y)));
    arr.push_back(JsonValue(static_cast<double>(v.z)));
    arr.push_back(JsonValue(static_cast<double>(v.w)));
    return JsonValue(arr);
}

// Helper to serialize ImVec2 to JSON array
static JsonValue Vec2ToJson(const ImVec2& v) {
    JsonArray arr;
    arr.push_back(JsonValue(static_cast<double>(v.x)));
    arr.push_back(JsonValue(static_cast<double>(v.y)));
    return JsonValue(arr);
}

// Helper to deserialize ImVec4 from JSON array
static ImVec4 JsonToVec4(const JsonValue& v, const ImVec4& def = ImVec4(0,0,0,1)) {
    if (v.type != JsonType::Array) return def;
    const auto& arr = v.AsArray();
    if (arr.size() < 4) return def;
    auto getFloat = [](const JsonValue& val) -> float {
        if (val.type == JsonType::Number) return static_cast<float>(std::get<double>(val.data));
        return 0.0f;
    };
    return ImVec4(getFloat(arr[0]), getFloat(arr[1]), getFloat(arr[2]), getFloat(arr[3]));
}

// Helper to deserialize ImVec2 from JSON array
static ImVec2 JsonToVec2(const JsonValue& v, const ImVec2& def = ImVec2(0,0)) {
    if (v.type != JsonType::Array) return def;
    const auto& arr = v.AsArray();
    if (arr.size() < 2) return def;
    auto getFloat = [](const JsonValue& val) -> float {
        if (val.type == JsonType::Number) return static_cast<float>(std::get<double>(val.data));
        return 0.0f;
    };
    return ImVec2(getFloat(arr[0]), getFloat(arr[1]));
}

static float JsonToFloat(const JsonValue& v, float def = 0.0f) {
    if (v.type == JsonType::Number) return static_cast<float>(std::get<double>(v.data));
    return def;
}

bool EditorSettings::Save() const {
    auto configDir = GetConfigDir();
    auto configPath = GetConfigPath();
    
    // Ensure config directory exists
    std::error_code ec;
    std::filesystem::create_directories(configDir, ec);
    if (ec) {
        Logger::Error("Failed to create config directory: " + ec.message());
        return false;
    }
    
    const auto& theme = m_Theme;
    
    // Serialize EditorColors
    JsonObject editorObj;
    editorObj["background"] = Vec4ToJson(theme.editor.background);
    editorObj["text"] = Vec4ToJson(theme.editor.text);
    editorObj["keyword"] = Vec4ToJson(theme.editor.keyword);
    editorObj["type"] = Vec4ToJson(theme.editor.type);
    editorObj["function"] = Vec4ToJson(theme.editor.function);
    editorObj["variable"] = Vec4ToJson(theme.editor.variable);
    editorObj["string"] = Vec4ToJson(theme.editor.string);
    editorObj["number"] = Vec4ToJson(theme.editor.number);
    editorObj["comment"] = Vec4ToJson(theme.editor.comment);
    editorObj["op"] = Vec4ToJson(theme.editor.op);
    editorObj["punctuation"] = Vec4ToJson(theme.editor.punctuation);
    editorObj["macro"] = Vec4ToJson(theme.editor.macro);
    editorObj["constant"] = Vec4ToJson(theme.editor.constant);
    editorObj["error"] = Vec4ToJson(theme.editor.error);
    editorObj["lineNumber"] = Vec4ToJson(theme.editor.lineNumber);
    editorObj["currentLine"] = Vec4ToJson(theme.editor.currentLine);
    editorObj["selection"] = Vec4ToJson(theme.editor.selection);
    editorObj["cursor"] = Vec4ToJson(theme.editor.cursor);
    editorObj["popupBg"] = Vec4ToJson(theme.editor.popupBg);
    editorObj["popupBorder"] = Vec4ToJson(theme.editor.popupBorder);
    editorObj["popupText"] = Vec4ToJson(theme.editor.popupText);
    editorObj["popupSelected"] = Vec4ToJson(theme.editor.popupSelected);
    
    // Serialize ThemeColors
    JsonObject colorsObj;
    colorsObj["windowBg"] = Vec4ToJson(theme.colors.windowBg);
    colorsObj["childBg"] = Vec4ToJson(theme.colors.childBg);
    colorsObj["popupBg"] = Vec4ToJson(theme.colors.popupBg);
    colorsObj["border"] = Vec4ToJson(theme.colors.border);
    colorsObj["frameBg"] = Vec4ToJson(theme.colors.frameBg);
    colorsObj["frameBgHovered"] = Vec4ToJson(theme.colors.frameBgHovered);
    colorsObj["frameBgActive"] = Vec4ToJson(theme.colors.frameBgActive);
    colorsObj["titleBg"] = Vec4ToJson(theme.colors.titleBg);
    colorsObj["titleBgActive"] = Vec4ToJson(theme.colors.titleBgActive);
    colorsObj["menuBarBg"] = Vec4ToJson(theme.colors.menuBarBg);
    colorsObj["scrollbarBg"] = Vec4ToJson(theme.colors.scrollbarBg);
    colorsObj["scrollbarGrab"] = Vec4ToJson(theme.colors.scrollbarGrab);
    colorsObj["scrollbarGrabHovered"] = Vec4ToJson(theme.colors.scrollbarGrabHovered);
    colorsObj["scrollbarGrabActive"] = Vec4ToJson(theme.colors.scrollbarGrabActive);
    colorsObj["checkMark"] = Vec4ToJson(theme.colors.checkMark);
    colorsObj["sliderGrab"] = Vec4ToJson(theme.colors.sliderGrab);
    colorsObj["sliderGrabActive"] = Vec4ToJson(theme.colors.sliderGrabActive);
    colorsObj["button"] = Vec4ToJson(theme.colors.button);
    colorsObj["buttonHovered"] = Vec4ToJson(theme.colors.buttonHovered);
    colorsObj["buttonActive"] = Vec4ToJson(theme.colors.buttonActive);
    colorsObj["header"] = Vec4ToJson(theme.colors.header);
    colorsObj["headerHovered"] = Vec4ToJson(theme.colors.headerHovered);
    colorsObj["headerActive"] = Vec4ToJson(theme.colors.headerActive);
    colorsObj["separator"] = Vec4ToJson(theme.colors.separator);
    colorsObj["separatorHovered"] = Vec4ToJson(theme.colors.separatorHovered);
    colorsObj["tab"] = Vec4ToJson(theme.colors.tab);
    colorsObj["tabHovered"] = Vec4ToJson(theme.colors.tabHovered);
    colorsObj["tabActive"] = Vec4ToJson(theme.colors.tabActive);
    colorsObj["tabUnfocused"] = Vec4ToJson(theme.colors.tabUnfocused);
    colorsObj["tabUnfocusedActive"] = Vec4ToJson(theme.colors.tabUnfocusedActive);
    colorsObj["dockingPreview"] = Vec4ToJson(theme.colors.dockingPreview);
    colorsObj["text"] = Vec4ToJson(theme.colors.text);
    colorsObj["textDisabled"] = Vec4ToJson(theme.colors.textDisabled);
    colorsObj["textSelectedBg"] = Vec4ToJson(theme.colors.textSelectedBg);
    
    // Serialize ThemeStyle
    JsonObject styleObj;
    styleObj["windowRounding"] = JsonValue(static_cast<double>(theme.style.windowRounding));
    styleObj["frameRounding"] = JsonValue(static_cast<double>(theme.style.frameRounding));
    styleObj["tabRounding"] = JsonValue(static_cast<double>(theme.style.tabRounding));
    styleObj["scrollbarRounding"] = JsonValue(static_cast<double>(theme.style.scrollbarRounding));
    styleObj["grabRounding"] = JsonValue(static_cast<double>(theme.style.grabRounding));
    styleObj["childRounding"] = JsonValue(static_cast<double>(theme.style.childRounding));
    styleObj["popupRounding"] = JsonValue(static_cast<double>(theme.style.popupRounding));
    styleObj["windowBorderSize"] = JsonValue(static_cast<double>(theme.style.windowBorderSize));
    styleObj["frameBorderSize"] = JsonValue(static_cast<double>(theme.style.frameBorderSize));
    styleObj["tabBorderSize"] = JsonValue(static_cast<double>(theme.style.tabBorderSize));
    styleObj["childBorderSize"] = JsonValue(static_cast<double>(theme.style.childBorderSize));
    styleObj["popupBorderSize"] = JsonValue(static_cast<double>(theme.style.popupBorderSize));
    styleObj["windowPadding"] = Vec2ToJson(theme.style.windowPadding);
    styleObj["framePadding"] = Vec2ToJson(theme.style.framePadding);
    styleObj["cellPadding"] = Vec2ToJson(theme.style.cellPadding);
    styleObj["itemSpacing"] = Vec2ToJson(theme.style.itemSpacing);
    styleObj["itemInnerSpacing"] = Vec2ToJson(theme.style.itemInnerSpacing);
    styleObj["indentSpacing"] = JsonValue(static_cast<double>(theme.style.indentSpacing));
    styleObj["scrollbarSize"] = JsonValue(static_cast<double>(theme.style.scrollbarSize));
    styleObj["grabMinSize"] = JsonValue(static_cast<double>(theme.style.grabMinSize));
    
    // Serialize ThemeFont
    JsonObject fontObj;
    fontObj["fontId"] = JsonValue(theme.font.fontId);
    fontObj["fontSize"] = JsonValue(static_cast<double>(theme.font.fontSize));
    fontObj["fontScale"] = JsonValue(static_cast<double>(theme.font.fontScale));
    
    // Build root object
    JsonObject root;
    root["name"] = JsonValue(theme.name);
    root["editor"] = JsonValue(editorObj);
    root["colors"] = JsonValue(colorsObj);
    root["style"] = JsonValue(styleObj);
    root["font"] = JsonValue(fontObj);
    
    std::string jsonStr = Json::Serialize(JsonValue(root));
    
    std::ofstream file(configPath);
    if (!file) {
        Logger::Error("Failed to open config file for writing: " + configPath.string());
        return false;
    }
    
    file << jsonStr;
    file.close();
    
    Logger::Info("Settings saved to " + configPath.string());
    return true;
}

bool EditorSettings::SaveKeybinds() const {
    auto configDir = GetConfigDir();
    auto keybindsPath = GetKeybindsPath();
    
    // Ensure config directory exists
    std::error_code ec;
    std::filesystem::create_directories(configDir, ec);
    if (ec) {
        Logger::Error("Failed to create config directory: " + ec.message());
        return false;
    }
    
    JsonObject keybindsObj;
    keybindsObj["leaderKey"] = JsonValue(ImGuiKeyToString(m_Keybinds.leaderKey));
    keybindsObj["modeKey"] = JsonValue(ImGuiKeyToString(m_Keybinds.modeKey));
    keybindsObj["insertKey"] = JsonValue(ImGuiKeyToString(m_Keybinds.insertKey));
    keybindsObj["defaultMode"] = JsonValue(std::string(EditorInputModeToString(m_Keybinds.defaultMode)));
    
    JsonArray bindingsArr;
    for (const auto& binding : m_Keybinds.bindings) {
        JsonObject bindObj;
        bindObj["keys"] = JsonValue(binding.keys);
        bindObj["event"] = JsonValue(binding.eventId);
        bindObj["context"] = JsonValue(binding.context);
        bindingsArr.push_back(JsonValue(bindObj));
    }
    keybindsObj["bindings"] = JsonValue(bindingsArr);
    
    std::string jsonStr = Json::Serialize(JsonValue(keybindsObj));
    
    std::ofstream file(keybindsPath);
    if (!file) {
        Logger::Error("Failed to open keybinds file for writing: " + keybindsPath.string());
        return false;
    }
    
    file << jsonStr;
    file.close();
    
    Logger::Info("Keybinds saved to " + keybindsPath.string());
    return true;
}

bool EditorSettings::Load() {
    auto configPath = GetConfigPath();
    
    if (!std::filesystem::exists(configPath)) {
        Logger::Info("No config file found at " + configPath.string() + ", using defaults");
        return false;
    }
    
    std::ifstream file(configPath);
    if (!file) {
        Logger::Error("Failed to open config file: " + configPath.string());
        return false;
    }
    
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string jsonStr = buffer.str();
    file.close();
    
    JsonValue root = Json::Parse(jsonStr);
    if (root.type != JsonType::Object) {
        Logger::Error("Invalid config file format");
        return false;
    }
    
    // Load theme name
    if (root.Has("name")) {
        m_Theme.name = root["name"].ToString();
    }
    
    // Load EditorColors
    if (root.Has("editor")) {
        const auto& e = root["editor"];
        auto& ed = m_Theme.editor;
        if (e.Has("background")) ed.background = JsonToVec4(e["background"], ed.background);
        if (e.Has("text")) ed.text = JsonToVec4(e["text"], ed.text);
        if (e.Has("keyword")) ed.keyword = JsonToVec4(e["keyword"], ed.keyword);
        if (e.Has("type")) ed.type = JsonToVec4(e["type"], ed.type);
        if (e.Has("function")) ed.function = JsonToVec4(e["function"], ed.function);
        if (e.Has("variable")) ed.variable = JsonToVec4(e["variable"], ed.variable);
        if (e.Has("string")) ed.string = JsonToVec4(e["string"], ed.string);
        if (e.Has("number")) ed.number = JsonToVec4(e["number"], ed.number);
        if (e.Has("comment")) ed.comment = JsonToVec4(e["comment"], ed.comment);
        if (e.Has("op")) ed.op = JsonToVec4(e["op"], ed.op);
        if (e.Has("punctuation")) ed.punctuation = JsonToVec4(e["punctuation"], ed.punctuation);
        if (e.Has("macro")) ed.macro = JsonToVec4(e["macro"], ed.macro);
        if (e.Has("constant")) ed.constant = JsonToVec4(e["constant"], ed.constant);
        if (e.Has("error")) ed.error = JsonToVec4(e["error"], ed.error);
        if (e.Has("lineNumber")) ed.lineNumber = JsonToVec4(e["lineNumber"], ed.lineNumber);
        if (e.Has("currentLine")) ed.currentLine = JsonToVec4(e["currentLine"], ed.currentLine);
        if (e.Has("selection")) ed.selection = JsonToVec4(e["selection"], ed.selection);
        if (e.Has("cursor")) ed.cursor = JsonToVec4(e["cursor"], ed.cursor);
        if (e.Has("popupBg")) ed.popupBg = JsonToVec4(e["popupBg"], ed.popupBg);
        if (e.Has("popupBorder")) ed.popupBorder = JsonToVec4(e["popupBorder"], ed.popupBorder);
        if (e.Has("popupText")) ed.popupText = JsonToVec4(e["popupText"], ed.popupText);
        if (e.Has("popupSelected")) ed.popupSelected = JsonToVec4(e["popupSelected"], ed.popupSelected);
    }
    
    // Load ThemeColors
    if (root.Has("colors")) {
        const auto& c = root["colors"];
        auto& tc = m_Theme.colors;
        if (c.Has("windowBg")) tc.windowBg = JsonToVec4(c["windowBg"], tc.windowBg);
        if (c.Has("childBg")) tc.childBg = JsonToVec4(c["childBg"], tc.childBg);
        if (c.Has("popupBg")) tc.popupBg = JsonToVec4(c["popupBg"], tc.popupBg);
        if (c.Has("border")) tc.border = JsonToVec4(c["border"], tc.border);
        if (c.Has("frameBg")) tc.frameBg = JsonToVec4(c["frameBg"], tc.frameBg);
        if (c.Has("frameBgHovered")) tc.frameBgHovered = JsonToVec4(c["frameBgHovered"], tc.frameBgHovered);
        if (c.Has("frameBgActive")) tc.frameBgActive = JsonToVec4(c["frameBgActive"], tc.frameBgActive);
        if (c.Has("titleBg")) tc.titleBg = JsonToVec4(c["titleBg"], tc.titleBg);
        if (c.Has("titleBgActive")) tc.titleBgActive = JsonToVec4(c["titleBgActive"], tc.titleBgActive);
        if (c.Has("menuBarBg")) tc.menuBarBg = JsonToVec4(c["menuBarBg"], tc.menuBarBg);
        if (c.Has("scrollbarBg")) tc.scrollbarBg = JsonToVec4(c["scrollbarBg"], tc.scrollbarBg);
        if (c.Has("scrollbarGrab")) tc.scrollbarGrab = JsonToVec4(c["scrollbarGrab"], tc.scrollbarGrab);
        if (c.Has("scrollbarGrabHovered")) tc.scrollbarGrabHovered = JsonToVec4(c["scrollbarGrabHovered"], tc.scrollbarGrabHovered);
        if (c.Has("scrollbarGrabActive")) tc.scrollbarGrabActive = JsonToVec4(c["scrollbarGrabActive"], tc.scrollbarGrabActive);
        if (c.Has("checkMark")) tc.checkMark = JsonToVec4(c["checkMark"], tc.checkMark);
        if (c.Has("sliderGrab")) tc.sliderGrab = JsonToVec4(c["sliderGrab"], tc.sliderGrab);
        if (c.Has("sliderGrabActive")) tc.sliderGrabActive = JsonToVec4(c["sliderGrabActive"], tc.sliderGrabActive);
        if (c.Has("button")) tc.button = JsonToVec4(c["button"], tc.button);
        if (c.Has("buttonHovered")) tc.buttonHovered = JsonToVec4(c["buttonHovered"], tc.buttonHovered);
        if (c.Has("buttonActive")) tc.buttonActive = JsonToVec4(c["buttonActive"], tc.buttonActive);
        if (c.Has("header")) tc.header = JsonToVec4(c["header"], tc.header);
        if (c.Has("headerHovered")) tc.headerHovered = JsonToVec4(c["headerHovered"], tc.headerHovered);
        if (c.Has("headerActive")) tc.headerActive = JsonToVec4(c["headerActive"], tc.headerActive);
        if (c.Has("separator")) tc.separator = JsonToVec4(c["separator"], tc.separator);
        if (c.Has("separatorHovered")) tc.separatorHovered = JsonToVec4(c["separatorHovered"], tc.separatorHovered);
        if (c.Has("tab")) tc.tab = JsonToVec4(c["tab"], tc.tab);
        if (c.Has("tabHovered")) tc.tabHovered = JsonToVec4(c["tabHovered"], tc.tabHovered);
        if (c.Has("tabActive")) tc.tabActive = JsonToVec4(c["tabActive"], tc.tabActive);
        if (c.Has("tabUnfocused")) tc.tabUnfocused = JsonToVec4(c["tabUnfocused"], tc.tabUnfocused);
        if (c.Has("tabUnfocusedActive")) tc.tabUnfocusedActive = JsonToVec4(c["tabUnfocusedActive"], tc.tabUnfocusedActive);
        if (c.Has("dockingPreview")) tc.dockingPreview = JsonToVec4(c["dockingPreview"], tc.dockingPreview);
        if (c.Has("text")) tc.text = JsonToVec4(c["text"], tc.text);
        if (c.Has("textDisabled")) tc.textDisabled = JsonToVec4(c["textDisabled"], tc.textDisabled);
        if (c.Has("textSelectedBg")) tc.textSelectedBg = JsonToVec4(c["textSelectedBg"], tc.textSelectedBg);
    }
    
    // Load ThemeStyle
    if (root.Has("style")) {
        const auto& s = root["style"];
        auto& ts = m_Theme.style;
        if (s.Has("windowRounding")) ts.windowRounding = JsonToFloat(s["windowRounding"], ts.windowRounding);
        if (s.Has("frameRounding")) ts.frameRounding = JsonToFloat(s["frameRounding"], ts.frameRounding);
        if (s.Has("tabRounding")) ts.tabRounding = JsonToFloat(s["tabRounding"], ts.tabRounding);
        if (s.Has("scrollbarRounding")) ts.scrollbarRounding = JsonToFloat(s["scrollbarRounding"], ts.scrollbarRounding);
        if (s.Has("grabRounding")) ts.grabRounding = JsonToFloat(s["grabRounding"], ts.grabRounding);
        if (s.Has("childRounding")) ts.childRounding = JsonToFloat(s["childRounding"], ts.childRounding);
        if (s.Has("popupRounding")) ts.popupRounding = JsonToFloat(s["popupRounding"], ts.popupRounding);
        if (s.Has("windowBorderSize")) ts.windowBorderSize = JsonToFloat(s["windowBorderSize"], ts.windowBorderSize);
        if (s.Has("frameBorderSize")) ts.frameBorderSize = JsonToFloat(s["frameBorderSize"], ts.frameBorderSize);
        if (s.Has("tabBorderSize")) ts.tabBorderSize = JsonToFloat(s["tabBorderSize"], ts.tabBorderSize);
        if (s.Has("childBorderSize")) ts.childBorderSize = JsonToFloat(s["childBorderSize"], ts.childBorderSize);
        if (s.Has("popupBorderSize")) ts.popupBorderSize = JsonToFloat(s["popupBorderSize"], ts.popupBorderSize);
        if (s.Has("windowPadding")) ts.windowPadding = JsonToVec2(s["windowPadding"], ts.windowPadding);
        if (s.Has("framePadding")) ts.framePadding = JsonToVec2(s["framePadding"], ts.framePadding);
        if (s.Has("cellPadding")) ts.cellPadding = JsonToVec2(s["cellPadding"], ts.cellPadding);
        if (s.Has("itemSpacing")) ts.itemSpacing = JsonToVec2(s["itemSpacing"], ts.itemSpacing);
        if (s.Has("itemInnerSpacing")) ts.itemInnerSpacing = JsonToVec2(s["itemInnerSpacing"], ts.itemInnerSpacing);
        if (s.Has("indentSpacing")) ts.indentSpacing = JsonToFloat(s["indentSpacing"], ts.indentSpacing);
        if (s.Has("scrollbarSize")) ts.scrollbarSize = JsonToFloat(s["scrollbarSize"], ts.scrollbarSize);
        if (s.Has("grabMinSize")) ts.grabMinSize = JsonToFloat(s["grabMinSize"], ts.grabMinSize);
    }
    
    // Load ThemeFont
    if (root.Has("font")) {
        const auto& f = root["font"];
        auto& tf = m_Theme.font;
        if (f.Has("fontId")) tf.fontId = f["fontId"].ToString();
        if (f.Has("fontSize")) tf.fontSize = JsonToFloat(f["fontSize"], tf.fontSize);
        if (f.Has("fontScale")) tf.fontScale = JsonToFloat(f["fontScale"], tf.fontScale);
    }
    
    Logger::Info("Settings loaded from " + configPath.string());
    return true;
}

bool EditorSettings::LoadKeybinds() {
    auto keybindsPath = GetKeybindsPath();
    
    if (!std::filesystem::exists(keybindsPath)) {
        Logger::Info("No keybinds file found at " + keybindsPath.string() + ", using defaults");
        m_Keybinds.bindings = GetDefaultKeybindings();
        return false;
    }
    
    std::ifstream file(keybindsPath);
    if (!file) {
        Logger::Error("Failed to open keybinds file: " + keybindsPath.string());
        m_Keybinds.bindings = GetDefaultKeybindings();
        return false;
    }
    
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string jsonStr = buffer.str();
    file.close();
    
    JsonValue root = Json::Parse(jsonStr);
    if (root.type != JsonType::Object) {
        Logger::Error("Invalid keybinds file format");
        m_Keybinds.bindings = GetDefaultKeybindings();
        return false;
    }
    
    if (root.Has("leaderKey")) {
        ImGuiKey key = ImGuiKeyFromString(root["leaderKey"].ToString());
        if (key != ImGuiKey_None) {
            m_Keybinds.leaderKey = key;
        }
    }
    
    if (root.Has("modeKey")) {
        ImGuiKey key = ImGuiKeyFromString(root["modeKey"].ToString());
        if (key != ImGuiKey_None) {
            m_Keybinds.modeKey = key;
        }
    }
    
    if (root.Has("insertKey")) {
        ImGuiKey key = ImGuiKeyFromString(root["insertKey"].ToString());
        if (key != ImGuiKey_None) {
            m_Keybinds.insertKey = key;
        }
    }
    
    if (root.Has("defaultMode")) {
        std::string mode = root["defaultMode"].ToString();
        if (mode == "Command") {
            m_Keybinds.defaultMode = EditorInputMode::Command;
        } else {
            m_Keybinds.defaultMode = EditorInputMode::Insert;
        }
    }
    
    if (root.Has("bindings") && root["bindings"].type == JsonType::Array) {
        m_Keybinds.bindings.clear();
        const auto& bindings = std::get<JsonArray>(root["bindings"].data);
        for (const auto& binding : bindings) {
            if (binding.type == JsonType::Object) {
                KeybindEntry entry;
                if (binding.Has("keys")) entry.keys = binding["keys"].ToString();
                if (binding.Has("event")) entry.eventId = binding["event"].ToString();
                if (binding.Has("context")) entry.context = binding["context"].ToString();
                if (!entry.keys.empty() && !entry.eventId.empty()) {
                    m_Keybinds.bindings.push_back(entry);
                }
            }
        }
    }
    
    // If no keybindings loaded, use defaults
    if (m_Keybinds.bindings.empty()) {
        m_Keybinds.bindings = GetDefaultKeybindings();
    }
    
    Logger::Info("Keybinds loaded from " + keybindsPath.string());
    return true;
}

} // namespace sol
