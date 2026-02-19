#include "application.h"
#include "core/event_system.h"
#include "core/logger.h"
#include "core/resource_system.h"
#include "core/file_dialog.h"
#include "core/text/text_buffer.h"
#include "core/lsp/lsp_manager.h"
#include "ui/layers/menu_bar.h"
#include "ui/layers/workspace.h"
#include "ui/layers/explorer.h"
#include "ui/layers/status_bar.h"
#include "ui/layers/settings.h"
#include "ui/layers/terminal_panel.h"
#include "ui/editor_settings.h"
#include "ui/input/command.h"
#include <imgui.h>

using sol::Logger;
using sol::EventSystem;

namespace sol {

Application::Application() {
}

Application::~Application() {
    // Ensure proper cleanup of systems
    LSPManager::GetInstance().Shutdown();
    JobSystem::Shutdown();
}

void Application::OnStart() {
    Logger::Info("Application starting...");
    
    // Initialize language registry for syntax highlighting
    LanguageRegistry::GetInstance().InitializeBuiltins();
    Logger::Info("Language registry initialized");
    
    // Initialize LSP
    LSPManager::GetInstance().Initialize(std::filesystem::current_path().string());
    Logger::Info("LSP Manager initialized");
    
    SetupEvents();
    SetupUILayers();
    SetupInputSystem();
    Logger::Info("Application initialized successfully");
}

void Application::OnUpdate() {
}

void Application::OnUI() {
    // Process input through the new InputSystem
    ProcessInput();
    
    m_UISystem.RenderLayers();
}

void Application::SetupEvents() {
    Logger::Info("Setting up events...");
    
    auto exitEvent = std::make_shared<Event>("exit");
    exitEvent->SetHandler([this](const EventData& data) {
        Logger::Info("Exit event triggered");
        Quit();
        return true;
    })
    .SetSuccessCallback([](const EventData& data) {
        Logger::Info("Exit event completed successfully");
    })
    .SetFailureCallback([](const std::string& error) {
        Logger::Error("Exit event failed: " + error);
    });
    EventSystem::Register(exitEvent);
    
    auto toggleWindowEvent = std::make_shared<Event>("toggle_window");
    toggleWindowEvent->SetHandler([this](const EventData& data) {
        auto it = data.find("window_id");
        if (it == data.end()) {
            Logger::Error("toggle_window event missing window_id");
            return false;
        }
        
        std::string windowId = std::any_cast<std::string>(it->second);
        auto layer = m_UISystem.GetLayer(windowId);
        if (layer) {
            layer->SetEnabled(!layer->IsEnabled());
            Logger::Info("Toggled window: " + windowId + " to " + (layer->IsEnabled() ? "enabled" : "disabled"));
            return true;
        }
        
        Logger::Error("Window not found: " + windowId);
        return false;
    });
    EventSystem::Register(toggleWindowEvent);
    
    // File events
    auto openFileEvent = std::make_shared<Event>("open_file_dialog");
    openFileEvent->SetHandler([](const EventData& data) {
        auto path = FileDialog::OpenFile("Open File");
        if (path) {
            ResourceSystem::GetInstance().OpenFile(*path);
            Logger::Info("Opened file: " + path->string());
            return true;
        }
        return false;
    });
    EventSystem::Register(openFileEvent);
    
    auto openFolderEvent = std::make_shared<Event>("open_folder_dialog");
    openFolderEvent->SetHandler([this](const EventData& data) {
        auto path = FileDialog::OpenFolder("Open Folder");
        if (path) {
            ResourceSystem::GetInstance().SetWorkingDirectory(*path);
            LSPManager::GetInstance().Initialize(path->string());
            Logger::Info("Opened folder: " + path->string());
            
            // Refresh explorer
            auto explorer = std::dynamic_pointer_cast<Explorer>(m_UISystem.GetLayer("Explorer"));
            if (explorer) {
                explorer->Refresh();
            }
            return true;
        }
        return false;
    });
    EventSystem::Register(openFolderEvent);
    
    auto saveFileEvent = std::make_shared<Event>("save_file");
    saveFileEvent->SetHandler([](const EventData& data) {
        auto activeBuffer = ResourceSystem::GetInstance().GetActiveBuffer();
        if (activeBuffer) {
            return ResourceSystem::GetInstance().SaveBuffer(activeBuffer->GetId());
        }
        return false;
    });
    EventSystem::Register(saveFileEvent);
    
    auto saveAllEvent = std::make_shared<Event>("save_all_files");
    saveAllEvent->SetHandler([](const EventData& data) {
        return ResourceSystem::GetInstance().SaveAllBuffers();
    });
    EventSystem::Register(saveAllEvent);
    
    auto closeBufferEvent = std::make_shared<Event>("close_buffer");
    closeBufferEvent->SetHandler([](const EventData& data) {
        auto activeBuffer = ResourceSystem::GetInstance().GetActiveBuffer();
        if (activeBuffer) {
            ResourceSystem::GetInstance().CloseBuffer(activeBuffer->GetId());
            return true;
        }
        return false;
    });
    EventSystem::Register(closeBufferEvent);
    
    // Terminal toggle event
    auto toggleTerminalEvent = std::make_shared<Event>("toggle_terminal");
    toggleTerminalEvent->SetHandler([this](const EventData& data) {
        if (m_TerminalPanel) {
            m_TerminalPanel->Toggle();
            return true;
        }
        return false;
    });
    EventSystem::Register(toggleTerminalEvent);
}

void Application::SetupUILayers() {
    // Apply initial theme
    EditorSettings::Get().ApplyTheme();

    auto menuBar = std::make_shared<MenuBar>(&m_UISystem);
    m_UISystem.RegisterLayer(menuBar);
    
    auto explorer = std::make_shared<Explorer>();
    m_UISystem.RegisterLayer(explorer);
    
    auto workspace = std::make_shared<Workspace>();
    m_UISystem.RegisterLayer(workspace);
    
    // Connect LSP diagnostics to workspace
    LSPManager::GetInstance().SetDiagnosticsCallback([workspace](const std::string& path, const std::vector<LSPDiagnostic>& diagnostics) {
        workspace->UpdateDiagnostics(path, diagnostics);
    });
    
    auto statusBar = std::make_shared<StatusBar>();
    m_UISystem.RegisterLayer(statusBar);

    auto settings = std::make_shared<SettingsWindow>();
    settings->SetEnabled(false);
    m_UISystem.RegisterLayer(settings);
    
    // Terminal panel - disabled by default, toggle with Ctrl+`
    m_TerminalPanel = std::make_shared<TerminalPanel>();
    m_TerminalPanel->SetEnabled(false);
    m_UISystem.RegisterLayer(m_TerminalPanel);
}

int Application::GetDockspaceFlags() {
    return ImGuiDockNodeFlags_PassthruCentralNode | ImGuiDockNodeFlags_NoDockingInCentralNode;
}

float Application::GetDockspaceBottomOffset() {
    return UISystem::StatusBarHeight * ImGui::GetIO().FontGlobalScale;
}

void Application::SetupInputSystem() {
    auto& input = InputSystem::GetInstance();
    auto& registry = CommandRegistry::GetInstance();
    auto keymap = input.GetActiveKeymap();
    
    // Register commands with actual handlers
    registry.Register("file.save", "Save File", [](const CommandArgs&) {
        EventSystem::Execute("save_file");
        return CommandResult::Success();
    });
    registry.Get("file.save")->SetCategory("File").SetDescription("Save the current file");
    
    registry.Register("file.saveAll", "Save All Files", [](const CommandArgs&) {
        EventSystem::Execute("save_all_files");
        return CommandResult::Success();
    });
    registry.Get("file.saveAll")->SetCategory("File").SetDescription("Save all open files");
    
    registry.Register("file.open", "Open File", [](const CommandArgs&) {
        EventSystem::Execute("open_file_dialog");
        return CommandResult::Success();
    });
    registry.Get("file.open")->SetCategory("File").SetDescription("Open a file");
    
    registry.Register("file.openFolder", "Open Folder", [](const CommandArgs&) {
        EventSystem::Execute("open_folder_dialog");
        return CommandResult::Success();
    });
    registry.Get("file.openFolder")->SetCategory("File").SetDescription("Open a folder");
    
    registry.Register("file.close", "Close File", [](const CommandArgs&) {
        EventSystem::Execute("close_buffer");
        return CommandResult::Success();
    });
    registry.Get("file.close")->SetCategory("File").SetDescription("Close the current file");
    
    registry.Register("view.toggleTerminal", "Toggle Terminal", [this](const CommandArgs&) {
        if (m_TerminalPanel) {
            m_TerminalPanel->Toggle();
        }
        return CommandResult::Success();
    });
    registry.Get("view.toggleTerminal")->SetCategory("View").SetDescription("Toggle the terminal panel");
    
    registry.Register("view.toggleExplorer", "Toggle Explorer", [this](const CommandArgs&) {
        auto explorer = m_UISystem.GetLayer("Explorer");
        if (explorer) {
            explorer->SetEnabled(!explorer->IsEnabled());
        }
        return CommandResult::Success();
    });
    registry.Get("view.toggleExplorer")->SetCategory("View").SetDescription("Toggle the file explorer");
    
    registry.Register("app.quit", "Quit Application", [this](const CommandArgs&) {
        Quit();
        return CommandResult::Success();
    });
    registry.Get("app.quit")->SetCategory("Application").SetDescription("Quit the application");
    
    // Set up keybindings
    keymap->Bind("Ctrl+S", "file.save");
    keymap->Bind("Ctrl+Shift+S", "file.saveAll");
    keymap->Bind("Ctrl+O", "file.open");
    keymap->Bind("Ctrl+Shift+O", "file.openFolder");
    keymap->Bind("Ctrl+W", "file.close");
    keymap->Bind("Ctrl+`", "view.toggleTerminal");
    keymap->Bind("Ctrl+B", "view.toggleExplorer");
    keymap->Bind("Ctrl+Q", "app.quit");
    
    Logger::Info("Input system initialized with default keybindings");
}

void Application::ProcessInput() {
    auto& input = InputSystem::GetInstance();
    
    // Check all keys to see if any keybinding matches
    // This is a simple approach - check each key that was pressed this frame
    ImGuiIO& io = ImGui::GetIO();
    
    // Skip if any text input widget has focus (they handle their own input)
    if (io.WantTextInput) {
        input.ResetPendingSequence();
        return;
    }
    
    // Get current modifiers
    Modifier mods = KeyChord::GetCurrentModifiers();
    
    // Check common keys
    static const ImGuiKey s_CheckKeys[] = {
        // Letters
        ImGuiKey_A, ImGuiKey_B, ImGuiKey_C, ImGuiKey_D, ImGuiKey_E, ImGuiKey_F,
        ImGuiKey_G, ImGuiKey_H, ImGuiKey_I, ImGuiKey_J, ImGuiKey_K, ImGuiKey_L,
        ImGuiKey_M, ImGuiKey_N, ImGuiKey_O, ImGuiKey_P, ImGuiKey_Q, ImGuiKey_R,
        ImGuiKey_S, ImGuiKey_T, ImGuiKey_U, ImGuiKey_V, ImGuiKey_W, ImGuiKey_X,
        ImGuiKey_Y, ImGuiKey_Z,
        // Numbers
        ImGuiKey_0, ImGuiKey_1, ImGuiKey_2, ImGuiKey_3, ImGuiKey_4,
        ImGuiKey_5, ImGuiKey_6, ImGuiKey_7, ImGuiKey_8, ImGuiKey_9,
        // Function keys
        ImGuiKey_F1, ImGuiKey_F2, ImGuiKey_F3, ImGuiKey_F4, ImGuiKey_F5, ImGuiKey_F6,
        ImGuiKey_F7, ImGuiKey_F8, ImGuiKey_F9, ImGuiKey_F10, ImGuiKey_F11, ImGuiKey_F12,
        // Navigation
        ImGuiKey_UpArrow, ImGuiKey_DownArrow, ImGuiKey_LeftArrow, ImGuiKey_RightArrow,
        ImGuiKey_Home, ImGuiKey_End, ImGuiKey_PageUp, ImGuiKey_PageDown,
        // Editing
        ImGuiKey_Insert, ImGuiKey_Delete, ImGuiKey_Backspace, ImGuiKey_Enter,
        ImGuiKey_Tab, ImGuiKey_Escape, ImGuiKey_Space,
        // Punctuation
        ImGuiKey_GraveAccent, ImGuiKey_Minus, ImGuiKey_Equal,
        ImGuiKey_LeftBracket, ImGuiKey_RightBracket, ImGuiKey_Backslash,
        ImGuiKey_Semicolon, ImGuiKey_Apostrophe, ImGuiKey_Comma, ImGuiKey_Period, ImGuiKey_Slash,
    };
    
    for (ImGuiKey key : s_CheckKeys) {
        if (ImGui::IsKeyPressed(key, false)) {
            KeyChord chord(key, mods);
            if (input.ProcessKey(chord, input.GetContext())) {
                // Key was handled by a binding
                break;
            }
        }
    }
}

} // namespace sol
