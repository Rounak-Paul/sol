#pragma once

#include <string>
#include <vector>
#include <optional>
#include <filesystem>

namespace sol {

struct FileFilter {
    std::string name;        // e.g., "Text Files"
    std::string extensions;  // e.g., "txt,md,cpp"
};

class FileDialog {
public:
    // Open a single file
    static std::optional<std::filesystem::path> OpenFile(
        const std::string& title = "Open File",
        const std::vector<FileFilter>& filters = {},
        const std::filesystem::path& defaultPath = "");
    
    // Open multiple files
    static std::vector<std::filesystem::path> OpenFiles(
        const std::string& title = "Open Files",
        const std::vector<FileFilter>& filters = {},
        const std::filesystem::path& defaultPath = "");
    
    // Open a folder
    static std::optional<std::filesystem::path> OpenFolder(
        const std::string& title = "Open Folder",
        const std::filesystem::path& defaultPath = "");
    
    // Save file dialog
    static std::optional<std::filesystem::path> SaveFile(
        const std::string& title = "Save File",
        const std::vector<FileFilter>& filters = {},
        const std::filesystem::path& defaultPath = "");
};

} // namespace sol
