#include "text_buffer.h"
#include "core/lsp/lsp_manager.h"
#include <tree_sitter/api.h>
#include <algorithm>
#include <set>
#include <cctype>
#include <cstring>

// Language declarations
extern "C" {
    const TSLanguage* tree_sitter_c();
    const TSLanguage* tree_sitter_cpp();
    const TSLanguage* tree_sitter_python();
    const TSLanguage* tree_sitter_cmake();
    const TSLanguage* tree_sitter_css();
    const TSLanguage* tree_sitter_html();
    const TSLanguage* tree_sitter_javascript();
    const TSLanguage* tree_sitter_json();
    const TSLanguage* tree_sitter_markdown();
    const TSLanguage* tree_sitter_typescript();
}

namespace sol {

// TextBuffer implementation
TextBuffer::TextBuffer() {
    m_Parser = ts_parser_new();
}

TextBuffer::TextBuffer(std::string_view text) : m_Rope(text) {
    m_Parser = ts_parser_new();
}

TextBuffer::~TextBuffer() {
    ReleaseTree();
    if (m_Parser) {
        ts_parser_delete(m_Parser);
    }
}

TextBuffer::TextBuffer(TextBuffer&& other) noexcept
    : m_Rope(std::move(other.m_Rope))
    , m_Parser(other.m_Parser)
    , m_Tree(other.m_Tree)
    , m_Language(other.m_Language)
    , m_FilePath(std::move(other.m_FilePath))
    , m_Modified(other.m_Modified) {
    other.m_Parser = nullptr;
    other.m_Tree = nullptr;
}

TextBuffer& TextBuffer::operator=(TextBuffer&& other) noexcept {
    if (this != &other) {
        ReleaseTree();
        if (m_Parser) ts_parser_delete(m_Parser);
        
        m_Rope = std::move(other.m_Rope);
        m_Parser = other.m_Parser;
        m_Tree = other.m_Tree;
        m_Language = other.m_Language;
        m_FilePath = std::move(other.m_FilePath);
        m_Modified = other.m_Modified;
        
        other.m_Parser = nullptr;
        other.m_Tree = nullptr;
    }
    return *this;
}

void TextBuffer::Insert(size_t pos, std::string_view text) {
    m_Rope.Insert(pos, text);
    m_Modified = true;
    ParseIncremental();
    
    if (m_Language && !m_Rope.IsLargeFile()) {
        LSPManager::GetInstance().DidChange(m_FilePath.string(), std::string(m_Rope.CStr(), m_Rope.Length()), m_Language->name);
    }
}

void TextBuffer::Delete(size_t pos, size_t len) {
    m_Rope.Delete(pos, len);
    m_Modified = true;
    ParseIncremental();
    
    if (m_Language && !m_Rope.IsLargeFile()) {
        LSPManager::GetInstance().DidChange(m_FilePath.string(), std::string(m_Rope.CStr(), m_Rope.Length()), m_Language->name);
    }
}

void TextBuffer::Replace(size_t pos, size_t len, std::string_view text) {
    m_Rope.Replace(pos, len, text);
    m_Modified = true;
    ParseIncremental();
    
    if (m_Language && !m_Rope.IsLargeFile()) {
        LSPManager::GetInstance().DidChange(m_FilePath.string(), std::string(m_Rope.CStr(), m_Rope.Length()), m_Language->name);
    }
}

void TextBuffer::SetLanguage(const Language* lang) {
    m_Language = lang;
    if (m_Parser && lang && lang->tsLanguage) {
        ts_parser_set_language(m_Parser, lang->tsLanguage);
        Parse();  // Full parse with new language
    }
}

void TextBuffer::ReleaseTree() {
    if (m_Tree) {
        ts_tree_delete(m_Tree);
        m_Tree = nullptr;
    }
}

const char* TextBuffer::TSRead(void* payload, uint32_t byteOffset, TSPoint position, uint32_t* bytesRead) {
    TextBuffer* buffer = static_cast<TextBuffer*>(payload);
    
    size_t len = buffer->m_Rope.Length();
    if (byteOffset >= len) {
        *bytesRead = 0;
        return nullptr;
    }
    
    // Use the cached string for reading
    const char* str = const_cast<TextBuffer*>(buffer)->m_Rope.CStr();
    *bytesRead = static_cast<uint32_t>(len - byteOffset);
    return str + byteOffset;
}

void TextBuffer::Parse() {
    if (!m_Parser || !m_Language || !m_Language->tsLanguage) {
        return;
    }
    
    if (m_Rope.IsLargeFile()) return;
    
    ReleaseTree();
    
    // Use cached buffer directly — no copy needed
    const char* content = m_Rope.CStr();
    uint32_t len = static_cast<uint32_t>(m_Rope.Length());
    m_Tree = ts_parser_parse_string(
        m_Parser,
        nullptr,
        content,
        len
    );
}

void TextBuffer::ParseIncremental() {
    if (!m_Parser || !m_Language || !m_Language->tsLanguage) {
        return;
    }
    
    if (m_Rope.IsLargeFile()) return;
    
    if (!m_Tree) {
        Parse();
        return;
    }
    
    // Get edit info from rope
    auto edit = m_Rope.GetLastEdit();
    
    TSInputEdit tsEdit = {
        .start_byte = static_cast<uint32_t>(edit.startByte),
        .old_end_byte = static_cast<uint32_t>(edit.oldEndByte),
        .new_end_byte = static_cast<uint32_t>(edit.newEndByte),
        .start_point = {
            .row = static_cast<uint32_t>(edit.startPoint.first),
            .column = static_cast<uint32_t>(edit.startPoint.second)
        },
        .old_end_point = {
            .row = static_cast<uint32_t>(edit.oldEndPoint.first),
            .column = static_cast<uint32_t>(edit.oldEndPoint.second)
        },
        .new_end_point = {
            .row = static_cast<uint32_t>(edit.newEndPoint.first),
            .column = static_cast<uint32_t>(edit.newEndPoint.second)
        }
    };
    
    ts_tree_edit(m_Tree, &tsEdit);
    
    // Reparse using cached buffer directly — no copy needed
    const char* content = m_Rope.CStr();
    uint32_t len = static_cast<uint32_t>(m_Rope.Length());
    TSTree* newTree = ts_parser_parse_string(
        m_Parser,
        m_Tree,
        content,
        len
    );
    
    ReleaseTree();
    m_Tree = newTree;
}

void TextBuffer::EnableDiskBuffering(const std::filesystem::path& path) {
    if (m_IsDiskBuffered) return;
    
    m_FilePath = path;
    m_IsDiskBuffered = true;
    IndexDiskFile();
}

void TextBuffer::IndexDiskFile() {
    m_DiskLineStarts.clear();
    m_DiskLineStarts.push_back(0);
    
    // Get file size
    try {
        m_DiskFileSize = std::filesystem::file_size(m_FilePath);
    } catch (...) {
        m_DiskFileSize = 0;
    }
    
    std::ifstream file(m_FilePath, std::ios::binary);
    if (!file.is_open()) {
        m_IsDiskBuffered = false;
        return;
    }
    
    // We can read in chunks to find newlines without loading whole file
    const size_t CHUNK_SIZE = 1024 * 64;
    std::vector<char> buffer(CHUNK_SIZE);
    
    size_t offset = 0;
    while (file.read(buffer.data(), CHUNK_SIZE) || file.gcount() > 0) {
        size_t count = file.gcount();
        for (size_t i = 0; i < count; ++i) {
            if (buffer[i] == '\n') {
                m_DiskLineStarts.push_back(offset + i + 1);
            }
        }
        offset += count;
    }
    
    // Create an empty rope just to satisfy structure, 
    // but we will override access methods.
    // Actually, we must ensure Rope is NOT used if m_IsDiskBuffered
}

std::string TextBuffer::Line(size_t lineNum) const {
    if (m_IsDiskBuffered) {
        return std::string(LineView(lineNum));
    }
    return m_Rope.Line(lineNum);
}

std::string_view TextBuffer::LineView(size_t lineNum) const {
    if (m_IsDiskBuffered) {
        size_t start = LineStart(lineNum);
        size_t end = LineEnd(lineNum);
        
        // Ensure end includes start
        if (end < start) end = start;
        
        // NOTE: LineEnd logic for disk should probably exclude newline if possible,
        // but since we just read raw bytes, it depends on what we want.
        // Rope::LineView usually excludes the newline?
        // Let's assume we want to return the raw bytes we have, excluding newline if prevalent.
        // But we don't know where newline is without reading.
        // Wait, m_DiskLineStarts stores the start of line.
        // LineEnd returns start of NEXT line - 1?
        // Let's re-verify LineEnd logic in header.
        // return next > 0 ? next - 1 : 0;
        // So it points to the newline character usually.
        // If we want LineView to NOT include newline, we rely on the caller or just return what we have?
        // Standard Rope::LineView tries to exclude newline.
        
        size_t len = end - start;
        std::string_view view = m_Rope.GetVisibleBufferView(start, len);
        
        // If we successfully got the view, check for newline at the end
        if (!view.empty() && view.back() == '\n') {
            view.remove_suffix(1);
        }
        return view;
    }
    return m_Rope.LineView(lineNum);
}

void TextBuffer::PrepareVisibleRange(size_t firstLine, size_t lastLine) {
    if (m_IsDiskBuffered) {
        size_t startByte = LineStart(firstLine);
        size_t endByte = LineEnd(lastLine);
        if (endByte < startByte) endByte = startByte;
        size_t len = endByte - startByte;
        
        std::string buffer;
        buffer.resize(len);
        
        std::ifstream file(m_FilePath, std::ios::binary);
        if (file.is_open()) {
            file.seekg(startByte);
            file.read(buffer.data(), len);
        }
        
        m_Rope.SetVisibleBuffer(buffer, firstLine, lastLine, startByte);
    } else {
        m_Rope.PrepareVisibleRange(firstLine, lastLine);
    }
}

std::vector<SyntaxToken> TextBuffer::GetSyntaxTokens(size_t startLine, size_t endLine) const {
    std::vector<SyntaxToken> tokens;
    
    if (!m_Tree) {
        return tokens;
    }
    
    TSNode root = ts_tree_root_node(m_Tree);
    
    // Get byte range for the requested lines
    size_t startByte = m_Rope.LineStart(startLine);
    size_t endByte = (endLine < m_Rope.LineCount()) ? m_Rope.LineEnd(endLine) : m_Rope.Length();
    
    // Use tree-sitter cursor for efficient traversal
    TSTreeCursor cursor = ts_tree_cursor_new(root);
    int currentDepth = 0;
    
    // Move cursor to the start position
    TSPoint startPoint = { static_cast<uint32_t>(startLine), 0 };
    
    // Stack-based iterative traversal (much faster than std::function recursion)
    bool visitChildren = true;
    
    while (true) {
        TSNode node = ts_tree_cursor_current_node(&cursor);
        uint32_t nodeStart = ts_node_start_byte(node);
        uint32_t nodeEnd = ts_node_end_byte(node);
        
        // Skip nodes entirely after our range
        if (nodeStart > endByte) {
            // Try to go to sibling or parent's sibling
            while (!ts_tree_cursor_goto_next_sibling(&cursor)) {
                if (!ts_tree_cursor_goto_parent(&cursor)) {
                    goto done;
                }
                currentDepth--;
            }
            visitChildren = true;
            continue;
        }
        
        // Skip nodes entirely before our range
        if (nodeEnd < startByte) {
            // Try to descend if it might contain our range, but here we know it ends before
            // Wait, nodeEnd < startByte means the whole node is before. 
            // So we don't need to visit children? Yes if children are contained in it.
            // But we can skip it.
            if (!ts_tree_cursor_goto_next_sibling(&cursor)) {
                if (!ts_tree_cursor_goto_parent(&cursor)) {
                    goto done;
                }
                currentDepth--;
                visitChildren = false; // We just came up from a parent, we might be at a node that we already visited or something?
                // No, goto_parent goes to parent. We need to go next sibling of parent.
                // The loop structure in original code was:
                /*
                 if (!ts_tree_cursor_goto_next_sibling(&cursor)) {
                    if (!ts_tree_cursor_goto_parent(&cursor)) { goto done; }
                    visitChildren = false;
                 }
                */
                // Wait, if I go to parent, I am at the parent node. parent node was already visited?
                // The cursor logic is tricky. 
                // Using standard preorder traversal:
                // Visit Node.
                // Goto First Child.
                // Goto Next Sibling.
                // Goto Parent.
                
                // If I am at a node completely before my range, I should try Next Sibling.
                // If no next sibling, go to parent.
            }
             continue;
        }
        
        // Node overlaps our range
        uint32_t childCount = ts_node_child_count(node);
        
        // Add leaf nodes or anonymous nodes
        if (childCount == 0 || !ts_node_is_named(node)) {
            const char* type = ts_node_type(node);
            TSPoint sp = ts_node_start_point(node);
            TSPoint ep = ts_node_end_point(node);
            
            tokens.push_back(SyntaxToken{
                .startByte = nodeStart,
                .endByte = nodeEnd,
                .startRow = sp.row,
                .startCol = sp.column,
                .endRow = ep.row,
                .endCol = ep.column,
                .type = type,
                .highlightId = static_cast<uint16_t>(MapNodeTypeToHighlight(type)),
                .depth = static_cast<uint16_t>(currentDepth)
            });
        }
        
        // Try to descend into children
        if (visitChildren && ts_tree_cursor_goto_first_child(&cursor)) {
            currentDepth++;
            visitChildren = true;
            continue;
        }
        
        // Try to go to sibling
        if (ts_tree_cursor_goto_next_sibling(&cursor)) {
            visitChildren = true;
            continue;
        }
        
        // Go up and try siblings of ancestors
        while (true) {
            if (!ts_tree_cursor_goto_parent(&cursor)) {
                goto done;
            }
            currentDepth--;
            if (ts_tree_cursor_goto_next_sibling(&cursor)) {
                visitChildren = true;
                break;
            }
        }
    }
    
done:
    ts_tree_cursor_delete(&cursor);
    
    // Sort by start position
    std::sort(tokens.begin(), tokens.end(), [](const SyntaxToken& a, const SyntaxToken& b) {
        return a.startByte < b.startByte;
    });
    
    return tokens;
}

HighlightGroup TextBuffer::GetHighlightAt(size_t pos) const {
    if (!m_Tree) {
        return HighlightGroup::None;
    }
    
    TSNode root = ts_tree_root_node(m_Tree);
    TSNode node = ts_node_descendant_for_byte_range(root, 
        static_cast<uint32_t>(pos), 
        static_cast<uint32_t>(pos + 1));
    
    if (ts_node_is_null(node)) {
        return HighlightGroup::None;
    }
    
    return MapNodeTypeToHighlight(ts_node_type(node));
}

std::pair<size_t, size_t> TextBuffer::GetScopeRange(size_t pos) const {
    if (!m_Tree) return {0, 0};
    
    TSNode root = ts_tree_root_node(m_Tree);
    TSNode node = ts_node_descendant_for_byte_range(root, (uint32_t)pos, (uint32_t)pos);
    
    TSNode current = node;
    while (!ts_node_is_null(current)) {
        const char* type = ts_node_type(current);
        std::string_view typeSv(type);
        
        // Check for common scope types or block containers
        if (typeSv == "compound_statement" || 
            typeSv == "block" || 
            typeSv == "function_definition" || 
            typeSv == "class_definition" ||
            typeSv == "translation_unit" || // file scope
            typeSv == "body") {
                return { ts_node_start_byte(current), ts_node_end_byte(current) };
        }
        
        // Also check if it's a statement block (often has braces)
        // Some languages use 'statement_block', 'block' etc.
        if (typeSv.find("block") != std::string_view::npos) {
             return { ts_node_start_byte(current), ts_node_end_byte(current) };
        }

        current = ts_node_parent(current);
    }
    
    return {0, 0};
}

size_t TextBuffer::GetMatchingBracket(size_t pos) const {
    if (!m_Tree) return SIZE_MAX;
    
    TSNode root = ts_tree_root_node(m_Tree);
    TSNode node = ts_node_descendant_for_byte_range(root, (uint32_t)pos, (uint32_t)pos + 1);
    
    if (ts_node_is_null(node)) return SIZE_MAX;
    
    const char* type = ts_node_type(node);
    if (!type) return SIZE_MAX; // Should not happen for valid node
    std::string_view typeSv(type);
    
    bool isOpen = (typeSv == "{" || typeSv == "(" || typeSv == "[");
    bool isClose = (typeSv == "}" || typeSv == ")" || typeSv == "]");
    
    if (!isOpen && !isClose) return SIZE_MAX;
    
    TSNode parent = ts_node_parent(node);
    if (ts_node_is_null(parent)) return SIZE_MAX;
    
    uint32_t count = ts_node_child_count(parent);
    
    const char* target = nullptr;
    if (typeSv == "{") target = "}";
    else if (typeSv == "}") target = "{";
    else if (typeSv == "(") target = ")";
    else if (typeSv == ")") target = "(";
    else if (typeSv == "[") target = "]";
    else if (typeSv == "]") target = "[";
    
    for (uint32_t i = 0; i < count; ++i) {
        TSNode child = ts_node_child(parent, i);
        if (ts_node_eq(child, node)) continue;
        
        const char* childType = ts_node_type(child);
        if (childType && strcmp(childType, target) == 0) {
            return ts_node_start_byte(child);
        }
    }
    
    return SIZE_MAX;
}

std::vector<FoldRange> TextBuffer::GetFoldRanges() const {
    std::vector<FoldRange> ranges;
    if (!m_Tree) return ranges;
    
    TSNode root = ts_tree_root_node(m_Tree);
    TSTreeCursor cursor = ts_tree_cursor_new(root);
    
    // Helper to get the end line, adjusting for tree-sitter's end point behavior
    auto getEndLine = [](TSNode node) -> uint32_t {
        TSPoint end = ts_node_end_point(node);
        TSPoint start = ts_node_start_point(node);
        // If end.column == 0, content ended on previous line
        if (end.column == 0 && end.row > start.row) {
            return end.row - 1;
        }
        return end.row;
    };
    
    // Helper to find a named child by field name
    auto findChildByField = [](TSNode node, const char* fieldName) -> TSNode {
        return ts_node_child_by_field_name(node, fieldName, strlen(fieldName));
    };
    
    // Helper to find first child of a specific type
    auto findChildByType = [](TSNode node, const char* typeName) -> TSNode {
        uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; ++i) {
            TSNode child = ts_node_child(node, i);
            if (strcmp(ts_node_type(child), typeName) == 0) {
                return child;
            }
        }
        return TSNode{}; // null node
    };
    
