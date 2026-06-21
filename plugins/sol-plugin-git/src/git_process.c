#include "git_plugin.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Build a Git argv that selects the repository without changing process cwd. */
static const char **git_process_arguments(const char *cwd,
                                          const char *const argv[])
{
    size_t count = 0u;
    while (argv[count]) ++count;
    const char **arguments = (const char **)calloc(count + 3u,
                                                   sizeof(const char *));
    if (!arguments) return NULL;
    arguments[0] = argv[0];
    arguments[1] = "-C";
    arguments[2] = cwd;
    for (size_t i = 1u; i < count; ++i) arguments[i + 2u] = argv[i];
    return arguments;
}

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;
#endif

/* Append captured bytes while continuing to drain data after truncation. */
static void git_capture_append(char *output,
                               size_t output_capacity,
                               size_t *length,
                               bool *truncated,
                               const char *data,
                               size_t data_length)
{
    if (!length || !truncated || !data) return;
    const size_t usable = output_capacity > 0u ? output_capacity - 1u : 0u;
    const size_t available = *length < usable ? usable - *length : 0u;
    const size_t copy = data_length < available ? data_length : available;
    if (copy > 0u && output) memcpy(output + *length, data, copy);
    *length += copy;
    if (copy < data_length) *truncated = true;
    if (output_capacity > 0u && output) output[*length] = '\0';
}

#if defined(_WIN32)

/* Convert a UTF-8 string to a newly allocated UTF-16 string. */
static wchar_t *git_utf8_to_wide(const char *text)
{
    if (!text) return NULL;
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                    text, -1, NULL, 0);
    if (count <= 0) return NULL;
    wchar_t *wide = (wchar_t *)calloc((size_t)count, sizeof(wchar_t));
    if (!wide) return NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                            text, -1, wide, count) <= 0) {
        free(wide);
        return NULL;
    }
    return wide;
}

/* Return the UTF-16 command-line length needed for one quoted argv item. */
static size_t git_windows_arg_length(const wchar_t *arg)
{
    size_t length = 2u;
    size_t slashes = 0u;
    for (const wchar_t *p = arg; *p; ++p) {
        if (*p == L'\\') {
            ++slashes;
        } else if (*p == L'"') {
            length += slashes * 2u + 2u;
            slashes = 0u;
        } else {
            length += slashes + 1u;
            slashes = 0u;
        }
    }
    return length + slashes * 2u;
}

/* Append one argv item using the Windows C runtime quoting rules. */
static wchar_t *git_windows_append_arg(wchar_t *dst, const wchar_t *arg)
{
    *dst++ = L'"';
    size_t slashes = 0u;
    for (const wchar_t *p = arg; *p; ++p) {
        if (*p == L'\\') {
            ++slashes;
            continue;
        }
        if (*p == L'"') {
            for (size_t i = 0u; i < slashes * 2u + 1u; ++i) *dst++ = L'\\';
            *dst++ = L'"';
        } else {
            for (size_t i = 0u; i < slashes; ++i) *dst++ = L'\\';
            *dst++ = *p;
        }
        slashes = 0u;
    }
    for (size_t i = 0u; i < slashes * 2u; ++i) *dst++ = L'\\';
    *dst++ = L'"';
    return dst;
}

/* Build a mutable UTF-16 command line from a NULL-terminated argv array. */
static wchar_t *git_windows_command_line(const char *const argv[])
{
    size_t count = 0u;
    while (argv[count]) ++count;
    wchar_t **wide_args = (wchar_t **)calloc(count, sizeof(wchar_t *));
    if (!wide_args) return NULL;

    size_t total = 1u;
    for (size_t i = 0u; i < count; ++i) {
        wide_args[i] = git_utf8_to_wide(argv[i]);
        if (!wide_args[i]) {
            for (size_t j = 0u; j < i; ++j) free(wide_args[j]);
            free(wide_args);
            return NULL;
        }
        total += git_windows_arg_length(wide_args[i]) + 1u;
    }

    wchar_t *command = (wchar_t *)calloc(total, sizeof(wchar_t));
    if (!command) {
        for (size_t i = 0u; i < count; ++i) free(wide_args[i]);
        free(wide_args);
        return NULL;
    }
    wchar_t *cursor = command;
    for (size_t i = 0u; i < count; ++i) {
        if (i > 0u) *cursor++ = L' ';
        cursor = git_windows_append_arg(cursor, wide_args[i]);
        free(wide_args[i]);
    }
    *cursor = L'\0';
    free(wide_args);
    return command;
}

