
#pragma once

#include <cerrno>
#include <spawn.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>

#include <vector>
#include <string>
#include <stdexcept>
#include <system_error>

extern char** environ;

class Supervisor {
private:
    std::vector<pid_t> m_processes;

public:
    Supervisor() = default;
    Supervisor(const Supervisor&) = delete;
    Supervisor& operator=(const Supervisor&) = delete;

    ~Supervisor() {
        stopAll();
    }

    pid_t startGraph(const std::string& executable) {
        pid_t pid = -1;

        char* const argv[] = {
            const_cast<char*>(executable.c_str()),
            nullptr
        };

        int result = posix_spawn(
            &pid,
            executable.c_str(),
            nullptr,
            nullptr,
            argv,
            environ
        );

        if (result != 0) {
            throw std::system_error(
                result,
                std::generic_category(),
                "posix_spawn failed"
            );
        }

        try {
            m_processes.push_back(pid);
        } catch (...) {
            kill(pid, SIGTERM);
            while (waitpid(pid, nullptr, 0) == -1 && errno == EINTR) {
            }
            throw;
        }
        return pid;
    }

    void stopAll() {
        // Najpierw wysyłamy sygnał do wszystkich.
        for (pid_t pid : m_processes) {
            kill(pid, SIGTERM);
        }

        // Następnie czekamy na zakończenie.
        for (pid_t pid : m_processes) {
            int status;
            while (waitpid(pid, &status, 0) == -1 &&
                   errno == EINTR) {
            }
        }

        m_processes.clear();
    }
};
