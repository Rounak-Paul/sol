#include "process.h"
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <cstring>
#include <vector>
#include <stdexcept>

namespace sol {

struct Process::Impl {
    pid_t pid = -1;
    int stdinPipe[2] = {-1, -1};  // Parent writes to [1], Child reads from [0]
    int stdoutPipe[2] = {-1, -1}; // Child writes to [1], Parent reads from [0]
    int stderrPipe[2] = {-1, -1}; // Child writes to [1], Parent reads from [0]
    bool running = false;
    int exitCode = 0;
};

Process::Process(const std::string& command, const std::vector<std::string>& args)
    : m_Command(command), m_Args(args), m_Impl(std::make_unique<Impl>()) {
}

Process::~Process() {
    Stop();
}

bool Process::Start() {
    if (m_Impl->running) return false;

    if (pipe(m_Impl->stdinPipe) < 0 || 
        pipe(m_Impl->stdoutPipe) < 0 || 
        pipe(m_Impl->stderrPipe) < 0) {
        return false;
    }

    m_Impl->pid = fork();

    if (m_Impl->pid < 0) {
        return false;
    }

    if (m_Impl->pid == 0) {
        // Child
        // Redirect stdin
        dup2(m_Impl->stdinPipe[0], STDIN_FILENO);
        close(m_Impl->stdinPipe[1]); // Close write end

        // Redirect stdout
        dup2(m_Impl->stdoutPipe[1], STDOUT_FILENO);
        close(m_Impl->stdoutPipe[0]); // Close read end

        // Redirect stderr
        dup2(m_Impl->stderrPipe[1], STDERR_FILENO);
        close(m_Impl->stderrPipe[0]); // Close read end

        // Prepare args
        std::vector<char*> args;
        args.push_back(const_cast<char*>(m_Command.c_str()));
        for (const auto& arg : m_Args) {
            args.push_back(const_cast<char*>(arg.c_str()));
        }
        args.push_back(nullptr);

        execvp(m_Command.c_str(), args.data());
        exit(1); // Failed
    } else {
        // Parent
        close(m_Impl->stdinPipe[0]);  // Close read end
        close(m_Impl->stdoutPipe[1]); // Close write end
        close(m_Impl->stderrPipe[1]); // Close write end
        
        // Set read pipes to non-blocking
        int flags = fcntl(m_Impl->stdoutPipe[0], F_GETFL, 0);
        fcntl(m_Impl->stdoutPipe[0], F_SETFL, flags | O_NONBLOCK);
        
        flags = fcntl(m_Impl->stderrPipe[0], F_GETFL, 0);
        fcntl(m_Impl->stderrPipe[0], F_SETFL, flags | O_NONBLOCK);

        m_Impl->running = true;
        return true;
    }
}

void Process::Stop() {
    if (!m_Impl->running) return;

    if (m_Impl->pid > 0) {
        kill(m_Impl->pid, SIGTERM);
        waitpid(m_Impl->pid, &m_Impl->exitCode, 0);
    }

    if (m_Impl->stdinPipe[1] != -1) close(m_Impl->stdinPipe[1]);
    if (m_Impl->stdoutPipe[0] != -1) close(m_Impl->stdoutPipe[0]);
    if (m_Impl->stderrPipe[0] != -1) close(m_Impl->stderrPipe[0]);

    m_Impl->running = false;
}

bool Process::Write(const std::string& data) {
    if (!m_Impl->running) return false;
    
    ssize_t written = write(m_Impl->stdinPipe[1], data.c_str(), data.length());
    return written == (ssize_t)data.length();
}

size_t Process::Read(char* buffer, size_t size) {
    if (!m_Impl->running) return 0;
    
    ssize_t bytesRead = read(m_Impl->stdoutPipe[0], buffer, size);
    if (bytesRead < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        return 0; // Error
    }
    return static_cast<size_t>(bytesRead);
}

size_t Process::ReadErr(char* buffer, size_t size) {
    if (!m_Impl->running) return 0;
    
    ssize_t bytesRead = read(m_Impl->stderrPipe[0], buffer, size);
    if (bytesRead < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        return 0; // Error
    }
    return static_cast<size_t>(bytesRead);
}

bool Process::IsRunning() const {
    if (!m_Impl->running) return false;
    
    int status;
    pid_t result = waitpid(m_Impl->pid, &status, WNOHANG);
    if (result == 0) {
        return true; // Still running
    } else {
        // Child exited
        const_cast<Process*>(this)->m_Impl->running = false;
        const_cast<Process*>(this)->m_Impl->exitCode = status;
        return false;
    }
}

int Process::GetExitCode() const {
    return m_Impl->exitCode;
}

} // namespace sol