/* Run Git on Windows with redirected output and a bounded wait. */
GitProcessResult git_process_run(const char *cwd,
                                 const char *const argv[],
                                 char *output,
                                 size_t output_capacity,
                                 uint32_t timeout_ms)
{
    GitProcessResult result = { .exit_code = -1 };
    if (!cwd || !argv || !argv[0] || !output || output_capacity == 0u) return result;
    output[0] = '\0';

    const char **arguments = git_process_arguments(cwd, argv);
    wchar_t *command = arguments ? git_windows_command_line(arguments) : NULL;
    if (!command) {
        free(arguments);
        free(command);
        return result;
    }

    SECURITY_ATTRIBUTES security = {
        .nLength = sizeof(SECURITY_ATTRIBUTES),
        .bInheritHandle = TRUE,
    };
    HANDLE read_pipe = NULL;
    HANDLE write_pipe = NULL;
    if (!CreatePipe(&read_pipe, &write_pipe, &security, 0) ||
        !SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0)) {
        if (read_pipe) CloseHandle(read_pipe);
        if (write_pipe) CloseHandle(write_pipe);
        free(arguments);
        free(command);
        return result;
    }

    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    memset(&startup, 0, sizeof(startup));
    memset(&process, 0, sizeof(process));
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write_pipe;
    startup.hStdError = write_pipe;
    HANDLE null_input = CreateFileW(L"NUL", GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    &security, OPEN_EXISTING, 0, NULL);
    startup.hStdInput = null_input != INVALID_HANDLE_VALUE ? null_input : NULL;

    BOOL created = CreateProcessW(NULL, command, NULL, NULL, TRUE,
                                  CREATE_NO_WINDOW, NULL, NULL,
                                  &startup, &process);
    if (null_input != INVALID_HANDLE_VALUE) CloseHandle(null_input);
    CloseHandle(write_pipe);
    free(arguments);
    free(command);
    if (!created) {
        CloseHandle(read_pipe);
        return result;
    }

    const ULONGLONG started = GetTickCount64();
    bool running = true;
    while (running) {
        DWORD available = 0u;
        while (PeekNamedPipe(read_pipe, NULL, 0, NULL, &available, NULL) &&
               available > 0u) {
            char chunk[8192];
            DWORD read_count = 0u;
            DWORD requested = available < sizeof(chunk) ? available : sizeof(chunk);
            if (!ReadFile(read_pipe, chunk, requested, &read_count, NULL) ||
                read_count == 0u) break;
            git_capture_append(output, output_capacity, &result.output_len,
                               &result.truncated, chunk, (size_t)read_count);
        }

        DWORD process_status = WaitForSingleObject(process.hProcess, 10u);
        if (process_status == WAIT_OBJECT_0) {
            running = false;
        } else if (timeout_ms > 0u && GetTickCount64() - started >= timeout_ms) {
            TerminateProcess(process.hProcess, 124u);
            WaitForSingleObject(process.hProcess, INFINITE);
            result.timed_out = true;
            running = false;
        }
    }

    for (;;) {
        char chunk[8192];
        DWORD read_count = 0u;
        if (!ReadFile(read_pipe, chunk, sizeof(chunk), &read_count, NULL) ||
            read_count == 0u) break;
        git_capture_append(output, output_capacity, &result.output_len,
                           &result.truncated, chunk, (size_t)read_count);
    }

    DWORD exit_code = 1u;
    if (GetExitCodeProcess(process.hProcess, &exit_code)) {
        result.exit_code = (int)exit_code;
    }
    CloseHandle(read_pipe);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return result;
}

