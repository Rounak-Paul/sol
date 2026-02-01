#include "logger.h"
#include <iostream>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace sol {

Logger& Logger::GetInstance() {
    static Logger instance;
    return instance;
}

Logger::~Logger() {
    if (m_FileStream && m_FileStream->is_open()) {
        m_FileStream->close();
    }
}

void Logger::SetLogFilePath(const std::string& filePath) {
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (m_FileStream && m_FileStream->is_open()) {
        m_FileStream->close();
    }

    m_LogFilePath = filePath;
    m_FileStream = std::make_unique<std::ofstream>(filePath, std::ios::app);

    if (!m_FileStream->is_open()) {
        std::cerr << "Failed to open log file: " << filePath << std::endl;
        m_FileStream.reset();
    }
}

std::string Logger::LevelToString(Level level) const {
    switch (level) {
        case Level::Debug:   return "DEBUG";
        case Level::Info:    return "INFO";
        case Level::Warning: return "WARN";
        case Level::Error:   return "ERROR";
        default:             return "UNKNOWN";
    }
}

std::string Logger::FormatMessage(Level level, const std::string& message) const {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::ostringstream oss;
    oss << "[" << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S")
        << "." << std::setfill('0') << std::setw(3) << ms.count() << "] "
        << "[" << LevelToString(level) << "] "
        << message;

    return oss.str();
}

void Logger::Log(Level level, const std::string& message) {
    std::lock_guard<std::mutex> lock(m_Mutex);

    std::string formattedMessage = FormatMessage(level, message);

    // Log to terminal
    if (level == Level::Error || level == Level::Warning) {
        std::cerr << formattedMessage << std::endl;
    } else {
        std::cout << formattedMessage << std::endl;
    }

    // Log to file if enabled
    if (m_FileStream && m_FileStream->is_open()) {
        *m_FileStream << formattedMessage << std::endl;
        m_FileStream->flush();
    }
}

} // namespace sol
