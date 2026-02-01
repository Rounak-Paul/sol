#pragma once

#include "../ui_system.h"

namespace sol {

class MainWindowLayer : public UILayer {
public:
    explicit MainWindowLayer(const Id& id = "main_window");
    ~MainWindowLayer() override = default;

    void OnUI() override;
    void SetCounter(float counter) { m_Counter = counter; }

private:
    float m_Counter = 0.0f;
    float m_DeltaTime = 0.0f;

public:
    void SetDeltaTime(float dt) { m_DeltaTime = dt; }
};

} // namespace sol
