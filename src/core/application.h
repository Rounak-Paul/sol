#pragma once

#include <tinyvk/tinyvk.h>
#include "ui/ui_system.h"
#include <memory>
#include <string>

namespace sol {

class TerminalPanel;
class Buffer;

class Application : public tvk::App {
public:
    Application();
    virtual ~Application();

protected:
    void OnStart() override;
    void OnUpdate() override;
    void OnUI() override;
    int GetDockspaceFlags() override;
    float GetDockspaceBottomOffset() override;

private:
    UISystem m_UISystem;
    std::shared_ptr<TerminalPanel> m_TerminalPanel;
    
    // SaveAs dialog state
    bool m_ShowSaveAsDialog = false;
    std::shared_ptr<Buffer> m_SaveAsBuffer;
    char m_SaveAsFilename[256] = {};
    char m_SaveAsPath[512] = {};

    void SetupEvents();
    void SetupUILayers();
    void SetupInputSystem();
    void ProcessInput();
    void RenderSaveAsDialog();
    void OpenSaveAsDialog(std::shared_ptr<Buffer> buffer);
};

} // namespace sol
