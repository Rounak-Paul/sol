#pragma once

#include "rope.h"
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <functional>
#include <filesystem>
#include <fstream>

// Forward declare tree-sitter types
extern "C" {
    typedef struct TSParser TSParser;
    typedef struct TSTree TSTree;
    typedef struct TSLanguage TSLanguage;
    struct TSPoint;
}

namespace sol {

// Syntax highlight token
struct SyntaxToken {
    size_t startByte;
    size_t endByte;
    size_t startRow;
    size_t startCol;
    size_t endRow;
    size_t endCol;
    const char* type;       // Tree-sitter node type
    uint16_t highlightId;   // Mapped highlight group
    uint16_t depth;         // Nesting depth for rainbow brackets
};

// Foldable range (for collapsible scopes)
struct FoldRange {
    size_t startLine;   // Line where fold starts (has the fold indicator)
    size_t endLine;     // Line where fold ends (inclusive)
    const char* type;   // Node type (for potential icons/hints)
};

// Highlight groups (similar to Neovim)
enum class HighlightGroup : uint16_t {
    None = 0,
    Keyword,
    Type,
    Function,
    Variable,
    String,
    Number,
    Comment,
    Operator,
    Punctuation,
    Namespace,
    Macro,
    Constant,
    Error,
    Count
};

// Language definition
struct Language {
    std::string name;
    std::vector<std::string> extensions;
    const TSLanguage* tsLanguage;
    
    // Highlight mappings (node type -> highlight group)
    std::vector<std::pair<std::string, HighlightGroup>> highlightMappings;
};

// TextBuffer - combines Rope with tree-sitter for nvim-like editing
class TextBuffer {
public:
    TextBuffer();
    explicit TextBuffer(std::string_view text);
    ~TextBuffer();
    
    TextBuffer(const TextBuffer&) = delete;
    TextBuffer& operator=(const TextBuffer&) = delete;
    TextBuffer(TextBuffer&&) noexcept;
    TextBuffer& operator=(TextBuffer&&) noexcept;
    
    // Text operations (delegated to Rope)
    void Insert(size_t pos, std::string_view text);
    void Delete(size_t pos, size_t len);
    void Replace(size_t pos, size_t len, std::string_view text);
    
    char At(size_t pos) const { return m_Rope.At(pos); }
    std::string Substring(size_t pos, size_t len) const { return m_Rope.Substring(pos, len); }
    std::string ToString() const { return m_Rope.ToString(); }
    size_t Length() const { return m_Rope.Length(); }
    bool Empty() const { return m_Rope.Empty(); }
    
    // Line operations
    size_t LineCount() const { 
        if (m_IsDiskBuffered) return m_DiskLineStarts.size();
        return m_Rope.LineCount(); 
    }
    std::string Line(size_t lineNum) const;
    std::string_view LineView(size_t lineNum) const;
    
    size_t LineStart(size_t lineNum) const { 
        if (m_IsDiskBuffered) {
             if (lineNum >= m_DiskLineStarts.size()) return 0;
             return m_DiskLineStarts[lineNum];
        }
        return m_Rope.LineStart(lineNum); 
    }
    size_t LineEnd(size_t lineNum) const { 
        if (m_IsDiskBuffered) {
            if (lineNum + 1 < m_DiskLineStarts.size()) {
                 size_t next = m_DiskLineStarts[lineNum+1];
                 return next > 0 ? next - 1 : 0;
            }
            return m_DiskFileSize;
        }
        return m_Rope.LineEnd(lineNum); 
    }
    std::pair<size_t, size_t> PosToLineCol(size_t pos) const { 
         if (m_IsDiskBuffered) {
              // Binary search m_DiskLineStarts
              auto it = std::upper_bound(m_DiskLineStarts.begin(), m_DiskLineStarts.end(), pos);
              size_t line = 0;
              if (it != m_DiskLineStarts.begin()) {
                  line = static_cast<size_t>(it - m_DiskLineStarts.begin()) - 1;
              }
              return {line, pos - m_DiskLineStarts[line]};
         }
         return m_Rope.PosToLineCol(pos); 
    }
    size_t LineColToPos(size_t line, size_t col) const { return m_Rope.LineColToPos(line, col); }
    void PrepareVisibleRange(size_t firstLine, size_t lastLine);
    
