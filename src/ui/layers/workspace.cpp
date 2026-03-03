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

    // Process terminal PTY reads every frame regardless of visibility
    m_TerminalPanel.ProcessAllInputs();

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

    // Layout: [Explorer | MainArea]
    // MainArea = [WindowTree + optional Terminal (Bottom/Right)]
    ImVec2 cursorPos = ImGui::GetCursorScreenPos();
    ImVec2 avail = ImGui::GetContentRegionAvail();

    if (avail.x > 0 && avail.y > 0) {
        if (m_ExplorerOpen) {
            float explorerW = std::min(m_ExplorerWidth, avail.x - DIVIDER_SIZE - 100.0f);
            float mainW = avail.x - explorerW - DIVIDER_SIZE;

            // Explorer panel
            ImGui::SetCursorScreenPos(cursorPos);
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.11f, 0.11f, 0.12f, 1.0f));
            ImGui::BeginChild("##explorer_panel", ImVec2(explorerW, avail.y), false,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

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

            // Main area (right of explorer)
            ImVec2 mainPos(cursorPos.x + explorerW + DIVIDER_SIZE, cursorPos.y);
            RenderMainArea(mainPos, ImVec2(mainW, avail.y));
        } else {
            RenderMainArea(cursorPos, avail);
        }
    }

    // Dim overlay behind floating terminal (drawn into workspace drawlist, below floating window z-order)
    if (m_TerminalOpen && m_TerminalPosition == TerminalPosition::Floating) {
        ImVec2 displaySize = ImGui::GetIO().DisplaySize;
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(0, 0), displaySize, IM_COL32(0, 0, 0, 120));
    }

    // Determine focus state for tree
    bool sidebarHasFocus = (m_ExplorerOpen && m_Explorer.IsFocused()) ||
                           (m_TerminalOpen && m_TerminalPanel.IsFocused());
    m_WindowTree.SetTreeFocused(!sidebarHasFocus);

    SyncActiveBuffer();

    ImGui::End();

    // Floating terminal overlay (separate window, renders above workspace + dim)
    if (m_TerminalOpen && m_TerminalPosition == TerminalPosition::Floating) {
        RenderFloatingTerminal();
    }
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

bool Workspace::CloseActiveSplit() {
    return m_WindowTree.CloseActive();
}

void Workspace::FocusNextWindow() {
    m_WindowTree.FocusNext();
}

