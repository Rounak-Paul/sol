#pragma once

#include <string>
#include <map>
#include <vector>
#include <variant>
#include <optional>
#include <sstream>

// Minimal JSON helper for LSP
// Note: In a real production codebase, use nlohmann/json or rapidjson.
// This is a lightweight implementation to avoid adding a large dependency in this session.

namespace sol {

enum class JsonType { Null, Bool, Number, String, Array, Object };

struct JsonValue;

using JsonObject = std::map<std::string, JsonValue>;
using JsonArray = std::vector<JsonValue>;

struct JsonValue {
    JsonType type = JsonType::Null;
    
    std::variant<std::monostate, bool, double, std::string, JsonArray, JsonObject> data;
    
    JsonValue() = default;
    JsonValue(bool v) : type(JsonType::Bool), data(v) {}
    JsonValue(int v) : type(JsonType::Number), data(static_cast<double>(v)) {}
    JsonValue(double v) : type(JsonType::Number), data(v) {}
    JsonValue(const char* v) : type(JsonType::String), data(std::string(v)) {}
    JsonValue(const std::string& v) : type(JsonType::String), data(v) {}
    JsonValue(const JsonArray& v) : type(JsonType::Array), data(v) {}
    JsonValue(const JsonObject& v) : type(JsonType::Object), data(v) {}
    
    // Helpers
    std::string ToString() const {
        if (type == JsonType::String) return std::get<std::string>(data);
        return "";
    }
    
    int ToInt() const {
        if (type == JsonType::Number) return static_cast<int>(std::get<double>(data));
        return 0;
    }
    
    bool ToBool() const {
        if (type == JsonType::Bool) return std::get<bool>(data);
        return false;
    }

    const JsonObject& AsObject() const {
        static JsonObject empty;
        if (type == JsonType::Object) return std::get<JsonObject>(data);
        return empty;
    }
    
    const JsonArray& AsArray() const {
        static JsonArray empty;
        if (type == JsonType::Array) return std::get<JsonArray>(data);
        return empty;
    }

    bool Has(const std::string& key) const {
        if (type != JsonType::Object) return false;
        const auto& obj = std::get<JsonObject>(data);
        return obj.find(key) != obj.end();
    }
    
    const JsonValue& operator[](const std::string& key) const {
        static JsonValue nullVal;
        if (type != JsonType::Object) return nullVal;
        const auto& obj = std::get<JsonObject>(data);
        auto it = obj.find(key);
        if (it != obj.end()) return it->second;
        return nullVal;
    }
};

class Json {
public:
    static std::string Serialize(const JsonValue& value) {
        std::stringstream ss;
        Serialize(value, ss);
        return ss.str();
    }
    
    static JsonValue Parse(const std::string& json) {
        size_t pos = 0;
        return ParseValue(json, pos);
    }
    
private:
    static void Serialize(const JsonValue& val, std::stringstream& ss) {
        switch (val.type) {
            case JsonType::Null: ss << "null"; break;
            case JsonType::Bool: ss << (std::get<bool>(val.data) ? "true" : "false"); break;
            case JsonType::Number: ss << std::get<double>(val.data); break;
            case JsonType::String: {
                ss << "\"";
                for (char c : std::get<std::string>(val.data)) {
                    if (c == '"') ss << "\\\"";
                    else if (c == '\\') ss << "\\\\";
                    else if (c == '\b') ss << "\\b";
                    else if (c == '\f') ss << "\\f";
                    else if (c == '\n') ss << "\\n";
                    else if (c == '\r') ss << "\\r";
                    else if (c == '\t') ss << "\\t";
                    else ss << c;
                }
                ss << "\"";
                break;
            }
            case JsonType::Array: {
                ss << "[";
                const auto& arr = std::get<JsonArray>(val.data);
                for (size_t i = 0; i < arr.size(); ++i) {
                    if (i > 0) ss << ",";
                    Serialize(arr[i], ss);
                }
                ss << "]";
                break;
            }
            case JsonType::Object: {
                ss << "{";
                const auto& obj = std::get<JsonObject>(val.data);
                bool first = true;
                for (const auto& [k, v] : obj) {
                    if (!first) ss << ",";
                    ss << "\"" << k << "\":";
                    Serialize(v, ss);
                    first = false;
                }
                ss << "}";
                break;
            }
        }
    }
    
