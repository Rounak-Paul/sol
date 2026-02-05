#include "lsp_manager.h"
#include "core/logger.h"

namespace sol {

namespace {
    // Decode percent-encoded URI path (e.g., %20 -> space)
    std::string DecodeURIPath(const std::string& encoded) {
        std::string result;
        result.reserve(encoded.size());
        for (size_t i = 0; i < encoded.size(); ++i) {
            if (encoded[i] == '%' && i + 2 < encoded.size()) {
                char high = encoded[i + 1];
                char low = encoded[i + 2];
                auto hexVal = [](char c) -> int {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                    return -1;
                };
                int h = hexVal(high);
                int l = hexVal(low);
                if (h >= 0 && l >= 0) {
                    result += static_cast<char>((h << 4) | l);
                    i += 2;
                    continue;
                }
            }
            result += encoded[i];
        }
        return result;
    }
} // anonymous namespace

LSPManager& LSPManager::GetInstance() {
    static LSPManager instance;
    return instance;
}

void LSPManager::Initialize(const std::string& projectRoot) {
    m_ProjectRoot = projectRoot;
    
    // Default configs - Can be overridden
    RegisterServer("cpp", "clangd", {"--background-index", "--header-insertion=never"});
    RegisterServer("python", "pylsp", {});
    RegisterServer("cmake", "cmake-language-server", {});
    
    // Web Technologies
    RegisterServer("html", "vscode-html-language-server", {"--stdio"});
    RegisterServer("css", "vscode-css-language-server", {"--stdio"});
    RegisterServer("javascript", "typescript-language-server", {"--stdio"});
    RegisterServer("typescript", "typescript-language-server", {"--stdio"});
    RegisterServer("javascriptreact", "typescript-language-server", {"--stdio"});
    RegisterServer("typescriptreact", "typescript-language-server", {"--stdio"});
    // Add more defaults...
}

void LSPManager::Shutdown() {
    std::lock_guard<std::mutex> lock(m_Mutex);
    for (auto& [lang, client] : m_Clients) {
        client->Shutdown();
    }
    m_Clients.clear();
}

void LSPManager::RegisterServer(const std::string& languageId, const std::string& command, const std::vector<std::string>& args) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_ServerConfigs[languageId] = {command, args};
}

bool LSPManager::HasServerFor(const std::string& languageId) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_ServerConfigs.count(languageId) > 0;
}

LSPClient* LSPManager::GetClient(const std::string& languageId) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    
    // Check if client already running
    auto it = m_Clients.find(languageId);
    if (it != m_Clients.end()) {
        if (it->second->IsRunning()) {
            return it->second.get();
        }
        // Client died, remove it so we can try restarting
        m_Clients.erase(it);
    }
    
    // Check if we have config
    auto configIt = m_ServerConfigs.find(languageId);
    if (configIt == m_ServerConfigs.end()) {
        return nullptr;
    }
    
    // Start new client
    auto client = std::make_unique<LSPClient>(configIt->second.first, configIt->second.second);
    if (client->Initialize(m_ProjectRoot)) {
        
        // Hook up diagnostics
        client->SetDiagnosticsCallback([this](const std::string& uri, const std::vector<LSPDiagnostic>& diagnostics) {
            if (m_DiagnosticsCallback) {
                // Convert URI back to file path: strip scheme and decode percent-encoding
                std::string path = uri;
                if (path.find("file://") == 0) path = path.substr(7);
                path = DecodeURIPath(path);
                m_DiagnosticsCallback(path, diagnostics);
            }
        });
        
        LSPClient* clientPtr = client.get();
        m_Clients[languageId] = std::move(client);
        return clientPtr;
    }
    
    return nullptr;
}

void LSPManager::DidOpen(const std::string& filePath, const std::string& content, const std::string& languageId) {
    if (auto* client = GetClient(languageId)) {
        client->DidOpen(filePath, content, languageId);
    }
}

void LSPManager::DidChange(const std::string& filePath, const std::string& content, const std::string& languageId) {
    if (auto* client = GetClient(languageId)) {
        client->DidChange(filePath, content);
    }
}

void LSPManager::DidClose(const std::string& filePath, const std::string& languageId) {
    // Only close if we have an active client for this language
    std::lock_guard<std::mutex> lock(m_Mutex);
    auto it = m_Clients.find(languageId);
    if (it != m_Clients.end()) {
        it->second->DidClose(filePath);
    }
}

bool LSPManager::RequestCompletion(const std::string& filePath, const std::string& languageId, int line, int character, LSPClient::CompletionCallback callback) {
    if (auto* client = GetClient(languageId)) {
        client->RequestCompletion(filePath, line, character, callback);
        return true;
    }
    return false;
}

void LSPManager::SetDiagnosticsCallback(GlobalDiagnosticsCallback callback) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_DiagnosticsCallback = callback;
}

} // namespace sol

namespace sol {
bool LSPManager::IsClientActive(const std::string& languageId) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    auto it = m_Clients.find(languageId);
    if (it == m_Clients.end()) return false;
    
    // Check if the process is actually running
    if (!it->second->IsRunning()) {
        // Cleanup dead client? Maybe lazily or here.
        // For now, just report truth.
        return false;
    }
    return true;
}
}