    // Helper to add a fold range if valid
    auto addFoldRange = [&](TSNode node, const char* type) {
        if (ts_node_is_null(node)) return;
        TSPoint start = ts_node_start_point(node);
        uint32_t endLine = getEndLine(node);
        if (start.row < endLine) {
            ranges.push_back(FoldRange{
                .startLine = start.row,
                .endLine = endLine,
                .type = type
            });
        }
    };
    
    // Iterative tree traversal
    bool visitChildren = true;
    
    while (true) {
        TSNode node = ts_tree_cursor_current_node(&cursor);
        
        if (visitChildren) {
            const char* type = ts_node_type(node);
            std::string_view typeSv(type);
            
            TSPoint start = ts_node_start_point(node);
            uint32_t endLine = getEndLine(node);
            
            // Handle different node types with specific folding logic
            
            // Functions/methods - fold the whole definition
            if (typeSv == "function_definition" || typeSv == "function_declaration" ||
                typeSv == "method_definition" || typeSv == "method_declaration" ||
                typeSv == "arrow_function" || typeSv == "function_expression" ||
                typeSv == "lambda_expression") {
                // Try to find the body for more precise folding
                TSNode body = findChildByField(node, "body");
                if (ts_node_is_null(body)) body = findChildByType(node, "compound_statement");
                if (ts_node_is_null(body)) body = findChildByType(node, "block");
                
                if (!ts_node_is_null(body) && start.row < getEndLine(body)) {
                    ranges.push_back(FoldRange{
                        .startLine = start.row,  // Start from function header
                        .endLine = getEndLine(body),
                        .type = type
                    });
                } else if (start.row < endLine) {
                    addFoldRange(node, type);
                }
            }
            // Classes, structs, enums - fold the whole definition
            else if (typeSv == "class_definition" || typeSv == "class_declaration" ||
                     typeSv == "class_specifier" || typeSv == "struct_specifier" ||
                     typeSv == "enum_specifier" || typeSv == "union_specifier" ||
                     typeSv == "interface_declaration" || typeSv == "trait_definition") {
                if (start.row < endLine) {
                    addFoldRange(node, type);
                }
            }
            // if_statement - fold only the consequence (body), NOT including else
            else if (typeSv == "if_statement") {
                // Find the consequence/body of the if
                TSNode consequence = findChildByField(node, "consequence");
                if (ts_node_is_null(consequence)) consequence = findChildByType(node, "compound_statement");
                if (ts_node_is_null(consequence)) consequence = findChildByType(node, "block");
                
                if (!ts_node_is_null(consequence)) {
                    uint32_t bodyEnd = getEndLine(consequence);
                    if (start.row < bodyEnd) {
                        ranges.push_back(FoldRange{
                            .startLine = start.row,  // if keyword line
                            .endLine = bodyEnd,
                            .type = "if"
                        });
                    }
                }
            }
            // else_clause - fold the body of else
            else if (typeSv == "else_clause" || typeSv == "elif_clause") {
                TSNode body = findChildByField(node, "consequence");
                if (ts_node_is_null(body)) body = findChildByField(node, "body");
                if (ts_node_is_null(body)) body = findChildByType(node, "compound_statement");
                if (ts_node_is_null(body)) body = findChildByType(node, "block");
                // Could also be another if_statement for "else if"
                if (ts_node_is_null(body)) body = findChildByType(node, "if_statement");
                
                if (!ts_node_is_null(body)) {
                    uint32_t bodyEnd = getEndLine(body);
                    if (start.row < bodyEnd) {
                        ranges.push_back(FoldRange{
                            .startLine = start.row,
                            .endLine = bodyEnd,
                            .type = type
                        });
                    }
                } else if (start.row < endLine) {
                    addFoldRange(node, type);
                }
            }
            // for/while/do - fold the body
            else if (typeSv == "for_statement" || typeSv == "while_statement" ||
                     typeSv == "do_statement" || typeSv == "for_in_statement" ||
                     typeSv == "for_range_loop" || typeSv == "enhanced_for_statement") {
                TSNode body = findChildByField(node, "body");
                if (ts_node_is_null(body)) body = findChildByType(node, "compound_statement");
                if (ts_node_is_null(body)) body = findChildByType(node, "block");
                
                if (!ts_node_is_null(body)) {
                    uint32_t bodyEnd = getEndLine(body);
                    if (start.row < bodyEnd) {
                        ranges.push_back(FoldRange{
                            .startLine = start.row,
                            .endLine = bodyEnd,
                            .type = type
                        });
                    }
                } else if (start.row < endLine) {
                    addFoldRange(node, type);
                }
            }
            // switch - fold the body
            else if (typeSv == "switch_statement") {
                TSNode body = findChildByField(node, "body");
                if (ts_node_is_null(body)) body = findChildByType(node, "compound_statement");
                
                if (!ts_node_is_null(body)) {
                    uint32_t bodyEnd = getEndLine(body);
                    if (start.row < bodyEnd) {
                        ranges.push_back(FoldRange{
                            .startLine = start.row,
                            .endLine = bodyEnd,
                            .type = type
                        });
                    }
                } else if (start.row < endLine) {
                    addFoldRange(node, type);
                }
            }
            // case statements
            else if (typeSv == "case_statement" || typeSv == "match_expression" || 
                     typeSv == "switch_expression") {
                if (start.row < endLine) {
                    addFoldRange(node, type);
                }
            }
            // try/catch/finally - fold each block separately
            else if (typeSv == "try_statement") {
                TSNode body = findChildByField(node, "body");
                if (ts_node_is_null(body)) body = findChildByType(node, "compound_statement");
                
                if (!ts_node_is_null(body)) {
                    uint32_t bodyEnd = getEndLine(body);
                    if (start.row < bodyEnd) {
                        ranges.push_back(FoldRange{
                            .startLine = start.row,
                            .endLine = bodyEnd,
                            .type = "try"
                        });
                    }
                }
            }
            else if (typeSv == "catch_clause" || typeSv == "except_clause" || 
                     typeSv == "finally_clause") {
                TSNode body = findChildByField(node, "body");
                if (ts_node_is_null(body)) body = findChildByType(node, "compound_statement");
                if (ts_node_is_null(body)) body = findChildByType(node, "block");
                
                if (!ts_node_is_null(body)) {
                    uint32_t bodyEnd = getEndLine(body);
                    if (start.row < bodyEnd) {
                        ranges.push_back(FoldRange{
                            .startLine = start.row,
                            .endLine = bodyEnd,
                            .type = type
                        });
                    }
                } else if (start.row < endLine) {
                    addFoldRange(node, type);
                }
            }
            // Namespace and modules
            else if (typeSv == "namespace_definition" || typeSv == "module" ||
                     typeSv == "module_definition") {
                if (start.row < endLine) {
                    addFoldRange(node, type);
                }
            }
            // Preprocessor: #if/#ifdef - fold to the line BEFORE #else/#elif/#endif
            else if (typeSv == "preproc_if" || typeSv == "preproc_ifdef") {
                // Find where the #if block ends (before #else, #elif, or #endif)
                uint32_t foldEnd = start.row;
                uint32_t count = ts_node_child_count(node);
                
                for (uint32_t i = 0; i < count; ++i) {
                    TSNode child = ts_node_child(node, i);
                    const char* childType = ts_node_type(child);
                    
                    // Stop at #else, #elif, or #endif directive
                    if (strcmp(childType, "preproc_else") == 0 ||
                        strcmp(childType, "preproc_elif") == 0 ||
                        strcmp(childType, "#endif") == 0) {
                        // Fold ends on line before this directive
                        TSPoint childStart = ts_node_start_point(child);
                        if (childStart.row > start.row) {
                            foldEnd = childStart.row - 1;
                        }
                        break;
                    }
                    // Track the last content line
                    uint32_t childEnd = getEndLine(child);
                    if (childEnd > foldEnd) {
                        foldEnd = childEnd;
                    }
                }
                
                // If no #else/#endif found, use the node end
                if (foldEnd == start.row) {
                    foldEnd = endLine > 0 ? endLine - 1 : endLine; // Exclude #endif line
                }
                
                if (start.row < foldEnd) {
                    ranges.push_back(FoldRange{
                        .startLine = start.row,
                        .endLine = foldEnd,
                        .type = type
                    });
                }
            }
            // #else / #elif blocks
            else if (typeSv == "preproc_else" || typeSv == "preproc_elif") {
                // Fold from #else to line before #endif or next #elif
                uint32_t foldEnd = endLine > 0 ? endLine - 1 : endLine;
                if (start.row < foldEnd) {
                    ranges.push_back(FoldRange{
                        .startLine = start.row,
                        .endLine = foldEnd,
                        .type = type
                    });
                }
            }
            // Multi-line comments
            else if (typeSv == "comment" || typeSv == "block_comment") {
                if (start.row < endLine) {
                    addFoldRange(node, type);
                }
            }
        }
        
        // Try to descend into children
        if (visitChildren && ts_tree_cursor_goto_first_child(&cursor)) {
            visitChildren = true;
            continue;
        }
        
        // Try to go to sibling
        if (ts_tree_cursor_goto_next_sibling(&cursor)) {
            visitChildren = true;
            continue;
        }
        
        // Go up and try siblings of ancestors
        while (true) {
            if (!ts_tree_cursor_goto_parent(&cursor)) {
                goto done;
            }
            if (ts_tree_cursor_goto_next_sibling(&cursor)) {
                visitChildren = true;
                break;
            }
        }
    }
    
done:
    ts_tree_cursor_delete(&cursor);
    
