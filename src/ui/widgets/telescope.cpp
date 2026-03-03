#include "telescope.h"
#include "core/logger.h"
#include "core/job_system.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <fstream>
#include <cctype>

namespace sol {

TelescopeWidget::TelescopeWidget() = default;

TelescopeWidget::~TelescopeWidget() {
    CancelScan();
}

void TelescopeWidget::Open(const std::filesystem::path& rootDir) {
    m_Open = true;
    m_WantsFocus = true;
    m_QueryBuf[0] = '\0';
    m_LastQuery.clear();
    m_SelectedIdx = 0;
    m_Results.clear();
    m_PreviewPath.clear();
    m_PreviewLines.clear();
    m_PreviewIsBinary = false;
    m_FilterDirty = true;

    // Normalize to avoid trailing-slash / symlink mismatches
    std::filesystem::path canonical;
    try {
        canonical = std::filesystem::weakly_canonical(rootDir);
    } catch (...) {
        canonical = rootDir;
    }

    if (canonical != m_RootDir || m_AllFiles.empty()) {
        m_RootDir = canonical;
        StartScan(canonical);
    }
}

void TelescopeWidget::Close() {
    m_Open = false;
}

void TelescopeWidget::CancelScan() {
    m_ScanCancelled.store(true);
    if (m_ScanThread.joinable())
        m_ScanThread.join();
    m_ScanCancelled.store(false);
}

void TelescopeWidget::StartScan(const std::filesystem::path& rootDir) {
    CancelScan();

    {
        std::lock_guard<std::mutex> lk(m_FilesMutex);
        m_AllFiles.clear();
    }

    m_Scanning.store(true);
    m_ScanThread = std::thread([this, rootDir]() {
        static const std::vector<std::string> s_IgnoreDirs = {
            ".git", ".svn", ".hg", "node_modules", ".cache", "__pycache__",
            "build", "out", "dist", ".idea", ".vscode",
        };

        std::vector<std::filesystem::path> collected;
        collected.reserve(4096);

        try {
            std::filesystem::recursive_directory_iterator it(
                rootDir,
                std::filesystem::directory_options::skip_permission_denied
            );
            std::filesystem::recursive_directory_iterator end;

            for (; it != end && !m_ScanCancelled.load(); ++it) {
                const auto& entry = *it;

                if (entry.is_directory()) {
                    const std::string name = entry.path().filename().string();
                    if (!name.empty() && name[0] == '.') {
                        it.disable_recursion_pending();
                        continue;
                    }
                    for (const auto& ig : s_IgnoreDirs) {
                        if (name == ig) {
                            it.disable_recursion_pending();
                            break;
                        }
                    }
                    continue;
                }

                if (entry.is_regular_file()) {
                    const std::string fname = entry.path().filename().string();
                    if (!fname.empty() && fname[0] == '.') continue;
                    collected.push_back(entry.path());
                    if ((int)collected.size() >= MAX_FILES) break;
                }
            }
        } catch (...) {}

        if (!m_ScanCancelled.load()) {
            std::lock_guard<std::mutex> lk(m_FilesMutex);
            m_AllFiles = std::move(collected);
            m_FilterDirty = true;  // re-run filter now that files are available
        }
        m_Scanning.store(false);
    });
}

// Tiered score (lower = better, INT_MAX = no match).
// Tiers for filename matches:
//   0        : exact
//   1000+    : prefix
//   2000+    : substring (position + length)
//   3000+    : fuzzy (all chars in order, scored by gap penalty & consecutive bonus)
// Path-only matches start at 10000 with the same sub-tiers.
int TelescopeWidget::ScoreMatch(const std::string& fname, const std::string& relPath,
                                const std::string& lowerQuery) {
    if (lowerQuery.empty()) return 0;

    auto makeLower = [](const std::string& s) {
        std::string r = s;
        for (char& c : r) c = (char)std::tolower((unsigned char)c);
        return r;
    };

    const std::string lf   = makeLower(fname);
    const std::string lrel = makeLower(relPath);
    const int qlen = (int)lowerQuery.size();

    // Fuzzy score helper: greedy match all query chars in order.
    // Rewards consecutive runs heavily, penalizes gaps.
    // Returns INT_MAX if not all chars matched.
    auto fuzzyScore = [&](const std::string& text) -> int {
        const int tlen = (int)text.size();
        int qi = 0, last = -1;
        int gap = 0, consecutive = 0;
        for (int ti = 0; ti < tlen && qi < qlen; ++ti) {
            if (text[ti] == lowerQuery[qi]) {
                if (last != -1 && ti == last + 1)
                    ++consecutive;
                else if (last != -1)
                    gap += ti - last - 1;
                last = ti;
                ++qi;
            }
        }
        if (qi < qlen) return INT_MAX;
        // Lower is better: penalize gaps, reward consecutive runs
        return gap * 8 - consecutive * 12 + (tlen - qlen);
    };

    // --- filename tiers ---
    if (lf == lowerQuery)                                return 0;
    if (qlen < (int)lf.size() && lf.substr(0, qlen) == lowerQuery)
        return 1000 + (int)lf.size();
    {
        auto pos = lf.find(lowerQuery);
        if (pos != std::string::npos)
            return 2000 + (int)pos + (int)lf.size();
    }
    {
        int s = fuzzyScore(lf);
        if (s != INT_MAX) return 3000 + s;
    }

    // --- path tiers (only reached when no filename match) ---
    if (lrel == lowerQuery)                              return 10000;
    if (qlen < (int)lrel.size() && lrel.substr(0, qlen) == lowerQuery)
        return 11000 + (int)lrel.size();
    {
        auto pos = lrel.find(lowerQuery);
        if (pos != std::string::npos)
            return 12000 + (int)pos + (int)lrel.size();
    }
    {
        int s = fuzzyScore(lrel);
        if (s != INT_MAX) return 13000 + s;
    }

    return INT_MAX;
}

void TelescopeWidget::SwapPendingResults() {
    if (!m_PendingReady.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lk(m_PendingMutex);
    // Only accept if this is still the latest generation
    if (m_PendingGeneration == m_FilterGeneration.load()) {
        m_Results = std::move(m_PendingResults);
        m_SelectedIdx = 0;
    }
    m_PendingResults.clear();
    m_PendingReady.store(false, std::memory_order_release);
}

void TelescopeWidget::SubmitFilterJob() {
    std::string query(m_QueryBuf);
    if (query == m_LastQuery && !m_FilterDirty.load(std::memory_order_relaxed)) return;
    m_LastQuery  = query;
    m_FilterDirty.store(false, std::memory_order_relaxed);

    // Snapshot files under lock — cheap, just copies pointers
    std::vector<std::filesystem::path> files;
    {
        std::lock_guard<std::mutex> lk(m_FilesMutex);
        files = m_AllFiles;
    }

    if (files.empty() && m_Scanning.load()) return;  // not ready yet, will retry when scan finishes

    const uint32_t gen    = m_FilterGeneration.fetch_add(1) + 1;
    const std::filesystem::path rootDir = m_RootDir;

    std::string lowerQuery = query;
    for (char& c : lowerQuery) c = (char)std::tolower((unsigned char)c);

    auto job = std::make_shared<Job>([this, files = std::move(files), lowerQuery, rootDir, gen](const JobData&) -> bool {
        std::vector<TelescopeEntry> results;
        results.reserve(std::min((int)files.size(), MAX_RESULTS * 2));

        for (const auto& path : files) {
            std::string rel;
            try {
                rel = std::filesystem::relative(path, rootDir).string();
            } catch (...) {
                rel = path.filename().string();
            }

            int score;
            if (lowerQuery.empty()) {
                score = 0;
            } else {
                score = ScoreMatch(path.filename().string(), rel, lowerQuery);
                if (score == INT_MAX) continue;
            }

            results.push_back({path, rel, score});
        }

        std::sort(results.begin(), results.end(), [](const TelescopeEntry& a, const TelescopeEntry& b) {
            return a.score < b.score;
        });
        if ((int)results.size() > MAX_RESULTS)
            results.resize(MAX_RESULTS);

        {
            std::lock_guard<std::mutex> lk(m_PendingMutex);
            m_PendingResults   = std::move(results);
            m_PendingGeneration = gen;
        }
        m_PendingReady.store(true, std::memory_order_release);
        return true;
    });

    JobSystem::Submit(job);
}

static bool IsBinaryFile(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return true;
    char buf[512];
    f.read(buf, sizeof(buf));
    std::streamsize n = f.gcount();
    for (std::streamsize i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)buf[i];
        if (c == 0) return true;
    }
    return false;
}

void TelescopeWidget::LoadPreview(const std::filesystem::path& path) {
    std::string pathStr = path.string();
    if (pathStr == m_PreviewPath) return;
    m_PreviewPath = pathStr;
    m_PreviewLines.clear();
    m_PreviewIsBinary = false;

    if (!std::filesystem::is_regular_file(path)) return;

    auto fileSize = std::filesystem::file_size(path);
    if (fileSize > 1024 * 1024) {
        m_PreviewLines.push_back("[File too large to preview]");
        return;
    }

    if (IsBinaryFile(path)) {
        m_PreviewIsBinary = true;
        m_PreviewLines.push_back("[Binary file]");
        return;
    }

    std::ifstream f(path);
    if (!f) {
        m_PreviewLines.push_back("[Cannot open file]");
        return;
    }

    std::string line;
    int count = 0;
    while (std::getline(f, line) && count < MAX_PREVIEW_LINES) {
        // Replace tabs with spaces for display
        std::string display;
        display.reserve(line.size());
        for (char c : line) {
            if (c == '\t') display += "    ";
            else if ((unsigned char)c >= 32 || c == '\0') display += c;
        }
        m_PreviewLines.push_back(std::move(display));
        ++count;
    }
}

void TelescopeWidget::RenderPreview(const ImVec2& size) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.08f, 0.09f, 1.0f));
    ImGui::BeginChild("##tele_preview", size, false,
                      ImGuiWindowFlags_HorizontalScrollbar);

    if (m_PreviewLines.empty()) {
        float pad = (size.y - ImGui::GetTextLineHeightWithSpacing()) * 0.45f;
        if (pad > 0.0f) ImGui::Dummy(ImVec2(0, pad));
        float textW = ImGui::CalcTextSize("No preview").x;
        ImGui::SetCursorPosX((size.x - textW) * 0.5f);
        ImGui::TextDisabled("No preview");
    } else {
        ImGuiListClipper clipper;
        clipper.Begin((int)m_PreviewLines.size());
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                // Line number gutter
                ImGui::TextDisabled("%4d ", i + 1);
                ImGui::SameLine();
                ImGui::TextUnformatted(m_PreviewLines[i].c_str());
            }
        }
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void TelescopeWidget::Render() {
    if (!m_Open) return;

    // Swap in completed filter job results, then submit a new job if query changed
    SwapPendingResults();
    SubmitFilterJob();

    ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    float w = displaySize.x * 0.7f;
    float h = displaySize.y * 0.7f;
    float x = (displaySize.x - w) * 0.5f;
    float y = (displaySize.y - h) * 0.5f;

    // Dim background overlay (drawn into the background drawlist so it sits below the popup)
    ImGui::GetBackgroundDrawList()->AddRectFilled(
        ImVec2(0, 0), displaySize, IM_COL32(0, 0, 0, 140));

    ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
    ImGui::SetNextWindowFocus();

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNavInputs;

    ImGui::PushStyleColor(ImGuiCol_WindowBg,  ImVec4(0.10f, 0.10f, 0.11f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border,    ImVec4(0.35f, 0.55f, 0.90f, 0.75f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,   8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(10.0f, 10.0f));

    ImGui::Begin("##telescope", nullptr, flags);

    // Close on Escape
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        Close();
        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);
        return;
    }

    if (m_WantsFocus) {
        ImGui::SetKeyboardFocusHere(0);
        m_WantsFocus = false;
    }

    float totalW = ImGui::GetContentRegionAvail().x;
    float totalH = ImGui::GetContentRegionAvail().y;

    // ----- Search bar -----
    ImGui::PushStyleColor(ImGuiCol_FrameBg,        ImVec4(0.14f, 0.14f, 0.16f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.17f, 0.17f, 0.20f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,  ImVec4(0.17f, 0.17f, 0.20f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,  ImVec2(8.0f, 6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);

    ImGui::SetNextItemWidth(totalW);
    ImGuiInputTextFlags searchFlags = ImGuiInputTextFlags_AutoSelectAll;

    bool queryChanged = ImGui::InputText("##tele_query", m_QueryBuf, sizeof(m_QueryBuf), searchFlags);
    if (queryChanged) m_FilterDirty.store(true, std::memory_order_relaxed);

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(3);

    // Separator drawn on draw list; Dummy advances cursor cleanly
    {
        ImVec2 sepPos = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(sepPos.x - 10.0f, sepPos.y + 3.0f),
            ImVec2(sepPos.x + totalW + 10.0f, sepPos.y + 3.0f),
            IM_COL32(60, 80, 130, 200), 1.0f);
        ImGui::Dummy(ImVec2(totalW, 8.0f));
    }

    // Navigate with keys while search bar is focused
    bool moveUp   = ImGui::IsKeyPressed(ImGuiKey_UpArrow,   true);
    bool moveDown = ImGui::IsKeyPressed(ImGuiKey_DownArrow, true);
    bool doOpen   = ImGui::IsKeyPressed(ImGuiKey_Enter,     false);

    if (moveDown && !m_Results.empty())
        m_SelectedIdx = std::min(m_SelectedIdx + 1, (int)m_Results.size() - 1);
    if (moveUp && !m_Results.empty())
        m_SelectedIdx = std::max(m_SelectedIdx - 1, 0);

    if (doOpen && (int)m_SelectedIdx < (int)m_Results.size()) {
        if (m_OnOpen) m_OnOpen(m_Results[m_SelectedIdx].fullPath);
        Close();
        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);
        return;
    }

    // ----- Body: results list | preview -----
    float bodyH = ImGui::GetContentRegionAvail().y;
    float listW = totalW * 0.40f;
    float previewW = totalW - listW - 6.0f;

    // Results list
    ImGui::PushStyleColor(ImGuiCol_ChildBg,    ImVec4(0.09f, 0.09f, 0.10f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Header,     ImVec4(0.20f, 0.35f, 0.65f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.22f, 0.38f, 0.68f, 0.85f));
    ImGui::BeginChild("##tele_list", ImVec2(listW, bodyH), false,
                      ImGuiWindowFlags_NoScrollbar);

    if (m_Scanning.load() && m_Results.empty()) {
        float pad = (bodyH - ImGui::GetTextLineHeightWithSpacing()) * 0.45f;
        if (pad > 0.0f) ImGui::Dummy(ImVec2(0, pad));
        float tw = ImGui::CalcTextSize("Scanning...").x;
        ImGui::SetCursorPosX((listW - tw) * 0.5f);
        ImGui::TextDisabled("Scanning...");
    } else if (m_Results.empty()) {
        float pad = (bodyH - ImGui::GetTextLineHeightWithSpacing()) * 0.45f;
        if (pad > 0.0f) ImGui::Dummy(ImVec2(0, pad));
        float tw = ImGui::CalcTextSize("No results").x;
        ImGui::SetCursorPosX((listW - tw) * 0.5f);
        ImGui::TextDisabled("No results");
    } else {
        // Scroll to keep selection visible
        float lineH = ImGui::GetTextLineHeightWithSpacing();
        float scrollY = ImGui::GetScrollY();
        float visH = bodyH;
        float selTop    = m_SelectedIdx * lineH;
        float selBottom = selTop + lineH;
        if (selBottom > scrollY + visH) ImGui::SetScrollY(selBottom - visH + 2.0f);
        if (selTop    < scrollY)        ImGui::SetScrollY(selTop);

        ImGuiListClipper clipper;
        clipper.Begin((int)m_Results.size(), lineH);
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                bool selected = (i == m_SelectedIdx);
                ImGui::PushID(i);
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 2.0f));
                if (ImGui::Selectable(m_Results[i].display.c_str(), selected,
                                      ImGuiSelectableFlags_AllowDoubleClick,
                                      ImVec2(listW - 8.0f, 0))) {
                    m_SelectedIdx = i;
                    if (ImGui::IsMouseDoubleClicked(0)) {
                        if (m_OnOpen) m_OnOpen(m_Results[i].fullPath);
                        Close();
                    }
                }
                ImGui::PopStyleVar();
                ImGui::PopID();
            }
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(3);

    // Vertical divider — use Dummy so ImGui tracks the size properly
    ImGui::SameLine(0, 0);
    ImVec2 divTop = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(
        divTop, ImVec2(divTop.x + 4.0f, divTop.y + bodyH), IM_COL32(40, 55, 90, 220));
    ImGui::Dummy(ImVec2(4.0f, bodyH));
    ImGui::SameLine(0, 2.0f);

    // Preview pane — load preview for currently selected entry
    if (!m_Results.empty() && m_SelectedIdx < (int)m_Results.size()) {
        LoadPreview(m_Results[m_SelectedIdx].fullPath);
        RenderPreview(ImVec2(previewW, bodyH));
    } else {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.08f, 0.09f, 1.0f));
        ImGui::BeginChild("##tele_preview", ImVec2(previewW, bodyH));
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

}  // namespace sol
