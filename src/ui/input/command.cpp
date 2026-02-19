#include "command.h"
#include "core/logger.h"
#include <algorithm>

namespace sol {

// Command implementation
Command::Command(const std::string& id, const std::string& title, CommandCallback callback)
    : m_Id(id), m_Title(title), m_Callback(callback) {}

CommandResult Command::Execute(const CommandArgs& args) const {
    if (!m_Enabled) {
        return CommandResult::Failure("Command is disabled");
    }
    
    if (!m_Callback) {
        return CommandResult::Failure("No callback registered");
    }
    
    try {
        return m_Callback(args);
    } catch (const std::exception& e) {
        return CommandResult::Failure(std::string("Exception: ") + e.what());
    }
}

// CommandRegistry implementation
CommandRegistry& CommandRegistry::GetInstance() {
    static CommandRegistry instance;
    return instance;
}

void CommandRegistry::Register(std::shared_ptr<Command> command) {
    if (!command) return;
    m_Commands[command->GetId()] = command;
}

void CommandRegistry::Register(const std::string& id, const std::string& title, CommandCallback callback) {
    Register(std::make_shared<Command>(id, title, callback));
}

void CommandRegistry::Unregister(const std::string& id) {
    m_Commands.erase(id);
}

std::shared_ptr<Command> CommandRegistry::Get(const std::string& id) const {
    auto it = m_Commands.find(id);
    if (it != m_Commands.end()) {
        return it->second;
    }
    return nullptr;
}

CommandResult CommandRegistry::Execute(const std::string& id, const CommandArgs& args) {
    auto cmd = Get(id);
    if (!cmd) {
        return CommandResult::Failure("Command not found: " + id);
    }
    return cmd->Execute(args);
}

std::vector<std::shared_ptr<Command>> CommandRegistry::GetAll() const {
    std::vector<std::shared_ptr<Command>> result;
    result.reserve(m_Commands.size());
    for (const auto& [id, cmd] : m_Commands) {
        result.push_back(cmd);
    }
    return result;
}

std::vector<std::shared_ptr<Command>> CommandRegistry::GetByCategory(const std::string& category) const {
    std::vector<std::shared_ptr<Command>> result;
    for (const auto& [id, cmd] : m_Commands) {
        if (cmd->GetCategory() == category) {
            result.push_back(cmd);
        }
    }
    return result;
}

std::vector<std::shared_ptr<Command>> CommandRegistry::Search(const std::string& query) const {
    std::vector<std::shared_ptr<Command>> result;
    
    std::string lowerQuery = query;
    std::transform(lowerQuery.begin(), lowerQuery.end(), lowerQuery.begin(), ::tolower);
    
    for (const auto& [id, cmd] : m_Commands) {
        std::string lowerId = id;
        std::string lowerTitle = cmd->GetTitle();
        std::transform(lowerId.begin(), lowerId.end(), lowerId.begin(), ::tolower);
        std::transform(lowerTitle.begin(), lowerTitle.end(), lowerTitle.begin(), ::tolower);
        
        if (lowerId.find(lowerQuery) != std::string::npos ||
            lowerTitle.find(lowerQuery) != std::string::npos) {
            result.push_back(cmd);
        }
    }
    
    return result;
}

// Keymap implementation
Keymap::Keymap(const std::string& name) : m_Name(name) {}

void Keymap::Bind(const KeySequence& keys, const std::string& commandId, InputContext context) {
    // Check if binding already exists, update if so
    for (auto& binding : m_Bindings) {
        if (binding.keys == keys && binding.context == context) {
            binding.commandId = commandId;
            return;
        }
    }
    
    m_Bindings.emplace_back(keys, commandId, context);
}

void Keymap::Bind(const std::string& keys, const std::string& commandId, InputContext context) {
    auto seq = KeySequence::Parse(keys);
    if (seq) {
        Bind(*seq, commandId, context);
    } else {
        Logger::Error("Failed to parse keybinding: " + keys);
    }
}

void Keymap::Unbind(const KeySequence& keys, InputContext context) {
    m_Bindings.erase(
        std::remove_if(m_Bindings.begin(), m_Bindings.end(),
            [&](const Keybinding& b) {
                return b.keys == keys && b.context == context;
            }),
        m_Bindings.end()
    );
}

void Keymap::Unbind(const std::string& keys, InputContext context) {
    auto seq = KeySequence::Parse(keys);
    if (seq) {
        Unbind(*seq, context);
    }
}

const Keybinding* Keymap::FindBinding(const KeySequence& keys, InputContext context) const {
    // First try exact context match
    for (const auto& binding : m_Bindings) {
        if (binding.keys == keys && binding.context == context) {
            return &binding;
        }
    }
    
    // Then try global context
    if (context != InputContext::Global) {
        for (const auto& binding : m_Bindings) {
            if (binding.keys == keys && binding.context == InputContext::Global) {
                return &binding;
            }
        }
    }
    
    return nullptr;
}

std::vector<const Keybinding*> Keymap::GetBindings(InputContext context) const {
    std::vector<const Keybinding*> result;
    for (const auto& binding : m_Bindings) {
        if (binding.context == context || binding.context == InputContext::Global) {
            result.push_back(&binding);
        }
    }
    return result;
}

std::vector<const Keybinding*> Keymap::GetBindingsForCommand(const std::string& commandId) const {
    std::vector<const Keybinding*> result;
    for (const auto& binding : m_Bindings) {
        if (binding.commandId == commandId) {
            result.push_back(&binding);
        }
    }
    return result;
}

std::vector<KeySequence> Keymap::GetAllKeySequences(InputContext context) const {
    std::vector<KeySequence> result;
    for (const auto& binding : m_Bindings) {
        if (binding.context == context || binding.context == InputContext::Global) {
            result.push_back(binding.keys);
        }
    }
    return result;
}

void Keymap::Clear() {
    m_Bindings.clear();
}

// InputSystem implementation
InputSystem::InputSystem() {
    // Create default keymap
    auto defaultKeymap = std::make_unique<Keymap>("default");
    m_ActiveKeymap = defaultKeymap.get();
    m_Keymaps["default"] = std::move(defaultKeymap);
}

InputSystem& InputSystem::GetInstance() {
    static InputSystem instance;
    return instance;
}

bool InputSystem::ProcessKey(const KeyChord& chord, InputContext context) {
    if (!m_ActiveKeymap) return false;
    
    // Update matcher bindings if needed
    auto allKeys = m_ActiveKeymap->GetAllKeySequences(context);
    m_Matcher.SetBindings(allKeys);
    
    // Process the chord
    auto result = m_Matcher.ProcessChord(chord);
    
    switch (result) {
        case KeySequenceMatcher::MatchResult::FullMatch: {
            // Find and execute the command
            auto binding = m_ActiveKeymap->FindBinding(m_Matcher.GetCurrentSequence(), context);
            if (binding) {
                auto cmdResult = CommandRegistry::GetInstance().Execute(binding->commandId);
                if (!cmdResult.success) {
                    Logger::Error("Command failed: " + binding->commandId + " - " + cmdResult.error);
                }
                m_Matcher.Reset();
                return true;
            }
            m_Matcher.Reset();
            return false;
        }
        
        case KeySequenceMatcher::MatchResult::PartialMatch:
            // Waiting for more input
            return true;
        
        case KeySequenceMatcher::MatchResult::NoMatch:
        default:
            return false;
    }
}

const KeySequence& InputSystem::GetPendingSequence() const {
    return m_Matcher.GetCurrentSequence();
}

bool InputSystem::HasPendingSequence() const {
    return !m_Matcher.GetCurrentSequence().Empty();
}

void InputSystem::ResetPendingSequence() {
    m_Matcher.Reset();
}

void InputSystem::SetActiveKeymap(const std::string& name) {
    auto it = m_Keymaps.find(name);
    if (it != m_Keymaps.end()) {
        m_ActiveKeymapName = name;
        m_ActiveKeymap = it->second.get();
    }
}

Keymap* InputSystem::GetKeymap(const std::string& name) {
    auto it = m_Keymaps.find(name);
    if (it != m_Keymaps.end()) {
        return it->second.get();
    }
    return nullptr;
}

Keymap* InputSystem::GetActiveKeymap() {
    return m_ActiveKeymap;
}

void InputSystem::AddKeymap(std::unique_ptr<Keymap> keymap) {
    if (keymap) {
        m_Keymaps[keymap->GetName()] = std::move(keymap);
    }
}

void InputSystem::RemoveKeymap(const std::string& name) {
    if (name != "default") {  // Don't remove default keymap
        if (m_ActiveKeymapName == name) {
            SetActiveKeymap("default");
        }
        m_Keymaps.erase(name);
    }
}

void InputSystem::SetupDefaultBindings() {
    auto& registry = CommandRegistry::GetInstance();
    auto keymap = GetActiveKeymap();
    if (!keymap) return;
    
    // File commands
    registry.Register("file.save", "Save File", [](const CommandArgs&) {
        // Will be connected to actual save function
        return CommandResult::Success();
    });
    registry.Get("file.save")->SetCategory("File");
    
    registry.Register("file.saveAll", "Save All Files", [](const CommandArgs&) {
        return CommandResult::Success();
    });
    registry.Get("file.saveAll")->SetCategory("File");

    registry.Register("file.open", "Open File", [](const CommandArgs&) {
        return CommandResult::Success();
    });
    registry.Get("file.open")->SetCategory("File");
    
    registry.Register("file.openFolder", "Open Folder", [](const CommandArgs&) {
        return CommandResult::Success();
    });
    registry.Get("file.openFolder")->SetCategory("File");
    
    registry.Register("file.close", "Close File", [](const CommandArgs&) {
        return CommandResult::Success();
    });
    registry.Get("file.close")->SetCategory("File");
    
    // Edit commands
    registry.Register("edit.undo", "Undo", [](const CommandArgs&) {
        return CommandResult::Success();
    });
    registry.Get("edit.undo")->SetCategory("Edit");
    
    registry.Register("edit.redo", "Redo", [](const CommandArgs&) {
        return CommandResult::Success();
    });
    registry.Get("edit.redo")->SetCategory("Edit");
    
    registry.Register("edit.cut", "Cut", [](const CommandArgs&) {
        return CommandResult::Success();
    });
    registry.Get("edit.cut")->SetCategory("Edit");
    
    registry.Register("edit.copy", "Copy", [](const CommandArgs&) {
        return CommandResult::Success();
    });
    registry.Get("edit.copy")->SetCategory("Edit");
    
    registry.Register("edit.paste", "Paste", [](const CommandArgs&) {
        return CommandResult::Success();
    });
    registry.Get("edit.paste")->SetCategory("Edit");
    
    registry.Register("edit.selectAll", "Select All", [](const CommandArgs&) {
        return CommandResult::Success();
    });
    registry.Get("edit.selectAll")->SetCategory("Edit");
    
    // View commands
    registry.Register("view.toggleTerminal", "Toggle Terminal", [](const CommandArgs&) {
        return CommandResult::Success();
    });
    registry.Get("view.toggleTerminal")->SetCategory("View");
    
    registry.Register("view.toggleExplorer", "Toggle Explorer", [](const CommandArgs&) {
        return CommandResult::Success();
    });
    registry.Get("view.toggleExplorer")->SetCategory("View");
    
    registry.Register("view.commandPalette", "Show Command Palette", [](const CommandArgs&) {
        return CommandResult::Success();
    });
    registry.Get("view.commandPalette")->SetCategory("View");
    
    // Application commands
    registry.Register("app.quit", "Quit", [](const CommandArgs&) {
        return CommandResult::Success();
    });
    registry.Get("app.quit")->SetCategory("Application");
    
    // Default keybindings
    keymap->Bind("Ctrl+S", "file.save");
    keymap->Bind("Ctrl+Shift+S", "file.saveAll");
    keymap->Bind("Ctrl+O", "file.open");
    keymap->Bind("Ctrl+Shift+O", "file.openFolder");
    keymap->Bind("Ctrl+W", "file.close");
    
    keymap->Bind("Ctrl+Z", "edit.undo");
    keymap->Bind("Ctrl+Shift+Z", "edit.redo");
    keymap->Bind("Ctrl+Y", "edit.redo");  // Alternative
    keymap->Bind("Ctrl+X", "edit.cut");
    keymap->Bind("Ctrl+C", "edit.copy");
    keymap->Bind("Ctrl+V", "edit.paste");
    keymap->Bind("Ctrl+A", "edit.selectAll");
    
    keymap->Bind("Ctrl+`", "view.toggleTerminal");
    keymap->Bind("Ctrl+B", "view.toggleExplorer");
    keymap->Bind("Ctrl+Shift+P", "view.commandPalette");
    keymap->Bind("Ctrl+P", "view.commandPalette");  // Quick open could be separate
    
    keymap->Bind("Ctrl+Q", "app.quit");
    
    // Editor-specific bindings (only active when editor has focus)
    keymap->Bind("Ctrl+/", "editor.toggleComment", InputContext::Editor);
    keymap->Bind("Ctrl+D", "editor.duplicateLine", InputContext::Editor);
    keymap->Bind("Ctrl+Shift+K", "editor.deleteLine", InputContext::Editor);
    keymap->Bind("Alt+Up", "editor.moveLineUp", InputContext::Editor);
    keymap->Bind("Alt+Down", "editor.moveLineDown", InputContext::Editor);
    keymap->Bind("Ctrl+F", "editor.find", InputContext::Editor);
    keymap->Bind("Ctrl+H", "editor.replace", InputContext::Editor);
    keymap->Bind("Ctrl+G", "editor.gotoLine", InputContext::Editor);
    
    Logger::Info("Default keybindings initialized");
}

} // namespace sol
