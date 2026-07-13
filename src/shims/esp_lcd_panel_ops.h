#ifndef ESP_LCD_PANEL_OPS_H
#define ESP_LCD_PANEL_OPS_H
#include "esp_err.h"
/* Opaque shim for the ESP LCD panel handle. No-op operations. */
typedef struct esp_lcd_panel_t *esp_lcd_panel_handle_t;

static inline esp_err_t esp_lcd_panel_del(esp_lcd_panel_handle_t) { return ESP_OK; }
static inline esp_err_t esp_lcd_panel_io_del(esp_lcd_panel_io_handle_t) { return ESP_OK; }
static inline esp_err_t esp_lcd_panel_reset(esp_lcd_panel_handle_t) { return ESP_OK; }
static inline esp_err_t esp_lcd_panel_init(esp_lcd_panel_handle_t) { return ESP_OK; }
static inline esp_err_t esp_lcd_panel_disp_on_off(esp_lcd_panel_handle_t, bool) { return ESP_OK; }
static inline esp_err_t esp_lcd_panel_invert_color(esp_lcd_panel_handle_t, bool) { return ESP_OK; }
static inline esp_err_t esp_lcd_panel_swap_xy(esp_lcd_panel_handle_t, bool) { return ESP_OK; }
static inline esp_err_t esp_lcd_panel_mirror(esp_lcd_panel_handle_t, bool, bool) { return ESP_OK; }
static inline esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t, int, int, int, int, const void *) { return ESP_OK; }
static inline esp_err_t esp_lcd_panel_set_gap(esp_lcd_panel_handle_t, int, int) { return ESP_OK; }

#endif