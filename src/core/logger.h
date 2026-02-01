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

    // Static convenience methods
    static void SetLogFile(const std::string& filePath) { GetInstance().SetLogFilePath(filePath); }
    static void Debug(const std::string& message) { GetInstance().Log(Level::Debug, message); }
    static void Info(const std::string& message) { GetInstance().Log(Level::Info, message); }
    static void Warning(const std::string& message) { GetInstance().Log(Level::Warning, message); }
    static void Error(const std::string& message) { GetInstance().Log(Level::Error, message); }

private:
    Logger() = default;
    ~Logger();

    void SetLogFilePath(const std::string& filePath);
    void Log(Level level, const std::string& message);

    std::string LevelToString(Level level) const;
    std::string FormatMessage(Level level, const std::string& message) const;

    std::string m_LogFilePath;
    std::unique_ptr<std::ofstream> m_FileStream;
    mutable std::mutex m_Mutex;
};

} // namespace sol
