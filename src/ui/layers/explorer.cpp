#include "explorer.h"
#include "core/resource_system.h"
#include "core/event_system.h"
#include <imgui.h>
#include <algorithm>

namespace sol {

Explorer::Explorer(const Id& id)
    : UILayer(id) {
}

void Explorer::OnUI() {
    ImGui::Begin("Explorer");
    
    auto& rs = ResourceSystem::GetInstance();
    
    if (!rs.HasWorkingDirectory()) {
        ImGui::TextDisabled("No folder opened");
        if (ImGui::Button("Open Folder")) {
            EventSystem::Execute("open_folder_dialog");
        }
    } else {
        ImGui::Text("%s", rs.GetWorkingDirectory().filename().string().c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Refresh")) {
            Refresh();
        }
        ImGui::Separator();
        
        if (m_NeedsRefresh) {
            m_Root.children.clear();
            m_Root.name = rs.GetWorkingDirectory().filename().string();
            m_Root.path = rs.GetWorkingDirectory();
            m_Root.isDirectory = true;
            m_Root.isExpanded = true;
            BuildTree(m_Root, rs.GetWorkingDirectory());
            m_NeedsRefresh = false;
        }
        
        RenderTree();
    }
    
    ImGui::End();
}

void Explorer::Refresh() {
    m_NeedsRefresh = true;
}

void Explorer::RenderTree() {
    for (auto& child : m_Root.children) {
        RenderNode(child);
    }
}

void Explorer::RenderNode(FileNode& node) {
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    
    if (!node.isDirectory) {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }
    
    bool isOpen = false;
    if (node.isDirectory) {
        isOpen = ImGui::TreeNodeEx(node.name.c_str(), flags);
    } else {
        ImGui::TreeNodeEx(node.name.c_str(), flags);
        
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
            ResourceSystem::GetInstance().OpenFile(node.path);
        }
    }
    
    if (isOpen && node.isDirectory) {
        for (auto& child : node.children) {
            RenderNode(child);
        }
        ImGui::TreePop();
    }
}

void Explorer::BuildTree(FileNode& parent, const std::filesystem::path& path) {
    try {
        std::vector<FileNode> dirs;
        std::vector<FileNode> files;
        
        for (const auto& entry : std::filesystem::directory_iterator(path)) {
            auto filename = entry.path().filename().string();
            
            // Skip hidden files and common ignore patterns
            if (filename[0] == '.' || filename == "build" || filename == "node_modules") {
                continue;
            }
            
            FileNode node;
            node.name = filename;
            node.path = entry.path();
            node.isDirectory = entry.is_directory();
            
            if (node.isDirectory) {
                BuildTree(node, entry.path());
                dirs.push_back(std::move(node));
            } else {
                files.push_back(std::move(node));
            }
        }
        
        // Sort: directories first, then files, both alphabetically
        std::sort(dirs.begin(), dirs.end(), 
            [](const FileNode& a, const FileNode& b) { return a.name < b.name; });
        std::sort(files.begin(), files.end(), 
            [](const FileNode& a, const FileNode& b) { return a.name < b.name; });
        
        parent.children.reserve(dirs.size() + files.size());
        for (auto& dir : dirs) {
            parent.children.push_back(std::move(dir));
        }
        for (auto& file : files) {
            parent.children.push_back(std::move(file));
        }
        
    } catch (const std::filesystem::filesystem_error& e) {
        // Silently ignore permission errors
    }
}

} // namespace sol