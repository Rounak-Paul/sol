#pragma once

#include "ui/ui_system.h"

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

    int m_SelectedPreset = 0;
    int m_SelectedFont = 0;
};

} // namespace sol
