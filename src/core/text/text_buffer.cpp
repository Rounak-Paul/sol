#include "text_buffer.h"
#include <tree_sitter/api.h>
#include <algorithm>

// Language declarations
extern "C" {
    const TSLanguage* tree_sitter_c();
    const TSLanguage* tree_sitter_cpp();
    const TSLanguage* tree_sitter_python();
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
}

void TextBuffer::Delete(size_t pos, size_t len) {
    m_Rope.Delete(pos, len);
    m_Modified = true;
    ParseIncremental();
}

void TextBuffer::Replace(size_t pos, size_t len, std::string_view text) {
    m_Rope.Replace(pos, len, text);
    m_Modified = true;
    ParseIncremental();
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
    
    ReleaseTree();
    
    // Use string input for simplicity (tree-sitter handles it efficiently)
    std::string content = m_Rope.ToString();
    m_Tree = ts_parser_parse_string(
        m_Parser,
        nullptr,  // No old tree for full parse
        content.c_str(),
        static_cast<uint32_t>(content.length())
    );
}

void TextBuffer::ParseIncremental() {
    if (!m_Parser || !m_Language || !m_Language->tsLanguage) {
        return;
    }
    
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
    
    // Reparse
    std::string content = m_Rope.ToString();
    TSTree* newTree = ts_parser_parse_string(
        m_Parser,
        m_Tree,  // Old tree for incremental parsing
        content.c_str(),
        static_cast<uint32_t>(content.length())
    );
    
    ReleaseTree();
    m_Tree = newTree;
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
    
    // Walk the tree and collect tokens in range
    std::function<void(TSNode)> walkTree = [&](TSNode node) {
        uint32_t nodeStart = ts_node_start_byte(node);
        uint32_t nodeEnd = ts_node_end_byte(node);
        
        // Skip nodes outside our range
        if (nodeEnd < startByte || nodeStart > endByte) {
            return;
        }
        
        // Only add leaf nodes (named nodes without children or anonymous nodes)
        uint32_t childCount = ts_node_child_count(node);
        if (childCount == 0 || !ts_node_is_named(node)) {
            const char* type = ts_node_type(node);
            TSPoint startPoint = ts_node_start_point(node);
            TSPoint endPoint = ts_node_end_point(node);
            
            tokens.push_back(SyntaxToken{
                .startByte = nodeStart,
                .endByte = nodeEnd,
                .startRow = startPoint.row,
                .startCol = startPoint.column,
                .endRow = endPoint.row,
                .endCol = endPoint.column,
                .type = type,
                .highlightId = static_cast<uint16_t>(MapNodeTypeToHighlight(type))
            });
        }
        
        // Recurse into children
        for (uint32_t i = 0; i < childCount; ++i) {
            walkTree(ts_node_child(node, i));
        }
    };
    
    walkTree(root);
    
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
}

} // namespace sol
