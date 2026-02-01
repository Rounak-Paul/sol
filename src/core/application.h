#pragma once

#include <tinyvk/tinyvk.h>
#include "event_system.h"
#include "../ui/ui_system.h"
#include "../ui/layers/main_window_layer.h"
#include <memory>

namespace sol {

class Application : public tvk::App {
public:
    Application();
    virtual ~Application();

protected:
    void OnStart() override;
    void OnUpdate() override;
    void OnUI() override;

private:
    float m_Counter = 0.0f;
    UISystem m_UISystem;
    std::shared_ptr<MainWindowLayer> m_MainWindow;

    void SetupEvents();
    void SetupUILayers();
};

} // namespace sol