#else

/* Return whether an environment entry defines the requested variable. */
static bool git_environment_has_name(const char *entry, const char *name)
{
    const size_t length = strlen(name);
    return strncmp(entry, name, length) == 0 && entry[length] == '=';
}

/* Build a child environment with deterministic, non-interactive Git settings. */
static char **git_child_environment(void)
{
    size_t source_count = 0u;
    while (environ && environ[source_count]) ++source_count;
    char **environment = (char **)calloc(source_count + 4u, sizeof(char *));
    if (!environment) return NULL;

    size_t output_count = 0u;
    for (size_t i = 0u; i < source_count; ++i) {
        if (git_environment_has_name(environ[i], "GIT_TERMINAL_PROMPT") ||
            git_environment_has_name(environ[i], "GIT_SSH_COMMAND") ||
            git_environment_has_name(environ[i], "LC_ALL")) continue;
        environment[output_count++] = environ[i];
    }
    environment[output_count++] = "GIT_TERMINAL_PROMPT=0";
    environment[output_count++] = "GIT_SSH_COMMAND=ssh -oBatchMode=yes";
    environment[output_count++] = "LC_ALL=C";
    environment[output_count] = NULL;
    return environment;
}

/* Resolve an executable through PATH before spawning the child. */
static char *git_resolve_executable(const char *name)
{
    if (!name || !name[0]) return NULL;
    if (strchr(name, '/')) {
        char *copy = (char *)malloc(strlen(name) + 1u);
        if (copy) strcpy(copy, name);
        return copy;
    }

    const char *path = getenv("PATH");
    if (!path || !path[0]) path = "/usr/local/bin:/usr/bin:/bin";
    const char *cursor = path;
    while (*cursor) {
        const char *end = strchr(cursor, ':');
        if (!end) end = cursor + strlen(cursor);
        const size_t directory_length = (size_t)(end - cursor);
        const size_t prefix_length = directory_length > 0u ? directory_length : 1u;
        const size_t total = prefix_length + 1u + strlen(name) + 1u;
        char *candidate = (char *)malloc(total);
        if (!candidate) return NULL;
        if (directory_length > 0u) {
            memcpy(candidate, cursor, directory_length);
        } else {
            candidate[0] = '.';
        }
        candidate[prefix_length] = '/';
        strcpy(candidate + prefix_length + 1u, name);
        if (access(candidate, X_OK) == 0) return candidate;
        free(candidate);
        cursor = *end ? end + 1u : end;
    }
    return NULL;
}

/* Return monotonic milliseconds for subprocess timeout checks. */
static uint64_t git_monotonic_ms(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0u;
    return (uint64_t)value.tv_sec * 1000u + (uint64_t)value.tv_nsec / 1000000u;
}

