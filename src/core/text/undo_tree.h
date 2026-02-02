#pragma once

#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <functional>

namespace sol {

// Represents a single edit operation
struct EditOperation {
    enum class Type { Insert, Delete, Replace };
    
    Type type;
    size_t position;
    std::string oldText;  // For delete/replace
    std::string newText;  // For insert/replace
    
    // Cursor position before and after the edit
    size_t cursorBefore;
    size_t cursorAfter;
    
    static EditOperation Insert(size_t pos, const std::string& text, size_t cursorBefore) {
        return { Type::Insert, pos, "", text, cursorBefore, pos + text.length() };
    }
    
    static EditOperation Delete(size_t pos, const std::string& deleted, size_t cursorBefore) {
        return { Type::Delete, pos, deleted, "", cursorBefore, pos };
    }
    
    static EditOperation Replace(size_t pos, const std::string& oldText, const std::string& newText, size_t cursorBefore) {
        return { Type::Replace, pos, oldText, newText, cursorBefore, pos + newText.length() };
    }
};

// Node in the undo tree
struct UndoNode {
    EditOperation operation;
    std::weak_ptr<UndoNode> parent;
    std::vector<std::shared_ptr<UndoNode>> children;
    size_t timestamp;  // For ordering branches
    
    UndoNode(const EditOperation& op, size_t ts) : operation(op), timestamp(ts) {}
};

// Undo tree with branching history (like vim's undotree)
class UndoTree {
public:
    UndoTree();
    
    // Record an edit operation
    void Push(const EditOperation& op);
    
    // Undo the last operation, returns the operation to reverse (if any)
    std::optional<EditOperation> Undo();
    
    // Redo a previously undone operation
    std::optional<EditOperation> Redo();
    
    // Navigate to a specific branch (for branch switching)
    bool GotoBranch(size_t branchIndex);
    
    // Get available redo branches at current position
    size_t GetRedoBranchCount() const;
    
    // Check states
    bool CanUndo() const;
    bool CanRedo() const;
    
    // Clear history
    void Clear();
    
    // Get current position info
    size_t GetUndoCount() const;
    size_t GetRedoCount() const;
    
    // Mark current state as saved (for tracking modified state)
    void MarkSaved();
    bool IsModified() const;
    
    // Merge consecutive similar operations (for grouping typing)
    void SetMergeWindow(size_t milliseconds) { m_MergeWindow = milliseconds; }
    
private:
    std::shared_ptr<UndoNode> m_Root;
    std::shared_ptr<UndoNode> m_Current;  // Current position in tree
    size_t m_Timestamp = 0;
    size_t m_SavedTimestamp = 0;
    size_t m_MergeWindow = 500;  // Merge operations within 500ms
    
    // Track current redo path when navigating branches
    size_t m_CurrentRedoBranch = 0;
    
    bool ShouldMerge(const EditOperation& newOp) const;
};

// Groups multiple operations into a single undo unit
class UndoGroup {
public:
    explicit UndoGroup(UndoTree& tree);
    ~UndoGroup();
    
    void Add(const EditOperation& op);
    void Commit();
    void Cancel();
    
private:
    UndoTree& m_Tree;
    std::vector<EditOperation> m_Operations;
    bool m_Committed = false;
};

} // namespace sol
