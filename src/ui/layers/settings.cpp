#include "settings.h"
#include "ui/editor_settings.h"
#include <tinyvk/tinyvk.h>
#include <tinyvk/ui/imgui_layer.h>
#include <imgui.h>
#include <cstring>

namespace sol {

static const char* s_PresetNames[] = { "Space Black", "Dark", "Light", "Nord" };
static constexpr int s_PresetCount = 4;

SettingsWindow::SettingsWindow(const Id& id) : UILayer(id) {
    // Find current preset index
    auto& theme = EditorSettings::Get().GetTheme();
    for (int i = 0; i < s_PresetCount; i++) {
        if (theme.name == s_PresetNames[i]) {
            m_SelectedPreset = i;
            break;
        }
    }
    // Find current font index
    auto& fonts = tvk::ImGuiLayer::GetAvailableFonts();
    for (int i = 0; i < static_cast<int>(fonts.size()); i++) {
        if (theme.font.fontId == fonts[i].id) {
            m_SelectedFont = i;
            break;
        }
    }
}

void SettingsWindow::OnUI() {
    if (!IsEnabled()) return;

    bool open = true;
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(520, 480), ImGuiCond_FirstUseEver);
    ImVec2 center(viewport->GetCenter());
    ImGui::SetNextWindowPos(center, ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    
    ImGuiWindowFlags flags = ImGuiWindowFlags_None;
    if (ImGui::Begin("Settings", &open, flags)) {
        if (ImGui::BeginTabBar("SettingsTabs")) {
            if (ImGui::BeginTabItem("Theme")) {
                RenderThemeTab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();

    if (!open) {
        SetEnabled(false);
    }
}

void SettingsWindow::RenderThemeTab() {
    RenderFontSection();
    ImGui::Separator();
    RenderColorSection();
    ImGui::Separator();
    RenderStyleSection();
}

void SettingsWindow::RenderFontSection() {
    ImGui::TextUnformatted("Font");
    ImGui::Spacing();

    auto& settings = EditorSettings::Get();
    auto& font = settings.GetTheme().font;
    auto& fonts = tvk::ImGuiLayer::GetAvailableFonts();

    // Font selector
    if (ImGui::BeginCombo("Font Family", fonts[m_SelectedFont].displayName)) {
        for (int i = 0; i < static_cast<int>(fonts.size()); i++) {
            bool selected = (m_SelectedFont == i);
            if (ImGui::Selectable(fonts[i].displayName, selected)) {
                m_SelectedFont = i;
                font.fontId = fonts[i].id;
                settings.SetFontDirty(true);
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    // Font size
    if (ImGui::DragFloat("Font Size", &font.fontSize, 0.5f, 8.0f, 32.0f, "%.1f")) {
        settings.SetFontDirty(true);
    }

    // Font scale
    if (ImGui::DragFloat("UI Scale", &font.fontScale, 0.01f, 0.5f, 2.0f, "%.2f")) {
        ImGui::GetIO().FontGlobalScale = font.fontScale;
    }

    // Apply font button (font reload is expensive so we do it on demand)
    if (settings.FontDirty()) {
        ImGui::SameLine();
        if (ImGui::Button("Apply Font")) {
            auto* app = tvk::App::Get();
            if (app) {
                app->GetImGuiLayer()->ReloadFont(font.fontId.c_str(), font.fontSize, 1.0f);
                ImGui::GetIO().FontGlobalScale = font.fontScale;
            }
            settings.SetFontDirty(false);
        }
    }
}

void SettingsWindow::RenderColorSection() {
    ImGui::TextUnformatted("Colors");
    ImGui::Spacing();

    auto& settings = EditorSettings::Get();

    // Preset selector
    if (ImGui::BeginCombo("Preset", s_PresetNames[m_SelectedPreset])) {
        for (int i = 0; i < s_PresetCount; i++) {
            bool selected = (m_SelectedPreset == i);
            if (ImGui::Selectable(s_PresetNames[i], selected)) {
                m_SelectedPreset = i;
                settings.SetPreset(s_PresetNames[i]);
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    auto& c = settings.GetTheme().colors;
    bool changed = false;

    if (ImGui::TreeNode("Window")) {
        changed |= ImGui::ColorEdit4("Background", &c.windowBg.x, ImGuiColorEditFlags_NoInputs);
        changed |= ImGui::ColorEdit4("Child Background", &c.childBg.x, ImGuiColorEditFlags_NoInputs);
        changed |= ImGui::ColorEdit4("Popup Background", &c.popupBg.x, ImGuiColorEditFlags_NoInputs);
        changed |= ImGui::ColorEdit4("Border", &c.border.x, ImGuiColorEditFlags_NoInputs);
        changed |= ImGui::ColorEdit4("Menu Bar", &c.menuBarBg.x, ImGuiColorEditFlags_NoInputs);
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Text")) {
        changed |= ImGui::ColorEdit4("Text", &c.text.x, ImGuiColorEditFlags_NoInputs);
        changed |= ImGui::ColorEdit4("Disabled", &c.textDisabled.x, ImGuiColorEditFlags_NoInputs);
        changed |= ImGui::ColorEdit4("Selected", &c.textSelectedBg.x, ImGuiColorEditFlags_NoInputs);
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Controls")) {
        changed |= ImGui::ColorEdit4("Frame", &c.frameBg.x, ImGuiColorEditFlags_NoInputs);
        changed |= ImGui::ColorEdit4("Frame Hovered", &c.frameBgHovered.x, ImGuiColorEditFlags_NoInputs);
        changed |= ImGui::ColorEdit4("Frame Active", &c.frameBgActive.x, ImGuiColorEditFlags_NoInputs);
        changed |= ImGui::ColorEdit4("Button", &c.button.x, ImGuiColorEditFlags_NoInputs);
        changed |= ImGui::ColorEdit4("Button Hovered", &c.buttonHovered.x, ImGuiColorEditFlags_NoInputs);
        changed |= ImGui::ColorEdit4("Button Active", &c.buttonActive.x, ImGuiColorEditFlags_NoInputs);
        changed |= ImGui::ColorEdit4("Check Mark", &c.checkMark.x, ImGuiColorEditFlags_NoInputs);
        changed |= ImGui::ColorEdit4("Slider", &c.sliderGrab.x, ImGuiColorEditFlags_NoInputs);
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Headers & Tabs")) {
        changed |= ImGui::ColorEdit4("Header", &c.header.x, ImGuiColorEditFlags_NoInputs);
        changed |= ImGui::ColorEdit4("Header Hovered", &c.headerHovered.x, ImGuiColorEditFlags_NoInputs);
        changed |= ImGui::ColorEdit4("Tab", &c.tab.x, ImGuiColorEditFlags_NoInputs);
        changed |= ImGui::ColorEdit4("Tab Hovered", &c.tabHovered.x, ImGuiColorEditFlags_NoInputs);
        changed |= ImGui::ColorEdit4("Tab Active", &c.tabActive.x, ImGuiColorEditFlags_NoInputs);
        changed |= ImGui::ColorEdit4("Title Bar", &c.titleBg.x, ImGuiColorEditFlags_NoInputs);
        changed |= ImGui::ColorEdit4("Title Bar Active", &c.titleBgActive.x, ImGuiColorEditFlags_NoInputs);
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Scrollbar")) {
        changed |= ImGui::ColorEdit4("Background", &c.scrollbarBg.x, ImGuiColorEditFlags_NoInputs);
        changed |= ImGui::ColorEdit4("Grab", &c.scrollbarGrab.x, ImGuiColorEditFlags_NoInputs);
        changed |= ImGui::ColorEdit4("Grab Hovered", &c.scrollbarGrabHovered.x, ImGuiColorEditFlags_NoInputs);
        changed |= ImGui::ColorEdit4("Grab Active", &c.scrollbarGrabActive.x, ImGuiColorEditFlags_NoInputs);
        ImGui::TreePop();
    }

    auto& e = settings.GetTheme().editor;

    if (ImGui::TreeNode("Editor")) {
        changed |= ImGui::ColorEdit4("Background##ed", &e.background.x, ImGuiColorEditFlags_NoInputs);
        changed |= ImGui::ColorEdit4("Text##ed", &e.text.x, ImGuiColorEditFlags_NoInputs);
        changed |= ImGui::ColorEdit4("Current Line##ed", &e.currentLine.x, ImGuiColorEditFlags_NoInputs);
        changed |= ImGui::ColorEdit4("Selection##ed", &e.selection.x, ImGuiColorEditFlags_NoInputs);
        changed |= ImGui::ColorEdit4("Cursor##ed", &e.cursor.x, ImGuiColorEditFlags_NoInputs);
        changed |= ImGui::ColorEdit4("Line Numbers##ed", &e.lineNumber.x, ImGuiColorEditFlags_NoInputs);
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Syntax Highlighting")) {
        changed |= ImGui::ColorEdit4("Keyword", &e.keyword.x, ImGuiColorEditFlags_NoInputs);
        changed |= ImGui::ColorEdit4("Type", &e.type.x, ImGuiColorEditFlags_NoInputs);
        changed |= ImGui::ColorEdit4("Function", &e.function.x, ImGuiColorEditFlags_NoInputs);
        changed |= ImGui::ColorEdit4("Variable", &e.variable.x, ImGuiColorEditFlags_NoInputs);
        changed |= ImGui::ColorEdit4("String", &e.string.x, ImGuiColorEditFlags_NoInputs);
        changed |= ImGui::ColorEdit4("Number", &e.number.x, ImGuiColorEditFlags_NoInputs);
        changed |= ImGui::ColorEdit4("Comment", &e.comment.x, ImGuiColorEditFlags_NoInputs);
        changed |= ImGui::ColorEdit4("Operator", &e.op.x, ImGuiColorEditFlags_NoInputs);
        changed |= ImGui::ColorEdit4("Punctuation", &e.punctuation.x, ImGuiColorEditFlags_NoInputs);
        changed |= ImGui::ColorEdit4("Macro", &e.macro.x, ImGuiColorEditFlags_NoInputs);
        changed |= ImGui::ColorEdit4("Constant", &e.constant.x, ImGuiColorEditFlags_NoInputs);
        changed |= ImGui::ColorEdit4("Error##syntax", &e.error.x, ImGuiColorEditFlags_NoInputs);
        ImGui::TreePop();
    }

    if (changed) {
        settings.ApplyTheme();
    }
}

void SettingsWindow::RenderStyleSection() {
    ImGui::TextUnformatted("Style");
    ImGui::Spacing();

    auto& settings = EditorSettings::Get();
    auto& s = settings.GetTheme().style;
    bool changed = false;

    if (ImGui::TreeNode("Rounding")) {
        changed |= ImGui::SliderFloat("Window", &s.windowRounding, 0.0f, 12.0f);
        changed |= ImGui::SliderFloat("Frame", &s.frameRounding, 0.0f, 12.0f);
        changed |= ImGui::SliderFloat("Tab", &s.tabRounding, 0.0f, 12.0f);
        changed |= ImGui::SliderFloat("Scrollbar", &s.scrollbarRounding, 0.0f, 12.0f);
        changed |= ImGui::SliderFloat("Grab", &s.grabRounding, 0.0f, 12.0f);
        changed |= ImGui::SliderFloat("Popup", &s.popupRounding, 0.0f, 12.0f);
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Borders")) {
        changed |= ImGui::SliderFloat("Window##border", &s.windowBorderSize, 0.0f, 2.0f);
        changed |= ImGui::SliderFloat("Frame##border", &s.frameBorderSize, 0.0f, 2.0f);
        changed |= ImGui::SliderFloat("Tab##border", &s.tabBorderSize, 0.0f, 2.0f);
        changed |= ImGui::SliderFloat("Child##border", &s.childBorderSize, 0.0f, 2.0f);
        changed |= ImGui::SliderFloat("Popup##border", &s.popupBorderSize, 0.0f, 2.0f);
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Spacing")) {
        changed |= ImGui::DragFloat2("Window Padding", &s.windowPadding.x, 0.5f, 0.0f, 20.0f);
        changed |= ImGui::DragFloat2("Frame Padding", &s.framePadding.x, 0.5f, 0.0f, 20.0f);
        changed |= ImGui::DragFloat2("Item Spacing", &s.itemSpacing.x, 0.5f, 0.0f, 20.0f);
        changed |= ImGui::DragFloat2("Item Inner Spacing", &s.itemInnerSpacing.x, 0.5f, 0.0f, 20.0f);
        changed |= ImGui::SliderFloat("Indent", &s.indentSpacing, 0.0f, 30.0f);
        changed |= ImGui::SliderFloat("Scrollbar Size", &s.scrollbarSize, 1.0f, 20.0f);
        ImGui::TreePop();
    }

    if (changed) {
        settings.ApplyTheme();
    }
}

} // namespace sol
