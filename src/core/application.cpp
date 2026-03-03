#include "application.h"
#include "core/event_system.h"
#include "core/logger.h"
#include "core/resource_system.h"
#include "core/file_dialog.h"
#include "core/text/text_buffer.h"
#include "core/lsp/lsp_manager.h"
#include "ui/layers/menu_bar.h"
#include "ui/layers/workspace.h"
#include "ui/layers/status_bar.h"
#include "ui/layers/settings.h"
#include "ui/editor_settings.h"
#include "ui/input/command.h"
#include <imgui.h>
#include <cstring>
#include <unistd.h>
#include <sys/wait.h>

using sol::Logger;
using sol::EventSystem;

namespace sol {

Application::Application() {
}

void Application::SetArgs(int argc, char* argv[]) {
    if (argc >= 1 && argv[0])
        m_ExecutablePath = argv[0];
    if (argc >= 2 && argv[1])
        m_InitialPath = argv[1];
}

Application::~Application() {
    // Ensure proper cleanup of systems
    LSPManager::GetInstance().Shutdown();
    JobSystem::Shutdown();
}

void Application::OnStart() {
    Logger::Info("Application starting...");
    
    // Load user settings from config
    EditorSettings::Get().Load();
    EditorSettings::Get().LoadKeybinds();
    EditorSettings::Get().LoadBehavior();
    Logger::Info("User settings loaded");
    
    // Initialize language registry for syntax highlighting
    LanguageRegistry::GetInstance().InitializeBuiltins();
    Logger::Info("Language registry initialized");
    
    // Initialize LSP
    LSPManager::GetInstance().Initialize(std::filesystem::current_path().string());
    Logger::Info("LSP Manager initialized");
    
    SetupEvents();
    SetupUILayers();
    SetupInputSystem();

    // Open path passed on command line (file or folder)
    if (!m_InitialPath.empty()) {
        std::filesystem::path p = m_InitialPath;
        if (std::filesystem::is_directory(p)) {
            ResourceSystem::GetInstance().SetWorkingDirectory(p);
            LSPManager::GetInstance().Initialize(p.string());
            if (m_Workspace) m_Workspace->GetExplorer().Refresh();
            Logger::Info("Opened initial folder: " + p.string());
        } else {
            // Also set working directory to the file's parent for LSP context
            ResourceSystem::GetInstance().SetWorkingDirectory(p.parent_path());
            LSPManager::GetInstance().Initialize(p.parent_path().string());
            if (m_Workspace) m_Workspace->GetExplorer().Refresh();
            ResourceSystem::GetInstance().OpenFile(p);
            Logger::Info("Opened initial file: " + p.string());
        }
    }

    Logger::Info("Application initialized successfully");
}

void Application::OnUpdate() {
}

void Application::OnUI() {
    // Process input through the new InputSystem
    ProcessInput();
    
    m_UISystem.RenderLayers();
    
    // Render SaveAs dialog if open
    RenderSaveAsDialog();
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
        }
        return true; // Dialog opened successfully, user cancel is not a failure
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
            if (m_Workspace) m_Workspace->GetExplorer().Refresh();
        }
        return true; // Dialog opened successfully, user cancel is not a failure
    });
    EventSystem::Register(openFolderEvent);
    
    auto saveFileEvent = std::make_shared<Event>("save_file");
    saveFileEvent->SetHandler([this](const EventData& data) {
        auto activeBuffer = ResourceSystem::GetInstance().GetActiveBuffer();
        if (activeBuffer) {
            if (activeBuffer->GetResource()->IsUntitled()) {
                OpenSaveAsDialog(activeBuffer);
                return true;  // Dialog will handle the save
            }
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
    
    // Write all files - only saves existing files, skips untitled buffers
    auto writeAllFilesEvent = std::make_shared<Event>("write_all_files");
    writeAllFilesEvent->SetHandler([](const EventData& data) {
        auto& rs = ResourceSystem::GetInstance();
        bool allSaved = true;
        for (const auto& buffer : rs.GetBuffers()) {
            if (!buffer->GetResource()->IsUntitled()) {
                if (!rs.SaveBuffer(buffer->GetId())) {
                    allSaved = false;
                }
            }
        }
        return allSaved;
    });
    EventSystem::Register(writeAllFilesEvent);
    
    // Next buffer or focus buffer (if not buffer-focused, focus buffer; otherwise cycle next)
    auto nextBufOrFocusEvent = std::make_shared<Event>("next_buffer_or_focus");
    nextBufOrFocusEvent->SetHandler([this](const EventData& data) {
        if (!m_Workspace) return false;
        if (m_Workspace->IsExplorerFocused() || m_Workspace->IsTerminalFocused()) {
            m_Workspace->Focus();
            return true;
        }
        // Already buffer-focused → cycle to next buffer
        ResourceSystem::GetInstance().NextBuffer();
        auto active = ResourceSystem::GetInstance().GetActiveBuffer();
        auto* win = m_Workspace->GetWindowTree().GetActiveWindow();
        if (active && win)
            win->ShowBuffer(active->GetId());
        return true;
    });
    EventSystem::Register(nextBufOrFocusEvent);

    // Previous buffer or focus buffer
    auto prevBufOrFocusEvent = std::make_shared<Event>("prev_buffer_or_focus");
    prevBufOrFocusEvent->SetHandler([this](const EventData& data) {
        if (!m_Workspace) return false;
        if (m_Workspace->IsExplorerFocused() || m_Workspace->IsTerminalFocused()) {
            m_Workspace->Focus();
            return true;
        }
        ResourceSystem::GetInstance().PrevBuffer();
        auto active = ResourceSystem::GetInstance().GetActiveBuffer();
        auto* win = m_Workspace->GetWindowTree().GetActiveWindow();
        if (active && win)
            win->ShowBuffer(active->GetId());
        return true;
    });
    EventSystem::Register(prevBufOrFocusEvent);

    // Close buffer event
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
    
    // Explorer toggle event
    auto toggleExplorerEvent = std::make_shared<Event>("toggle_explorer");
    toggleExplorerEvent->SetHandler([this](const EventData& data) {
        if (m_Workspace) {
            m_Workspace->ToggleExplorer();
            return true;
        }
        return false;
    });
    EventSystem::Register(toggleExplorerEvent);

    // Terminal toggle events
    auto toggleTerminalEvent = std::make_shared<Event>("toggle_terminal");
    toggleTerminalEvent->SetHandler([this](const EventData& data) {
        if (m_Workspace) {
            m_Workspace->ToggleTerminal(TerminalPosition::Bottom);
            return true;
        }
        return false;
    });
    EventSystem::Register(toggleTerminalEvent);

    auto toggleTerminalRightEvent = std::make_shared<Event>("toggle_terminal_right");
    toggleTerminalRightEvent->SetHandler([this](const EventData& data) {
        if (m_Workspace) {
            m_Workspace->ToggleTerminal(TerminalPosition::Right);
            return true;
        }
        return false;
    });
    EventSystem::Register(toggleTerminalRightEvent);

    auto toggleTerminalFloatEvent = std::make_shared<Event>("toggle_terminal_float");
    toggleTerminalFloatEvent->SetHandler([this](const EventData& data) {
        if (m_Workspace) {
            m_Workspace->ToggleTerminal(TerminalPosition::Floating);
            return true;
        }
        return false;
    });
    EventSystem::Register(toggleTerminalFloatEvent);

    // New terminal tab
    auto newTerminalEvent = std::make_shared<Event>("new_terminal");
    newTerminalEvent->SetHandler([this](const EventData& data) {
        if (m_Workspace) {
            m_Workspace->NewTerminal();
            return true;
        }
        return false;
    });
    EventSystem::Register(newTerminalEvent);

    // Close active terminal tab
    auto closeTerminalEvent = std::make_shared<Event>("close_terminal");
    closeTerminalEvent->SetHandler([this](const EventData& data) {
        if (m_Workspace) {
            m_Workspace->CloseTerminal();
            return true;
        }
        return false;
    });
    EventSystem::Register(closeTerminalEvent);

    // Next/prev terminal tab (fired by terminal widget on Tab in command mode)
    auto nextTermEvent = std::make_shared<Event>("next_terminal");
    nextTermEvent->SetHandler([this](const EventData& data) {
        if (m_Workspace) { m_Workspace->GetTerminalPanel().NextTab(); return true; }
        return false;
    });
    EventSystem::Register(nextTermEvent);

    auto prevTermEvent = std::make_shared<Event>("prev_terminal");
    prevTermEvent->SetHandler([this](const EventData& data) {
        if (m_Workspace) { m_Workspace->GetTerminalPanel().PrevTab(); return true; }
        return false;
    });
    EventSystem::Register(prevTermEvent);
    
    // Next buffer event (cycle through buffer tabs)
    auto nextBufferEvent = std::make_shared<Event>("next_buffer");
    nextBufferEvent->SetHandler([this](const EventData& data) {
        ResourceSystem::GetInstance().NextBuffer();
        // Show the new active buffer in the active window
        if (m_Workspace) {
            auto active = ResourceSystem::GetInstance().GetActiveBuffer();
            auto* win = m_Workspace->GetWindowTree().GetActiveWindow();
            if (active && win)
                win->ShowBuffer(active->GetId());
        }
        return true;
    });
    EventSystem::Register(nextBufferEvent);

    // Previous buffer event (cycle through buffer tabs)
    auto prevBufferEvent = std::make_shared<Event>("prev_buffer");
    prevBufferEvent->SetHandler([this](const EventData& data) {
        ResourceSystem::GetInstance().PrevBuffer();
        if (m_Workspace) {
            auto active = ResourceSystem::GetInstance().GetActiveBuffer();
            auto* win = m_Workspace->GetWindowTree().GetActiveWindow();
            if (active && win)
                win->ShowBuffer(active->GetId());
        }
        return true;
    });
    EventSystem::Register(prevBufferEvent);

    // New buffer event
    auto newBufferEvent = std::make_shared<Event>("new_buffer");
    newBufferEvent->SetHandler([this](const EventData& data) {
        auto buffer = ResourceSystem::GetInstance().CreateNewBuffer();
        if (buffer && m_Workspace) {
            auto* win = m_Workspace->GetWindowTree().GetActiveWindow();
            if (win) win->ShowBuffer(buffer->GetId());
        }
        return buffer != nullptr;
    });
    EventSystem::Register(newBufferEvent);
    
    // Write file event (alias for save_file, matches vim :w)
    auto writeFileEvent = std::make_shared<Event>("write_file");
    writeFileEvent->SetHandler([this](const EventData& data) {
        auto activeBuffer = ResourceSystem::GetInstance().GetActiveBuffer();
        if (activeBuffer) {
            if (activeBuffer->GetResource()->IsUntitled()) {
                OpenSaveAsDialog(activeBuffer);
                return true;  // Dialog will handle the save
            }
            return ResourceSystem::GetInstance().SaveBuffer(activeBuffer->GetId());
        }
        return false;
    });
    EventSystem::Register(writeFileEvent);
    
    // Navigation events (for command mode WASD)
    auto navUpEvent = std::make_shared<Event>("nav_up");
    navUpEvent->SetHandler([](const EventData& data) {
        InputSystem::GetInstance().SetPendingNavigation(ImGuiKey_UpArrow);
        return true;
    });
    EventSystem::Register(navUpEvent);
    
    auto navDownEvent = std::make_shared<Event>("nav_down");
    navDownEvent->SetHandler([](const EventData& data) {
        InputSystem::GetInstance().SetPendingNavigation(ImGuiKey_DownArrow);
        return true;
    });
    EventSystem::Register(navDownEvent);
    
    auto navLeftEvent = std::make_shared<Event>("nav_left");
    navLeftEvent->SetHandler([](const EventData& data) {
        InputSystem::GetInstance().SetPendingNavigation(ImGuiKey_LeftArrow);
        return true;
    });
    EventSystem::Register(navLeftEvent);
    
    auto navRightEvent = std::make_shared<Event>("nav_right");
    navRightEvent->SetHandler([](const EventData& data) {
        InputSystem::GetInstance().SetPendingNavigation(ImGuiKey_RightArrow);
        return true;
    });
    EventSystem::Register(navRightEvent);
    
    // Undo/Redo events
    auto undoEvent = std::make_shared<Event>("undo");
    undoEvent->SetHandler([this](const EventData& data) {
        if (m_Workspace) {
            return m_Workspace->Undo();
        }
        return false;
    });
    EventSystem::Register(undoEvent);
    
    auto redoEvent = std::make_shared<Event>("redo");
    redoEvent->SetHandler([this](const EventData& data) {
        if (m_Workspace) {
            return m_Workspace->Redo();
        }
        return false;
    });
    EventSystem::Register(redoEvent);

    // Open current buffer in a new sol instance
    auto openInNewInstanceEvent = std::make_shared<Event>("open_in_new_instance");
    openInNewInstanceEvent->SetHandler([this](const EventData& data) {
        if (m_ExecutablePath.empty()) {
            Logger::Error("open_in_new_instance: executable path unknown");
            return false;
        }
        auto activeBuffer = ResourceSystem::GetInstance().GetActiveBuffer();
        const char* fileArg = nullptr;
        std::string filePath;
        if (activeBuffer && !activeBuffer->GetResource()->IsUntitled()) {
            filePath = activeBuffer->GetResource()->GetPath().string();
            fileArg = filePath.c_str();
        }
        // Double-fork so the grandchild is adopted by init and we never get a zombie.
        pid_t pid = fork();
        if (pid == 0) {
            pid_t pid2 = fork();
            if (pid2 == 0) {
                setsid();
                if (fileArg)
                    execl(m_ExecutablePath.c_str(), m_ExecutablePath.c_str(), fileArg, nullptr);
                else
                    execl(m_ExecutablePath.c_str(), m_ExecutablePath.c_str(), nullptr);
                _exit(1);
            }
            _exit(0);
        }
        if (pid > 0) {
            int status;
            waitpid(pid, &status, 0);
        }
        Logger::Info(fileArg ? std::string("Opened new instance for: ") + filePath
                              : "Opened new empty instance");
        return true;
    });
    EventSystem::Register(openInNewInstanceEvent);

    // Window split events
    auto splitVerticalEvent = std::make_shared<Event>("split_vertical");
    splitVerticalEvent->SetHandler([this](const EventData& data) {
        if (m_Workspace) { m_Workspace->SplitVertical(); return true; }
        return false;
    });
    EventSystem::Register(splitVerticalEvent);

    auto splitHorizontalEvent = std::make_shared<Event>("split_horizontal");
    splitHorizontalEvent->SetHandler([this](const EventData& data) {
        if (m_Workspace) { m_Workspace->SplitHorizontal(); return true; }
        return false;
    });
    EventSystem::Register(splitHorizontalEvent);

    auto focusNextWindowEvent = std::make_shared<Event>("focus_next_window");
    focusNextWindowEvent->SetHandler([this](const EventData& data) {
        if (m_Workspace) { m_Workspace->FocusNextWindow(); return true; }
        return false;
    });
    EventSystem::Register(focusNextWindowEvent);

    auto focusPrevWindowEvent = std::make_shared<Event>("focus_prev_window");
    focusPrevWindowEvent->SetHandler([this](const EventData& data) {
        if (m_Workspace) { m_Workspace->FocusPrevWindow(); return true; }
        return false;
    });
    EventSystem::Register(focusPrevWindowEvent);

    // Show buffer in active window when a file is opened
    ResourceSystem::GetInstance().SetOnBufferOpened([this](std::shared_ptr<Buffer> buffer) {
        if (m_Workspace && buffer) {
            auto* win = m_Workspace->GetWindowTree().GetActiveWindow();
            if (win)
                win->ShowBuffer(buffer->GetId());
        }
    });
}

void Application::SetupUILayers() {
    // Apply initial theme
    EditorSettings::Get().ApplyTheme();

    auto menuBar = std::make_shared<MenuBar>(&m_UISystem);
    m_UISystem.RegisterLayer(menuBar);
    
    m_Workspace = std::make_shared<Workspace>();
    m_UISystem.RegisterLayer(m_Workspace);
    
    // Connect LSP diagnostics to workspace
    LSPManager::GetInstance().SetDiagnosticsCallback([this](const std::string& path, const std::vector<LSPDiagnostic>& diagnostics) {
        m_Workspace->UpdateDiagnostics(path, diagnostics);
    });
    
    auto statusBar = std::make_shared<StatusBar>();
    m_UISystem.RegisterLayer(statusBar);

    auto settings = std::make_shared<SettingsWindow>();
    settings->SetEnabled(false);
    m_UISystem.RegisterLayer(settings);
}

int Application::GetDockspaceFlags() {
    return ImGuiDockNodeFlags_PassthruCentralNode | ImGuiDockNodeFlags_NoDockingInCentralNode;
}

float Application::GetDockspaceBottomOffset() {
    return UISystem::StatusBarHeight * ImGui::GetIO().FontGlobalScale;
}

void Application::SetupInputSystem() {
    // Setup default keybindings (Leader+Q for quit)
    InputSystem::GetInstance().SetupDefaultBindings();
    Logger::Info("Input system initialized");
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
        // Use repeat only when no pending sequence (for nav keys, etc.)
        // For multi-key sequences, only trigger on initial press
        bool hasPending = input.HasPendingSequence();
        bool pressed = hasPending ? ImGui::IsKeyPressed(key, false) : ImGui::IsKeyPressed(key, true);
        
        if (pressed) {
            KeyChord chord(key, mods);
            if (input.ProcessKey(chord, input.GetContext())) {
                // Key was handled by a binding
                break;
            }
        }
    }
}

void Application::OpenSaveAsDialog(std::shared_ptr<Buffer> buffer) {
    m_ShowSaveAsDialog = true;
    m_SaveAsBuffer = buffer;
    
    // Initialize filename from buffer name (e.g., "Untitled-1")
    std::string name = buffer->GetName();
    strncpy(m_SaveAsFilename, name.c_str(), sizeof(m_SaveAsFilename) - 1);
    m_SaveAsFilename[sizeof(m_SaveAsFilename) - 1] = '\0';
    
    // Initialize path to empty (will use working directory or runtime directory)
    m_SaveAsPath[0] = '\0';
}

void Application::RenderSaveAsDialog() {
    if (!m_ShowSaveAsDialog) return;
    
    ImGui::OpenPopup("Save As");
    
    // Center the modal
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImVec2 center = viewport->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(400, 0), ImGuiCond_Always);
    
    bool hasWorkingDir = ResourceSystem::GetInstance().HasWorkingDirectory();
    
    if (ImGui::BeginPopupModal("Save As", &m_ShowSaveAsDialog, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Save file to disk");
        ImGui::Separator();
        ImGui::Spacing();
        
        // Filename input
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##filename", m_SaveAsFilename, sizeof(m_SaveAsFilename));
        ImGui::TextDisabled("Filename (e.g., main.cpp)");
        
        ImGui::Spacing();
        
        // Path input (only if working directory is set)
        if (hasWorkingDir) {
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##path", m_SaveAsPath, sizeof(m_SaveAsPath));
            ImGui::TextDisabled("Relative path (e.g., src/utils) - leave empty for project root");
        } else {
            ImGui::BeginDisabled();
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##path", m_SaveAsPath, sizeof(m_SaveAsPath));
            ImGui::TextDisabled("Open a folder to enable relative paths");
            ImGui::EndDisabled();
            m_SaveAsPath[0] = '\0';  // Clear path when disabled
        }
        
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        
        // Preview the full path
        std::filesystem::path fullPath;
        if (hasWorkingDir) {
            fullPath = ResourceSystem::GetInstance().GetWorkingDirectory();
            if (strlen(m_SaveAsPath) > 0) {
                fullPath /= m_SaveAsPath;
            }
        } else {
            fullPath = std::filesystem::current_path();
        }
        fullPath /= m_SaveAsFilename;
        
        ImGui::TextWrapped("Will save to: %s", fullPath.string().c_str());
        
        ImGui::Spacing();
        
        // Buttons
        float buttonWidth = 80.0f;
        float totalWidth = buttonWidth * 2 + ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - totalWidth) * 0.5f);
        
        bool validFilename = strlen(m_SaveAsFilename) > 0;
        
        ImGui::BeginDisabled(!validFilename);
        if (ImGui::Button("Save", ImVec2(buttonWidth, 0))) {
            // Create directory if it doesn't exist
            std::filesystem::path dir = fullPath.parent_path();
            if (!dir.empty() && !std::filesystem::exists(dir)) {
                std::filesystem::create_directories(dir);
            }
            
            // Set the new path and save
            if (m_SaveAsBuffer) {
                m_SaveAsBuffer->GetResource()->SetPath(fullPath);
                ResourceSystem::GetInstance().SaveBuffer(m_SaveAsBuffer->GetId());
            }
            
            m_ShowSaveAsDialog = false;
            m_SaveAsBuffer = nullptr;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        
        ImGui::SameLine();
        
        if (ImGui::Button("Cancel", ImVec2(buttonWidth, 0))) {
            m_ShowSaveAsDialog = false;
            m_SaveAsBuffer = nullptr;
            ImGui::CloseCurrentPopup();
        }
        
        ImGui::EndPopup();
    }
}

} // namespace sol
