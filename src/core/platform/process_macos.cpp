#include "process.h"
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <cstring>
#include <vector>
#include <errno.h>

namespace sol {

struct Process::Impl {
    pid_t pid = -1;
    int stdinPipe[2] = {-1, -1};
    int stdoutPipe[2] = {-1, -1};
    int stderrPipe[2] = {-1, -1};
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
        close(m_Impl->stdinPipe[0]);
        close(m_Impl->stdinPipe[1]);
        close(m_Impl->stdoutPipe[0]);
        close(m_Impl->stdoutPipe[1]);
        close(m_Impl->stderrPipe[0]);
        close(m_Impl->stderrPipe[1]);
        return false;
    }

    if (m_Impl->pid == 0) {
        // Child process
        dup2(m_Impl->stdinPipe[0], STDIN_FILENO);
        close(m_Impl->stdinPipe[0]);
        close(m_Impl->stdinPipe[1]);

        dup2(m_Impl->stdoutPipe[1], STDOUT_FILENO);
        close(m_Impl->stdoutPipe[0]);
        close(m_Impl->stdoutPipe[1]);

        dup2(m_Impl->stderrPipe[1], STDERR_FILENO);
        close(m_Impl->stderrPipe[0]);
        close(m_Impl->stderrPipe[1]);

        std::vector<char*> args;
        args.push_back(const_cast<char*>(m_Command.c_str()));
        for (const auto& arg : m_Args) {
            args.push_back(const_cast<char*>(arg.c_str()));
        }
        args.push_back(nullptr);

        execvp(m_Command.c_str(), args.data());
        _exit(1);
    }

    // Parent process
    close(m_Impl->stdinPipe[0]);
    close(m_Impl->stdoutPipe[1]);
    close(m_Impl->stderrPipe[1]);

    int flags = fcntl(m_Impl->stdoutPipe[0], F_GETFL, 0);
    fcntl(m_Impl->stdoutPipe[0], F_SETFL, flags | O_NONBLOCK);

    flags = fcntl(m_Impl->stderrPipe[0], F_GETFL, 0);
    fcntl(m_Impl->stderrPipe[0], F_SETFL, flags | O_NONBLOCK);

    m_Impl->running = true;
    return true;
}

void Process::Stop() {
    if (!m_Impl->running) return;

    if (m_Impl->pid > 0) {
        kill(m_Impl->pid, SIGTERM);
        int status;
        waitpid(m_Impl->pid, &status, 0);
        if (WIFEXITED(status)) {
            m_Impl->exitCode = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            m_Impl->exitCode = 128 + WTERMSIG(status);
        }
    }

    if (m_Impl->stdinPipe[1] != -1) {
        close(m_Impl->stdinPipe[1]);
        m_Impl->stdinPipe[1] = -1;
    }
    if (m_Impl->stdoutPipe[0] != -1) {
        close(m_Impl->stdoutPipe[0]);
        m_Impl->stdoutPipe[0] = -1;
    }
    if (m_Impl->stderrPipe[0] != -1) {
        close(m_Impl->stderrPipe[0]);
        m_Impl->stderrPipe[0] = -1;
    }

    m_Impl->running = false;
}

bool Process::Write(const std::string& data) {
    if (!m_Impl->running || m_Impl->stdinPipe[1] == -1) return false;
    
    ssize_t written = write(m_Impl->stdinPipe[1], data.c_str(), data.length());
    return written == static_cast<ssize_t>(data.length());
}

size_t Process::Read(char* buffer, size_t size) {
    if (!m_Impl->running || m_Impl->stdoutPipe[0] == -1) return 0;
    
    ssize_t bytesRead = read(m_Impl->stdoutPipe[0], buffer, size);
    if (bytesRead < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        return 0;
    }
    return static_cast<size_t>(bytesRead);
}

size_t Process::ReadErr(char* buffer, size_t size) {
    if (!m_Impl->running || m_Impl->stderrPipe[0] == -1) return 0;
    
    ssize_t bytesRead = read(m_Impl->stderrPipe[0], buffer, size);
    if (bytesRead < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        return 0;
    }
    return static_cast<size_t>(bytesRead);
}

bool Process::IsRunning() const {
    if (!m_Impl->running) return false;
    
    int status;
    pid_t result = waitpid(m_Impl->pid, &status, WNOHANG);
    if (result == 0) {
        return true;
    }
    
    const_cast<Process*>(this)->m_Impl->running = false;
    if (WIFEXITED(status)) {
        const_cast<Process*>(this)->m_Impl->exitCode = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        const_cast<Process*>(this)->m_Impl->exitCode = 128 + WTERMSIG(status);
    }
    return false;
}

int Process::GetExitCode() const {
    return m_Impl->exitCode;
}

} // namespace sol