    // Sort by start line
    std::sort(ranges.begin(), ranges.end(), [](const FoldRange& a, const FoldRange& b) {
        return a.startLine < b.startLine;
    });
    
    return ranges;
}

HighlightGroup TextBuffer::MapNodeTypeToHighlight(const char* nodeType) const {
    if (!nodeType) return HighlightGroup::None;
    
    // Check language-specific mappings first
    if (m_Language) {
        for (const auto& [type, group] : m_Language->highlightMappings) {
            if (type == nodeType) {
                return group;
            }
        }
    }
    
    // Default mappings (common across languages)
    std::string_view type = nodeType;
    
    // Comments
    if (type.find("comment") != std::string_view::npos) {
        return HighlightGroup::Comment;
    }
    
    // Strings
    if (type.find("string") != std::string_view::npos ||
        type == "raw_string_literal" ||
        type == "char_literal") {
        return HighlightGroup::String;
    }
    
    // Numbers
    if (type.find("number") != std::string_view::npos ||
        type.find("integer") != std::string_view::npos ||
        type.find("float") != std::string_view::npos) {
        return HighlightGroup::Number;
    }
    
    // Keywords
    if (type == "if" || type == "else" || type == "while" || type == "for" ||
        type == "return" || type == "break" || type == "continue" ||
        type == "switch" || type == "case" || type == "default" ||
        type == "try" || type == "catch" || type == "throw" ||
        type == "class" || type == "struct" || type == "enum" ||
        type == "namespace" || type == "using" || type == "template" ||
        type == "public" || type == "private" || type == "protected" ||
        type == "virtual" || type == "override" || type == "final" ||
        type == "const" || type == "static" || type == "inline" ||
        type == "extern" || type == "typedef" || type == "typename" ||
        type == "auto" || type == "nullptr" || type == "true" || type == "false" ||
        type == "def" || type == "import" || type == "from" || type == "as") {
        return HighlightGroup::Keyword;
    }
    
    // Types
    if (type == "type_identifier" || type == "primitive_type" ||
        type.find("type") != std::string_view::npos) {
        return HighlightGroup::Type;
    }
    
    // Functions
    if (type == "function_declarator" || type == "call_expression" ||
        type == "function_definition" || type == "identifier" && 
        type.find("function") != std::string_view::npos) {
        return HighlightGroup::Function;
    }
    
    // Operators
    if (type == "+" || type == "-" || type == "*" || type == "/" ||
        type == "=" || type == "==" || type == "!=" || type == "<" ||
        type == ">" || type == "<=" || type == ">=" || type == "&&" ||
        type == "||" || type == "!" || type == "&" || type == "|" ||
        type == "^" || type == "~" || type == "<<" || type == ">>" ||
        type == "++" || type == "--" || type == "+=" || type == "-=" ||
        type == "*=" || type == "/=" || type == "->" || type == "::") {
        return HighlightGroup::Operator;
    }
    
    // Punctuation
    if (type == "(" || type == ")" || type == "[" || type == "]" ||
        type == "{" || type == "}" || type == ";" || type == "," ||
        type == "." || type == ":") {
        return HighlightGroup::Punctuation;
    }
    
    // Macros/preprocessor
    if (type.find("preproc") != std::string_view::npos ||
        type == "#include" || type == "#define" || type == "#ifdef") {
        return HighlightGroup::Macro;
    }
    
    return HighlightGroup::None;
}

