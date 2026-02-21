#pragma once

#include "ui/ui_system.h"
#include <string>

namespace sol {

class SettingsWindow : public UILayer {
public:
    explicit SettingsWindow(const Id& id = "Settings");
    ~SettingsWindow() override = default;

    void OnUI() override;

private:
    void RenderThemeTab();
    void RenderFontSection();
    void RenderColorSection();
    void RenderStyleSection();
    void RenderKeybindingsTab();

    int m_SelectedPreset = 0;
    int m_SelectedFont = 0;
    
    // Keybinding editor state
    std::string m_RebindingCommandId;
    bool m_IsCapturingKey = false;
    int m_CapturingKeyType = 0;  // 0=keybind, 1=leader, 2=mode, 3=insert
};

} // namespace sol
