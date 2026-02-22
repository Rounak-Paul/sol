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
#include <cstring>

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
    
    // Load user settings from config
    EditorSettings::Get().Load();
    EditorSettings::Get().LoadKeybinds();
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
            auto explorer = std::dynamic_pointer_cast<Explorer>(m_UISystem.GetLayer("Explorer"));
            if (explorer) {
                explorer->Refresh();
            }
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
    
    // New terminal event
    auto newTerminalEvent = std::make_shared<Event>("new_terminal");
    newTerminalEvent->SetHandler([this](const EventData& data) {
        if (m_TerminalPanel) {
            m_TerminalPanel->CreateTerminal();
            m_TerminalPanel->SetEnabled(true);
            m_TerminalPanel->Focus();
            return true;
        }
        return false;
    });
    EventSystem::Register(newTerminalEvent);
    
    // Next terminal event
    auto nextTerminalEvent = std::make_shared<Event>("next_terminal");
    nextTerminalEvent->SetHandler([this](const EventData& data) {
        if (m_TerminalPanel && m_TerminalPanel->IsEnabled()) {
            m_TerminalPanel->NextTerminal();
            return true;
        }
        return false;
    });
    EventSystem::Register(nextTerminalEvent);
    
    // Previous terminal event
    auto prevTerminalEvent = std::make_shared<Event>("prev_terminal");
    prevTerminalEvent->SetHandler([this](const EventData& data) {
        if (m_TerminalPanel && m_TerminalPanel->IsEnabled()) {
            m_TerminalPanel->PrevTerminal();
            return true;
        }
        return false;
    });
    EventSystem::Register(prevTerminalEvent);
    
    // Next buffer event (cycle through buffer tabs)
    auto nextBufferEvent = std::make_shared<Event>("next_buffer");
    nextBufferEvent->SetHandler([](const EventData& data) {
        ResourceSystem::GetInstance().NextBuffer();
        return true;
    });
    EventSystem::Register(nextBufferEvent);
    
    // Previous buffer event (cycle through buffer tabs)
    auto prevBufferEvent = std::make_shared<Event>("prev_buffer");
    prevBufferEvent->SetHandler([](const EventData& data) {
        ResourceSystem::GetInstance().PrevBuffer();
        return true;
    });
    EventSystem::Register(prevBufferEvent);
    
    // New buffer event
    auto newBufferEvent = std::make_shared<Event>("new_buffer");
    newBufferEvent->SetHandler([](const EventData& data) {
        auto buffer = ResourceSystem::GetInstance().CreateNewBuffer();
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
        if (ImGui::IsKeyPressed(key, false)) {
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
