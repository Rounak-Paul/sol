#pragma once

#include <string>
#include <memory>
#include <map>
#include <vector>

namespace sol {

class UILayer {
public:
    using Id = std::string;

    explicit UILayer(const Id& id);
    virtual ~UILayer() = default;

    virtual void OnUI() = 0;

    const Id& GetId() const { return m_Id; }
    bool IsEnabled() const { return m_Enabled; }
    void SetEnabled(bool enabled) { m_Enabled = enabled; }

private:
    Id m_Id;
    bool m_Enabled = true;
};

class UISystem {
public:
    // Layout constants
    static constexpr float StatusBarHeight = 16.0f;
    
    // Dockspace IDs
    static constexpr const char* MainDockSpaceId = "MainDockSpace";
    static constexpr const char* EditorTabBarId = "EditorTabs";

    UISystem() = default;
    ~UISystem() = default;

    UISystem(const UISystem&) = delete;
    UISystem& operator=(const UISystem&) = delete;
    UISystem(UISystem&&) = delete;
    UISystem& operator=(UISystem&&) = delete;

    void RegisterLayer(std::shared_ptr<UILayer> layer);
    void UnregisterLayer(const UILayer::Id& id);
    void RenderLayers();

    std::shared_ptr<UILayer> GetLayer(const UILayer::Id& id);

private:
    std::vector<std::shared_ptr<UILayer>> m_Layers;
    std::map<UILayer::Id, std::shared_ptr<UILayer>> m_LayersMap;
};

} // namespace sol
