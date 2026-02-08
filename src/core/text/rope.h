#pragma once

#include <string>
#include <string_view>
#include <memory>
#include <vector>
#include <functional>

namespace sol {

// Rope data structure for efficient text editing
// Similar to what Neovim uses - O(log n) insertions/deletions
class Rope {
public:
    Rope();
    explicit Rope(std::string_view text);
    Rope(const Rope& other);
    Rope(Rope&& other) noexcept;
    Rope& operator=(const Rope& other);
    Rope& operator=(Rope&& other) noexcept;
    ~Rope();
    
    // Basic operations
    void Insert(size_t pos, std::string_view text);
    void Delete(size_t pos, size_t len);
    void Replace(size_t pos, size_t len, std::string_view text);
    
    // Access
    char At(size_t pos) const;
    std::string Substring(size_t pos, size_t len) const;
    std::string ToString() const;
    size_t Length() const;
    bool Empty() const { return Length() == 0; }
    
    // Line operations (for editor)
    size_t LineCount() const;
    size_t LineStart(size_t lineNum) const;  // 0-indexed
    size_t LineEnd(size_t lineNum) const;
    size_t LineLength(size_t lineNum) const;
    std::string Line(size_t lineNum) const;
    std::string_view LineView(size_t lineNum) const;
    std::pair<size_t, size_t> PosToLineCol(size_t pos) const;  // Returns (line, col)
    size_t LineColToPos(size_t line, size_t col) const;
    
    // Large file optimization: prepare visible line range for efficient rendering
    void PrepareVisibleRange(size_t firstLine, size_t lastLine);
    void SetVisibleBuffer(const std::string& buffer, size_t firstLine, size_t lastLine, size_t startByte);
    
    // Helper to get view into visible buffer using global offsets
    std::string_view GetVisibleBufferView(size_t globalStart, size_t len) const;
    
    bool IsLargeFile() const { return Length() > LARGE_FILE_THRESHOLD; }

    // Iteration
    using CharCallback = std::function<bool(size_t pos, char c)>;
    void ForEach(CharCallback callback) const;
    void ForEachInRange(size_t start, size_t end, CharCallback callback) const;
    
    // For tree-sitter integration
    struct EditInfo {
        size_t startByte;
        size_t oldEndByte;
        size_t newEndByte;
        std::pair<size_t, size_t> startPoint;   // (row, col)
        std::pair<size_t, size_t> oldEndPoint;
        std::pair<size_t, size_t> newEndPoint;
    };
    EditInfo GetLastEdit() const { return m_LastEdit; }
    
    // For ImGui compatibility - returns a contiguous buffer (cached)
    const char* CStr() const;
    char* Data();
    size_t Capacity() const;
    void SyncFromBuffer();  // Call after ImGui modifies the buffer
    
private:
    static constexpr size_t LEAF_SIZE = 512;  // Max chars per leaf node
    
    struct Node {
        virtual ~Node() = default;
        virtual size_t Length() const = 0;
        virtual size_t LineCount() const = 0;
        virtual char At(size_t pos) const = 0;
        virtual void CollectString(std::string& out) const = 0;
        virtual std::unique_ptr<Node> Clone() const = 0;
        virtual size_t Depth() const = 0;
        
        // Tree-based line operations (O(log N))
        virtual size_t FindLineStart(size_t lineNum, size_t& linesSkipped) const = 0;
        virtual void CollectRange(std::string& out, size_t start, size_t end) const = 0;
    };
    
    struct Leaf : Node {
        std::string text;
        size_t newlineCount = 0;
        
        explicit Leaf(std::string_view t);
        size_t Length() const override { return text.length(); }
        size_t LineCount() const override { return newlineCount; }
        char At(size_t pos) const override { return text[pos]; }
        void CollectString(std::string& out) const override { out += text; }
        std::unique_ptr<Node> Clone() const override;
        size_t Depth() const override { return 1; }
        
        size_t FindLineStart(size_t lineNum, size_t& linesSkipped) const override;
        void CollectRange(std::string& out, size_t start, size_t end) const override;
        
        void UpdateNewlineCount();
    };
    
    struct Branch : Node {
        std::unique_ptr<Node> left;
        std::unique_ptr<Node> right;
        size_t leftLen = 0;
        size_t leftLines = 0;
        
        Branch(std::unique_ptr<Node> l, std::unique_ptr<Node> r);
        size_t Length() const override;
        size_t LineCount() const override;
        char At(size_t pos) const override;
        void CollectString(std::string& out) const override;
        std::unique_ptr<Node> Clone() const override;
        size_t Depth() const override;
        
        size_t FindLineStart(size_t lineNum, size_t& linesSkipped) const override;
        void CollectRange(std::string& out, size_t start, size_t end) const override;
        
        void UpdateMetrics();
    };
    
    std::unique_ptr<Node> m_Root;
    EditInfo m_LastEdit{};
    
    // Cache for contiguous text access — updated incrementally on edits
    mutable std::string m_Cache;
    mutable bool m_CacheDirty = true;
    
    // Line starts cache for O(1) line access — updated incrementally on edits
    mutable std::vector<size_t> m_LineStarts;
    mutable bool m_LineStartsDirty = true;
    
    // Large file optimization:
    // Instead of caching the whole file string, we only cache the visible range + padding
    mutable std::string m_VisibleRangeBuf;
    mutable size_t m_VisibleFirstLine = 0;
    mutable size_t m_VisibleLastLine = 0;
    mutable size_t m_VisibleRangeStart = 0;
    static constexpr size_t LARGE_FILE_THRESHOLD = 1024 * 1024; // 1MB

    // Internal helpers
    static std::unique_ptr<Node> BuildFromText(std::string_view text);
    static std::pair<std::unique_ptr<Node>, std::unique_ptr<Node>> Split(std::unique_ptr<Node> node, size_t pos);
    static std::unique_ptr<Node> Concat(std::unique_ptr<Node> left, std::unique_ptr<Node> right);
    static std::unique_ptr<Node> Rebalance(std::unique_ptr<Node> node);
    
    size_t FindLineStartDirect(size_t lineNum) const;
    std::string SubstringDirect(size_t pos, size_t len) const;
    
    void InvalidateCache() { m_CacheDirty = true; m_LineStartsDirty = true; }
    void EnsureCache() const;
    void EnsureLineStarts() const;
    
    // Incremental cache/line-starts patching (avoids full rebuild)
    void PatchCacheInsert(size_t pos, std::string_view text);
    void PatchCacheDelete(size_t pos, size_t len);
    
    size_t CountNewlines(std::string_view text) const;
};

} // namespace sol
