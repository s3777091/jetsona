#pragma once

#include <string>

namespace jetson::platform {

struct ShellCommandResult {
    int status = -1;
    std::string output;

    bool Ok() const { return status == 0; }
};

// Runs a trusted command through /bin/sh and captures stdout plus stderr.
ShellCommandResult RunShellCommand(const std::string &command);

// Quotes one untrusted value for safe interpolation as a single shell argument.
std::string QuoteShellArgument(const std::string &value);

// Removes trailing spaces and line endings from command output.
void TrimTrailingWhitespace(std::string &value);

} // namespace jetson::platform
