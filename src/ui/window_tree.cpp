#include "window_tree.h"
#include "core/resource_system.h"
#include "core/text/text_buffer.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>

namespace sol {

// --- Window ---

uint32_t Window::GenerateId() {
    static uint32_t nextId = 1;
    return nextId++;
}

Window::Window(uint32_t id) : m_Id(id ? id : GenerateId()) {}

void Window::ShowBuffer(size_t bufferId) {
    if (m_ContentType == WindowContent::Buffer && m_BufferId == bufferId)
        return;

    m_ContentType = WindowContent::Buffer;
    m_BufferId = bufferId;

    if (!m_Editor)
        m_Editor = std::make_unique<SyntaxEditor>();
}

void Window::Clear() {
    m_Editor.reset();
    m_ContentType = WindowContent::Empty;
    m_BufferId = 0;
}


bool Window::Render(const char* label, const ImVec2& size, bool isActiveWindow) {
    bool modified = false;

    switch (m_ContentType) {
        case WindowContent::Buffer: {
            auto& rs = ResourceSystem::GetInstance();
            auto buffer = rs.GetBuffer(m_BufferId);
            if (!buffer) {
                ImGui::TextDisabled("Buffer not found");
                break;
            }
            auto resource = buffer->GetResource();
            if (resource->GetType() == ResourceType::Text) {
                auto textResource = std::dynamic_pointer_cast<TextResource>(resource);
                if (textResource && m_Editor) {
                    m_Editor->SetWindowActive(isActiveWindow);
                    if (m_WantsFocus) {
                        m_Editor->Focus();
                        m_WantsFocus = false;
                    }
                    TextBuffer& textBuf = textResource->GetBuffer();
                    if (m_Editor->Render(label, textBuf, size)) {
                        textResource->SetModified(true);
                        modified = true;
                    }
                }
            } else {
                ImGui::TextDisabled("Unsupported file type");
            }
            break;
        }
        case WindowContent::Empty: {
            ImVec2 region = ImGui::GetContentRegionAvail();
            ImGui::SetCursorPos(ImVec2(region.x * 0.5f - 40, region.y * 0.5f));
            ImGui::TextDisabled("No buffer open");
            break;
        }
    }
    return modified;
}

void Window::Focus() {
    m_WantsFocus = true;
}

// --- SplitNode ---

SplitNode::SplitNode() : window(std::make_unique<Window>()) {}

// --- WindowTree ---

WindowTree::WindowTree() {
    m_Root = std::make_unique<SplitNode>();
    m_ActiveWindow = m_Root->window.get();
}

void WindowTree::Render(const ImVec2& pos, const ImVec2& size) {
    RenderNode(m_Root.get(), pos, size);
}

void WindowTree::RenderNode(SplitNode* node, const ImVec2& pos, const ImVec2& size) {
    if (!node) return;

    if (node->isLeaf) {
        Window* window = node->window.get();
        if (!window) return;

        bool isActive = m_TreeFocused && (window == m_ActiveWindow);

        char childId[64];
        snprintf(childId, sizeof(childId), "##win_%u", window->GetId());

        // Active window gets a subtle highlight border
        ImVec4 borderColor = isActive ? ImVec4(0.35f, 0.55f, 0.9f, 0.7f) : ImVec4(0.15f, 0.15f, 0.15f, 0.4f);
        ImGui::PushStyleColor(ImGuiCol_Border, borderColor);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);

        ImGui::SetCursorScreenPos(pos);
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        ImGui::BeginChild(childId, size, ImGuiChildFlags_Borders, flags);

        // Track focus: if this child (or its descendants) got focused, make it active
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)) {
            m_ActiveWindow = window;
        }

        ImVec2 avail = ImGui::GetContentRegionAvail();
        window->Render(childId, avail, isActive);

        ImGui::EndChild();

        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
        return;
    }

    // Non-leaf: split into two with a draggable divider
    ImGui::PushID(node);

    float divSize = DIVIDER_SIZE;

    if (node->direction == SplitDir::Vertical) {
        float total = size.x;
        float w1 = (total - divSize) * node->ratio;
        float w2 = total - w1 - divSize;

        RenderNode(node->first.get(), pos, ImVec2(w1, size.y));

        // Divider
        ImVec2 divPos(pos.x + w1, pos.y);
        ImGui::SetCursorScreenPos(divPos);
        ImGui::InvisibleButton("##d", ImVec2(divSize, size.y));
        if (ImGui::IsItemActive()) {
            float newW1 = w1 + ImGui::GetIO().MouseDelta.x;
            node->ratio = std::clamp(newW1 / (total - divSize), 0.1f, 0.9f);
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        } else if (ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }
        ImGui::GetWindowDrawList()->AddRectFilled(divPos, ImVec2(divPos.x + divSize, divPos.y + size.y), IM_COL32(35, 35, 38, 255));

        RenderNode(node->second.get(), ImVec2(pos.x + w1 + divSize, pos.y), ImVec2(w2, size.y));

    } else { // Horizontal
        float total = size.y;
        float h1 = (total - divSize) * node->ratio;
        float h2 = total - h1 - divSize;

        RenderNode(node->first.get(), pos, ImVec2(size.x, h1));

        // Divider
        ImVec2 divPos(pos.x, pos.y + h1);
        ImGui::SetCursorScreenPos(divPos);
        ImGui::InvisibleButton("##d", ImVec2(size.x, divSize));
        if (ImGui::IsItemActive()) {
            float newH1 = h1 + ImGui::GetIO().MouseDelta.y;
            node->ratio = std::clamp(newH1 / (total - divSize), 0.1f, 0.9f);
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        } else if (ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        }
        ImGui::GetWindowDrawList()->AddRectFilled(divPos, ImVec2(divPos.x + size.x, divPos.y + divSize), IM_COL32(35, 35, 38, 255));

        RenderNode(node->second.get(), ImVec2(pos.x, pos.y + h1 + divSize), ImVec2(size.x, h2));
    }

    ImGui::PopID();
}

