#pragma once

#include <tinyvk/tinyvk.h>
#include "ui/ui_system.h"
#include <memory>

namespace sol {

class TerminalPanel;

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

    void SetupEvents();
    void SetupUILayers();
    void SetupInputSystem();
    void ProcessInput();
};

} // namespace sol