// LanguageRegistry implementation
LanguageRegistry& LanguageRegistry::GetInstance() {
    static LanguageRegistry instance;
    return instance;
}

void LanguageRegistry::RegisterLanguage(std::unique_ptr<Language> lang) {
    m_Languages.push_back(std::move(lang));
}

const Language* LanguageRegistry::GetLanguageForFile(const std::filesystem::path& path) const {
    std::string ext = path.extension().string();
    
    for (const auto& lang : m_Languages) {
        for (const auto& langExt : lang->extensions) {
            if (ext == langExt) {
                return lang.get();
            }
        }
    }
    
    return nullptr;
}

const Language* LanguageRegistry::GetLanguageByName(const std::string& name) const {
    for (const auto& lang : m_Languages) {
        if (lang->name == name) {
            return lang.get();
        }
    }
    return nullptr;
}

void LanguageRegistry::InitializeBuiltins() {
    // C language
    auto cLang = std::make_unique<Language>();
    cLang->name = "c";
    cLang->extensions = {".c", ".h"};
    cLang->tsLanguage = tree_sitter_c();
    cLang->highlightMappings = {
        {"primitive_type", HighlightGroup::Type},
        {"type_identifier", HighlightGroup::Type},
        {"sized_type_specifier", HighlightGroup::Type},
        {"function_declarator", HighlightGroup::Function},
        {"call_expression", HighlightGroup::Function},
        {"preproc_include", HighlightGroup::Macro},
        {"preproc_def", HighlightGroup::Macro},
        {"preproc_ifdef", HighlightGroup::Macro},
        {"string_literal", HighlightGroup::String},
        {"char_literal", HighlightGroup::String},
        {"number_literal", HighlightGroup::Number},
        {"comment", HighlightGroup::Comment},
    };
    RegisterLanguage(std::move(cLang));
    
    // C++ language
    auto cppLang = std::make_unique<Language>();
    cppLang->name = "cpp";
    cppLang->extensions = {".cpp", ".hpp", ".cc", ".cxx", ".hxx", ".h"};
    cppLang->tsLanguage = tree_sitter_cpp();
    cppLang->highlightMappings = {
        {"primitive_type", HighlightGroup::Type},
        {"type_identifier", HighlightGroup::Type},
        {"sized_type_specifier", HighlightGroup::Type},
        {"namespace_identifier", HighlightGroup::Namespace},
        {"function_declarator", HighlightGroup::Function},
        {"call_expression", HighlightGroup::Function},
        {"template_type", HighlightGroup::Type},
        {"auto", HighlightGroup::Keyword},
        {"nullptr", HighlightGroup::Constant},
        {"true", HighlightGroup::Constant},
        {"false", HighlightGroup::Constant},
        {"string_literal", HighlightGroup::String},
        {"raw_string_literal", HighlightGroup::String},
        {"char_literal", HighlightGroup::String},
        {"number_literal", HighlightGroup::Number},
        {"comment", HighlightGroup::Comment},
    };
    RegisterLanguage(std::move(cppLang));
    
    // Python language
    auto pyLang = std::make_unique<Language>();
    pyLang->name = "python";
    pyLang->extensions = {".py", ".pyw", ".pyi"};
    pyLang->tsLanguage = tree_sitter_python();
    pyLang->highlightMappings = {
        {"identifier", HighlightGroup::Variable},
        {"type", HighlightGroup::Type},
        {"function_definition", HighlightGroup::Function},
        {"call", HighlightGroup::Function},
        {"decorator", HighlightGroup::Macro},
        {"string", HighlightGroup::String},
        {"integer", HighlightGroup::Number},
        {"float", HighlightGroup::Number},
        {"comment", HighlightGroup::Comment},
        {"true", HighlightGroup::Constant},
        {"false", HighlightGroup::Constant},
        {"none", HighlightGroup::Constant},
    };
    RegisterLanguage(std::move(pyLang));

    // CMake language
    auto cmakeLang = std::make_unique<Language>();
    cmakeLang->name = "cmake";
    cmakeLang->extensions = {".cmake", "CMakeLists.txt"};
    cmakeLang->tsLanguage = tree_sitter_cmake();
    cmakeLang->highlightMappings = {
        {"identifier", HighlightGroup::Variable},
        {"function", HighlightGroup::Function},
        {"variable", HighlightGroup::Variable},
        {"string", HighlightGroup::String},
        {"number", HighlightGroup::Number},
        {"comment", HighlightGroup::Comment},
        {"boolean", HighlightGroup::Constant},
    };
    RegisterLanguage(std::move(cmakeLang));

    // CSS language
    auto cssLang = std::make_unique<Language>();
    cssLang->name = "css";
    cssLang->extensions = {".css"};
    cssLang->tsLanguage = tree_sitter_css();
    cssLang->highlightMappings = {
        {"tag_name", HighlightGroup::Keyword},
        {"class_name", HighlightGroup::Type},
        {"id_name", HighlightGroup::Type},
        {"attribute_name", HighlightGroup::Variable},
        {"property_name", HighlightGroup::Variable},
        {"string_value", HighlightGroup::String},
        {"integer_value", HighlightGroup::Number},
        {"float_value", HighlightGroup::Number},
        {"comment", HighlightGroup::Comment},
        {"color_value", HighlightGroup::Constant},
    };
    RegisterLanguage(std::move(cssLang));

    // HTML language
    auto htmlLang = std::make_unique<Language>();
    htmlLang->name = "html";
    htmlLang->extensions = {".html", ".htm"};
    htmlLang->tsLanguage = tree_sitter_html();
    htmlLang->highlightMappings = {
        {"tag_name", HighlightGroup::Keyword},
        {"attribute_name", HighlightGroup::Variable},
        {"attribute_value", HighlightGroup::String},
        {"comment", HighlightGroup::Comment},
        {"doctype", HighlightGroup::Macro},
    };
    RegisterLanguage(std::move(htmlLang));

    // JavaScript language
    auto jsLang = std::make_unique<Language>();
    jsLang->name = "javascript";
    jsLang->extensions = {".js", ".mjs", ".cjs"};
    jsLang->tsLanguage = tree_sitter_javascript();
    jsLang->highlightMappings = {
        {"identifier", HighlightGroup::Variable},
        {"property_identifier", HighlightGroup::Variable},
        {"shorthand_property_identifier", HighlightGroup::Variable},
        {"function_declaration", HighlightGroup::Function},
        {"function", HighlightGroup::Function},
        {"call_expression", HighlightGroup::Function},
        {"string", HighlightGroup::String},
        {"template_string", HighlightGroup::String},
        {"number", HighlightGroup::Number},
        {"comment", HighlightGroup::Comment},
        {"true", HighlightGroup::Constant},
        {"false", HighlightGroup::Constant},
        {"null", HighlightGroup::Constant},
        {"undefined", HighlightGroup::Constant},
    };
    RegisterLanguage(std::move(jsLang));

    // JSON language
    auto jsonLang = std::make_unique<Language>();
    jsonLang->name = "json";
    jsonLang->extensions = {".json"};
    jsonLang->tsLanguage = tree_sitter_json();
    jsonLang->highlightMappings = {
        {"pair_key", HighlightGroup::Variable},
        {"string", HighlightGroup::String},
        {"number", HighlightGroup::Number},
        {"true", HighlightGroup::Constant},
        {"false", HighlightGroup::Constant},
        {"null", HighlightGroup::Constant},
    };
    RegisterLanguage(std::move(jsonLang));

    // Markdown language
    auto mdLang = std::make_unique<Language>();
    mdLang->name = "markdown";
    mdLang->extensions = {".md", ".markdown"};
    mdLang->tsLanguage = tree_sitter_markdown();
    mdLang->highlightMappings = {
        {"atx_heading", HighlightGroup::Function},
        {"setext_heading", HighlightGroup::Function},
        {"fenced_code_block", HighlightGroup::String}, // Treat code blocks as "string" for now
        {"link_text", HighlightGroup::Keyword},
        {"link_destination", HighlightGroup::String},
        {"list_marker_plus", HighlightGroup::Operator},
        {"list_marker_minus", HighlightGroup::Operator},
        {"list_marker_star", HighlightGroup::Operator},
        {"list_marker_dot", HighlightGroup::Operator},
        {"emphasis", HighlightGroup::Type},
        {"strong_emphasis", HighlightGroup::Type},
    };
    RegisterLanguage(std::move(mdLang));

    // TypeScript language
    auto tsLang = std::make_unique<Language>();
    tsLang->name = "typescript";
    tsLang->extensions = {".ts", ".tsx"};
    tsLang->tsLanguage = tree_sitter_typescript();
    tsLang->highlightMappings = {
        {"identifier", HighlightGroup::Variable},
        {"property_identifier", HighlightGroup::Variable},
        {"shorthand_property_identifier", HighlightGroup::Variable},
        {"type_identifier", HighlightGroup::Type},
        {"predefined_type", HighlightGroup::Type},
        {"function_declaration", HighlightGroup::Function},
        {"function", HighlightGroup::Function},
        {"call_expression", HighlightGroup::Function},
        {"string", HighlightGroup::String},
        {"template_string", HighlightGroup::String},
        {"number", HighlightGroup::Number},
        {"comment", HighlightGroup::Comment},
        {"true", HighlightGroup::Constant},
        {"false", HighlightGroup::Constant},
        {"null", HighlightGroup::Constant},
        {"undefined", HighlightGroup::Constant},
    };
    RegisterLanguage(std::move(tsLang));
}

} // namespace sol