void Workspace::FocusPrevWindow() {
    m_WindowTree.FocusPrev();
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

// --- Terminal panel management ---

void Workspace::ToggleTerminal(TerminalPosition pos) {
    if (m_TerminalOpen) {
        if (m_TerminalPosition == pos && m_TerminalPanel.IsFocused()) {
            // Same position and focused → close
            m_TerminalOpen = false;
            auto* win = m_WindowTree.GetActiveWindow();
            if (win) win->Focus();
        } else if (m_TerminalPosition == pos) {
            // Same position but not focused → focus it
            m_TerminalWantsFocus = true;
        } else {
            // Different position → switch position and focus
            m_TerminalPosition = pos;
            m_TerminalWantsFocus = true;
        }
    } else {
        m_TerminalOpen = true;
        m_TerminalPosition = pos;
        m_TerminalWantsFocus = true;
    }
}

void Workspace::NewTerminal() {
    if (!m_TerminalOpen) {
        m_TerminalOpen = true;
        m_TerminalPosition = TerminalPosition::Bottom;
    }
    m_TerminalPanel.NewTab();
    m_TerminalWantsFocus = true;
}

void Workspace::CloseTerminal() {
    if (!m_TerminalOpen) return;
    m_TerminalPanel.CloseActiveTab();
    if (!m_TerminalPanel.HasTabs()) {
        m_TerminalOpen = false;
        auto* win = m_WindowTree.GetActiveWindow();
        if (win) win->Focus();
    }
}

void Workspace::RenderMainArea(const ImVec2& pos, const ImVec2& size) {
    bool termBottom = m_TerminalOpen && m_TerminalPosition == TerminalPosition::Bottom;
    bool termRight = m_TerminalOpen && m_TerminalPosition == TerminalPosition::Right;
    // Floating is rendered as overlay in OnUI, not here

    if (termBottom) {
        float maxH = size.y * TERMINAL_MAX_RATIO;
        float termH = std::clamp(m_TerminalHeight, TERMINAL_MIN_SIZE, maxH);
        float treeH = size.y - termH - DIVIDER_SIZE;

        // Window tree (top)
        m_WindowTree.Render(pos, ImVec2(size.x, treeH));

        // Divider (horizontal)
        ImVec2 divPos(pos.x, pos.y + treeH);
        ImGui::SetCursorScreenPos(divPos);
        ImGui::InvisibleButton("##term_div_h", ImVec2(size.x, DIVIDER_SIZE));
        if (ImGui::IsItemActive()) {
            m_TerminalHeight = std::clamp(termH - ImGui::GetIO().MouseDelta.y,
                                          TERMINAL_MIN_SIZE, maxH);
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        } else if (ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        }
        ImGui::GetWindowDrawList()->AddRectFilled(
            divPos, ImVec2(divPos.x + size.x, divPos.y + DIVIDER_SIZE),
            IM_COL32(35, 35, 38, 255));

        // Terminal panel (bottom)
        ImVec2 termPos(pos.x, pos.y + treeH + DIVIDER_SIZE);
        ImGui::SetCursorScreenPos(termPos);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.1f, 0.1f, 0.1f, 1.0f));
        ImGui::BeginChild("##terminal_panel", ImVec2(size.x, termH), false,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        if (m_TerminalWantsFocus) {
            ImGui::SetWindowFocus();
            m_TerminalWantsFocus = false;
        }

        m_TerminalPanel.SetWindowActive(ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows));
        m_TerminalPanel.Render(ImVec2(size.x, termH));

        ImGui::EndChild();
        ImGui::PopStyleColor();

    } else if (termRight) {
        float maxW = size.x * TERMINAL_MAX_RATIO;
        float termW = std::clamp(m_TerminalWidth, TERMINAL_MIN_SIZE, maxW);
        float treeW = size.x - termW - DIVIDER_SIZE;

        // Window tree (left)
        m_WindowTree.Render(pos, ImVec2(treeW, size.y));

        // Divider (vertical)
        ImVec2 divPos(pos.x + treeW, pos.y);
        ImGui::SetCursorScreenPos(divPos);
        ImGui::InvisibleButton("##term_div_v", ImVec2(DIVIDER_SIZE, size.y));
        if (ImGui::IsItemActive()) {
            m_TerminalWidth = std::clamp(termW - ImGui::GetIO().MouseDelta.x,
                                         TERMINAL_MIN_SIZE, maxW);
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        } else if (ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }
        ImGui::GetWindowDrawList()->AddRectFilled(
            divPos, ImVec2(divPos.x + DIVIDER_SIZE, divPos.y + size.y),
            IM_COL32(35, 35, 38, 255));

        // Terminal panel (right)
        ImVec2 termPos(pos.x + treeW + DIVIDER_SIZE, pos.y);
        ImGui::SetCursorScreenPos(termPos);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.1f, 0.1f, 0.1f, 1.0f));
        ImGui::BeginChild("##terminal_panel", ImVec2(termW, size.y), false,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        if (m_TerminalWantsFocus) {
            ImGui::SetWindowFocus();
            m_TerminalWantsFocus = false;
        }

        m_TerminalPanel.SetWindowActive(ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows));
        m_TerminalPanel.Render(ImVec2(termW, size.y));

        ImGui::EndChild();
        ImGui::PopStyleColor();

    } else {
        // No docked terminal, just render the tree
        m_WindowTree.Render(pos, size);
    }
}

void Workspace::RenderFloatingTerminal() {
    ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    float w = displaySize.x * 0.7f;
    float h = displaySize.y * 0.7f;
    float x = (displaySize.x - w) * 0.5f;
    float y = (displaySize.y - h) * 0.5f;

    ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);

    ImGuiWindowFlags floatFlags = ImGuiWindowFlags_NoTitleBar |
                                  ImGuiWindowFlags_NoResize |
                                  ImGuiWindowFlags_NoMove |
                                  ImGuiWindowFlags_NoCollapse |
                                  ImGuiWindowFlags_NoDocking |
                                  ImGuiWindowFlags_NoScrollbar |
                                  ImGuiWindowFlags_NoScrollWithMouse |
                                  ImGuiWindowFlags_NoNavInputs;

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.1f, 0.1f, 0.1f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.35f, 0.55f, 0.9f, 0.7f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);

    ImGui::Begin("##floating_terminal", nullptr, floatFlags);

    if (m_TerminalWantsFocus) {
        ImGui::SetWindowFocus();
        m_TerminalWantsFocus = false;
    }

    m_TerminalPanel.SetWindowActive(ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows));
    ImVec2 avail = ImGui::GetContentRegionAvail();
    m_TerminalPanel.Render(avail);

    ImGui::End();

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
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
