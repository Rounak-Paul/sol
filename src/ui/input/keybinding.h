#pragma once

#include <imgui.h>
#include <string>
#include <vector>
#include <optional>
#include <cstdint>

namespace sol {

// Modifier flags
enum class Modifier : uint8_t {
    None  = 0,
    Ctrl  = 1 << 0,
    Shift = 1 << 1,
    Alt   = 1 << 2,
    Super = 1 << 3,  // Cmd on macOS, Win on Windows
};

// Get the configured leader key from settings
ImGuiKey GetLeaderKey();

inline Modifier operator|(Modifier a, Modifier b) {
    return static_cast<Modifier>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

inline Modifier operator&(Modifier a, Modifier b) {
    return static_cast<Modifier>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}

inline bool HasModifier(Modifier mods, Modifier flag) {
    return (static_cast<uint8_t>(mods) & static_cast<uint8_t>(flag)) != 0;
}

// A single key chord (key + modifiers)
struct KeyChord {
    ImGuiKey key = ImGuiKey_None;
    Modifier mods = Modifier::None;
    
    KeyChord() = default;
    KeyChord(ImGuiKey k, Modifier m = Modifier::None) : key(k), mods(m) {}
    
    bool operator==(const KeyChord& other) const {
        return key == other.key && mods == other.mods;
    }
    
    bool operator!=(const KeyChord& other) const {
        return !(*this == other);
    }
    
    // Check if this chord matches current input state
    bool IsPressed() const;
    
    // Convert to human-readable string (e.g., "Ctrl+S")
    std::string ToString() const;
    
    // Parse from string (e.g., "Ctrl+S")
    static std::optional<KeyChord> Parse(const std::string& str);
    
    // Get current modifier state from ImGui
    static Modifier GetCurrentModifiers();
};

// A sequence of key chords (e.g., "Ctrl+X Ctrl+S")
class KeySequence {
public:
    KeySequence() = default;
    KeySequence(std::initializer_list<KeyChord> chords) : m_Chords(chords) {}
    explicit KeySequence(const KeyChord& chord) : m_Chords{chord} {}
    
    // Add a chord to the sequence
    void Add(const KeyChord& chord) { m_Chords.push_back(chord); }
    
    // Clear the sequence
    void Clear() { m_Chords.clear(); }
    
    // Check if empty
    bool Empty() const { return m_Chords.empty(); }
    
    // Get number of chords
    size_t Size() const { return m_Chords.size(); }
    
    // Access chords
    const KeyChord& operator[](size_t i) const { return m_Chords[i]; }
    const std::vector<KeyChord>& GetChords() const { return m_Chords; }
    
    bool operator==(const KeySequence& other) const {
        return m_Chords == other.m_Chords;
    }
    
    // Check if this sequence is a prefix of another
    bool IsPrefixOf(const KeySequence& other) const;
    
    // Convert to human-readable string (e.g., "Ctrl+X Ctrl+S")
    std::string ToString() const;
    
    // Parse from string (e.g., "Ctrl+X Ctrl+S")
    static std::optional<KeySequence> Parse(const std::string& str);
    
private:
    std::vector<KeyChord> m_Chords;
};

// Hash for KeyChord (for use in unordered containers)
struct KeyChordHash {
    size_t operator()(const KeyChord& chord) const {
        return std::hash<int>()(static_cast<int>(chord.key)) ^ 
               (std::hash<uint8_t>()(static_cast<uint8_t>(chord.mods)) << 16);
    }
};

// Hash for KeySequence
struct KeySequenceHash {
    size_t operator()(const KeySequence& seq) const {
        size_t hash = 0;
        KeyChordHash chordHash;
        for (const auto& chord : seq.GetChords()) {
            hash ^= chordHash(chord) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
        }
        return hash;
    }
};

// Input context - where focus currently is
enum class InputContext {
    Global,      // Always available
    Editor,      // Text editor has focus
    Terminal,    // Terminal has focus
    Explorer,    // File explorer has focus
    CommandPalette,  // Command palette is open
    Search,      // Search/find is active
    Menu,        // Menu is open
};

// Convert InputContext to string
const char* InputContextToString(InputContext ctx);

// Key sequence matcher for tracking multi-chord sequences
class KeySequenceMatcher {
public:
    // Reset the matcher state
    void Reset();
    
    // Feed a key chord, returns match result
    enum class MatchResult {
        NoMatch,      // No binding matches current sequence
        PartialMatch, // Current sequence is prefix of some binding
        FullMatch,    // Found a complete match
    };
    
    // Process a key chord
    MatchResult ProcessChord(const KeyChord& chord);
    
    // Get the current partial sequence
    const KeySequence& GetCurrentSequence() const { return m_CurrentSequence; }
    
    // Set the bindings to match against (called when keymaps change)
    void SetBindings(const std::vector<KeySequence>& bindings);
    
    // Get time since last chord (for UI display)
    float GetTimeSinceLastChord() const;
    
private:
    KeySequence m_CurrentSequence;
    std::vector<KeySequence> m_Bindings;
    double m_LastChordTime = 0.0;
};

} // namespace sol
