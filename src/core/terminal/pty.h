#pragma once

#include <string>
#include <memory>

namespace sol {

// Platform-specific PTY implementation
class Pty {
public:
    Pty();
    ~Pty();
    
    // Spawn a shell process with the given dimensions
    bool Spawn(const std::string& shell, int rows, int cols, const std::string& workingDir = "");
    
    // Close the PTY
    void Close();
    
    // Read from the PTY (non-blocking)
    // Returns number of bytes read, or -1 on error, or 0 if nothing available
    int Read(char* buffer, size_t size);
    
    // Write to the PTY
    bool Write(const char* data, size_t size);
    bool Write(const std::string& data);
    
    // Resize the PTY
    bool Resize(int rows, int cols);
    
    // Check if the child process is still running
    bool IsAlive() const;
    
    // Get the exit code of the child process (valid only after IsAlive returns false)
    int GetExitCode() const;
    
    // Get the master file descriptor (for advanced use)
    int GetMasterFd() const;
    
    // Get current dimensions
    int GetRows() const { return m_Rows; }
    int GetCols() const { return m_Cols; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
    int m_Rows = 24;
    int m_Cols = 80;
};

} // namespace sol
