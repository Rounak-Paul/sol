#pragma once

#include "../ui_system.h"

namespace sol
{
class Explorer : public UILayer
{
public:
    explicit Explorer(const Id& id = "Explorer");
    ~Explorer() override = default;

    void OnUI() override;

private:
    void SetupExplorer();
};
} // namespace sol