Window* WindowTree::SplitActive(SplitDir direction, float ratio) {
    if (!m_ActiveWindow) return nullptr;

    SplitNode* leaf = FindLeaf(m_Root.get(), m_ActiveWindow);
    if (!leaf) return nullptr;

    // Convert leaf → split node
    leaf->isLeaf = false;
    leaf->direction = direction;
    leaf->ratio = ratio;

    // Move existing window to first child
    leaf->first = std::make_unique<SplitNode>();
    leaf->first->window = std::move(leaf->window);

    // Create new window in second child
    leaf->second = std::make_unique<SplitNode>();

    Window* origWindow = leaf->first->window.get();
    Window* newWindow = leaf->second->window.get();

    // New window inherits same buffer (like vim :vsplit)
    if (origWindow->GetContentType() == WindowContent::Buffer) {
        newWindow->ShowBuffer(origWindow->GetBufferId());
    }

    m_ActiveWindow = newWindow;
    newWindow->Focus();
    return newWindow;
}

bool WindowTree::CloseActive() {
    if (!m_ActiveWindow || IsSingleWindow()) return false;

    SplitNode* leaf = FindLeaf(m_Root.get(), m_ActiveWindow);
    if (!leaf) return false;

    SplitNode* parent = FindParent(m_Root.get(), leaf);
    if (!parent) return false;

    // Determine sibling
    bool closingFirst = (parent->first.get() == leaf);
    auto& sibling = closingFirst ? parent->second : parent->first;

    // Promote sibling into parent's position
    if (sibling->isLeaf) {
        parent->isLeaf = true;
        parent->window = std::move(sibling->window);
        parent->first.reset();
        parent->second.reset();
        m_ActiveWindow = parent->window.get();
    } else {
        parent->isLeaf = false;
        parent->direction = sibling->direction;
        parent->ratio = sibling->ratio;
        // Must move children before resetting sibling
        auto f = std::move(sibling->first);
        auto s = std::move(sibling->second);
        parent->first = std::move(f);
        parent->second = std::move(s);

        auto windows = GetAllWindows();
        m_ActiveWindow = windows.empty() ? nullptr : windows[0];
    }

    if (m_ActiveWindow)
        m_ActiveWindow->Focus();
    return true;
}

void WindowTree::FocusNext() {
    auto windows = GetAllWindows();
    if (windows.size() <= 1) return;
    for (size_t i = 0; i < windows.size(); ++i) {
        if (windows[i] == m_ActiveWindow) {
            m_ActiveWindow = windows[(i + 1) % windows.size()];
            m_ActiveWindow->Focus();
            return;
        }
    }
}

void WindowTree::FocusPrev() {
    auto windows = GetAllWindows();
    if (windows.size() <= 1) return;
    for (size_t i = 0; i < windows.size(); ++i) {
        if (windows[i] == m_ActiveWindow) {
            m_ActiveWindow = windows[i == 0 ? windows.size() - 1 : i - 1];
            m_ActiveWindow->Focus();
            return;
        }
    }
}

Window* WindowTree::GetActiveWindow() {
    return m_ActiveWindow;
}

void WindowTree::SetActiveWindow(Window* window) {
    m_ActiveWindow = window;
    if (m_ActiveWindow)
        m_ActiveWindow->Focus();
}

std::vector<Window*> WindowTree::GetAllWindows() {
    std::vector<Window*> result;
    CollectWindows(m_Root.get(), result);
    return result;
}

size_t WindowTree::GetWindowCount() {
    return GetAllWindows().size();
}

bool WindowTree::IsSingleWindow() const {
    return m_Root && m_Root->isLeaf;
}

Window* WindowTree::FindWindowWithBuffer(size_t bufferId) {
    for (auto* w : GetAllWindows()) {
        if (w->GetContentType() == WindowContent::Buffer && w->GetBufferId() == bufferId)
            return w;
    }
    return nullptr;
}

void WindowTree::CollectWindows(SplitNode* node, std::vector<Window*>& out) {
    if (!node) return;
    if (node->isLeaf) {
        if (node->window) out.push_back(node->window.get());
        return;
    }
    CollectWindows(node->first.get(), out);
    CollectWindows(node->second.get(), out);
}

SplitNode* WindowTree::FindLeaf(SplitNode* node, Window* window) {
    if (!node) return nullptr;
    if (node->isLeaf)
        return (node->window.get() == window) ? node : nullptr;
    auto* r = FindLeaf(node->first.get(), window);
    return r ? r : FindLeaf(node->second.get(), window);
}

SplitNode* WindowTree::FindParent(SplitNode* node, SplitNode* target) {
    if (!node || node->isLeaf) return nullptr;
    if (node->first.get() == target || node->second.get() == target) return node;
    auto* r = FindParent(node->first.get(), target);
    return r ? r : FindParent(node->second.get(), target);
}

} // namespace sol
