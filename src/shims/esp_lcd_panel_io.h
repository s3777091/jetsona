#ifndef ESP_LCD_PANEL_IO_H
#define ESP_LCD_PANEL_IO_H
/* Opaque shim: the Linux port does not use the ESP LCD panel-IO abstraction.
 * The typedef exists only so copied display headers compile unchanged. */
typedef struct esp_lcd_panel_io_t *esp_lcd_panel_io_handle_t;
#endif