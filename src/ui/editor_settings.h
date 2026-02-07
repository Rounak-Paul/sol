#pragma once

#include <cstddef>
#include <string>
#include <imgui.h>

namespace sol {

// Editor/buffer area colors (syntax highlighting, gutter, cursor, etc.)
struct EditorColors {
    ImVec4 background   = ImVec4(0.118f, 0.118f, 0.118f, 1.00f);
    ImVec4 text         = ImVec4(0.831f, 0.831f, 0.831f, 1.00f);
    ImVec4 keyword      = ImVec4(0.337f, 0.612f, 0.839f, 1.00f);
    ImVec4 type         = ImVec4(0.306f, 0.788f, 0.690f, 1.00f);
    ImVec4 function     = ImVec4(0.863f, 0.863f, 0.667f, 1.00f);
    ImVec4 variable     = ImVec4(0.612f, 0.863f, 0.996f, 1.00f);
    ImVec4 string       = ImVec4(0.808f, 0.569f, 0.471f, 1.00f);
    ImVec4 number       = ImVec4(0.710f, 0.808f, 0.659f, 1.00f);
    ImVec4 comment      = ImVec4(0.416f, 0.600f, 0.333f, 1.00f);
    ImVec4 op           = ImVec4(0.831f, 0.831f, 0.831f, 1.00f);
    ImVec4 punctuation  = ImVec4(0.831f, 0.831f, 0.831f, 1.00f);
    ImVec4 macro        = ImVec4(0.741f, 0.388f, 0.773f, 1.00f);
    ImVec4 constant     = ImVec4(0.392f, 0.588f, 0.863f, 1.00f);
    ImVec4 error        = ImVec4(0.957f, 0.278f, 0.278f, 1.00f);
    ImVec4 lineNumber   = ImVec4(0.392f, 0.392f, 0.392f, 1.00f);
    ImVec4 currentLine  = ImVec4(0.157f, 0.157f, 0.157f, 1.00f);
    ImVec4 selection    = ImVec4(0.149f, 0.310f, 0.471f, 1.00f);
    ImVec4 cursor       = ImVec4(1.000f, 1.000f, 1.000f, 1.00f);
    ImVec4 popupBg      = ImVec4(0.145f, 0.145f, 0.149f, 1.00f);
    ImVec4 popupBorder  = ImVec4(0.271f, 0.271f, 0.271f, 1.00f);
    ImVec4 popupText    = ImVec4(0.800f, 0.800f, 0.800f, 1.00f);
    ImVec4 popupSelected = ImVec4(0.035f, 0.278f, 0.443f, 1.00f);
};

struct ThemeColors {
    ImVec4 windowBg          = ImVec4(0.05f, 0.05f, 0.06f, 1.00f);
    ImVec4 childBg           = ImVec4(0.06f, 0.06f, 0.07f, 1.00f);
    ImVec4 popupBg           = ImVec4(0.05f, 0.05f, 0.06f, 1.00f);
    ImVec4 border            = ImVec4(0.20f, 0.20f, 0.22f, 1.00f);
    ImVec4 frameBg           = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);
    ImVec4 frameBgHovered    = ImVec4(0.15f, 0.15f, 0.17f, 1.00f);
    ImVec4 frameBgActive     = ImVec4(0.20f, 0.20f, 0.22f, 1.00f);
    ImVec4 titleBg           = ImVec4(0.03f, 0.03f, 0.04f, 1.00f);
    ImVec4 titleBgActive     = ImVec4(0.08f, 0.08f, 0.10f, 1.00f);
    ImVec4 menuBarBg         = ImVec4(0.08f, 0.08f, 0.10f, 1.00f);
    ImVec4 scrollbarBg       = ImVec4(0.05f, 0.05f, 0.06f, 1.00f);
    ImVec4 scrollbarGrab     = ImVec4(0.25f, 0.25f, 0.28f, 1.00f);
    ImVec4 scrollbarGrabHovered = ImVec4(0.35f, 0.35f, 0.38f, 1.00f);
    ImVec4 scrollbarGrabActive  = ImVec4(0.45f, 0.45f, 0.48f, 1.00f);
    ImVec4 checkMark         = ImVec4(0.80f, 0.80f, 0.85f, 1.00f);
    ImVec4 sliderGrab        = ImVec4(0.50f, 0.50f, 0.55f, 1.00f);
    ImVec4 sliderGrabActive  = ImVec4(0.70f, 0.70f, 0.75f, 1.00f);
    ImVec4 button            = ImVec4(0.12f, 0.12f, 0.15f, 1.00f);
    ImVec4 buttonHovered     = ImVec4(0.20f, 0.20f, 0.23f, 1.00f);
    ImVec4 buttonActive      = ImVec4(0.28f, 0.28f, 0.31f, 1.00f);
    ImVec4 header            = ImVec4(0.12f, 0.12f, 0.15f, 1.00f);
    ImVec4 headerHovered     = ImVec4(0.20f, 0.20f, 0.23f, 1.00f);
    ImVec4 headerActive      = ImVec4(0.28f, 0.28f, 0.31f, 1.00f);
    ImVec4 separator         = ImVec4(0.20f, 0.20f, 0.22f, 1.00f);
    ImVec4 separatorHovered  = ImVec4(0.28f, 0.28f, 0.31f, 1.00f);
    ImVec4 tab               = ImVec4(0.08f, 0.08f, 0.10f, 1.00f);
    ImVec4 tabHovered        = ImVec4(0.18f, 0.18f, 0.21f, 1.00f);
    ImVec4 tabActive         = ImVec4(0.12f, 0.12f, 0.15f, 1.00f);
    ImVec4 tabUnfocused      = ImVec4(0.05f, 0.05f, 0.06f, 1.00f);
    ImVec4 tabUnfocusedActive = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);
    ImVec4 dockingPreview    = ImVec4(0.45f, 0.45f, 0.48f, 0.70f);
    ImVec4 text              = ImVec4(0.95f, 0.95f, 0.97f, 1.00f);
    ImVec4 textDisabled      = ImVec4(0.50f, 0.50f, 0.53f, 1.00f);
    ImVec4 textSelectedBg    = ImVec4(0.28f, 0.28f, 0.31f, 1.00f);
};

