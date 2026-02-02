#include "../file_dialog.h"
#include "../logger.h"

#include <cstdlib>
#include <array>
#include <memory>

namespace sol {

static std::string ExecuteCommand(const std::string& cmd) {
    std::array<char, 256> buffer;
    std::string result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) {
        return "";
    }
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    if (!result.empty() && result.back() == '\n') {
        result.pop_back();
    }
    return result;
}

static bool HasZenity() {
    return system("which zenity > /dev/null 2>&1") == 0;
}

static bool HasKdialog() {
    return system("which kdialog > /dev/null 2>&1") == 0;
}

std::optional<std::filesystem::path> FileDialog::OpenFile(
    const std::string& title,
    const std::vector<FileFilter>& filters,
    const std::filesystem::path& defaultPath) {
    
    std::string cmd;
    
    if (HasZenity()) {
        cmd = "zenity --file-selection --title=\"" + title + "\"";
        if (!defaultPath.empty()) {
            cmd += " --filename=\"" + defaultPath.string() + "\"";
        }
    } else if (HasKdialog()) {
        cmd = "kdialog --getopenfilename";
        if (!defaultPath.empty()) {
            cmd += " \"" + defaultPath.string() + "\"";
        }
        cmd += " --title \"" + title + "\"";
    } else {
        Logger::Error("No file dialog available (install zenity or kdialog)");
        return std::nullopt;
    }
    
    std::string result = ExecuteCommand(cmd);
    if (!result.empty()) {
        return std::filesystem::path(result);
    }
    return std::nullopt;
}

std::vector<std::filesystem::path> FileDialog::OpenFiles(
    const std::string& title,
    const std::vector<FileFilter>& filters,
    const std::filesystem::path& defaultPath) {
    
    std::vector<std::filesystem::path> results;
    std::string cmd;
    
    if (HasZenity()) {
        cmd = "zenity --file-selection --multiple --separator=\"|\" --title=\"" + title + "\"";
    } else if (HasKdialog()) {
        cmd = "kdialog --getopenfilename --multiple --title \"" + title + "\"";
    } else {
        Logger::Error("No file dialog available (install zenity or kdialog)");
        return results;
    }
    
    std::string result = ExecuteCommand(cmd);
    if (!result.empty()) {
        size_t pos = 0;
        std::string delimiter = "|";
        while ((pos = result.find(delimiter)) != std::string::npos) {
            results.push_back(std::filesystem::path(result.substr(0, pos)));
            result.erase(0, pos + delimiter.length());
        }
        if (!result.empty()) {
            results.push_back(std::filesystem::path(result));
        }
    }
    return results;
}

std::optional<std::filesystem::path> FileDialog::OpenFolder(
    const std::string& title,
    const std::filesystem::path& defaultPath) {
    
    std::string cmd;
    
    if (HasZenity()) {
        cmd = "zenity --file-selection --directory --title=\"" + title + "\"";
        if (!defaultPath.empty()) {
            cmd += " --filename=\"" + defaultPath.string() + "/\"";
        }
    } else if (HasKdialog()) {
        cmd = "kdialog --getexistingdirectory";
        if (!defaultPath.empty()) {
            cmd += " \"" + defaultPath.string() + "\"";
        }
        cmd += " --title \"" + title + "\"";
    } else {
        Logger::Error("No file dialog available (install zenity or kdialog)");
        return std::nullopt;
    }
    
    std::string result = ExecuteCommand(cmd);
    if (!result.empty()) {
        return std::filesystem::path(result);
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> FileDialog::SaveFile(
    const std::string& title,
    const std::vector<FileFilter>& filters,
    const std::filesystem::path& defaultPath) {
    
    std::string cmd;
    
    if (HasZenity()) {
        cmd = "zenity --file-selection --save --confirm-overwrite --title=\"" + title + "\"";
        if (!defaultPath.empty()) {
            cmd += " --filename=\"" + defaultPath.string() + "\"";
        }
    } else if (HasKdialog()) {
        cmd = "kdialog --getsavefilename";
        if (!defaultPath.empty()) {
            cmd += " \"" + defaultPath.string() + "\"";
        }
        cmd += " --title \"" + title + "\"";
    } else {
        Logger::Error("No file dialog available (install zenity or kdialog)");
        return std::nullopt;
    }
    
    std::string result = ExecuteCommand(cmd);
    if (!result.empty()) {
        return std::filesystem::path(result);
    }
    return std::nullopt;
}

} // namespace sol
