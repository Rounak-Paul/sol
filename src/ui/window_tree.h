#pragma once

#include "widgets/syntax_editor.h"
#include <imgui.h>
#include <memory>
#include <vector>

namespace sol {

enum class WindowContent {
    Buffer,
    Empty
};

// A window is a viewport that displays a buffer
class Window {
public:
    explicit Window(uint32_t id = 0);

    void ShowBuffer(size_t bufferId);
    void Clear();

    uint32_t GetId() const { return m_Id; }
    WindowContent GetContentType() const { return m_ContentType; }
    size_t GetBufferId() const { return m_BufferId; }

    // Render content into current ImGui context. Returns true if buffer was modified.
    bool Render(const char* label, const ImVec2& size, bool isActiveWindow);

    void Focus();
    SyntaxEditor* GetEditor() { return m_Editor.get(); }

private:
    static uint32_t GenerateId();

    uint32_t m_Id;
    WindowContent m_ContentType = WindowContent::Empty;
    size_t m_BufferId = 0;

    std::unique_ptr<SyntaxEditor> m_Editor;

    bool m_WantsFocus = false;
};

enum class SplitDir { Horizontal, Vertical };

// A node in the window split tree (leaf = window, non-leaf = split)
struct SplitNode {
    bool isLeaf = true;

    // Leaf
    std::unique_ptr<Window> window;

    // Non-leaf
    SplitDir direction = SplitDir::Horizontal;
    float ratio = 0.5f;
    std::unique_ptr<SplitNode> first;
    std::unique_ptr<SplitNode> second;

    SplitNode();
};

// Manages a tree of split windows (like vim/neovim)
class WindowTree {
public:
    WindowTree();

    // Render the entire tree into the given rect
    void Render(const ImVec2& pos, const ImVec2& size);

    // Split the active window. Returns the new window (second child).
    Window* SplitActive(SplitDir direction, float ratio = 0.5f);

    // Close the active window. Returns false if last window.
    bool CloseActive();

    // Navigation
    void FocusNext();
    void FocusPrev();

    Window* GetActiveWindow();
    void SetActiveWindow(Window* window);

    std::vector<Window*> GetAllWindows();
    size_t GetWindowCount();
    bool IsSingleWindow() const;

    Window* FindWindowWithBuffer(size_t bufferId);

    // Set whether the tree itself (vs explorer/sidebar) has focus
    void SetTreeFocused(bool focused) { m_TreeFocused = focused; }

private:
    static constexpr float DIVIDER_SIZE = 3.0f;

    std::unique_ptr<SplitNode> m_Root;
    Window* m_ActiveWindow = nullptr;
    bool m_TreeFocused = true;

    void RenderNode(SplitNode* node, const ImVec2& pos, const ImVec2& size);
    void CollectWindows(SplitNode* node, std::vector<Window*>& out);
    SplitNode* FindLeaf(SplitNode* node, Window* window);
    SplitNode* FindParent(SplitNode* node, SplitNode* target);
};

} // namespace sol
