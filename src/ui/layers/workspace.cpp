#include "workspace.h"
#include "core/resource_system.h"
#include "core/text/text_buffer.h"
#include "ui/input/input_mode.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>

namespace sol {

Workspace::Workspace(const Id& id)
    : UILayer(id) {
}

void Workspace::OnUI() {
    ProcessPendingCloses();
    ProcessPendingDiagnostics();

    m_DockspaceID = ImGui::GetID(UISystem::MainDockSpaceId);

    ImGuiWindowClass windowClass;
    windowClass.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_NoTabBar;
    ImGui::SetNextWindowClass(&windowClass);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoScrollWithMouse |
                             ImGuiWindowFlags_NoBringToFrontOnFocus |
                             ImGuiWindowFlags_NoNavInputs;

    ImGui::SetNextWindowDockID(m_DockspaceID, ImGuiCond_Always);
    ImGui::Begin("##Workspace", nullptr, flags);

    m_IsFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (m_WantsFocus) {
        ImGui::SetWindowFocus();
        m_WantsFocus = false;
        auto* win = m_WindowTree.GetActiveWindow();
        if (win) win->Focus();
    }

    RenderTabBar();

    // Layout: [Explorer | WindowTree]
    ImVec2 cursorPos = ImGui::GetCursorScreenPos();
    ImVec2 avail = ImGui::GetContentRegionAvail();

    if (avail.x > 0 && avail.y > 0) {
        if (m_ExplorerOpen) {
            float explorerW = std::min(m_ExplorerWidth, avail.x - DIVIDER_SIZE - 100.0f);
            float treeW = avail.x - explorerW - DIVIDER_SIZE;

            // Explorer panel
            ImGui::SetCursorScreenPos(cursorPos);
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.11f, 0.11f, 0.12f, 1.0f));
            ImGui::BeginChild("##explorer_panel", ImVec2(explorerW, avail.y), false,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

            // Apply pending focus to the explorer panel child window
            if (m_ExplorerWantsFocus) {
                ImGui::SetWindowFocus();
                m_ExplorerWantsFocus = false;
            }

            m_Explorer.SetWindowActive(ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows));
            ImVec2 explorerAvail = ImGui::GetContentRegionAvail();
            m_Explorer.Render("##explorer", explorerAvail);

            ImGui::EndChild();
            ImGui::PopStyleColor();

            // Draggable divider
            ImVec2 divPos(cursorPos.x + explorerW, cursorPos.y);
            ImGui::SetCursorScreenPos(divPos);
            ImGui::InvisibleButton("##explorer_div", ImVec2(DIVIDER_SIZE, avail.y));
            if (ImGui::IsItemActive()) {
                m_ExplorerWidth = std::clamp(explorerW + ImGui::GetIO().MouseDelta.x,
                                             EXPLORER_MIN_WIDTH, EXPLORER_MAX_WIDTH);
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            } else if (ImGui::IsItemHovered()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            }
            ImGui::GetWindowDrawList()->AddRectFilled(
                divPos, ImVec2(divPos.x + DIVIDER_SIZE, divPos.y + avail.y),
                IM_COL32(35, 35, 38, 255));

            // Window tree (right side) — not focused when explorer has focus
            bool explorerHasFocus = m_Explorer.IsFocused();
            m_WindowTree.SetTreeFocused(!explorerHasFocus);
            ImVec2 treePos(cursorPos.x + explorerW + DIVIDER_SIZE, cursorPos.y);
            m_WindowTree.Render(treePos, ImVec2(treeW, avail.y));
        } else {
            m_WindowTree.SetTreeFocused(true);
            m_WindowTree.Render(cursorPos, avail);
        }
    }

    // Keep ResourceSystem active buffer in sync with visible active window
    SyncActiveBuffer();

    ImGui::End();
}

void Workspace::SyncActiveBuffer() {
    auto* win = m_WindowTree.GetActiveWindow();
    if (!win || win->GetContentType() != WindowContent::Buffer) return;

    size_t winBufferId = win->GetBufferId();
    auto& rs = ResourceSystem::GetInstance();
    auto active = rs.GetActiveBuffer();
    if (!active || active->GetId() != winBufferId) {
        auto buf = rs.GetBuffer(winBufferId);
        if (buf) rs.SetActiveBuffer(winBufferId);
    }
}

void Workspace::UpdateDiagnostics(const std::string& path, const std::vector<LSPDiagnostic>& diagnostics) {
    std::lock_guard<std::mutex> lock(m_DiagnosticsMutex);
    m_PendingDiagnostics.emplace_back(path, diagnostics);
}

