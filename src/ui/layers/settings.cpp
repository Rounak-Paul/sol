#include "settings.h"
#include "ui/editor_settings.h"
#include "ui/input/command.h"
#include "ui/input/keybinding.h"
#include <tinyvk/tinyvk.h>
#include <tinyvk/ui/imgui_layer.h>
#include <imgui.h>
#include <cstring>
#include <algorithm>

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
            if (ImGui::BeginTabItem("Keybindings")) {
                RenderKeybindingsTab();
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
                settings.Save();
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    // Font size
    if (ImGui::DragFloat("Font Size", &font.fontSize, 0.5f, 8.0f, 32.0f, "%.1f")) {
        settings.SetFontDirty(true);
        settings.Save();
    }

    // Font scale
    if (ImGui::DragFloat("UI Scale", &font.fontScale, 0.01f, 0.5f, 2.0f, "%.2f")) {
        ImGui::GetIO().FontGlobalScale = font.fontScale;
        settings.Save();
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
                settings.Save();
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
        settings.Save();
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
        settings.Save();
    }
}

void SettingsWindow::RenderKeybindingsTab() {
    auto& settings = EditorSettings::Get();
    auto& keybinds = settings.GetKeybinds();
    auto& inputSystem = InputSystem::GetInstance();
    auto* keymap = inputSystem.GetActiveKeymap();
    
    if (!keymap) {
        ImGui::TextUnformatted("No active keymap");
        return;
    }
    
    bool settingsChanged = false;
    bool rebindNeeded = false;
    
    // Modal Keys Configuration Section
    ImGui::TextUnformatted("Modal Editing Keys");
    ImGui::Separator();
    ImGui::Spacing();
    
    // Leader key
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Leader Key: %s", ImGuiKeyToString(keybinds.leaderKey).c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Set##Leader")) {
        m_CapturingKeyType = 1;
        m_IsCapturingKey = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(Prefix key for keybinds)");
    
    // Mode key (to enter Command mode)
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Command Mode Key: %s", ImGuiKeyToString(keybinds.modeKey).c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Set##Mode")) {
        m_CapturingKeyType = 2;
        m_IsCapturingKey = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(Enter command mode from insert)");
    
    // Insert key (to enter Insert mode)
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Insert Mode Key: %s", ImGuiKeyToString(keybinds.insertKey).c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Set##Insert")) {
        m_CapturingKeyType = 3;
        m_IsCapturingKey = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(Enter insert mode from command)");
    
    ImGui::Spacing();
    ImGui::TextUnformatted("Command Mode Navigation");
    ImGui::Separator();
    ImGui::Spacing();
    
    // Navigation keys for command mode
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Nav Up: %s", ImGuiKeyToString(keybinds.navUp).c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Set##NavUp")) {
        m_CapturingKeyType = 4;
        m_IsCapturingKey = true;
    }
    
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Nav Down: %s", ImGuiKeyToString(keybinds.navDown).c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Set##NavDown")) {
        m_CapturingKeyType = 5;
        m_IsCapturingKey = true;
    }
    
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Nav Left: %s", ImGuiKeyToString(keybinds.navLeft).c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Set##NavLeft")) {
        m_CapturingKeyType = 6;
        m_IsCapturingKey = true;
    }
    
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Nav Right: %s", ImGuiKeyToString(keybinds.navRight).c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Set##NavRight")) {
        m_CapturingKeyType = 7;
        m_IsCapturingKey = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(Arrow keys also work)");
    
    // Current mode indicator
    ImGui::Spacing();
    const char* modeStr = inputSystem.GetInputMode() == EditorInputMode::Command ? "COMMAND" : "INSERT";
    ImGui::Text("Current Mode: %s", modeStr);
    
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    
    // Use settings bindings as source of truth, not keymap
    auto& bindingsFromSettings = keybinds.bindings;
    
    // Search filter
    static char searchBuf[128] = "";
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##search", "Search keybindings...", searchBuf, sizeof(searchBuf));
    ImGui::Spacing();
    
    // Key capture modal
    if (m_IsCapturingKey) {
        ImGui::OpenPopup("Capture Key");
    }
    
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Capture Key", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const char* captureTitle = "";
        switch (m_CapturingKeyType) {
            case 0: captureTitle = m_RebindingCommandId.c_str(); break;
            case 1: captureTitle = "Leader Key"; break;
            case 2: captureTitle = "Command Mode Key"; break;
            case 3: captureTitle = "Insert Mode Key"; break;
            case 4: captureTitle = "Nav Up"; break;
            case 5: captureTitle = "Nav Down"; break;
            case 6: captureTitle = "Nav Left"; break;
            case 7: captureTitle = "Nav Right"; break;
        }
        
        if (m_CapturingKeyType == 0) {
            // Sequence capture mode
            ImGui::Text("Recording keybind for: %s", m_RebindingCommandId.c_str());
            ImGui::Spacing();
            if (!m_CapturedSequence.empty()) {
                ImGui::Text("Current: %s", m_CapturedSequence.c_str());
            } else {
                ImGui::TextDisabled("Press keys...");
            }
            ImGui::Spacing();
            ImGui::TextDisabled("Press Enter to confirm, Escape to cancel");
        } else {
            ImGui::Text("Press key for: %s", captureTitle);
            ImGui::Spacing();
            ImGui::TextDisabled("Press Escape to cancel");
        }
        
        // Capture key input
        for (ImGuiKey key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; key = (ImGuiKey)(key + 1)) {
            // Skip modifier keys for all captures
            if (key == ImGuiKey_LeftCtrl || key == ImGuiKey_RightCtrl ||
                key == ImGuiKey_LeftShift || key == ImGuiKey_RightShift ||
                key == ImGuiKey_LeftAlt || key == ImGuiKey_RightAlt ||
                key == ImGuiKey_LeftSuper || key == ImGuiKey_RightSuper) {
                continue;
            }
            
            if (ImGui::IsKeyPressed(key, false)) {
                if (key == ImGuiKey_Escape) {
                    // Cancel capture
                    m_IsCapturingKey = false;
                    m_RebindingCommandId.clear();
                    m_CapturingKeyType = 0;
                    m_CapturedSequence.clear();
                    m_CapturingSequence = false;
                    ImGui::CloseCurrentPopup();
                    break;
                }
                
                if (m_CapturingKeyType == 0) {
                    // Keybind sequence capture
                    if (key == ImGuiKey_Enter && !m_CapturedSequence.empty()) {
                        // Confirm the sequence
                        for (auto& entry : bindingsFromSettings) {
                            if (entry.eventId == m_RebindingCommandId) {
                                entry.keys = m_CapturedSequence;
                                break;
                            }
                        }
                        settingsChanged = true;
                        rebindNeeded = true;
                        m_IsCapturingKey = false;
                        m_RebindingCommandId.clear();
                        m_CapturedSequence.clear();
                        m_CapturingSequence = false;
                        ImGui::CloseCurrentPopup();
                    } else if (key != ImGuiKey_Enter) {
                        // Add key to sequence
                        if (!m_CapturedSequence.empty()) {
                            m_CapturedSequence += " ";
                        }
                        
                        // Check if it's the leader key
                        if (key == keybinds.leaderKey) {
                            m_CapturedSequence += "Leader";
                        } else {
                            // Get modifiers
                            Modifier mods = KeyChord::GetCurrentModifiers();
                            
                            // For letters, check shift for uppercase
                            bool isLetter = (key >= ImGuiKey_A && key <= ImGuiKey_Z);
                            bool hasShift = HasModifier(mods, Modifier::Shift);
                            
                            if (isLetter && hasShift && !HasModifier(mods, Modifier::Ctrl) && !HasModifier(mods, Modifier::Alt) && !HasModifier(mods, Modifier::Super)) {
                                // Uppercase letter
                                m_CapturedSequence += static_cast<char>('A' + (key - ImGuiKey_A));
                            } else {
                                // Build with modifiers
                                if (HasModifier(mods, Modifier::Ctrl)) m_CapturedSequence += "Ctrl+";
                                if (HasModifier(mods, Modifier::Shift)) m_CapturedSequence += "Shift+";
                                if (HasModifier(mods, Modifier::Alt)) m_CapturedSequence += "Alt+";
                                if (HasModifier(mods, Modifier::Super)) m_CapturedSequence += "Cmd+";
                                
                                // Add key name (lowercase for letters)
                                if (isLetter) {
                                    m_CapturedSequence += static_cast<char>('a' + (key - ImGuiKey_A));
                                } else {
                                    m_CapturedSequence += ImGuiKeyToString(key);
                                }
                            }
                        }
                    }
                } else {
                    // Single key capture (leader, mode, insert, nav keys)
                    switch (m_CapturingKeyType) {
                        case 1: // Leader key
                            keybinds.leaderKey = key;
                            settingsChanged = true;
                            rebindNeeded = true;
                            break;
                        case 2: // Mode key
                            keybinds.modeKey = key;
                            settingsChanged = true;
                            break;
                        case 3: // Insert key
                            keybinds.insertKey = key;
                            settingsChanged = true;
                            break;
                        case 4: // Nav up
                            keybinds.navUp = key;
                            settingsChanged = true;
                            break;
                        case 5: // Nav down
                            keybinds.navDown = key;
                            settingsChanged = true;
                            break;
                        case 6: // Nav left
                            keybinds.navLeft = key;
                            settingsChanged = true;
                            break;
                        case 7: // Nav right
                            keybinds.navRight = key;
                            settingsChanged = true;
                            break;
                    }
                    
                    m_IsCapturingKey = false;
                    m_RebindingCommandId.clear();
                    m_CapturingKeyType = 0;
                    ImGui::CloseCurrentPopup();
                }
                break;
            }
        }
        
        ImGui::EndPopup();
    }
    
    // Keybindings table - display from settings, not keymap
    if (ImGui::BeginTable("KeybindingsTable", 3, 
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
        ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 2))) {
        
        ImGui::TableSetupColumn("Event", ImGuiTableColumnFlags_WidthStretch, 0.45f);
        ImGui::TableSetupColumn("Keybinding", ImGuiTableColumnFlags_WidthStretch, 0.35f);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        
        std::string searchStr = searchBuf;
        std::transform(searchStr.begin(), searchStr.end(), searchStr.begin(), ::tolower);
        
        for (size_t i = 0; i < bindingsFromSettings.size(); ++i) {
            auto& entry = bindingsFromSettings[i];
            
            // Filter by search
            if (!searchStr.empty()) {
                std::string eventId = entry.eventId;
                std::string keyStr = entry.keys;
                std::transform(eventId.begin(), eventId.end(), eventId.begin(), ::tolower);
                std::transform(keyStr.begin(), keyStr.end(), keyStr.begin(), ::tolower);
                if (eventId.find(searchStr) == std::string::npos && 
                    keyStr.find(searchStr) == std::string::npos) {
                    continue;
                }
            }
            
            ImGui::TableNextRow();
            
            // Event ID
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(entry.eventId.c_str());
            
            // Keybinding - show with "<L>" instead of "Leader" for compact display
            ImGui::TableNextColumn();
            std::string displayKeys = entry.keys;
            size_t pos = 0;
            while ((pos = displayKeys.find("Leader", pos)) != std::string::npos) {
                displayKeys.replace(pos, 6, "<L>");
                pos += 3;
            }
            ImGui::TextUnformatted(displayKeys.c_str());
            
            // Actions
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::SmallButton("Edit")) {
                m_RebindingCommandId = entry.eventId;
                m_CapturingKeyType = 0;
                m_IsCapturingKey = true;
                m_CapturedSequence.clear();
                m_CapturingSequence = false;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear")) {
                entry.keys = "";
                settingsChanged = true;
                rebindNeeded = true;
            }
            ImGui::PopID();
        }
        
        ImGui::EndTable();
    }
    
    // Buttons row
    if (ImGui::Button("Reset to Defaults")) {
        keybinds.leaderKey = ImGuiKey_Space;
        keybinds.modeKey = ImGuiKey_Escape;
        keybinds.insertKey = ImGuiKey_I;
        keybinds.navUp = ImGuiKey_W;
        keybinds.navDown = ImGuiKey_S;
        keybinds.navLeft = ImGuiKey_A;
        keybinds.navRight = ImGuiKey_D;
        keybinds.defaultMode = EditorInputMode::Insert;
        keybinds.bindings = GetDefaultKeybindings();
        settingsChanged = true;
        rebindNeeded = true;
    }
    
    // Save and rebuild keymap if needed
    if (settingsChanged) {
        settings.SaveKeybinds();
    }
    if (rebindNeeded) {
        inputSystem.SetupDefaultBindings();
    }
}

} // namespace sol