struct ThemeStyle {
    float windowRounding    = 0.0f;
    float frameRounding     = 0.0f;
    float tabRounding       = 0.0f;
    float scrollbarRounding = 0.0f;
    float grabRounding      = 0.0f;
    float childRounding     = 0.0f;
    float popupRounding     = 0.0f;

    float windowBorderSize  = 1.0f;
    float frameBorderSize   = 1.0f;
    float tabBorderSize     = 0.0f;
    float childBorderSize   = 1.0f;
    float popupBorderSize   = 1.0f;

    ImVec2 windowPadding    = ImVec2(6.0f, 6.0f);
    ImVec2 framePadding     = ImVec2(4.0f, 2.0f);
    ImVec2 cellPadding      = ImVec2(4.0f, 2.0f);
    ImVec2 itemSpacing      = ImVec2(6.0f, 4.0f);
    ImVec2 itemInnerSpacing = ImVec2(4.0f, 4.0f);
    float indentSpacing     = 16.0f;
    float scrollbarSize     = 12.0f;
    float grabMinSize       = 8.0f;
};

struct ThemeFont {
    std::string fontId   = "jetbrains_mono_nerd";
    float fontSize       = 16.0f;
    float fontScale      = 0.9f;
};

struct Theme {
    std::string name = "Space Black";
    ThemeColors colors;
    ThemeStyle style;
    ThemeFont font;
    EditorColors editor;
};

class EditorSettings {
public:
    static EditorSettings& Get() {
        static EditorSettings instance;
        return instance;
    }
    
    // Cursor position for status bar
    size_t GetCursorLine() const { return m_CursorLine; }
    size_t GetCursorCol() const { return m_CursorCol; }
    void SetCursorPos(size_t line, size_t col) { m_CursorLine = line; m_CursorCol = col; }

    Theme& GetTheme() { return m_Theme; }
    const Theme& GetTheme() const { return m_Theme; }

    // Apply current theme to ImGui
    void ApplyTheme();

    // Set a preset theme
    void SetPreset(const std::string& name);

    // Mark that font needs reload
    bool FontDirty() const { return m_FontDirty; }
    void SetFontDirty(bool dirty) { m_FontDirty = dirty; }

private:
    EditorSettings() = default;
    
    size_t m_CursorLine = 1;
    size_t m_CursorCol = 1;
    Theme m_Theme;
    bool m_FontDirty = false;
};

} // namespace sol