void Workspace::ProcessPendingDiagnostics() {
    std::vector<std::pair<std::string, std::vector<LSPDiagnostic>>> pending;
    {
        std::lock_guard<std::mutex> lock(m_DiagnosticsMutex);
        pending = std::move(m_PendingDiagnostics);
        m_PendingDiagnostics.clear();
    }
    if (pending.empty()) return;

    auto& rs = ResourceSystem::GetInstance();

    for (const auto& [path, diagnostics] : pending) {
        std::filesystem::path targetPath(path);
        for (const auto& buffer : rs.GetBuffers()) {
            std::filesystem::path bufPath = buffer->GetResource()->GetPath();
            bool match = false;
            try {
                if (std::filesystem::exists(bufPath) && std::filesystem::exists(targetPath))
                    match = std::filesystem::equivalent(bufPath, targetPath);
                else
                    match = (bufPath == targetPath);
            } catch (...) {
                match = (bufPath.string() == targetPath.string());
            }
            if (match) {
                // Push diagnostics to all windows showing this buffer
                for (auto* win : m_WindowTree.GetAllWindows()) {
                    if (win->GetContentType() == WindowContent::Buffer &&
                        win->GetBufferId() == buffer->GetId()) {
                        if (auto* ed = win->GetEditor())
                            ed->UpdateDiagnostics(path, diagnostics);
                    }
                }
                break;
            }
        }
    }
}

void Workspace::RenderTabBar() {
    auto& rs = ResourceSystem::GetInstance();
    const auto& buffers = rs.GetBuffers();

    if (buffers.empty()) {
        m_LastActiveBufferId = 0;
        return;
    }

    auto activeBuffer = rs.GetActiveBuffer();
    size_t currentActiveId = activeBuffer ? activeBuffer->GetId() : 0;
    bool forceSelection = (currentActiveId != m_LastActiveBufferId) && (currentActiveId != 0);
    m_LastActiveBufferId = currentActiveId;

    ImGuiTabBarFlags tabBarFlags = ImGuiTabBarFlags_Reorderable |
                                   ImGuiTabBarFlags_AutoSelectNewTabs |
                                   ImGuiTabBarFlags_FittingPolicyScroll;

    if (ImGui::BeginTabBar(UISystem::EditorTabBarId, tabBarFlags)) {
        for (const auto& buffer : buffers) {
            std::string label = buffer->GetName();
            if (buffer->GetResource()->IsModified())
                label += " *";
            label += "###buf_" + std::to_string(buffer->GetId());

            bool open = true;
            ImGuiTabItemFlags itemFlags = 0;
            if (forceSelection && buffer->IsActive())
                itemFlags |= ImGuiTabItemFlags_SetSelected;

            if (ImGui::BeginTabItem(label.c_str(), &open, itemFlags)) {
                if (!forceSelection && !buffer->IsActive()) {
                    // Tab clicked → show this buffer in the active window
                    rs.SetActiveBuffer(buffer->GetId());
                    auto* win = m_WindowTree.GetActiveWindow();
                    if (win) win->ShowBuffer(buffer->GetId());
                }
                ImGui::EndTabItem();
            }

            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Close"))
                    m_PendingCloseBuffers.push_back(buffer->GetId());
                ImGui::EndPopup();
            }

            if (!open)
                m_PendingCloseBuffers.push_back(buffer->GetId());
        }
        ImGui::EndTabBar();
    }
}

void Workspace::ProcessPendingCloses() {
    auto& rs = ResourceSystem::GetInstance();
    for (auto bufferId : m_PendingCloseBuffers) {
        // Clear any windows showing this buffer
        for (auto* win : m_WindowTree.GetAllWindows()) {
            if (win->GetContentType() == WindowContent::Buffer && win->GetBufferId() == bufferId)
                win->Clear();
        }
        rs.CloseBuffer(bufferId);
    }
    m_PendingCloseBuffers.clear();

    // If active window is empty after close, show the new active buffer
    auto* win = m_WindowTree.GetActiveWindow();
    if (win && win->GetContentType() == WindowContent::Empty) {
        auto active = rs.GetActiveBuffer();
        if (active) win->ShowBuffer(active->GetId());
    }
}

void Workspace::Focus() {
    m_WantsFocus = true;
}

// --- Window management ---

Window* Workspace::SplitVertical() {
    return m_WindowTree.SplitActive(SplitDir::Vertical);
}

Window* Workspace::SplitHorizontal() {
    return m_WindowTree.SplitActive(SplitDir::Horizontal);
}

