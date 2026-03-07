#pragma once

#include <tinyvk/tinyvk.h>
#include "ui/ui_system.h"
#include "ui/layers/menu_bar.h"
#include <memory>
#include <string>

namespace sol {

class Buffer;
class Workspace;

class Application : public tvk::App {
public:
    Application();
    virtual ~Application();

    void SetArgs(int argc, char* argv[]);

protected:
    void OnStart() override;
    void OnUpdate() override;
    void OnMenuBar() override;
    void OnUI() override;
    int GetDockspaceFlags() override;
    float GetDockspaceBottomOffset() override;

private:
    UISystem m_UISystem;
    MenuBar m_MenuBar{&m_UISystem};
    std::shared_ptr<Workspace> m_Workspace;

    std::string m_ExecutablePath;
    std::string m_InitialPath;
    
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
