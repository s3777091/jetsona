#include "platform/shell_command.h"

#include <cstdio>

namespace jetson::platform {

ShellCommandResult RunShellCommand(const std::string &command) {
    ShellCommandResult result;
    FILE *process = popen((command + " 2>&1").c_str(), "r");
    if (!process) return result;

    char buffer[1024];
    while (fgets(buffer, sizeof(buffer), process)) result.output += buffer;
    result.status = pclose(process);
    return result;
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