/* Run Git on Unix with redirected output and a bounded wait. */
GitProcessResult git_process_run(const char *cwd,
                                 const char *const argv[],
                                 char *output,
                                 size_t output_capacity,
                                 uint32_t timeout_ms)
{
    GitProcessResult result = { .exit_code = -1 };
    if (!cwd || !argv || !argv[0] || !output || output_capacity == 0u) return result;
    output[0] = '\0';

    const char **arguments = git_process_arguments(cwd, argv);
    if (!arguments) return result;

    int pipe_fd[2];
    if (pipe(pipe_fd) != 0) {
        free(arguments);
        return result;
    }
    int null_fd = open("/dev/null", O_RDONLY);
    char *executable = git_resolve_executable(argv[0]);
    char **child_environment = git_child_environment();
    if (null_fd < 0 || !executable || !child_environment) {
        if (!executable) {
            result.exit_code = 127;
            int written = snprintf(output, output_capacity,
                                   "Git executable was not found");
            if (written > 0) {
                result.output_len = (size_t)written < output_capacity
                    ? (size_t)written : output_capacity - 1u;
                result.truncated = (size_t)written >= output_capacity;
            }
        }
        free(executable);
        free(child_environment);
        free(arguments);
        if (null_fd >= 0) close(null_fd);
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        return result;
    }

    posix_spawn_file_actions_t actions;
    int spawn_error = posix_spawn_file_actions_init(&actions);
    const bool actions_initialized = spawn_error == 0;
    if (spawn_error == 0) spawn_error = posix_spawn_file_actions_addclose(
        &actions, pipe_fd[0]);
    if (spawn_error == 0) spawn_error = posix_spawn_file_actions_adddup2(
        &actions, null_fd, STDIN_FILENO);
    if (spawn_error == 0) spawn_error = posix_spawn_file_actions_adddup2(
        &actions, pipe_fd[1], STDOUT_FILENO);
    if (spawn_error == 0) spawn_error = posix_spawn_file_actions_adddup2(
        &actions, pipe_fd[1], STDERR_FILENO);
    if (spawn_error == 0 && null_fd != STDIN_FILENO) {
        spawn_error = posix_spawn_file_actions_addclose(&actions, null_fd);
    }
    if (spawn_error == 0 && pipe_fd[1] != STDOUT_FILENO &&
        pipe_fd[1] != STDERR_FILENO) {
        spawn_error = posix_spawn_file_actions_addclose(&actions, pipe_fd[1]);
    }

    pid_t child = -1;
    if (spawn_error == 0) {
        spawn_error = posix_spawn(&child, executable, &actions, NULL,
                                  (char *const *)arguments,
                                  child_environment);
    }
    if (actions_initialized) (void)posix_spawn_file_actions_destroy(&actions);
    free(executable);
    free(child_environment);
    free(arguments);
    close(null_fd);
    close(pipe_fd[1]);
    if (spawn_error != 0) {
        result.exit_code = spawn_error == ENOENT ? 127 : 126;
        int written = snprintf(output, output_capacity,
                               "Unable to launch Git: %s", strerror(spawn_error));
        if (written > 0) {
            result.output_len = (size_t)written < output_capacity
                ? (size_t)written : output_capacity - 1u;
            result.truncated = (size_t)written >= output_capacity;
        }
        close(pipe_fd[0]);
        return result;
    }

    int flags = fcntl(pipe_fd[0], F_GETFL, 0);
    if (flags >= 0) (void)fcntl(pipe_fd[0], F_SETFL, flags | O_NONBLOCK);

    const uint64_t started = git_monotonic_ms();
    bool child_done = false;
    bool stream_done = false;
    int wait_status = 0;
    while (!child_done || !stream_done) {
        for (;;) {
            char chunk[8192];
            ssize_t count = read(pipe_fd[0], chunk, sizeof(chunk));
            if (count > 0) {
                git_capture_append(output, output_capacity, &result.output_len,
                                   &result.truncated, chunk, (size_t)count);
                continue;
            }
            if (count == 0) stream_done = true;
            break;
        }

        if (!child_done) {
            pid_t waited = waitpid(child, &wait_status, WNOHANG);
            if (waited == child) child_done = true;
        }

        if (!child_done && timeout_ms > 0u &&
            git_monotonic_ms() - started >= timeout_ms) {
            (void)kill(child, SIGKILL);
            (void)waitpid(child, &wait_status, 0);
            child_done = true;
            result.timed_out = true;
        }

        if (!child_done || !stream_done) {
            struct timespec pause = { .tv_nsec = 10000000L };
            nanosleep(&pause, NULL);
        }
    }
    close(pipe_fd[0]);

    if (result.timed_out) {
        result.exit_code = 124;
    } else if (WIFEXITED(wait_status)) {
        result.exit_code = WEXITSTATUS(wait_status);
    } else if (WIFSIGNALED(wait_status)) {
        result.exit_code = 128 + WTERMSIG(wait_status);
    }
    return result;
}

#endif
