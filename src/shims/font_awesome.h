#ifndef FONT_AWESOME_H
#define FONT_AWESOME_H
/* Font Awesome glyph stubs for the Linux port.
 * The DS-02 system bar draws the battery icon with rectangles, so most of
 * these are only referenced by the (overridden) base status-bar code. They are
 * mapped to LVGL built-in symbols where a match exists, otherwise to a plain
 * text fallback so the firmware still links. For real FA icons, drop a
 * fontawesome.ttf into assets/fonts and wire it as BUILTIN_ICON_FONT. */

#include <lvgl.h>

#ifndef FONT_AWESOME
#define FONT_AWESOME
#endif

#define FONT_AWESOME_VOLUME_XMARK           LV_SYMBOL_MUTE
#define FONT_AWESOME_VOLUME_HIGH            LV_SYMBOL_AUDIO
#define FONT_AWESOME_BLUETOOTH              "BT"
#define FONT_AWESOME_POWER                  LV_SYMBOL_REFRESH
#define FONT_AWESOME_MICROPHONE             LV_SYMBOL_AUDIO
#define FONT_AWESOME_MICROPHONE_ALT         LV_SYMBOL_AUDIO
#define FONT_AWESOME_MICROPHONE_LINES       LV_SYMBOL_AUDIO
#define FONT_AWESOME_WIFI                   LV_SYMBOL_WIFI
#define FONT_AWESOME_WIFI_WEAK              LV_SYMBOL_WIFI
#define FONT_AWESOME_WIFI_FAIR              LV_SYMBOL_WIFI
#define FONT_AWESOME_WIFI_SLASH             LV_SYMBOL_CLOSE
#define FONT_AWESOME_BATTERY_BOLT           LV_SYMBOL_BATTERY_FULL
#define FONT_AWESOME_BATTERY_EMPTY          LV_SYMBOL_BATTERY_EMPTY
#define FONT_AWESOME_BATTERY_QUARTER        LV_SYMBOL_BATTERY_1
#define FONT_AWESOME_BATTERY_HALF           LV_SYMBOL_BATTERY_2
#define FONT_AWESOME_BATTERY_THREE_QUARTERS LV_SYMBOL_BATTERY_3
#define FONT_AWESOME_BATTERY_FULL           LV_SYMBOL_BATTERY_FULL
#define FONT_AWESOME_SIGNAL_OFF             LV_SYMBOL_CLOSE
#define FONT_AWESOME_SIGNAL_WEAK            LV_SYMBOL_OK
#define FONT_AWESOME_SIGNAL_FAIR            LV_SYMBOL_OK
#define FONT_AWESOME_SIGNAL_GOOD            LV_SYMBOL_OK
#define FONT_AWESOME_SIGNAL_STRONG          LV_SYMBOL_OK
#define FONT_AWESOME_GEAR                   LV_SYMBOL_SETTINGS
#define FONT_AWESOME_GLOBE                  LV_SYMBOL_GPS
#define FONT_AWESOME_EARTH_AMERICAS         LV_SYMBOL_GPS
#define FONT_AWESOME_LANGUAGE               "A>"
#define FONT_AWESOME_BOOK                   LV_SYMBOL_FILE
#define FONT_AWESOME_BOOK_OPEN              LV_SYMBOL_FILE
#define FONT_AWESOME_CODE                   LV_SYMBOL_KEYBOARD
#define FONT_AWESOME_TERMINAL               LV_SYMBOL_KEYBOARD
#define FONT_AWESOME_MUSIC                  LV_SYMBOL_PLAY
#define FONT_AWESOME_GRADUATION_CAP         LV_SYMBOL_FILE
#define FONT_AWESOME_CHALKBOARD_USER        LV_SYMBOL_FILE
#define FONT_AWESOME_MICROCHIP_AI           LV_SYMBOL_EYE_OPEN
#define FONT_AWESOME_NEUTRAL                "  "

#endif