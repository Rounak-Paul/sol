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
            }
            visitChildren = true;
            continue;
        }
        
        // Skip nodes entirely before our range
        if (nodeEnd < startByte) {
            if (!ts_tree_cursor_goto_next_sibling(&cursor)) {
                if (!ts_tree_cursor_goto_parent(&cursor)) {
                    goto done;
                }
                visitChildren = false;
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
                .highlightId = static_cast<uint16_t>(MapNodeTypeToHighlight(type))
            });
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

