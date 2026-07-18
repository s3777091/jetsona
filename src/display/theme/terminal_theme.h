#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace jetson {

struct TerminalTheme {
    const char *id;
    const char *name;
    uint32_t background;
    uint32_t foreground;
    uint32_t cursor;
    uint32_t selection;
    uint32_t accent;
    uint32_t muted;
};

inline constexpr const char *kDefaultTerminalTheme = "flexoki_dark";
inline constexpr int kDefaultTerminalTextSize = 14;
inline constexpr int kMinTerminalTextSize = 12;
inline constexpr int kMaxTerminalTextSize = 28;
inline constexpr int kTerminalTextSizeStep = 2;

size_t TerminalThemeCount();
const TerminalTheme &TerminalThemeAt(size_t index);
const TerminalTheme &FindTerminalTheme(const std::string &id);

} // namespace jetson
