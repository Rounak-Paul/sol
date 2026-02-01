#pragma once

#include <tinyvk/tinyvk.h>

namespace sol {

class Application : public tvk::App {
public:
    Application();
    virtual ~Application();

protected:
    void OnUpdate() override;
    void OnUI() override;

private:
    float m_Counter = 0.0f;
};

} // namespace sol
