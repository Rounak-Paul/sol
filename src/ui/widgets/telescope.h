#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>
#include <imgui.h>

namespace sol {

struct TelescopeEntry {
    std::filesystem::path fullPath;
    std::string display;  // path relative to root
    int score = 0;
};

class TelescopeWidget {
public:
    using OpenCallback = std::function<void(const std::filesystem::path&)>;

    TelescopeWidget();
    ~TelescopeWidget();

    TelescopeWidget(const TelescopeWidget&) = delete;
    TelescopeWidget& operator=(const TelescopeWidget&) = delete;

    void Open(const std::filesystem::path& rootDir);
    void Close();
    bool IsOpen() const { return m_Open; }

    void SetOpenCallback(OpenCallback cb) { m_OnOpen = std::move(cb); }

    // Call every frame from workspace OnUI
    void Render();

private:
    void StartScan(const std::filesystem::path& rootDir);
    void CancelScan();
    void SubmitFilterJob();
    void SwapPendingResults();
    void LoadPreview(const std::filesystem::path& path);
    void RenderPreview(const ImVec2& size);

    static int ScoreMatch(const std::string& fname, const std::string& relPath,
                          const std::string& lowerQuery);

    bool m_Open = false;
    bool m_WantsFocus = false;
    std::atomic<bool> m_FilterDirty{false};

    // Query
    char m_QueryBuf[256] = {};
    std::string m_LastQuery;

    // File scan (background thread)
    std::filesystem::path m_RootDir;
    std::mutex m_FilesMutex;
    std::vector<std::filesystem::path> m_AllFiles;
    std::thread m_ScanThread;
    std::atomic<bool> m_ScanCancelled{false};
    std::atomic<bool> m_Scanning{false};

    static constexpr int MAX_FILES         = 50000;
    static constexpr int MAX_RESULTS       = 500;
    static constexpr int MAX_PREVIEW_LINES = 120;

    // Parallel filter results (written by job thread, read by main thread)
    std::atomic<uint32_t> m_FilterGeneration{0};
    std::mutex m_PendingMutex;
    std::vector<TelescopeEntry> m_PendingResults;
    uint32_t m_PendingGeneration = 0;
    std::atomic<bool> m_PendingReady{false};

    // Active filtered results (main-thread only after swap)
    std::vector<TelescopeEntry> m_Results;
    int m_SelectedIdx = 0;

    // Preview
    std::string m_PreviewPath;
    std::vector<std::string> m_PreviewLines;
    bool m_PreviewIsBinary = false;

    OpenCallback m_OnOpen;
};

}  // namespace sol
