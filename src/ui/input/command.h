#pragma once

#include "keybinding.h"
#include "ui/editor_settings.h"
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <any>

namespace sol {

// Command arguments - allows passing context to commands
using CommandArgs = std::unordered_map<std::string, std::any>;

// Command execution result
struct CommandResult {
    bool success = true;
    std::string error;
    
    static CommandResult Success() { return {true, ""}; }
    static CommandResult Failure(const std::string& err) { return {false, err}; }
};

// Command callback signature
using CommandCallback = std::function<CommandResult(const CommandArgs&)>;

// A command that can be executed
class Command {
public:
    Command(const std::string& id, const std::string& title, CommandCallback callback);
    
    const std::string& GetId() const { return m_Id; }
    const std::string& GetTitle() const { return m_Title; }
    const std::string& GetDescription() const { return m_Description; }
    const std::string& GetCategory() const { return m_Category; }
    
    Command& SetDescription(const std::string& desc) { m_Description = desc; return *this; }
    Command& SetCategory(const std::string& cat) { m_Category = cat; return *this; }
    
    // Execute the command
    CommandResult Execute(const CommandArgs& args = {}) const;
    
    // Check if command is enabled (can add conditions later)
    bool IsEnabled() const { return m_Enabled; }
    void SetEnabled(bool enabled) { m_Enabled = enabled; }
    
private:
    std::string m_Id;           // Unique identifier (e.g., "editor.save")
    std::string m_Title;        // Display name (e.g., "Save File")
    std::string m_Description;  // Longer description
    std::string m_Category;     // Category for grouping (e.g., "File", "Edit")
    CommandCallback m_Callback;
    bool m_Enabled = true;
};

// Registry of all commands
class CommandRegistry {
public:
    static CommandRegistry& GetInstance();
    
    // Register a command
    void Register(std::shared_ptr<Command> command);
    void Register(const std::string& id, const std::string& title, CommandCallback callback);
    
    // Unregister a command
    void Unregister(const std::string& id);
    
    // Get a command by ID
    std::shared_ptr<Command> Get(const std::string& id) const;
    
    // Execute a command
    CommandResult Execute(const std::string& id, const CommandArgs& args = {});
    
    // Get all commands
    std::vector<std::shared_ptr<Command>> GetAll() const;
    
    // Get commands by category
    std::vector<std::shared_ptr<Command>> GetByCategory(const std::string& category) const;
    
    // Search commands by title/id
    std::vector<std::shared_ptr<Command>> Search(const std::string& query) const;
    
private:
    CommandRegistry() = default;
    std::unordered_map<std::string, std::shared_ptr<Command>> m_Commands;
};

// A keybinding associates a key sequence with a command
struct Keybinding {
    KeySequence keys;
    std::string commandId;
    InputContext context = InputContext::Global;
    std::string when;  // Optional condition expression (for future use)
    
    Keybinding() = default;
    Keybinding(const KeySequence& k, const std::string& cmd, InputContext ctx = InputContext::Global)
        : keys(k), commandId(cmd), context(ctx) {}
};

// Keymap - a collection of keybindings
class Keymap {
public:
    Keymap(const std::string& name = "default");
    
    const std::string& GetName() const { return m_Name; }
    
    // Add a binding
    void Bind(const KeySequence& keys, const std::string& commandId, InputContext context = InputContext::Global);
    void Bind(const std::string& keys, const std::string& commandId, InputContext context = InputContext::Global);
    
    // Remove a binding
    void Unbind(const KeySequence& keys, InputContext context = InputContext::Global);
    void Unbind(const std::string& keys, InputContext context = InputContext::Global);
    
    // Find binding for a key sequence and context
    const Keybinding* FindBinding(const KeySequence& keys, InputContext context) const;
    
    // Get all bindings for a context
    std::vector<const Keybinding*> GetBindings(InputContext context) const;
    
    // Get all bindings
    const std::vector<Keybinding>& GetAllBindings() const { return m_Bindings; }
    
    // Get all bindings for a command
    std::vector<const Keybinding*> GetBindingsForCommand(const std::string& commandId) const;
    
    // Get all key sequences (for matcher setup)
    std::vector<KeySequence> GetAllKeySequences(InputContext context) const;
    
    // Clear all bindings
    void Clear();
    
private:
    std::string m_Name;
    std::vector<Keybinding> m_Bindings;
};

// Input system - manages keymaps and processes input
class InputSystem {
public:
    static InputSystem& GetInstance();
    
    // Process a key chord - returns true if a command was executed or input was consumed
    bool ProcessKey(const KeyChord& chord, InputContext context);
    
    // Get current partial sequence (for status display)
    const KeySequence& GetPendingSequence() const;
    bool HasPendingSequence() const;
    
    // Reset pending sequence
    void ResetPendingSequence();
    
    // Get bindings that match the current pending sequence prefix
    // Returns pairs of (remaining keys after prefix, command id)
    std::vector<std::pair<std::string, std::string>> GetMatchingBindings() const;
    
    // Modal editing state
    EditorInputMode GetInputMode() const { return m_InputMode; }
    void SetInputMode(EditorInputMode mode) { m_InputMode = mode; }
    void SwitchToCommandMode() { m_InputMode = EditorInputMode::Command; ResetPendingSequence(); }
    void SwitchToInsertMode() { m_InputMode = EditorInputMode::Insert; ResetPendingSequence(); }
    
    // Keymap management
    void SetActiveKeymap(const std::string& name);
    const std::string& GetActiveKeymapName() const { return m_ActiveKeymapName; }
    
    Keymap* GetKeymap(const std::string& name);
    Keymap* GetActiveKeymap();
    
    void AddKeymap(std::unique_ptr<Keymap> keymap);
    void RemoveKeymap(const std::string& name);
    
    // Set current input context
    void SetContext(InputContext ctx) { m_CurrentContext = ctx; }
    InputContext GetContext() const { return m_CurrentContext; }
    
    // Initialize with default bindings
    void SetupDefaultBindings();
    
    // Pending navigation (for command mode nav keys)
    void SetPendingNavigation(ImGuiKey direction) { m_PendingNav = direction; }
    ImGuiKey ConsumePendingNavigation() { 
        ImGuiKey nav = m_PendingNav; 
        m_PendingNav = ImGuiKey_None; 
        return nav; 
    }
    
private:
    InputSystem();
    
    std::unordered_map<std::string, std::unique_ptr<Keymap>> m_Keymaps;
    std::string m_ActiveKeymapName = "default";
    Keymap* m_ActiveKeymap = nullptr;
    
    InputContext m_CurrentContext = InputContext::Global;
    KeySequenceMatcher m_Matcher;
    EditorInputMode m_InputMode = EditorInputMode::Insert;  // Modal editing state
    ImGuiKey m_PendingNav = ImGuiKey_None;  // Pending navigation direction
};

// Convenience macros for registering commands
#define SOL_REGISTER_COMMAND(id, title, callback) \
    CommandRegistry::GetInstance().Register(id, title, callback)

#define SOL_BIND_KEY(keys, command) \
    InputSystem::GetInstance().GetActiveKeymap()->Bind(keys, command)

#define SOL_BIND_KEY_CONTEXT(keys, command, context) \
    InputSystem::GetInstance().GetActiveKeymap()->Bind(keys, command, context)

} // namespace sol