namespace sol {
std::vector<std::string> TextBuffer::GetWordCompletions(const std::string& prefix, size_t cursorPos) const {
    std::set<std::string> words;
    
    // 1. Try Tree-sitter completions first if available (Smarter, "Educated" fallback)
    if (m_Tree) {
        TSNode root = ts_tree_root_node(m_Tree);
        TSTreeCursor cursor = ts_tree_cursor_new(root);
        
        bool visitChildren = true;
        while (true) {
            TSNode node = ts_tree_cursor_current_node(&cursor);
            const char* type = ts_node_type(node);
            std::string typeStr = (type ? type : "");
            
            // Skip comments and strings to keep completions clean
            bool isStringOrComment = (typeStr.find("string") != std::string::npos || 
                                     typeStr.find("comment") != std::string::npos);
            
            if (isStringOrComment) {
                visitChildren = false; 
            } else {
                // Collect identifiers
                if (ts_node_is_named(node)) {
                    bool isId = false;
                    // Heuristic: check for identifier-like node types
                    if (typeStr.find("identifier") != std::string::npos || 
                        typeStr.find("name") != std::string::npos || 
                        typeStr == "word") {
                        isId = true;
                    }

                    if (isId) {
                        uint32_t start = ts_node_start_byte(node);
                        uint32_t end = ts_node_end_byte(node);
                        if (end > start) {
                            std::string text = m_Rope.Substring(start, end - start);
                            if (!text.empty() && (isalpha(text[0]) || text[0] == '_')) {
                                bool match = true;
                                if (!prefix.empty()) {
                                    if (text.length() < prefix.length()) match = false;
                                    else {
                                        for(size_t i = 0; i < prefix.length(); ++i) {
                                            if (std::tolower(text[i]) != std::tolower(prefix[i])) {
                                                match = false;
                                                break;
                                            }
                                        }
                                    }
                                }
                                if (match && text != prefix) {
                                    words.insert(text);
                                }
                            }
                        }
                    }
                }
            }
            
            // Traverse
            if (visitChildren && ts_tree_cursor_goto_first_child(&cursor)) {
                visitChildren = true;
                continue;
            }
            
            bool moved = false;
            while (!moved) {
                if (ts_tree_cursor_goto_next_sibling(&cursor)) {
                    visitChildren = true;
                    moved = true;
                } else {
                    if (!ts_tree_cursor_goto_parent(&cursor)) {
                        ts_tree_cursor_delete(&cursor);
                        // If we found words, return them
                        if (!words.empty()) {
                            return std::vector<std::string>(words.begin(), words.end());
                        }
                        goto raw_fallback;
                    }
                }
            }
        }
    }

raw_fallback:
    std::string text = m_Rope.ToString(); // Not optimal for huge files, but safe
    const char* ptr = text.c_str();
    const char* end = ptr + text.length();
    
    while (ptr < end) {
        // Skip non-word chars
        while (ptr < end && !(isalnum(*ptr) || *ptr == '_')) {
            ptr++;
        }
        
        if (ptr >= end) break;
        
        const char* start = ptr;
        while (ptr < end && (isalnum(*ptr) || *ptr == '_')) {
            ptr++;
        }
        
        std::string word(start, ptr - start);
        // If prefix matches (case insensitive)
        if (word.length() > prefix.length()) {
            bool match = true;
            for(size_t i = 0; i < prefix.length(); ++i) {
                if (std::tolower(word[i]) != std::tolower(prefix[i])) {
                    match = false;
                    break;
                }
            }
            if (match) words.insert(word);
        }
        // If empty prefix, just all words? Maybe too many. Limit length > 3
        else if (prefix.empty() && word.length() > 3) {
            words.insert(word);
        }
    }
    
    return std::vector<std::string>(words.begin(), words.end());
}
}

