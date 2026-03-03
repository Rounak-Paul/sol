#pragma once

#include <imgui.h>
#include <filesystem>
#include <vector>
#include <string>

namespace sol {

// File tree widget rendered as a left sidebar panel (like NvimTree)
class ExplorerWidget {
public:
    ExplorerWidget() = default;

    // Render the explorer tree into the current ImGui context
    void Render(const char* label, const ImVec2& size);

    void Refresh() { m_NeedsRefresh = true; }

    bool IsFocused() const { return m_IsFocused; }
    void Focus() { m_WantsFocus = true; }

    void SetWindowActive(bool active) { m_IsWindowActive = active; }

private:
    struct FileNode {
        std::string name;
        std::filesystem::path path;
        bool isDirectory;
        bool isExpanded = false;
        std::vector<FileNode> children;
    };

    void BuildFlatList();
    void BuildFlatListRecursive(FileNode& node, int depth);
    void HandleInput();
    void BuildTree(FileNode& node, const std::filesystem::path& path);

    // Flat visible entry for keyboard navigation
    struct FlatEntry {
        FileNode* node;
        int depth;
    };

    FileNode m_Root;
    std::vector<FlatEntry> m_FlatList;
    int m_SelectedIndex = 0;
    bool m_NeedsRefresh = true;
    bool m_FlatListDirty = true;
    bool m_IsFocused = false;
    bool m_IsWindowActive = false;
    bool m_WantsFocus = false;
    bool m_ScrollToSelected = false;
};

} // namespace sol