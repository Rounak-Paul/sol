#include "process.h"
#include <windows.h>
#include <string>
#include <vector>

namespace sol {

struct Process::Impl {
    HANDLE hProcess = nullptr;
    HANDLE hThread = nullptr;
    HANDLE hStdinWrite = nullptr;
    HANDLE hStdoutRead = nullptr;
    HANDLE hStderrRead = nullptr;
    bool running = false;
    DWORD exitCode = 0;
};

Process::Process(const std::string& command, const std::vector<std::string>& args)
    : m_Command(command), m_Args(args), m_Impl(std::make_unique<Impl>()) {
}

Process::~Process() {
    Stop();
}

bool Process::Start() {
    if (m_Impl->running) return false;

    SECURITY_ATTRIBUTES saAttr;
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    saAttr.bInheritHandle = TRUE;
    saAttr.lpSecurityDescriptor = nullptr;

    HANDLE hStdinRead = nullptr;
    HANDLE hStdoutWrite = nullptr;
    HANDLE hStderrWrite = nullptr;

    if (!CreatePipe(&hStdinRead, &m_Impl->hStdinWrite, &saAttr, 0) ||
        !SetHandleInformation(m_Impl->hStdinWrite, HANDLE_FLAG_INHERIT, 0)) {
        return false;
    }

    if (!CreatePipe(&m_Impl->hStdoutRead, &hStdoutWrite, &saAttr, 0) ||
        !SetHandleInformation(m_Impl->hStdoutRead, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(hStdinRead);
        CloseHandle(m_Impl->hStdinWrite);
        return false;
    }

    if (!CreatePipe(&m_Impl->hStderrRead, &hStderrWrite, &saAttr, 0) ||
        !SetHandleInformation(m_Impl->hStderrRead, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(hStdinRead);
        CloseHandle(m_Impl->hStdinWrite);
        CloseHandle(m_Impl->hStdoutRead);
        CloseHandle(hStdoutWrite);
        return false;
    }

    PROCESS_INFORMATION piProcInfo;
    ZeroMemory(&piProcInfo, sizeof(PROCESS_INFORMATION));

    STARTUPINFOA siStartInfo;
    ZeroMemory(&siStartInfo, sizeof(STARTUPINFOA));
    siStartInfo.cb = sizeof(STARTUPINFOA);
    siStartInfo.hStdError = hStderrWrite;
    siStartInfo.hStdOutput = hStdoutWrite;
    siStartInfo.hStdInput = hStdinRead;
    siStartInfo.dwFlags |= STARTF_USESTDHANDLES;

    std::string cmdline = m_Command;
    for (const auto& arg : m_Args) {
        cmdline += " \"" + arg + "\"";
    }

    std::vector<char> cmdlineBuf(cmdline.begin(), cmdline.end());
    cmdlineBuf.push_back('\0');

    BOOL success = CreateProcessA(
        nullptr,
        cmdlineBuf.data(),
        nullptr,
        nullptr,
        TRUE,
        0,
        nullptr,
        nullptr,
        &siStartInfo,
        &piProcInfo
    );

    CloseHandle(hStdinRead);
    CloseHandle(hStdoutWrite);
    CloseHandle(hStderrWrite);

    if (!success) {
        CloseHandle(m_Impl->hStdinWrite);
        CloseHandle(m_Impl->hStdoutRead);
        CloseHandle(m_Impl->hStderrRead);
        return false;
    }

    m_Impl->hProcess = piProcInfo.hProcess;
    m_Impl->hThread = piProcInfo.hThread;
    m_Impl->running = true;

    return true;
}

void Process::Stop() {
    if (!m_Impl->running) return;

    if (m_Impl->hProcess) {
        TerminateProcess(m_Impl->hProcess, 1);
        WaitForSingleObject(m_Impl->hProcess, INFINITE);
        GetExitCodeProcess(m_Impl->hProcess, &m_Impl->exitCode);
        CloseHandle(m_Impl->hProcess);
        m_Impl->hProcess = nullptr;
    }

    if (m_Impl->hThread) {
        CloseHandle(m_Impl->hThread);
        m_Impl->hThread = nullptr;
    }

    if (m_Impl->hStdinWrite) {
        CloseHandle(m_Impl->hStdinWrite);
        m_Impl->hStdinWrite = nullptr;
    }

    if (m_Impl->hStdoutRead) {
        CloseHandle(m_Impl->hStdoutRead);
        m_Impl->hStdoutRead = nullptr;
    }

    if (m_Impl->hStderrRead) {
        CloseHandle(m_Impl->hStderrRead);
        m_Impl->hStderrRead = nullptr;
    }

    m_Impl->running = false;
}

bool Process::Write(const std::string& data) {
    if (!m_Impl->running || !m_Impl->hStdinWrite) return false;

    DWORD written;
    BOOL success = WriteFile(m_Impl->hStdinWrite, data.c_str(), 
                             static_cast<DWORD>(data.length()), &written, nullptr);
    return success && written == data.length();
}

size_t Process::Read(char* buffer, size_t size) {
    if (!m_Impl->running || !m_Impl->hStdoutRead) return 0;

    DWORD bytesAvail = 0;
    if (!PeekNamedPipe(m_Impl->hStdoutRead, nullptr, 0, nullptr, &bytesAvail, nullptr)) {
        return 0;
    }

    if (bytesAvail == 0) return 0;

    DWORD bytesRead = 0;
    DWORD toRead = static_cast<DWORD>(size < bytesAvail ? size : bytesAvail);
    
    if (!ReadFile(m_Impl->hStdoutRead, buffer, toRead, &bytesRead, nullptr)) {
        return 0;
    }

    return static_cast<size_t>(bytesRead);
}

size_t Process::ReadErr(char* buffer, size_t size) {
    if (!m_Impl->running || !m_Impl->hStderrRead) return 0;

    DWORD bytesAvail = 0;
    if (!PeekNamedPipe(m_Impl->hStderrRead, nullptr, 0, nullptr, &bytesAvail, nullptr)) {
        return 0;
    }

    if (bytesAvail == 0) return 0;

    DWORD bytesRead = 0;
    DWORD toRead = static_cast<DWORD>(size < bytesAvail ? size : bytesAvail);
    
    if (!ReadFile(m_Impl->hStderrRead, buffer, toRead, &bytesRead, nullptr)) {
        return 0;
    }

    return static_cast<size_t>(bytesRead);
}

bool Process::IsRunning() const {
    if (!m_Impl->running || !m_Impl->hProcess) return false;

    DWORD exitCode;
    if (GetExitCodeProcess(m_Impl->hProcess, &exitCode)) {
        if (exitCode == STILL_ACTIVE) {
            return true;
        }
        const_cast<Process*>(this)->m_Impl->running = false;
        const_cast<Process*>(this)->m_Impl->exitCode = exitCode;
        return false;
    }

    return false;
}

int Process::GetExitCode() const {
    return static_cast<int>(m_Impl->exitCode);
}

} // namespace sol
