#include "input_mode.h"
#include "core/text/text_buffer.h"
#include "core/text/undo_tree.h"
#include <cctype>
#include <algorithm>

namespace sol {
namespace TextOps {

size_t WordStart(const TextBuffer& buffer, size_t pos) {
    if (pos == 0) return 0;
    
    std::string content = buffer.ToString();
    size_t p = pos;
    
    // Skip current word chars backward
    while (p > 0 && (std::isalnum(content[p - 1]) || content[p - 1] == '_')) {
        --p;
    }
    
    return p;
}

size_t WordEnd(const TextBuffer& buffer, size_t pos) {
    std::string content = buffer.ToString();
    size_t len = content.length();
    size_t p = pos;
    
    // Skip to end of current word
    while (p < len && (std::isalnum(content[p]) || content[p] == '_')) {
        ++p;
    }
    
    return p;
}

size_t NextWord(const TextBuffer& buffer, size_t pos) {
    std::string content = buffer.ToString();
    size_t len = content.length();
    size_t p = pos;
    
    // Skip current word
    while (p < len && (std::isalnum(content[p]) || content[p] == '_')) {
        ++p;
    }
    
    // Skip whitespace
    while (p < len && std::isspace(content[p])) {
        ++p;
    }
    
    return p;
}

size_t PrevWord(const TextBuffer& buffer, size_t pos) {
    if (pos == 0) return 0;
    
    std::string content = buffer.ToString();
    size_t p = pos;
    
    // Skip whitespace backward
    while (p > 0 && std::isspace(content[p - 1])) {
        --p;
    }
    
    // Skip word chars backward
    while (p > 0 && (std::isalnum(content[p - 1]) || content[p - 1] == '_')) {
        --p;
    }
    
    return p;
}

size_t LineStart(const TextBuffer& buffer, size_t pos) {
    auto [line, col] = buffer.PosToLineCol(pos);
    return buffer.LineStart(line);
}

size_t LineEnd(const TextBuffer& buffer, size_t pos) {
    auto [line, col] = buffer.PosToLineCol(pos);
    return buffer.LineEnd(line);
}

size_t FirstNonWhitespace(const TextBuffer& buffer, size_t pos) {
    auto [line, col] = buffer.PosToLineCol(pos);
    std::string lineText = buffer.Line(line);
    
    size_t start = buffer.LineStart(line);
    for (size_t i = 0; i < lineText.length(); ++i) {
        if (!std::isspace(lineText[i])) {
            return start + i;
        }
    }
    return start + lineText.length();
}

size_t ParagraphStart(const TextBuffer& buffer, size_t pos) {
    auto [line, col] = buffer.PosToLineCol(pos);
    
    while (line > 0) {
        std::string lineText = buffer.Line(line - 1);
        bool isEmpty = lineText.empty() || 
            std::all_of(lineText.begin(), lineText.end(), ::isspace);
        if (isEmpty) break;
        --line;
    }
    
    return buffer.LineStart(line);
}

size_t ParagraphEnd(const TextBuffer& buffer, size_t pos) {
    auto [line, col] = buffer.PosToLineCol(pos);
    size_t lineCount = buffer.LineCount();
    
    while (line < lineCount - 1) {
        std::string lineText = buffer.Line(line + 1);
        bool isEmpty = lineText.empty() || 
            std::all_of(lineText.begin(), lineText.end(), ::isspace);
        if (isEmpty) break;
        ++line;
    }
    
    return buffer.LineEnd(line);
}

size_t MatchingBracket(const TextBuffer& buffer, size_t pos) {
    std::string content = buffer.ToString();
    if (pos >= content.length()) return pos;
    
    char c = content[pos];
    char match;
    int dir;
    
    switch (c) {
        case '(': match = ')'; dir = 1; break;
        case ')': match = '('; dir = -1; break;
        case '[': match = ']'; dir = 1; break;
        case ']': match = '['; dir = -1; break;
        case '{': match = '}'; dir = 1; break;
        case '}': match = '{'; dir = -1; break;
        case '<': match = '>'; dir = 1; break;
        case '>': match = '<'; dir = -1; break;
        default: return pos;
    }
    
    int depth = 1;
    size_t p = pos;
    
    while (depth > 0) {
        p += dir;
        if (p == 0 && dir == -1) return pos;
        if (p >= content.length()) return pos;
        
        if (content[p] == c) ++depth;
        else if (content[p] == match) --depth;
    }
    
    return p;
}

size_t FindChar(const TextBuffer& buffer, size_t pos, char c, bool forward) {
    std::string content = buffer.ToString();
    
    if (forward) {
        for (size_t i = pos + 1; i < content.length(); ++i) {
            if (content[i] == c) return i;
            if (content[i] == '\n') break;  // Stay on current line
        }
    } else {
        for (size_t i = pos; i > 0; --i) {
            if (content[i - 1] == c) return i - 1;
            if (content[i - 1] == '\n') break;
        }
    }
    
    return pos;
}

size_t FindString(const TextBuffer& buffer, size_t pos, const std::string& str, bool forward) {
    std::string content = buffer.ToString();
    
    if (forward) {
        size_t found = content.find(str, pos + 1);
        if (found != std::string::npos) return found;
    } else {
        if (pos > 0) {
            size_t found = content.rfind(str, pos - 1);
            if (found != std::string::npos) return found;
        }
    }
    
    return pos;
}

void Insert(TextBuffer& buffer, UndoTree& undo, size_t pos, const std::string& text, size_t cursorBefore) {
    undo.Push(EditOperation::Insert(pos, text, cursorBefore));
    buffer.Insert(pos, text);
}

void Delete(TextBuffer& buffer, UndoTree& undo, size_t pos, size_t len, size_t cursorBefore) {
    std::string deleted = buffer.Substring(pos, len);
    undo.Push(EditOperation::Delete(pos, deleted, cursorBefore));
    buffer.Delete(pos, len);
}

void Replace(TextBuffer& buffer, UndoTree& undo, size_t pos, size_t len, const std::string& text, size_t cursorBefore) {
    std::string oldText = buffer.Substring(pos, len);
    undo.Push(EditOperation::Replace(pos, oldText, text, cursorBefore));
    buffer.Replace(pos, len, text);
}

std::optional<size_t> Undo(TextBuffer& buffer, UndoTree& undo) {
    auto op = undo.Undo();
    if (!op) return std::nullopt;
    
    // Validate positions before applying
    size_t bufLen = buffer.Length();
    
    // Reverse the operation
    switch (op->type) {
        case EditOperation::Type::Insert:
            // Undo insert = delete the inserted text
            if (op->position <= bufLen && op->position + op->newText.length() <= bufLen) {
                buffer.Delete(op->position, op->newText.length());
            }
            break;
        case EditOperation::Type::Delete:
            // Undo delete = re-insert the deleted text
            if (op->position <= buffer.Length()) {
                buffer.Insert(op->position, op->oldText);
            }
            break;
        case EditOperation::Type::Replace:
            // Undo replace = replace newText with oldText
            if (op->position <= bufLen && op->position + op->newText.length() <= bufLen) {
                buffer.Replace(op->position, op->newText.length(), op->oldText);
            }
            break;
    }
    
    // Return cursor position, clamped to valid range
    size_t newLen = buffer.Length();
    return std::min(op->cursorBefore, newLen);
}

std::optional<size_t> Redo(TextBuffer& buffer, UndoTree& undo) {
    auto op = undo.Redo();
    if (!op) return std::nullopt;
    
    // Validate positions before applying
    size_t bufLen = buffer.Length();
    
    // Reapply the operation
    switch (op->type) {
        case EditOperation::Type::Insert:
            if (op->position <= bufLen) {
                buffer.Insert(op->position, op->newText);
            }
            break;
        case EditOperation::Type::Delete:
            if (op->position <= bufLen && op->position + op->oldText.length() <= bufLen) {
                buffer.Delete(op->position, op->oldText.length());
            }
            break;
        case EditOperation::Type::Replace:
            if (op->position <= bufLen && op->position + op->oldText.length() <= bufLen) {
                buffer.Replace(op->position, op->oldText.length(), op->newText);
            }
            break;
    }
    
    // Return cursor position, clamped to valid range
    size_t newLen = buffer.Length();
    return std::min(op->cursorAfter, newLen);
}

} // namespace TextOps
} // namespace sol
