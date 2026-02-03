#pragma once

#include "core/utils/json.h"
#include <string>
#include <vector>

namespace sol {

struct LSPPosition {
    int line;
    int character;
    
    JsonObject ToJson() const {
        return {
            {"line", line},
            {"character", character}
        };
    }
    
    static LSPPosition FromJson(const JsonValue& json) {
        if (json.type != JsonType::Object) return {0, 0};
        return {
            json["line"].ToInt(),
            json["character"].ToInt()
        };
    }
};

struct LSPRange {
    LSPPosition start;
    LSPPosition end;
    
    JsonObject ToJson() const {
        return {
            {"start", start.ToJson()},
            {"end", end.ToJson()}
        };
    }
    
    static LSPRange FromJson(const JsonValue& json) {
        return {
            LSPPosition::FromJson(json["start"]),
            LSPPosition::FromJson(json["end"])
        };
    }
};

struct LSPCompletionItem {
    std::string label;
    int kind;
    std::string detail;
    std::string documentation;
    std::string insertText;
};

struct LSPDiagnostic {
    LSPRange range;
    std::string message;
    int severity; // 1: Error, 2: Warning, 3: Info, 4: Hint
};

} // namespace sol
