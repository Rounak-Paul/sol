#pragma once

#include "lsp_client.h"
#include <map>
#include <memory>
#include <string>
#include <mutex>
#include <future>

namespace sol {

class LSPManager {
public:
    static LSPManager& GetInstance();
    
    void Initialize(const std::string& projectRoot);
    void Shutdown();
    
    // Register a language server command for a language ID
    void RegisterServer(const std::string& languageId, const std::string& command, const std::vector<std::string>& args);
    
    // Check if we have language support
    bool HasServerFor(const std::string& languageId);
    
    // Check if client is actually active
    bool IsClientActive(const std::string& languageId);
    
    // Document sync
    void DidOpen(const std::string& filePath, const std::string& content, const std::string& languageId);
    void DidChange(const std::string& filePath, const std::string& content, const std::string& languageId);
    void DidClose(const std::string& filePath, const std::string& languageId);
    
    // Features
    // Returns true if request was sent (client active/started), false otherwise
    bool RequestCompletion(const std::string& filePath, const std::string& languageId, int line, int character, LSPClient::CompletionCallback callback);
    
    // Setup callbacks
    // callback: (filePath, diagnostics)
    using GlobalDiagnosticsCallback = std::function<void(const std::string&, const std::vector<LSPDiagnostic>&)>;
    void SetDiagnosticsCallback(GlobalDiagnosticsCallback callback);

private:
    LSPManager() = default;
    
    LSPClient* GetClient(const std::string& languageId);
    
    std::string m_ProjectRoot;
    std::map<std::string, std::unique_ptr<LSPClient>> m_Clients;
    std::map<std::string, std::pair<std::string, std::vector<std::string>>> m_ServerConfigs;
    
    GlobalDiagnosticsCallback m_DiagnosticsCallback;
    std::mutex m_Mutex;
};

} // namespace sol
