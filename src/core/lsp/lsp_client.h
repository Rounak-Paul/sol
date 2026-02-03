#pragma once

#include "lsp_types.h"
#include "core/platform/process.h"
#include "core/job_system.h"
#include <functional>
#include <mutex>
#include <unordered_map>
#include <string>
#include <thread>
#include <atomic>

namespace sol {

class LSPClient {
public:
    using CompletionCallback = std::function<void(const std::vector<LSPCompletionItem>&)>;
    using DiagnosticsCallback = std::function<void(const std::string& uri, const std::vector<LSPDiagnostic>&)>;

    LSPClient(const std::string& command, const std::vector<std::string>& args);
    ~LSPClient();

    bool Initialize(const std::string& rootPath);
    void Shutdown();

    // Document sync
    void DidOpen(const std::string& filePath, const std::string& content, const std::string& languageId);
    void DidChange(const std::string& filePath, const std::string& content);
    void DidClose(const std::string& filePath);
    
    // Features
    void RequestCompletion(const std::string& filePath, int line, int character, CompletionCallback callback);
    void SetDiagnosticsCallback(DiagnosticsCallback callback); // Set call back to handle errors

    bool IsRunning() const;

private:
    void SendRequest(const std::string& method, const JsonObject& params, std::function<void(const JsonValue&)> callback = nullptr);
    void SendNotification(const std::string& method, const JsonObject& params);
    void ReadLoop();
    void HandleMessage(const JsonValue& json);
    
    std::unique_ptr<Process> m_Process;
    std::thread m_ReadThread;
    std::atomic<bool> m_Running{false};
    
    int m_NextRequestId = 1;
    std::mutex m_Mutex;
    std::unordered_map<int, std::function<void(const JsonValue&)>> m_PendingRequests;
    
    DiagnosticsCallback m_DiagnosticsCallback;
    
    std::string FilePathToURI(const std::string& path);
};

} // namespace sol
