#include "display/common/airplane_icon.h"

namespace jetson::ui {
namespace {

lv_obj_t *AddStroke(lv_obj_t *parent, const lv_point_precise_t *points,
                    uint32_t point_count, lv_color_t color, int width) {
    auto *line = lv_line_create(parent);
    lv_obj_remove_style_all(line);
    lv_obj_set_style_line_color(line, color, 0);
    lv_obj_set_style_line_width(line, width, 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    lv_line_set_points(line, points, point_count);
    lv_obj_clear_flag(
        line, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    return line;
}

} // namespace

lv_obj_t *CreateAirplaneIcon(lv_obj_t *parent, lv_color_t color) {
    auto *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, 24, 24);
    lv_obj_clear_flag(
        root, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    // Nose points to the upper-right. The bent strokes form the wings and
    // tail around a thicker fuselage and stay legible on the 20 px top bar.
    static const lv_point_precise_t fuselage[] = {{3, 21}, {21, 3}};
    static const lv_point_precise_t wings[] = {{3, 7}, {12, 13}, {21, 19}};
    static const lv_point_precise_t tail[] = {{3, 17}, {7, 20}, {10, 23}};
    AddStroke(root, fuselage, 2, color, 4);
    AddStroke(root, wings, 3, color, 3);
    AddStroke(root, tail, 3, color, 3);
    return root;
}

} // namespace jetson::ui
