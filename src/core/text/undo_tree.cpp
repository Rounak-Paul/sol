#include "undo_tree.h"
#include <algorithm>

namespace sol {

UndoTree::UndoTree() {
    // Create a dummy root node
    m_Root = std::make_shared<UndoNode>(
        EditOperation{ EditOperation::Type::Insert, 0, "", "", 0, 0 },
        0
    );
    m_Current = m_Root;
}

void UndoTree::Push(const EditOperation& op) {
    ++m_Timestamp;
    
    // Check if we should merge with the previous operation
    if (ShouldMerge(op) && !m_Current->children.empty()) {
        // Merge with the last operation
        auto& lastChild = m_Current->children.back();
        if (lastChild->operation.type == EditOperation::Type::Insert &&
            op.type == EditOperation::Type::Insert) {
            // Merge consecutive inserts
            lastChild->operation.newText += op.newText;
            lastChild->operation.cursorAfter = op.cursorAfter;
            lastChild->timestamp = m_Timestamp;
            return;
        }
    }
    
    auto newNode = std::make_shared<UndoNode>(op, m_Timestamp);
    newNode->parent = m_Current;
    m_Current->children.push_back(newNode);
    m_Current = newNode;
    m_CurrentRedoBranch = 0;
}

std::optional<EditOperation> UndoTree::Undo() {
    if (!CanUndo()) {
        return std::nullopt;
    }
    
    EditOperation op = m_Current->operation;
    
    // Find which child index we are in the parent
    auto parent = m_Current->parent.lock();
    if (parent) {
        for (size_t i = 0; i < parent->children.size(); ++i) {
            if (parent->children[i] == m_Current) {
                m_CurrentRedoBranch = i;
                break;
            }
        }
    }
    
    m_Current = parent;
    return op;
}

std::optional<EditOperation> UndoTree::Redo() {
    if (!CanRedo()) {
        return std::nullopt;
    }
    
    // Navigate to the current redo branch
    m_Current = m_Current->children[m_CurrentRedoBranch];
    return m_Current->operation;
}

bool UndoTree::GotoBranch(size_t branchIndex) {
    if (branchIndex >= m_Current->children.size()) {
        return false;
    }
    m_CurrentRedoBranch = branchIndex;
    return true;
}

size_t UndoTree::GetRedoBranchCount() const {
    return m_Current->children.size();
}

bool UndoTree::CanUndo() const {
    return m_Current != m_Root && m_Current->parent.lock() != nullptr;
}

bool UndoTree::CanRedo() const {
    return !m_Current->children.empty();
}

void UndoTree::Clear() {
    m_Root = std::make_shared<UndoNode>(
        EditOperation{ EditOperation::Type::Insert, 0, "", "", 0, 0 },
        0
    );
    m_Current = m_Root;
    m_Timestamp = 0;
    m_SavedTimestamp = 0;
    m_CurrentRedoBranch = 0;
}

size_t UndoTree::GetUndoCount() const {
    size_t count = 0;
    auto node = m_Current;
    while (node && node != m_Root) {
        ++count;
        node = node->parent.lock();
    }
    return count;
}

size_t UndoTree::GetRedoCount() const {
    size_t count = 0;
    auto node = m_Current;
    while (!node->children.empty()) {
        ++count;
        node = node->children[0];  // Count along main branch
    }
    return count;
}

void UndoTree::MarkSaved() {
    m_SavedTimestamp = m_Current ? m_Current->timestamp : 0;
}

bool UndoTree::IsModified() const {
    return m_Current && m_Current->timestamp != m_SavedTimestamp;
}

bool UndoTree::ShouldMerge(const EditOperation& newOp) const {
    if (m_Current->children.empty()) return false;
    
    const auto& lastChild = m_Current->children.back();
    
    // Only merge consecutive character inserts
    if (lastChild->operation.type != EditOperation::Type::Insert ||
        newOp.type != EditOperation::Type::Insert) {
        return false;
    }
    
    // Check if insert is right after the previous insert
    if (newOp.position != lastChild->operation.position + lastChild->operation.newText.length()) {
        return false;
    }
    
    // Don't merge if it includes newlines
    if (newOp.newText.find('\n') != std::string::npos) {
        return false;
    }
    
    // Check time window (simplified - would need actual time tracking)
    return m_Timestamp - lastChild->timestamp < 10;  // Merge within 10 operations
}

// UndoGroup implementation
UndoGroup::UndoGroup(UndoTree& tree) : m_Tree(tree) {}

UndoGroup::~UndoGroup() {
    if (!m_Committed && !m_Operations.empty()) {
        Commit();
    }
}

void UndoGroup::Add(const EditOperation& op) {
    m_Operations.push_back(op);
}

void UndoGroup::Commit() {
    if (m_Committed) return;
    
    // Combine all operations into one compound operation
    if (!m_Operations.empty()) {
        // For now, push each operation individually
        // A proper implementation would create a compound operation
        for (const auto& op : m_Operations) {
            m_Tree.Push(op);
        }
    }
    m_Committed = true;
}

void UndoGroup::Cancel() {
    m_Operations.clear();
    m_Committed = true;
}

} // namespace sol
