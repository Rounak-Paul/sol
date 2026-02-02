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
    std::pair<size_t, size_t> PosToLineCol(size_t pos) const;  // Returns (line, col)
    size_t LineColToPos(size_t line, size_t col) const;
    
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
    const char* CStr();
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
        void UpdateMetrics();
    };
    
    std::unique_ptr<Node> m_Root;
    EditInfo m_LastEdit{};
    
    // Cache for ImGui compatibility
    mutable std::string m_Cache;
    mutable bool m_CacheDirty = true;
    
    // Internal helpers
    static std::unique_ptr<Node> BuildFromText(std::string_view text);
    static std::pair<std::unique_ptr<Node>, std::unique_ptr<Node>> Split(std::unique_ptr<Node> node, size_t pos);
    static std::unique_ptr<Node> Concat(std::unique_ptr<Node> left, std::unique_ptr<Node> right);
    static std::unique_ptr<Node> Rebalance(std::unique_ptr<Node> node);
    
    void InvalidateCache() { m_CacheDirty = true; }
    void EnsureCache() const;
    
    size_t CountNewlines(std::string_view text) const;
};

} // namespace sol