    static void SkipWhitespace(const std::string& json, size_t& pos) {
        while (pos < json.length() && std::isspace(json[pos])) pos++;
    }
    
    static JsonValue ParseValue(const std::string& json, size_t& pos) {
        SkipWhitespace(json, pos);
        if (pos >= json.length()) return JsonValue();
        
        char c = json[pos];
        if (c == '{') return ParseObject(json, pos);
        if (c == '[') return ParseArray(json, pos);
        if (c == '"') return ParseString(json, pos);
        if (c == 't' || c == 'f') return ParseBool(json, pos);
        if (c == 'n') return ParseNull(json, pos);
        if (std::isdigit(c) || c == '-') return ParseNumber(json, pos);
        
        return JsonValue();
    }
    
    static JsonValue ParseString(const std::string& json, size_t& pos) {
        pos++; // quote
        std::string str;
        while (pos < json.length()) {
            char c = json[pos];
            if (c == '"') {
                pos++;
                return JsonValue(str);
            }
            if (c == '\\') {
                pos++;
                if (pos >= json.length()) break;
                char escaped = json[pos];
                switch (escaped) {
                    case '"': str += '"'; break;
                    case '\\': str += '\\'; break;
                    case 'n': str += '\n'; break;
                    case 't': str += '\t'; break;
                    case 'r': str += '\r'; break;
                    case 'b': str += '\b'; break;
                    case 'f': str += '\f'; break;
                    default: str += escaped; break;
                }
            } else {
                str += c;
            }
            pos++;
        }
        return JsonValue(str);
    }
    
    static JsonValue ParseNumber(const std::string& json, size_t& pos) {
        size_t start = pos;
        if (json[pos] == '-') pos++;
        while (pos < json.length() && std::isdigit(json[pos])) pos++;
        if (pos < json.length() && json[pos] == '.') {
            pos++;
            while (pos < json.length() && std::isdigit(json[pos])) pos++;
        }
        std::string numStr = json.substr(start, pos - start);
        return JsonValue(std::stod(numStr));
    }
    
    static JsonValue ParseBool(const std::string& json, size_t& pos) {
        if (json.compare(pos, 4, "true") == 0) {
            pos += 4;
            return JsonValue(true);
        }
        if (json.compare(pos, 5, "false") == 0) {
            pos += 5;
            return JsonValue(false);
        }
        return JsonValue();
    }
    
    static JsonValue ParseNull(const std::string& json, size_t& pos) {
        if (json.compare(pos, 4, "null") == 0) {
            pos += 4;
        }
        return JsonValue();
    }
    
    static JsonValue ParseObject(const std::string& json, size_t& pos) {
        JsonObject obj;
        pos++; // {
        while (pos < json.length()) {
            SkipWhitespace(json, pos);
            if (json[pos] == '}') {
                pos++;
                return JsonValue(obj);
            }
            
            JsonValue keyVal = ParseString(json, pos); // Parse key
            std::string key = keyVal.ToString();
            
            SkipWhitespace(json, pos);
            if (json[pos] == ':') pos++;
            
            JsonValue val = ParseValue(json, pos);
            obj[key] = val;
            
            SkipWhitespace(json, pos);
            if (json[pos] == ',') pos++;
        }
        return JsonValue(obj);
    }
    
    static JsonValue ParseArray(const std::string& json, size_t& pos) {
        JsonArray arr;
        pos++; // [
        while (pos < json.length()) {
            SkipWhitespace(json, pos);
            if (json[pos] == ']') {
                pos++;
                return JsonValue(arr);
            }
            
            arr.push_back(ParseValue(json, pos));
            
            SkipWhitespace(json, pos);
            if (json[pos] == ',') pos++;
        }
        return JsonValue(arr);
    }
};

} // namespace sol
