#include "platform/shell_command.h"

#include <cstdio>
#include <utility>

#include <sys/wait.h>

namespace jetson::platform {

ShellCommandResult RunShellCommand(const std::string &command) {
    ShellCommandResult result;
    FILE *process = popen((command + " 2>&1").c_str(), "r");
    if (!process) return result;

    char buffer[1024];
    while (fgets(buffer, sizeof(buffer), process)) result.output += buffer;
    // pclose() returns a raw wait(2) status; report the child's exit code
    // (e.g. 124 from `timeout`) instead of the shifted value (124<<8=31744).
    const int raw = pclose(process);
    if (raw == -1) result.status = -1;
    else if (WIFEXITED(raw)) result.status = WEXITSTATUS(raw);
    else if (WIFSIGNALED(raw)) result.status = 128 + WTERMSIG(raw);
    else result.status = raw;
    return result;
}

int RunShellCommand(const std::string &command, std::string &output) {
    auto result = RunShellCommand(command);
    output = std::move(result.output);
    return result.status;
}

std::string QuoteShellArgument(const std::string &value) {
    std::string quoted = "'";
    for (char c : value) {
        if (c == '\'') quoted += "'\\''";
        else quoted += c;
    }
    quoted += "'";
    return quoted;
}

void TrimTrailingWhitespace(std::string &value) {
    while (!value.empty()) {
        const char c = value.back();
        if (c != '\n' && c != '\r' && c != ' ' && c != '\t') break;
        value.pop_back();
    }
}

} // namespace jetson::platform
