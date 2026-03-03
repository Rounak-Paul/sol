#pragma once

#include "terminal.h"
#include <imgui.h>
#include <vector>
#include <memory>
#include <string>

namespace sol {

enum class TerminalPosition {
    Bottom,
    Right,
    Floating
};

class TerminalPanel {
public:
    TerminalPanel() = default;

    void Render(const ImVec2& size);

    // Tab management
    void NewTab();
    void CloseActiveTab();
    void NextTab();
    void PrevTab();

    // Focus
    bool IsFocused() const { return m_IsFocused; }
    void SetWindowActive(bool active) { m_IsWindowActive = active; }
    void RequestFocus() { m_WantsFocus = true; }

    bool HasTabs() const { return !m_Tabs.empty(); }
    int GetTabCount() const { return (int)m_Tabs.size(); }

    // Process PTY input for all terminals (call every frame)
    void ProcessAllInputs();

private:
    struct Tab {
        std::unique_ptr<TerminalWidget> terminal;
        std::string title;
    };

    std::vector<Tab> m_Tabs;
    int m_ActiveTab = 0;
    bool m_IsFocused = false;
    bool m_IsWindowActive = false;
    bool m_WantsFocus = false;
};

} // namespace sol
