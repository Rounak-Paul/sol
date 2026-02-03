#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>

namespace sol {

class Process {
public:
    Process(const std::string& command, const std::vector<std::string>& args);
    ~Process();

    bool Start();
    void Stop();
    
    // Write data to the process's stdin
    bool Write(const std::string& data);
    
    // Read data from the process's stdout (non-blocking)
    // Returns bytes read, or 0 if nothing available
    size_t Read(char* buffer, size_t size);
    
    // Read stderr (non-blocking)
    size_t ReadErr(char* buffer, size_t size);
    
    bool IsRunning() const;
    int GetExitCode() const;

private:
    std::string m_Command;
    std::vector<std::string> m_Args;
    
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace sol
