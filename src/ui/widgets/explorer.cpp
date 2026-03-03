#include "explorer.h"
#include "core/resource_system.h"
#include "core/event_system.h"
#include "ui/icons_nerd.h"
#include "ui/input/command.h"
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
    if (ext == ".h" || ext == ".hpp" || ext == ".hxx") return ICON_NF_CPP;

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

} // anonymous namespace

void ExplorerWidget::Render(const char* label, const ImVec2& size) {
    m_IsFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);

    auto& rs = ResourceSystem::GetInstance();

    if (!rs.HasWorkingDirectory()) {
        ImGui::TextDisabled("No folder opened");
        if (ImGui::Button("Open Folder")) {
            EventSystem::Execute("open_folder_dialog");
        }
        return;
    }

    ImGui::Text("%s", rs.GetWorkingDirectory().filename().string().c_str());
    ImGui::Separator();

    if (m_NeedsRefresh) {
        m_Root.children.clear();
        m_Root.name = rs.GetWorkingDirectory().filename().string();
        m_Root.path = rs.GetWorkingDirectory();
        m_Root.isDirectory = true;
        m_Root.isExpanded = true;
        BuildTree(m_Root, rs.GetWorkingDirectory());
        m_NeedsRefresh = false;
        m_FlatListDirty = true;
    }

    // Build flat visible list for rendering and navigation
    if (m_FlatListDirty) {
        BuildFlatList();
        m_FlatListDirty = false;
    }

    // Handle keyboard nav when focused
    if (m_IsWindowActive) {
        HandleInput();
    }

    // Scrollable tree region
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float lineHeight = ImGui::GetTextLineHeightWithSpacing();

    ImGui::BeginChild("##explorer_tree", avail, false, ImGuiWindowFlags_None);

    // Clamp selected index
    if (!m_FlatList.empty()) {
        m_SelectedIndex = std::clamp(m_SelectedIndex, 0, (int)m_FlatList.size() - 1);
    }

    // Scroll to selected if needed
    if (m_ScrollToSelected && !m_FlatList.empty()) {
        float selectedY = m_SelectedIndex * lineHeight;
        float scrollY = ImGui::GetScrollY();
        float viewHeight = avail.y;
        if (selectedY < scrollY)
            ImGui::SetScrollY(selectedY);
        else if (selectedY + lineHeight > scrollY + viewHeight)
            ImGui::SetScrollY(selectedY + lineHeight - viewHeight);
        m_ScrollToSelected = false;
    }

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 windowPos = ImGui::GetCursorScreenPos();

    for (int i = 0; i < (int)m_FlatList.size(); ++i) {
        auto& entry = m_FlatList[i];
        FileNode* node = entry.node;
        float indent = entry.depth * 10.0f;

        ImVec2 rowMin(windowPos.x, windowPos.y + i * lineHeight);
        ImVec2 rowMax(windowPos.x + avail.x, rowMin.y + lineHeight);

        // Selected highlight
        bool isSelected = (i == m_SelectedIndex) && m_IsWindowActive;
        if (isSelected) {
            drawList->AddRectFilled(rowMin, rowMax, IM_COL32(38, 79, 120, 200));
        }

        // Hover highlight
        ImGui::SetCursorScreenPos(rowMin);
        char btnId[64];
        snprintf(btnId, sizeof(btnId), "##row_%d", i);
        if (ImGui::InvisibleButton(btnId, ImVec2(avail.x, lineHeight))) {
            m_SelectedIndex = i;
            if (node->isDirectory) {
                node->isExpanded = !node->isExpanded;
                m_FlatListDirty = true;
            } else {
                ResourceSystem::GetInstance().OpenFile(node->path);
            }
        }
        if (ImGui::IsItemHovered() && !isSelected) {
            drawList->AddRectFilled(rowMin, rowMax, IM_COL32(255, 255, 255, 20));
        }

        // Icon and label
        std::string icon;
        if (node->isDirectory) {
            icon = node->isExpanded ? ICON_NF_FOLDER_OPEN : ICON_NF_FOLDER;
        } else {
            icon = GetIconForFile(node->path);
        }

        ImVec2 textPos(rowMin.x + indent + 2.0f, rowMin.y);
        drawList->AddText(textPos, IM_COL32(200, 200, 200, 255), (icon + " " + node->name).c_str());
    }

    // Dummy to set content size for scrolling
    if (!m_FlatList.empty()) {
        ImGui::Dummy(ImVec2(avail.x, m_FlatList.size() * lineHeight));
    }

    ImGui::EndChild();
}

void ExplorerWidget::HandleInput() {
    auto& input = InputSystem::GetInstance();
    ImGuiKey nav = input.ConsumePendingNavigation();

    if (m_FlatList.empty()) return;

    int maxIdx = (int)m_FlatList.size() - 1;

    switch (nav) {
        case ImGuiKey_UpArrow:
            m_SelectedIndex = std::max(0, m_SelectedIndex - 1);
            m_ScrollToSelected = true;
            break;
        case ImGuiKey_DownArrow:
            m_SelectedIndex = std::min(maxIdx, m_SelectedIndex + 1);
            m_ScrollToSelected = true;
            break;
        case ImGuiKey_RightArrow: {
            auto* node = m_FlatList[m_SelectedIndex].node;
            if (node->isDirectory && !node->isExpanded) {
                node->isExpanded = true;
                m_FlatListDirty = true;
            } else if (node->isDirectory && node->isExpanded) {
                // Move into the directory (select first child)
                if (m_SelectedIndex + 1 <= maxIdx)
                    m_SelectedIndex++;
                m_ScrollToSelected = true;
            }
            break;
        }
        case ImGuiKey_LeftArrow: {
            auto* node = m_FlatList[m_SelectedIndex].node;
            if (node->isDirectory && node->isExpanded) {
                node->isExpanded = false;
                m_FlatListDirty = true;
            } else {
                // Go to parent (find node at depth-1 above)
                int targetDepth = m_FlatList[m_SelectedIndex].depth - 1;
                for (int i = m_SelectedIndex - 1; i >= 0; --i) {
                    if (m_FlatList[i].depth == targetDepth && m_FlatList[i].node->isDirectory) {
                        m_SelectedIndex = i;
                        m_ScrollToSelected = true;
                        break;
                    }
                }
            }
            break;
        }
        default:
            break;
    }

    // Enter to open file or toggle folder
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false)) {
        auto* node = m_FlatList[m_SelectedIndex].node;
        if (node->isDirectory) {
            node->isExpanded = !node->isExpanded;
            m_FlatListDirty = true;
        } else {
            ResourceSystem::GetInstance().OpenFile(node->path);
        }
    }
}

void ExplorerWidget::BuildFlatList() {
    m_FlatList.clear();
    for (auto& child : m_Root.children) {
        BuildFlatListRecursive(child, 0);
    }
}

void ExplorerWidget::BuildFlatListRecursive(FileNode& node, int depth) {
    m_FlatList.push_back({&node, depth});
    if (node.isDirectory && node.isExpanded) {
        for (auto& child : node.children) {
            BuildFlatListRecursive(child, depth + 1);
        }
    }
}

void ExplorerWidget::BuildTree(FileNode& parent, const std::filesystem::path& path) {
    try {
        std::vector<FileNode> dirs;
        std::vector<FileNode> files;

        for (const auto& entry : std::filesystem::directory_iterator(path)) {
            auto filename = entry.path().filename().string();

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

    } catch (const std::exception&) {
        // Silently ignore filesystem errors
    }
}

} // namespace sol
