#include "explorer.h"
#include "core/resource_system.h"
#include "core/event_system.h"
#include "ui/icons_nerd.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <tinyvk/assets/icons_font_awesome.h>
#include <algorithm>

namespace sol {

namespace {

std::string GetIconForFile(const std::filesystem::path& path) {
    auto ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    
    if (ext == ".cpp" || ext == ".cc" || ext == ".mm" || ext == ".cxx") return ICON_NF_CPP;
    if (ext == ".c") return ICON_NF_C;
    if (ext == ".h" || ext == ".hpp" || ext == ".hxx") return ICON_NF_CPP; // Use C++ icon for headers too
    
    if (ext == ".py") return ICON_NF_PYTHON;
    if (ext == ".js" || ext == ".mjs" || ext == ".cjs") return ICON_NF_JAVASCRIPT;
    if (ext == ".ts") return ICON_NF_TYPESCRIPT;
    if (ext == ".jsx" || ext == ".tsx") return ICON_NF_REACT;
    
    if (ext == ".html" || ext == ".htm") return ICON_NF_HTML;
    if (ext == ".css" || ext == ".scss" || ext == ".less") return ICON_NF_CSS;
    if (ext == ".json") return ICON_NF_JSON;
    if (ext == ".xml" || ext == ".yaml" || ext == ".yml") return ICON_NF_FILE_CODE;
    if (ext == ".md" || ext == ".markdown") return ICON_NF_MARKDOWN;
    if (ext == ".lua") return ICON_NF_LUA;
    if (ext == ".cmake" || path.filename() == "CMakeLists.txt") return ICON_NF_CMAKE;
    
    if (ext == ".txt" || ext == ".log") return ICON_NF_FILE_TEXT;
    if (ext == ".ini" || ext == ".conf" || ext == ".cfg") return ICON_NF_CONFIG;
    
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".gif" || ext == ".tga" || ext == ".ico" || ext == ".svg") return ICON_NF_IMAGE;
    if (ext == ".wav" || ext == ".mp3" || ext == ".ogg") return ICON_NF_AUDIO;
    if (ext == ".mp4" || ext == ".avi" || ext == ".mkv") return ICON_NF_VIDEO;
    
    if (ext == ".pdf") return ICON_NF_PDF;
    if (ext == ".zip" || ext == ".tar" || ext == ".gz" || ext == ".7z" || ext == ".rar") return ICON_NF_ARCHIVE;
    
    if (ext == ".sh" || ext == ".bat" || ext == ".cmd" || ext == ".ps1") return ICON_NF_TERMINAL;
    if (ext == ".lock" || ext == ".gitignore") return ICON_NF_LOCK;
    
    return ICON_NF_FILE;
}

}

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
        
        ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, 10.0f);
        RenderTree();
        ImGui::PopStyleVar();
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
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth;
    
    if (!node.isDirectory) {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }
    
    // We need to know if the node is open BEFORE rendering to pick the right icon for folders.
    // However, ImGui::TreeNodeEx returns the open state.
    // To solve this, we can check ImGui::GetStateStorage()->GetInt(id) but that's internal.
    // A simpler way is to just use the closed folder icon for now, or just trust the tree node logic.
    // Actually, "isOpen" variable in previous code was the result of TreeNodeEx.
    // To support open/closed folder icons properly we'd need to track state or use ImGui internals.
    // For now, let's just use the simpler approach: relying on ImGui to draw the arrow, 
    // and we draw the icon. We can't easily know if it's open before drawing it.
    // BUT! we can use the result from previous frame if we wanted, or just stick to one icon.
    // Wait, RenderNode is called recursively. 
    
    // Let's use a trick: standard ImGui doesn't give us "is open" before drawing.
    // But we can check if it's open in the tree state.
    
    std::string icon = node.isDirectory ? ICON_NF_FOLDER : GetIconForFile(node.path);
    std::string label = icon + " " + node.name;

    bool isOpen = false;
    if (node.isDirectory) {
        // Use the storage to check if the tree node is open to select the correct icon
        ImGuiContext& g = *GImGui;
        ImGuiStorage* storage = ImGui::GetStateStorage();
        ImGuiID id = ImGui::GetID(label.c_str());
        bool is_open = storage->GetInt(id, (flags & ImGuiTreeNodeFlags_DefaultOpen) ? 1 : 0) != 0;
        
        if (is_open) {
             icon = ICON_NF_FOLDER_OPEN;
        }
        
        // Use separate ID from label to avoid ID changing when icon changes
        // Use the full path as the unique ID
        std::string uniqueId = "###" + node.path.string();
        std::string displayLabel = icon + " " + node.name + uniqueId;

        isOpen = ImGui::TreeNodeEx(displayLabel.c_str(), flags);
    } else {
        std::string uniqueId = "###" + node.path.string();
        std::string displayLabel = label + uniqueId;
        
        ImGui::TreeNodeEx(displayLabel.c_str(), flags);
        
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