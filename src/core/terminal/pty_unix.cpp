#include "pty.h"

#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <cstring>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <util.h>   // openpty on macOS
#include <pwd.h>
#include <cstdlib>

namespace sol {

struct Pty::Impl {
    int masterFd = -1;
    pid_t childPid = -1;
    int exitCode = 0;
    bool alive = false;
};

Pty::Pty() : m_Impl(std::make_unique<Impl>()) {}

Pty::~Pty() {
    Close();
}

bool Pty::Spawn(const std::string& shell, int rows, int cols, const std::string& workingDir) {
    if (m_Impl->alive) {
        Close();
    }
    
    m_Rows = rows;
    m_Cols = cols;
    
    int masterFd, slaveFd;
    
    // Set up terminal size
    struct winsize ws;
    ws.ws_row = static_cast<unsigned short>(rows);
    ws.ws_col = static_cast<unsigned short>(cols);
    ws.ws_xpixel = 0;
    ws.ws_ypixel = 0;
    
    // Open a pseudo-terminal
    if (openpty(&masterFd, &slaveFd, nullptr, nullptr, &ws) < 0) {
        return false;
    }
    
    pid_t pid = fork();
    
    if (pid < 0) {
        // Fork failed
        close(masterFd);
        close(slaveFd);
        return false;
    }
    
    if (pid == 0) {
        // Child process
        
        // Create a new session and set controlling terminal
        setsid();
        
        // Set the slave as the controlling terminal
        if (ioctl(slaveFd, TIOCSCTTY, 0) < 0) {
            _exit(1);
        }
        
        // Redirect standard I/O to slave
        dup2(slaveFd, STDIN_FILENO);
        dup2(slaveFd, STDOUT_FILENO);
        dup2(slaveFd, STDERR_FILENO);
        
        // Close the original file descriptors
        if (slaveFd > STDERR_FILENO) {
            close(slaveFd);
        }
        close(masterFd);
        
        // Change to working directory if specified
        if (!workingDir.empty()) {
            chdir(workingDir.c_str());
        }
        
        // Get user's shell if not specified
        std::string shellPath = shell;
        if (shellPath.empty()) {
            const char* envShell = getenv("SHELL");
            if (envShell) {
                shellPath = envShell;
            } else {
                struct passwd* pw = getpwuid(getuid());
                if (pw && pw->pw_shell) {
                    shellPath = pw->pw_shell;
                } else {
                    shellPath = "/bin/sh";
                }
            }
        }
        
        // Set up environment
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        
        // Execute the shell
        const char* argv[] = { shellPath.c_str(), nullptr };
        execvp(shellPath.c_str(), const_cast<char* const*>(argv));
        
        // If exec fails
        _exit(1);
    }
    
    // Parent process
    close(slaveFd);
    
    // Set master to non-blocking
    int flags = fcntl(masterFd, F_GETFL, 0);
    fcntl(masterFd, F_SETFL, flags | O_NONBLOCK);
    
    m_Impl->masterFd = masterFd;
    m_Impl->childPid = pid;
    m_Impl->alive = true;
    
    return true;
}

void Pty::Close() {
    if (m_Impl->childPid > 0) {
        // Send SIGHUP to the process group
        kill(-m_Impl->childPid, SIGHUP);
        
        // Wait for the child to exit
        int status;
        int result = waitpid(m_Impl->childPid, &status, WNOHANG);
        if (result == 0) {
            // Child hasn't exited yet, give it some time
            usleep(50000); // 50ms
            result = waitpid(m_Impl->childPid, &status, WNOHANG);
            if (result == 0) {
                // Force kill
                kill(-m_Impl->childPid, SIGKILL);
                waitpid(m_Impl->childPid, &status, 0);
            }
        }
        
        if (WIFEXITED(status)) {
            m_Impl->exitCode = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            m_Impl->exitCode = 128 + WTERMSIG(status);
        }
        
        m_Impl->childPid = -1;
    }
    
    if (m_Impl->masterFd >= 0) {
        close(m_Impl->masterFd);
        m_Impl->masterFd = -1;
    }
    
    m_Impl->alive = false;
}

int Pty::Read(char* buffer, size_t size) {
    if (m_Impl->masterFd < 0) return -1;
    
    ssize_t n = read(m_Impl->masterFd, buffer, size);
    
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;  // No data available
        }
        return -1;  // Error
    }
    
    return static_cast<int>(n);
}

bool Pty::Write(const char* data, size_t size) {
    if (m_Impl->masterFd < 0) return false;
    
    size_t written = 0;
    while (written < size) {
        ssize_t n = write(m_Impl->masterFd, data + written, size - written);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            return false;
        }
        written += static_cast<size_t>(n);
    }
    
    return true;
}

bool Pty::Write(const std::string& data) {
    return Write(data.c_str(), data.size());
}

bool Pty::Resize(int rows, int cols) {
    if (m_Impl->masterFd < 0) return false;
    
    m_Rows = rows;
    m_Cols = cols;
    
    struct winsize ws;
    ws.ws_row = static_cast<unsigned short>(rows);
    ws.ws_col = static_cast<unsigned short>(cols);
    ws.ws_xpixel = 0;
    ws.ws_ypixel = 0;
    
    return ioctl(m_Impl->masterFd, TIOCSWINSZ, &ws) == 0;
}

bool Pty::IsAlive() const {
    if (!m_Impl->alive || m_Impl->childPid <= 0) return false;
    
    int status;
    pid_t result = waitpid(m_Impl->childPid, &status, WNOHANG);
    
    if (result == 0) {
        return true;  // Still running
    }
    
    // Child has exited
    m_Impl->alive = false;
    if (WIFEXITED(status)) {
        m_Impl->exitCode = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        m_Impl->exitCode = 128 + WTERMSIG(status);
    }
    
    return false;
}

int Pty::GetExitCode() const {
    return m_Impl->exitCode;
}

int Pty::GetMasterFd() const {
    return m_Impl->masterFd;
}

} // namespace sol
