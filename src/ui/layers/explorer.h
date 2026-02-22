#pragma once

#include "ui/ui_system.h"
#include <filesystem>
#include <vector>
#include <string>

namespace sol {

class Explorer : public UILayer {
public:
    explicit Explorer(const Id& id = "Explorer");
    ~Explorer() override = default;

    void OnUI() override;
    void Refresh();
    
    bool IsFocused() const { return m_IsFocused; }
    void Focus() { m_WantsFocus = true; }

private:
    struct FileNode {
        std::string name;
        std::filesystem::path path;
        bool isDirectory;
        bool isExpanded = false;
        std::vector<FileNode> children;
    };
    
    void RenderTree();
    void RenderNode(FileNode& node);
    void BuildTree(FileNode& node, const std::filesystem::path& path);
    
    FileNode m_Root;
    bool m_NeedsRefresh = true;
    bool m_IsFocused = false;
    bool m_WantsFocus = false;
};

} // namespace sol