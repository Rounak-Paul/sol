#include "rope.h"
#include <algorithm>
#include <cassert>
#include <cstring>

namespace sol {

// Leaf implementation
Rope::Leaf::Leaf(std::string_view t) : text(t) {
    UpdateNewlineCount();
}

void Rope::Leaf::UpdateNewlineCount() {
    newlineCount = std::count(text.begin(), text.end(), '\n');
}

std::unique_ptr<Rope::Node> Rope::Leaf::Clone() const {
    return std::make_unique<Leaf>(text);
}

// Branch implementation
Rope::Branch::Branch(std::unique_ptr<Node> l, std::unique_ptr<Node> r)
    : left(std::move(l)), right(std::move(r)) {
    UpdateMetrics();
}

void Rope::Branch::UpdateMetrics() {
    leftLen = left ? left->Length() : 0;
    leftLines = left ? left->LineCount() : 0;
}

size_t Rope::Branch::Length() const {
    return leftLen + (right ? right->Length() : 0);
}

size_t Rope::Branch::LineCount() const {
    return leftLines + (right ? right->LineCount() : 0);
}

char Rope::Branch::At(size_t pos) const {
    if (pos < leftLen) {
        return left->At(pos);
    }
    return right->At(pos - leftLen);
}

void Rope::Branch::CollectString(std::string& out) const {
    if (left) left->CollectString(out);
    if (right) right->CollectString(out);
}

std::unique_ptr<Rope::Node> Rope::Branch::Clone() const {
    return std::make_unique<Branch>(
        left ? left->Clone() : nullptr,
        right ? right->Clone() : nullptr
    );
}

size_t Rope::Branch::Depth() const {
    size_t ld = left ? left->Depth() : 0;
    size_t rd = right ? right->Depth() : 0;
    return 1 + std::max(ld, rd);
}

// Rope implementation
Rope::Rope() : m_Root(std::make_unique<Leaf>("")) {}

Rope::Rope(std::string_view text) {
    if (text.empty()) {
        m_Root = std::make_unique<Leaf>("");
    } else {
        m_Root = BuildFromText(text);
    }
}

Rope::Rope(const Rope& other) {
    if (other.m_Root) {
        m_Root = other.m_Root->Clone();
    }
    m_LastEdit = other.m_LastEdit;
}

Rope::Rope(Rope&& other) noexcept
    : m_Root(std::move(other.m_Root))
    , m_LastEdit(other.m_LastEdit)
    , m_Cache(std::move(other.m_Cache))
    , m_CacheDirty(other.m_CacheDirty) {
}

Rope& Rope::operator=(const Rope& other) {
    if (this != &other) {
        m_Root = other.m_Root ? other.m_Root->Clone() : nullptr;
        m_LastEdit = other.m_LastEdit;
        m_CacheDirty = true;
    }
    return *this;
}

Rope& Rope::operator=(Rope&& other) noexcept {
    if (this != &other) {
        m_Root = std::move(other.m_Root);
        m_LastEdit = other.m_LastEdit;
        m_Cache = std::move(other.m_Cache);
        m_CacheDirty = other.m_CacheDirty;
    }
    return *this;
}

Rope::~Rope() = default;

std::unique_ptr<Rope::Node> Rope::BuildFromText(std::string_view text) {
    if (text.length() <= LEAF_SIZE) {
        return std::make_unique<Leaf>(text);
    }
    
    size_t mid = text.length() / 2;
    auto left = BuildFromText(text.substr(0, mid));
    auto right = BuildFromText(text.substr(mid));
    return std::make_unique<Branch>(std::move(left), std::move(right));
}

std::pair<std::unique_ptr<Rope::Node>, std::unique_ptr<Rope::Node>>
Rope::Split(std::unique_ptr<Node> node, size_t pos) {
    if (!node) {
        return {nullptr, nullptr};
    }
    
    if (auto* leaf = dynamic_cast<Leaf*>(node.get())) {
        if (pos == 0) {
            return {nullptr, std::move(node)};
        }
        if (pos >= leaf->text.length()) {
            return {std::move(node), nullptr};
        }
        auto left = std::make_unique<Leaf>(leaf->text.substr(0, pos));
        auto right = std::make_unique<Leaf>(leaf->text.substr(pos));
        return {std::move(left), std::move(right)};
    }
    
    auto* branch = dynamic_cast<Branch*>(node.get());
    if (pos <= branch->leftLen) {
        auto [ll, lr] = Split(std::move(branch->left), pos);
        auto right = Concat(std::move(lr), std::move(branch->right));
        return {std::move(ll), std::move(right)};
    } else {
        auto [rl, rr] = Split(std::move(branch->right), pos - branch->leftLen);
        auto left = Concat(std::move(branch->left), std::move(rl));
        return {std::move(left), std::move(rr)};
    }
}

std::unique_ptr<Rope::Node> Rope::Concat(std::unique_ptr<Node> left, std::unique_ptr<Node> right) {
    if (!left) return right;
    if (!right) return left;
    
    // Try to merge small leaves
    auto* ll = dynamic_cast<Leaf*>(left.get());
    auto* rl = dynamic_cast<Leaf*>(right.get());
    if (ll && rl && ll->text.length() + rl->text.length() <= LEAF_SIZE) {
        return std::make_unique<Leaf>(ll->text + rl->text);
    }
    
    auto result = std::make_unique<Branch>(std::move(left), std::move(right));
    return Rebalance(std::move(result));
}

std::unique_ptr<Rope::Node> Rope::Rebalance(std::unique_ptr<Node> node) {
    if (!node) return nullptr;
    
    auto* branch = dynamic_cast<Branch*>(node.get());
    if (!branch) return node;
    
    size_t ld = branch->left ? branch->left->Depth() : 0;
    size_t rd = branch->right ? branch->right->Depth() : 0;
    
    // Simple rebalancing: if imbalanced by more than 2, rebuild
    if (ld > rd + 2 || rd > ld + 2) {
        std::string content;
        node->CollectString(content);
        return BuildFromText(content);
    }
    
    return node;
}

void Rope::Insert(size_t pos, std::string_view text) {
    if (text.empty()) return;
    
    pos = std::min(pos, Length());
    
    // Ensure caches are materialized before edit ONLY for small files
    if (!IsLargeFile()) {
        EnsureCache();
        EnsureLineStarts();
    }
    
    // Record edit info for tree-sitter
    auto [startLine, startCol] = PosToLineCol(pos);
    m_LastEdit.startByte = pos;
    m_LastEdit.oldEndByte = pos;
    m_LastEdit.newEndByte = pos + text.length();
    m_LastEdit.startPoint = {startLine, startCol};
    m_LastEdit.oldEndPoint = {startLine, startCol};
    
    // Structural edit on the rope tree
    auto [left, right] = Split(std::move(m_Root), pos);
    auto middle = BuildFromText(text);
    auto temp = Concat(std::move(left), std::move(middle));
    m_Root = Concat(std::move(temp), std::move(right));
    
    if (IsLargeFile()) {
        m_VisibleRangeBuf.clear();
        auto [endLine, endCol] = PosToLineCol(pos + text.length());
        m_LastEdit.newEndPoint = {endLine, endCol};
    } else {
        // Patch cache and line starts incrementally
        PatchCacheInsert(pos, text);
        auto [endLine, endCol] = PosToLineCol(pos + text.length());
        m_LastEdit.newEndPoint = {endLine, endCol};
    }
}

void Rope::Delete(size_t pos, size_t len) {
    if (len == 0) return;
    
    size_t totalLen = Length();
    pos = std::min(pos, totalLen);
    len = std::min(len, totalLen - pos);
    
    if (!IsLargeFile()) {
        EnsureCache();
        EnsureLineStarts();
    }
    
    // Record edit info for tree-sitter
    auto [startLine, startCol] = PosToLineCol(pos);
    auto [endLine, endCol] = PosToLineCol(pos + len);
    m_LastEdit.startByte = pos;
    m_LastEdit.oldEndByte = pos + len;
    m_LastEdit.newEndByte = pos;
    m_LastEdit.startPoint = {startLine, startCol};
    m_LastEdit.oldEndPoint = {endLine, endCol};
    m_LastEdit.newEndPoint = {startLine, startCol};
    
    // Structural edit on the rope tree
    auto [left, temp] = Split(std::move(m_Root), pos);
    auto [_, right] = Split(std::move(temp), len);
    m_Root = Concat(std::move(left), std::move(right));
    
    if (!m_Root) {
        m_Root = std::make_unique<Leaf>("");
    }
    
    if (IsLargeFile()) {
        m_VisibleRangeBuf.clear();
    } else {
        PatchCacheDelete(pos, len);
    }
}

void Rope::Replace(size_t pos, size_t len, std::string_view text) {
    Delete(pos, len);
    Insert(pos, text);
}

char Rope::At(size_t pos) const {
    if (pos >= Length()) {
        return '\0';  // Return null char for out of bounds
    }
    return m_Root->At(pos);
}

std::string Rope::Substring(size_t pos, size_t len) const {
    if (len == 0) return "";
    
    size_t totalLen = Length();
    pos = std::min(pos, totalLen);
    len = std::min(len, totalLen - pos);
    
    // For small substrings, use the cache
    EnsureCache();
    return m_Cache.substr(pos, len);
}

std::string Rope::ToString() const {
    EnsureCache();
    return m_Cache;
}

size_t Rope::Length() const {
    return m_Root ? m_Root->Length() : 0;
}

size_t Rope::LineCount() const {
    if (IsLargeFile()) {
        return (m_Root ? m_Root->LineCount() : 0) + 1; // +1 for the last implicit line? Or 0-based?
        // Metrics store "newlineCount".
        // "A\nB" has 1 newline, 2 lines.
        // So return newlineCount + 1.
    }
    EnsureLineStarts();
    return m_LineStarts.size();
}

void Rope::EnsureLineStarts() const {
    if (IsLargeFile()) return;
    if (!m_LineStartsDirty) return;
    
    EnsureCache();
    m_LineStarts.clear();
    m_LineStarts.push_back(0);  // First line starts at 0
    
    for (size_t i = 0; i < m_Cache.length(); ++i) {
        if (m_Cache[i] == '\n') {
            m_LineStarts.push_back(i + 1);
        }
    }
    
    m_LineStartsDirty = false;
}

size_t Rope::LineStart(size_t lineNum) const {
    if (IsLargeFile()) {
        return FindLineStartDirect(lineNum);
    }
    EnsureLineStarts();
    if (lineNum >= m_LineStarts.size()) {
        return m_Cache.length();
    }
    return m_LineStarts[lineNum];
}

size_t Rope::LineEnd(size_t lineNum) const {
    if (IsLargeFile()) {
        size_t nextStart = FindLineStartDirect(lineNum + 1);
        if (nextStart == 0) return 0;

        size_t currentStart = FindLineStartDirect(lineNum);
        if (nextStart <= currentStart) return currentStart;
        
        size_t len = Length();
        if (nextStart > len) nextStart = len;
        
        if (At(nextStart - 1) == '\n') {
            return nextStart - 1;
        }
        return nextStart;
    }
    EnsureLineStarts();
    if (lineNum + 1 < m_LineStarts.size()) {
        size_t nextLineStart = m_LineStarts[lineNum + 1];
        // Safely handle the case where the line ends at position 0
        return nextLineStart > 0 ? nextLineStart - 1 : 0;
    }
    return m_Cache.length();
}

size_t Rope::LineLength(size_t lineNum) const {
    return LineEnd(lineNum) - LineStart(lineNum);
}

std::string Rope::Line(size_t lineNum) const {
    size_t start = LineStart(lineNum);
    size_t end = LineEnd(lineNum);
    return Substring(start, end - start);
}

std::string_view Rope::LineView(size_t lineNum) const {
    if (IsLargeFile()) {
        if (lineNum >= m_VisibleFirstLine && lineNum < m_VisibleLastLine) {
            size_t globalStart = FindLineStartDirect(lineNum);
            size_t globalEnd = FindLineStartDirect(lineNum + 1);
            if (globalEnd > 0 && At(globalEnd - 1) == '\n') globalEnd--; // Exclude newline? 
            // Better: LineView usually excludes newline? 
            // The original implementation:
            // start = LineStart(lineNum), end = LineEnd(lineNum)
            // LineEnd returns position BEFORE newline if it exists?
            // LineEnd implementation: return nextLineStart > 0 ? nextLineStart - 1 : 0
            // Wait, nextLineStart is index AFTER \n. So -1 is the \n.
            // So LineEnd includes the content UP TO \n? Or excludes \n?
            // "return nextLineStart > 0 ? nextLineStart - 1 : 0" 
            // If text is "A\nB", line 0 start=0, line 1 start=2.
            // LineEnd(0) = 2 - 1 = 1. Substring(0, 1) -> "A".
            // So LineEnd excludes \n.
            
            // Adjust for VisibleRange logic
            if (globalStart >= m_VisibleRangeStart) {
                size_t localStart = globalStart - m_VisibleRangeStart;
                size_t len = globalEnd - globalStart;
                if (len > 0 && globalEnd > 0) {
                     // Check if ends with newline
                     // We can peek into visible buffer
                     size_t localEnd = localStart + len;
                     if (localEnd <= m_VisibleRangeBuf.length()) {
                         if (localEnd > 0 && m_VisibleRangeBuf[localEnd-1] == '\n') {
                             len--;
                         }
                         return std::string_view(m_VisibleRangeBuf.data() + localStart, len);
                     }
                }
            }
        }
        // Fallback for lines outside visible range (slow path)
        static std::string temp;
        size_t s = FindLineStartDirect(lineNum);
        size_t nextS = FindLineStartDirect(lineNum + 1);
        size_t e = (nextS > 0 && nextS > s) ? nextS - 1 : s; // approximate
        // Accurate check for newline:
        // We'd have to read char at e.
        temp = SubstringDirect(s, nextS - s);
        if (!temp.empty() && temp.back() == '\n') temp.pop_back();
        return temp;
    }

    EnsureCache();
    size_t start = LineStart(lineNum);
    size_t end = LineEnd(lineNum);
    return std::string_view(m_Cache.data() + start, end - start);
}

std::pair<size_t, size_t> Rope::PosToLineCol(size_t pos) const {
    if (IsLargeFile()) {
        size_t len = Length();
        if (pos > len) pos = len;

        size_t offset = pos;
        size_t line = 0;
        const Node* node = m_Root.get();

        while (node) {
            if (auto* leaf = dynamic_cast<const Leaf*>(node)) {
                 size_t limit = std::min(offset, leaf->text.length());
                 for (size_t i = 0; i < limit; ++i) {
                     if (leaf->text[i] == '\n') line++;
                 }
                 break;
            } else if (auto* branch = dynamic_cast<const Branch*>(node)) {
                 if (offset < branch->leftLen) {
                     node = branch->left.get();
                 } else {
                     line += branch->leftLines;
                     offset -= branch->leftLen;
                     node = branch->right.get();
                 }
            }
        }
        size_t start = FindLineStartDirect(line);
        return {line, (pos >= start) ? (pos - start) : 0};
    }

    EnsureLineStarts();
    if (m_LineStarts.empty()) {
        return {0, 0};
    }
    pos = std::min(pos, m_Cache.length());
    
    // Binary search to find the line containing pos
    auto it = std::upper_bound(m_LineStarts.begin(), m_LineStarts.end(), pos);
    size_t line = 0;
    if (it != m_LineStarts.begin()) {
        line = static_cast<size_t>(it - m_LineStarts.begin()) - 1;
    }
    
    size_t lineStart = m_LineStarts[line];
    return {line, pos - lineStart};
}

size_t Rope::LineColToPos(size_t line, size_t col) const {
    size_t start = LineStart(line);
    size_t end = LineEnd(line);
    return std::min(start + col, end);
}

void Rope::ForEach(CharCallback callback) const {
    ForEachInRange(0, Length(), callback);
}

void Rope::ForEachInRange(size_t start, size_t end, CharCallback callback) const {
    EnsureCache();
    end = std::min(end, m_Cache.length());
    for (size_t i = start; i < end; ++i) {
        if (!callback(i, m_Cache[i])) break;
    }
}

void Rope::EnsureCache() const {
    if (IsLargeFile()) return;
    if (m_CacheDirty) {
        m_Cache.clear();
        if (m_Root) {
            m_Root->CollectString(m_Cache);
        }
        m_CacheDirty = false;
    }
}

const char* Rope::CStr() const {
    EnsureCache();
    return m_Cache.c_str();
}

char* Rope::Data() {
    EnsureCache();
    // Reserve extra space for editing
    if (m_Cache.capacity() < m_Cache.size() + 4096) {
        m_Cache.reserve(m_Cache.size() + 8192);
    }
    return m_Cache.data();
}

size_t Rope::Capacity() const {
    const_cast<Rope*>(this)->EnsureCache();
    return m_Cache.capacity();
}

void Rope::SyncFromBuffer() {
    // Rebuild rope from the modified cache
    std::string newContent = m_Cache;
    m_Root = BuildFromText(newContent);
    m_CacheDirty = false;
    m_LineStartsDirty = true;  // Line starts need rebuild after external edit
}

void Rope::PatchCacheInsert(size_t pos, std::string_view text) {
    // Patch m_Cache in-place: insert text at pos
    m_Cache.insert(pos, text.data(), text.length());
    m_CacheDirty = false;
    
    // Patch m_LineStarts incrementally
    // Find the line containing pos via binary search
    auto it = std::upper_bound(m_LineStarts.begin(), m_LineStarts.end(), pos);
    size_t lineIdx = static_cast<size_t>(it - m_LineStarts.begin());
    
    // Collect positions of newlines in inserted text
    std::vector<size_t> newNewlines;
    for (size_t i = 0; i < text.length(); ++i) {
        if (text[i] == '\n') {
            newNewlines.push_back(pos + i + 1);  // Line starts after the newline
        }
    }
    
    // Shift all line starts after pos by text.length()
    for (size_t i = lineIdx; i < m_LineStarts.size(); ++i) {
        m_LineStarts[i] += text.length();
    }
    
    // Insert new line starts for newlines in the inserted text
    if (!newNewlines.empty()) {
        m_LineStarts.insert(m_LineStarts.begin() + lineIdx, newNewlines.begin(), newNewlines.end());
    }
    
    m_LineStartsDirty = false;
}

void Rope::PatchCacheDelete(size_t pos, size_t len) {
    // Patch m_Cache in-place: erase len bytes at pos
    m_Cache.erase(pos, len);
    m_CacheDirty = false;
    
    // Patch m_LineStarts incrementally
    size_t delEnd = pos + len;
    
    // Find range of line starts that fall within the deleted region
    auto eraseBegin = std::upper_bound(m_LineStarts.begin(), m_LineStarts.end(), pos);
    auto eraseEnd = std::upper_bound(m_LineStarts.begin(), m_LineStarts.end(), delEnd);
    
    // Only erase line starts that are strictly inside the deleted region (> pos and <= delEnd)
    // A line start at exactly delEnd will be shifted, not erased
    size_t shiftIdx;
    if (eraseBegin != eraseEnd) {
        auto afterErase = m_LineStarts.erase(eraseBegin, eraseEnd);
        shiftIdx = static_cast<size_t>(afterErase - m_LineStarts.begin());
    } else {
        shiftIdx = static_cast<size_t>(eraseBegin - m_LineStarts.begin());
    }
    
    // Shift all remaining line starts after pos by -len
    for (size_t i = shiftIdx; i < m_LineStarts.size(); ++i) {
        m_LineStarts[i] -= len;
    }
    
    m_LineStartsDirty = false;
}

size_t Rope::CountNewlines(std::string_view text) const {
    return std::count(text.begin(), text.end(), '\n');
}

// -------------------------------------------------------------------------------------------------
// Large File Optimizations
// -------------------------------------------------------------------------------------------------

size_t Rope::Leaf::FindLineStart(size_t lineNum, size_t& linesSkipped) const {
    size_t targetIndex = lineNum - linesSkipped;
    if (targetIndex == 0) return 0; // Starts at beginning of this leaf range
    
    size_t count = 0;
    for (size_t i = 0; i < text.length(); ++i) {
        if (text[i] == '\n') {
            count++;
            if (count == targetIndex) {
                return i + 1; // Start of next line
            }
        }
    }
    return text.length(); 
}

void Rope::Leaf::CollectRange(std::string& out, size_t start, size_t end) const {
    size_t len = text.length();
    if (start >= len) return;
    
    size_t realEnd = std::min(end, len);
    if (realEnd > start) {
        out.append(text, start, realEnd - start);
    }
}

size_t Rope::Branch::FindLineStart(size_t lineNum, size_t& linesSkipped) const {
    size_t currentLines = linesSkipped;
    
    if (lineNum <= currentLines + leftLines) {
         // It's in the left child
         return left->FindLineStart(lineNum, linesSkipped);
    }
    
    // It's in the right child
    linesSkipped += leftLines;
    return leftLen + right->FindLineStart(lineNum, linesSkipped);
}

void Rope::Branch::CollectRange(std::string& out, size_t start, size_t end) const {
    if (start < leftLen) {
        left->CollectRange(out, start, end); 
    }
    if (end > leftLen) {
        size_t rightStart = (start > leftLen) ? (start - leftLen) : 0;
        size_t rightEnd = end - leftLen; // Safe because end > leftLen
        right->CollectRange(out, rightStart, rightEnd);
    }
}

size_t Rope::FindLineStartDirect(size_t lineNum) const {
    if (lineNum == 0) return 0;
    if (!m_Root) return 0;
    
    size_t skippedLines = 0;
    return m_Root->FindLineStart(lineNum, skippedLines);
}

std::string Rope::SubstringDirect(size_t pos, size_t len) const {
    std::string out;
    out.reserve(len);
    if (m_Root) {
        m_Root->CollectRange(out, pos, pos + len);
    }
    return out;
}

void Rope::PrepareVisibleRange(size_t firstLine, size_t lastLine) {
    if (!IsLargeFile()) return;
    
    size_t pad = 50;
    size_t startL = (firstLine > pad) ? (firstLine - pad) : 0;
    size_t endL = lastLine + pad;
    size_t maxL = LineCount(); // This might be slow if using old LineCount? No, LineCount needs optimization too.
    
    // For large files, LineCount() comes from root metrics which is fast.
    if (m_Root) maxL = m_Root->LineCount() + 1; // +1 because line count is newlines + 1 roughly
    
    if (endL > maxL) endL = maxL;
    
    // Check coverage
    if (startL >= m_VisibleFirstLine && endL <= m_VisibleLastLine && !m_VisibleRangeBuf.empty()) {
        return;
    }
    
    m_VisibleFirstLine = startL;
    m_VisibleLastLine = endL;
    
    size_t startByte = FindLineStartDirect(startL);
    size_t endByte = FindLineStartDirect(endL); 
    
    if (endByte < startByte) endByte = startByte;
    
    m_VisibleRangeStart = startByte;
    m_VisibleRangeBuf = SubstringDirect(startByte, endByte - startByte);
}

void Rope::SetVisibleBuffer(const std::string& buffer, size_t firstLine, size_t lastLine, size_t startByte) {
    m_VisibleRangeBuf = buffer;
    m_VisibleFirstLine = firstLine;
    m_VisibleLastLine = lastLine;
    m_VisibleRangeStart = startByte;
}

std::string_view Rope::GetVisibleBufferView(size_t globalStart, size_t len) const {
    if (globalStart >= m_VisibleRangeStart) {
        size_t localStart = globalStart - m_VisibleRangeStart;
        if (localStart + len <= m_VisibleRangeBuf.length()) {
            return std::string_view(m_VisibleRangeBuf.data() + localStart, len);
        }
    }
    return "";
}

} // namespace sol
