#pragma once

#include "overlay_view.h"

#include <lvgl.h>
#include <string>

namespace home {

/* Settings overlay: a light/dark theme switch + device info. Toggling the
 * switch flips jetson::UiTheme (persisted) and recolors this view live. The
 * home screen repaints via its own UiTheme subscription. */
class SettingsView : public OverlayView {
public:
    SettingsView(lv_obj_t *parent, int width, int height, ClosedCb on_closed);

protected:
    void OnStart() override;

private:
    lv_obj_t *theme_row_ = nullptr;
    lv_obj_t *theme_label_ = nullptr;
    lv_obj_t *theme_switch_ = nullptr;
    lv_obj_t *mode_label_ = nullptr;
    lv_obj_t *about_label_ = nullptr;

    void BuildBody();
    void Recolor();
    void OnToggle();

    static void OnSwitchChanged(lv_event_t *e);
};

} // namespace home