// --- Explorer sidebar ---

void Workspace::ToggleExplorer() {
    if (m_ExplorerOpen) {
        if (m_Explorer.IsFocused()) {
            // Focused → close and return focus to editor
            m_ExplorerOpen = false;
            auto* win = m_WindowTree.GetActiveWindow();
            if (win) win->Focus();
        } else {
            // Open but not focused → just focus it
            m_ExplorerWantsFocus = true;
        }
    } else {
        m_ExplorerOpen = true;
        m_ExplorerWantsFocus = true;
    }
}

// --- Terminal management ---

void Workspace::ToggleTerminal() {
    auto* termWin = m_WindowTree.FindTerminalWindow();
    if (termWin) {
        if (m_WindowTree.GetActiveWindow() == termWin) {
            // Focused on terminal → close it
            if (!m_WindowTree.IsSingleWindow()) {
                m_WindowTree.SetActiveWindow(termWin);
                m_WindowTree.CloseActive();
                auto* win = m_WindowTree.GetActiveWindow();
                if (win && win->GetContentType() == WindowContent::Empty) {
                    auto active = ResourceSystem::GetInstance().GetActiveBuffer();
                    if (active) win->ShowBuffer(active->GetId());
                }
            }
        } else {
            // Terminal exists but not focused → focus it
            m_WindowTree.SetActiveWindow(termWin);
        }
    } else {
        // No terminal → create one via horizontal split at bottom
        auto* newWin = m_WindowTree.SplitActive(SplitDir::Horizontal, 0.7f);
        if (newWin) {
            TerminalConfig cfg;
            auto& rs = ResourceSystem::GetInstance();
            if (rs.HasWorkingDirectory())
                cfg.workingDir = rs.GetWorkingDirectory().string();
            newWin->ShowTerminal(cfg);
        }
    }
}

void Workspace::NewTerminal() {
    auto* newWin = m_WindowTree.SplitActive(SplitDir::Horizontal, 0.7f);
    if (newWin) {
        TerminalConfig cfg;
        auto& rs = ResourceSystem::GetInstance();
        if (rs.HasWorkingDirectory())
            cfg.workingDir = rs.GetWorkingDirectory().string();
        newWin->ShowTerminal(cfg);
    }
}

void Workspace::CloseTerminal() {
    auto* activeWin = m_WindowTree.GetActiveWindow();
    if (activeWin && activeWin->IsTerminal()) {
        if (!m_WindowTree.IsSingleWindow()) {
            m_WindowTree.CloseActive();
        } else {
            activeWin->Clear();
        }
    }
}

// --- Undo/Redo ---

bool Workspace::Undo() {
    auto* win = m_WindowTree.GetActiveWindow();
    if (!win || win->GetContentType() != WindowContent::Buffer) return false;

    auto* editor = win->GetEditor();
    if (!editor) return false;

    auto& rs = ResourceSystem::GetInstance();
    auto buffer = rs.GetBuffer(win->GetBufferId());
    if (!buffer) return false;

    auto resource = buffer->GetResource();
    if (resource->GetType() != ResourceType::Text) return false;

    auto textResource = std::dynamic_pointer_cast<TextResource>(resource);
    if (!textResource) return false;

    TextBuffer& textBuf = textResource->GetBuffer();
    auto newPos = TextOps::Undo(textBuf, editor->GetUndoTree());
    if (newPos) {
        size_t pos = std::min(*newPos, textBuf.Length());
        editor->SetCursorPos(pos);
        textResource->SetModified(true);
        return true;
    }
    return false;
}

bool Workspace::Redo() {
    auto* win = m_WindowTree.GetActiveWindow();
    if (!win || win->GetContentType() != WindowContent::Buffer) return false;

    auto* editor = win->GetEditor();
    if (!editor) return false;

    auto& rs = ResourceSystem::GetInstance();
    auto buffer = rs.GetBuffer(win->GetBufferId());
    if (!buffer) return false;

    auto resource = buffer->GetResource();
    if (resource->GetType() != ResourceType::Text) return false;

    auto textResource = std::dynamic_pointer_cast<TextResource>(resource);
    if (!textResource) return false;

    TextBuffer& textBuf = textResource->GetBuffer();
    auto newPos = TextOps::Redo(textBuf, editor->GetUndoTree());
    if (newPos) {
        size_t pos = std::min(*newPos, textBuf.Length());
        editor->SetCursorPos(pos);
        textResource->SetModified(true);
        return true;
    }
    return false;
}

} // namespace sol
