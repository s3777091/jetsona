#pragma once

/* Global UI theme (light/dark) for the Jetson DS-02 firmware.
 *
 * One place to ask "what color is X right now?" so the home screen and every
 * overlay view stay consistent and can be flipped at runtime. The mode is
 * persisted in Settings ("ui", "theme_mode") and a list of subscribers is
 * notified (under lv_lock) whenever it changes, so live UI can repaint.
 *
 * Kept deliberately separate from the older LvglTheme class (which is about
 * the ESP display stack); this is the small runtime palette the views use. */

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace jetson {

enum class UiMode { Dark, Light };

struct UiPalette {
    // Screen + surfaces.
    uint32_t bg = 0;          // full-screen background
    uint32_t panel = 0;       // header / keyboard panel
    uint32_t row = 0;         // list row (idle)
    uint32_t row_active = 0;  // list row (selected/connected)
    uint32_t header = 0;
    uint32_t bar_bg = 0;      // system bar background
    uint32_t dock_bg = 0;
    uint32_t button = 0;      // small button fill
    // Text.
    uint32_t text = 0;
    uint32_t sub_text = 0;    // secondary / captions
    uint32_t accent = 0;     // highlights / "connected" tag
    // Lines.
    uint32_t border = 0;
    uint32_t scrim = 0;        // dim overlay color (rgba-ish; we use as rgb)
    // Standby gradient (top -> bottom).
    uint32_t grad_top = 0;
    uint32_t grad_bottom = 0;
};

class UiTheme {
public:
    static UiTheme &Instance();

    UiMode Mode() const { return mode_; }
    const UiPalette &Palette() const { return palette_; }

    void SetMode(UiMode m);
    void Toggle(); // Dark <-> Light
    static const char *ModeName(UiMode m) { return m == UiMode::Dark ? "dark" : "light"; }

    // Register a callback fired (under lv_lock) when the mode changes.
    void Subscribe(std::function<void()> cb);

private:
    UiTheme();
    void Load();
    void Save();
    void Apply();

    UiMode mode_ = UiMode::Dark;
    UiPalette palette_;
    std::vector<std::function<void()>> subs_;
};

} // namespace jetson