#include "lsp_client.h"
#include "core/logger.h"
#include <iostream>
#include <sstream>
#include <unistd.h>

namespace sol {

LSPClient::LSPClient(const std::string& command, const std::vector<std::string>& args) {
    m_Process = std::make_unique<Process>(command, args);
}

LSPClient::~LSPClient() {
    Shutdown();
}

bool LSPClient::Initialize(const std::string& rootPath) {
    if (!m_Process->Start()) {
        Logger::Error("Failed to start LSP server");
        return false;
    }

    // Give the child process a moment to either start or fail
    // This catches immediate exec failures (command not found)
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    if (!m_Process->IsRunning()) {
        Logger::Error("LSP server process exited immediately (command not found?)");
        return false;
    }

    m_Running = true;
    m_ReadThread = std::thread(&LSPClient::ReadLoop, this);

    JsonObject params;
    params["processId"] = static_cast<int>(getpid());
    params["rootUri"] = FilePathToURI(rootPath);
    params["capabilities"] = JsonObject{}; // Simplified capabilities

    SendRequest("initialize", params, [this](const JsonValue& result) {
        SendNotification("initialized", {});
    });

    return true;
}

void LSPClient::Shutdown() {
    if (m_Running) {
        m_Running = false;  // Signal read thread to stop first
        
        // Only send shutdown if process still running
        if (m_Process && m_Process->IsRunning()) {
            SendRequest("shutdown", {}, [](const JsonValue&) {});
            SendNotification("exit", {});
            
            // Give it a moment to exit gracefully
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            m_Process->Stop();
        }
        
        // Wait for read thread to finish
        if (m_ReadThread.joinable()) {
            m_ReadThread.join();
        }
    }
}

void LSPClient::SendRequest(const std::string& method, const JsonObject& params, std::function<void(const JsonValue&)> callback) {
    if (!m_Running || !m_Process || !m_Process->IsRunning()) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(m_Mutex);
    int id = m_NextRequestId++;
    
    if (callback) {
        m_PendingRequests[id] = callback;
    }

    JsonObject msg;
    msg["jsonrpc"] = "2.0";
    msg["id"] = id;
    msg["method"] = method;
    msg["params"] = params;

    std::string jsonStr = Json::Serialize(msg);
    std::string packet = "Content-Length: " + std::to_string(jsonStr.length()) + "\r\n\r\n" + jsonStr;
    
    m_Process->Write(packet);
}

void LSPClient::SendNotification(const std::string& method, const JsonObject& params) {
    if (!m_Running || !m_Process || !m_Process->IsRunning()) {
        return;
    }
    
    JsonObject msg;
    msg["jsonrpc"] = "2.0";
    msg["method"] = method;
    msg["params"] = params;

    std::string jsonStr = Json::Serialize(msg);
    std::string packet = "Content-Length: " + std::to_string(jsonStr.length()) + "\r\n\r\n" + jsonStr;
    
    m_Process->Write(packet);
}

void LSPClient::ReadLoop() {
    std::vector<char> buffer(4096);
    std::vector<char> errBuffer(4096);
    std::string accumulator;
    
    while (m_Running) {
        // Check if process is still alive
        if (!m_Process || !m_Process->IsRunning()) {
            break;
        }
        
        // Drain stderr to prevent blocking, but don't log it
        m_Process->ReadErr(errBuffer.data(), errBuffer.size());

        size_t bytesRead = m_Process->Read(buffer.data(), buffer.size());
        if (bytesRead > 0) {
            accumulator.append(buffer.data(), bytesRead);
            
            while (true) {
                // Find Content-Length
                size_t headerEnd = accumulator.find("\r\n\r\n");
                if (headerEnd == std::string::npos) break;
                
                size_t contentLengthPos = accumulator.find("Content-Length: ");
                if (contentLengthPos == std::string::npos || contentLengthPos > headerEnd) {
                    // Invalid header? discard
                    accumulator.erase(0, headerEnd + 4);
                    continue;
                }
                
                size_t lenStart = contentLengthPos + 16;
                size_t lenEnd = accumulator.find("\r\n", lenStart);
                if (lenEnd == std::string::npos) break;
                
                int contentLength = 0;
                try {
                    contentLength = std::stoi(accumulator.substr(lenStart, lenEnd - lenStart));
                } catch (...) {
                    // Invalid content-length, discard header
                    accumulator.erase(0, headerEnd + 4);
                    continue;
                }
                
                if (accumulator.length() >= headerEnd + 4 + contentLength) {
                    std::string payload = accumulator.substr(headerEnd + 4, contentLength);
                    accumulator.erase(0, headerEnd + 4 + contentLength);
                    
                    try {
                        JsonValue json = Json::Parse(payload);
                        HandleMessage(json);
                    } catch (...) {
                        Logger::Error("Failed to parse JSON message from LSP");
                    }
                } else {
                    break; // Wait for more data
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void LSPClient::HandleMessage(const JsonValue& json) {
    if (json.type != JsonType::Object) return;
    
    // Response
    if (json.Has("id") && json.Has("result")) {
        int id = json["id"].ToInt();
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (m_PendingRequests.count(id)) {
            m_PendingRequests[id](json["result"]);
            m_PendingRequests.erase(id);
        }
    }
    // Notification or Request from Server
    else if (json.Has("method")) {
        std::string method = json["method"].ToString();
        
        if (method == "textDocument/publishDiagnostics") {
            if (m_DiagnosticsCallback) {
                auto params = json["params"];
                std::string uri = params["uri"].ToString();
                auto diagnosticsJson = params["diagnostics"].AsArray();
                
                std::vector<LSPDiagnostic> diagnostics;
                for (const auto& item : diagnosticsJson) {
                    LSPDiagnostic d;
                    d.range = LSPRange::FromJson(item["range"]);
                    d.message = item["message"].ToString();
                    d.severity = item["severity"].ToInt();
                    diagnostics.push_back(d);
                }
                
                m_DiagnosticsCallback(uri, diagnostics);
            }
        }
    }
}

void LSPClient::DidOpen(const std::string& filePath, const std::string& content, const std::string& languageId) {
    JsonObject params;
    JsonObject textDocument;
    textDocument["uri"] = FilePathToURI(filePath);
    textDocument["languageId"] = languageId;
    
    int version = 1;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_FileVersions[filePath] = version;
    }
    
    textDocument["version"] = version;
    textDocument["text"] = content;
    params["textDocument"] = textDocument;
    
    SendNotification("textDocument/didOpen", params);
}

void LSPClient::DidChange(const std::string& filePath, const std::string& content) {
    JsonObject params;
    JsonObject textDocument;
    textDocument["uri"] = FilePathToURI(filePath);
    
    int version = 0;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        version = ++m_FileVersions[filePath];
    }
    
    textDocument["version"] = version; 
    params["textDocument"] = textDocument;
    
    JsonArray contentChanges;
    JsonObject change;
    change["text"] = content; // Full sync for now
    contentChanges.push_back(change);
    params["contentChanges"] = contentChanges;
    
    SendNotification("textDocument/didChange", params);
}

bool LSPClient::IsRunning() const {
    return m_Running && m_Process && m_Process->IsRunning();
}

void LSPClient::DidClose(const std::string& filePath) {
    JsonObject params;
    JsonObject textDocument;
    textDocument["uri"] = FilePathToURI(filePath);
    params["textDocument"] = textDocument;
    
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_FileVersions.erase(filePath);
    }
    
    SendNotification("textDocument/didClose", params);
}

void LSPClient::RequestCompletion(const std::string& filePath, int line, int character, CompletionCallback callback) {
    JsonObject params;
    JsonObject textDocument;
    textDocument["uri"] = FilePathToURI(filePath);
    params["textDocument"] = textDocument;
    
    LSPPosition pos{line, character};
    params["position"] = pos.ToJson();
    
    SendRequest("textDocument/completion", params, [callback](const JsonValue& result) {
        std::vector<LSPCompletionItem> items;
        
        JsonArray itemsJson;
        if (result.type == JsonType::Array) {
            itemsJson = result.AsArray();
        } else if (result.type == JsonType::Object && result.Has("items")) {
            itemsJson = result["items"].AsArray();
        }
        
        for (const auto& item : itemsJson) {
            LSPCompletionItem completion;
            completion.label = item["label"].ToString();
            completion.kind = item["kind"].ToInt();
            if (item.Has("detail")) completion.detail = item["detail"].ToString();
            if (item.Has("documentation")) completion.documentation = item["documentation"].ToString(); // Could be object
            if (item.Has("insertText")) completion.insertText = item["insertText"].ToString();
            else completion.insertText = completion.label;
            
            items.push_back(completion);
        }
        
        if (callback) callback(items);
    });
}

void LSPClient::SetDiagnosticsCallback(DiagnosticsCallback callback) {
    m_DiagnosticsCallback = callback;
}

std::string LSPClient::FilePathToURI(const std::string& path) {
    return "file://" + path;
}

} // namespace sol
