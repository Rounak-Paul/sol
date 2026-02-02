#pragma once

#include <tinyvk/tinyvk.h>
#include "event_system.h"
#include <memory>
#include "../ui/ui_system.h"

namespace sol {

class Application : public tvk::App {
public:
    Application();
    virtual ~Application();

protected:
    void OnStart() override;
    void OnUpdate() override;
    void OnUI() override;
    int GetDockspaceFlags() override;

private:
    UISystem m_UISystem;

    void SetupEvents();
    void SetupUILayers();
};

} // namespace sol
