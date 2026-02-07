#include "editor_settings.h"
#include <imgui.h>

namespace sol {

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

} // namespace sol
