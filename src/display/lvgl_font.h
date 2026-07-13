#ifndef LVGL_FONT_H
#define LVGL_FONT_H

#include <lvgl.h>

class LvglFont {
public:
    virtual const lv_font_t *font() const = 0;
    virtual ~LvglFont() = default;
};

class LvglBuiltInFont : public LvglFont {
public:
    LvglBuiltInFont(const lv_font_t *font) : font_(font) {}
    virtual const lv_font_t *font() const override { return font_; }
private:
    const lv_font_t *font_;
};

/* cbin runtime font is ESP-asset-specific; stubbed. */
class LvglCBinFont : public LvglFont {
public:
    LvglCBinFont(void * /*data*/) : font_(&lv_font_montserrat_14) {}
    virtual const lv_font_t *font() const override { return font_; }
private:
    const lv_font_t *font_;
};

#endif