    // Language/syntax
    void SetLanguage(const Language* lang);
    const Language* GetLanguage() const { return m_Language; }
    bool HasLanguage() const { return m_Language != nullptr; }
    
    // Syntax highlighting
    void Parse();
    void ParseIncremental();
    bool IsParsed() const { return m_Tree != nullptr; }
    
    // Get syntax tokens for a line range (for rendering)
    std::vector<SyntaxToken> GetSyntaxTokens(size_t startLine, size_t endLine) const;
    HighlightGroup GetHighlightAt(size_t pos) const;

    // Advanced syntax features
    std::pair<size_t, size_t> GetScopeRange(size_t pos) const;
    size_t GetMatchingBracket(size_t pos) const; // Returns pos, or -1 (SIZE_MAX) if none
    
    // Code folding - returns all foldable ranges sorted by startLine
    std::vector<FoldRange> GetFoldRanges() const;
    
    // Built-in completion
    std::vector<std::string> GetWordCompletions(const std::string& prefix, size_t cursorPos = 0) const;

    // For ImGui compatibility
    const char* CStr() const { return m_Rope.CStr(); }
    char* Data() { return m_Rope.Data(); }
    size_t Capacity() const { return m_Rope.Capacity(); }
    void SyncFromBuffer() { m_Rope.SyncFromBuffer(); ParseIncremental(); }
    
    // File association
    void SetFilePath(const std::filesystem::path& path) { m_FilePath = path; }
    const std::filesystem::path& GetFilePath() const { return m_FilePath; }
    
    // Streaming support
    void EnableDiskBuffering(const std::filesystem::path& path);
    bool IsDiskBuffered() const { return m_IsDiskBuffered; }

    // Modified state
    bool IsModified() const { return m_Modified; }

    void SetModified(bool modified) { m_Modified = modified; }
    void MarkModified() { m_Modified = true; }
    
    // Undo/Redo support (future)
    struct EditRecord {
        size_t pos;
        std::string oldText;
        std::string newText;
    };
    
private:
    Rope m_Rope;
    // Disk buffering members
    bool m_IsDiskBuffered = false;
    mutable std::ifstream m_FileStream; // Unused but kept for ABI compat if needed? No, I can remove users.
    mutable std::vector<size_t> m_DiskLineStarts;
    size_t m_DiskFileSize = 0;
    void IndexDiskFile();
    
    TSParser* m_Parser = nullptr;
    TSTree* m_Tree = nullptr;
    const Language* m_Language = nullptr;
    std::filesystem::path m_FilePath;
    bool m_Modified = false;
    
    // Tree-sitter read callback
    static const char* TSRead(void* payload, uint32_t byteOffset, TSPoint position, uint32_t* bytesRead);
    
    void ReleaseTree();
    HighlightGroup MapNodeTypeToHighlight(const char* nodeType) const;
};

// Language registry
class LanguageRegistry {
public:
    static LanguageRegistry& GetInstance();
    
    LanguageRegistry(const LanguageRegistry&) = delete;
    LanguageRegistry& operator=(const LanguageRegistry&) = delete;
    
    void RegisterLanguage(std::unique_ptr<Language> lang);
    const Language* GetLanguageForFile(const std::filesystem::path& path) const;
    const Language* GetLanguageByName(const std::string& name) const;
    const std::vector<std::unique_ptr<Language>>& GetLanguages() const { return m_Languages; }
    
    // Initialize built-in languages
    void InitializeBuiltins();
    
private:
    LanguageRegistry() = default;
    ~LanguageRegistry() = default;
    
    std::vector<std::unique_ptr<Language>> m_Languages;
};

} // namespace sol
