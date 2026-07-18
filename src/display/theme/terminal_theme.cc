#include "display/theme/terminal_theme.h"

namespace jetson {
namespace {

constexpr TerminalTheme kThemes[] = {
    {"flexoki_dark", "Flexoki Dark", 0x100f0f, 0xcecdc3, 0xcecdc3, 0x343331, 0x879a39, 0x878580},
    {"hacker_blue", "Hacker Blue", 0x00111a, 0x25bfff, 0x00c8ff, 0x003d5c, 0x00aeef, 0x5bbee8},
    {"hacker_green", "Hacker Green", 0x001900, 0x39ff14, 0x39ff14, 0x0a4d0a, 0x00d000, 0x7be36d},
    {"hacker_red", "Hacker Red", 0x210000, 0xff3b30, 0xff3b30, 0x5a0b0b, 0xd71920, 0xaa5151},
    {"night_owl", "Night Owl", 0x011627, 0xd6deeb, 0x80a4c2, 0x1d3b53, 0x82aaff, 0x637777},
    {"light_owl", "Light Owl", 0xfbfbfb, 0x403f53, 0x403f53, 0xd9e4ec, 0x0099cc, 0x90a7b2},
    {"kanagawa_wave", "Kanagawa Wave", 0x1f1f28, 0xdcd7ba, 0xc8c093, 0x2d4f67, 0x7e9cd8, 0x727169},
    {"kanagawa_dragon", "Kanagawa Dragon", 0x181616, 0xc5c9c5, 0xc8c093, 0x282727, 0x8a9a7b, 0x737c73},
    {"kanagawa_lotus", "Kanagawa Lotus", 0xf2ecbc, 0x545464, 0x43436c, 0xc9c7a9, 0x6f894e, 0x8a8980},
    {"dracula", "Dracula", 0x282a36, 0xf8f8f2, 0xf8f8f2, 0x44475a, 0xbd93f9, 0x6272a4},
    {"monokai", "Monokai", 0x272822, 0xf8f8f2, 0xf8f8f2, 0x49483e, 0xa6e22e, 0x75715e},
    {"nord_light", "Nord Light", 0xeceff4, 0x2e3440, 0x5e81ac, 0xd8dee9, 0x5e81ac, 0x7b88a1},
    {"gruvbox_dark", "Gruvbox Dark", 0x282828, 0xebdbb2, 0xfabd2f, 0x504945, 0xb8bb26, 0x928374},
    {"gruvbox_light", "Gruvbox Light", 0xfbf1c7, 0x3c3836, 0xd79921, 0xd5c4a1, 0x98971a, 0x928374},
    {"material_dark", "Material Dark", 0x263238, 0xeeffff, 0xffcc00, 0x37474f, 0x80cbc4, 0x78909c},
    {"material_light", "Material Light", 0xfafafa, 0x546e7a, 0x009688, 0xcfd8dc, 0x009688, 0x90a4ae},
    {"manhattan", "Manhattan", 0x1b1d1e, 0xd8d8d8, 0x8fb573, 0x3a3d3e, 0xa7afaf, 0x878a8c},
    {"plastic_world", "Plastic World", 0x1f1025, 0xff8de8, 0xff3fcb, 0x53304f, 0xf53bc8, 0xb86aaa},
};

} // namespace

size_t TerminalThemeCount() { return sizeof(kThemes) / sizeof(kThemes[0]); }

const TerminalTheme &TerminalThemeAt(size_t index) {
    return kThemes[index < TerminalThemeCount() ? index : 0];
}

const TerminalTheme &FindTerminalTheme(const std::string &id) {
    for (const auto &theme : kThemes) {
        if (id == theme.id) return theme;
    }
    return kThemes[0];
}

} // namespace jetson
