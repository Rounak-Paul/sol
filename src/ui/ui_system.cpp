#include "ui_system.h"

namespace sol {

UILayer::UILayer(const Id& id)
    : m_Id(id) {
}

void UISystem::RegisterLayer(std::shared_ptr<UILayer> layer) {
    if (!layer) {
        return;
    }

    m_Layers.push_back(layer);
    m_LayersMap[layer->GetId()] = layer;
}

void UISystem::UnregisterLayer(const UILayer::Id& id) {
    auto it = m_LayersMap.find(id);
    if (it != m_LayersMap.end()) {
        m_LayersMap.erase(it);
        m_Layers.erase(
            std::remove_if(m_Layers.begin(), m_Layers.end(),
                          [&id](const std::shared_ptr<UILayer>& layer) {
                              return layer->GetId() == id;
                          }),
            m_Layers.end()
        );
    }
}

void UISystem::RenderLayers() {
    for (auto& layer : m_Layers) {
        if (layer->IsEnabled()) {
            layer->OnUI();
        }
    }
}

std::shared_ptr<UILayer> UISystem::GetLayer(const UILayer::Id& id) {
    auto it = m_LayersMap.find(id);
    if (it != m_LayersMap.end()) {
        return it->second;
    }
    return nullptr;
}

} // namespace sol
