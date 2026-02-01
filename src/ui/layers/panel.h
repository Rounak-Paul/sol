#pragma once

#include "../ui_system.h"

namespace sol
{
class Panel : public UILayer
{
public:
    explicit Panel(const Id& id = "pannel");
    ~Panel() override = default;

    void OnUI() override;

private:
    void SetupPannel();
};
} // namespace sol
