#pragma once

#include <string>
#include <memory>
#include <mutex>
#include <fstream>

namespace sol {

class Logger {
public:
    enum class Level {
        Debug,
        Info,
        Warning,
        Error
    };

    static Logger& GetInstance();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    Logger(Logger&&) = delete;
    Logger& operator=(Logger&&) = delete;

    void SetLogFilePath(const std::string& filePath);
    void Log(Level level, const std::string& message);

    // Convenience methods
    void Debug(const std::string& message) { Log(Level::Debug, message); }
    void Info(const std::string& message) { Log(Level::Info, message); }
    void Warning(const std::string& message) { Log(Level::Warning, message); }
    void Error(const std::string& message) { Log(Level::Error, message); }

private:
    Logger() = default;
    ~Logger();

    std::string LevelToString(Level level) const;
    std::string FormatMessage(Level level, const std::string& message) const;

    std::string m_LogFilePath;
    std::unique_ptr<std::ofstream> m_FileStream;
    mutable std::mutex m_Mutex;
};

} // namespace sol
