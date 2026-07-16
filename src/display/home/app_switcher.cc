#include "display/home/app_switcher.h"
#include "display/common/lvgl_utils.h"
#include "display/theme/ui_theme.h"
#include "fonts.h"

#include <utility>

namespace home {

using jetson::ui::Color;
using jetson::ui::LvglLockGuard;

namespace {

constexpr int kCardW = 300;
constexpr int kThumbH = 180;
constexpr int kTitleRowH = 28;
constexpr int kCardH = kTitleRowH + 6 + kThumbH;
constexpr int kOpenMs = 280;
constexpr int kCloseMs = 220;
constexpr lv_opa_t kBackdropOpa = 150;
// 10% of full size -- the visual size of the island the switcher blooms from.
constexpr int kMinScale = 26;

} // namespace

AppSwitcher::AppSwitcher(lv_obj_t *parent, int width, int height,
                         std::vector<Card> cards, int island_cx, int island_cy,
                         AppCb on_activate, AppCb on_kill, VoidCb on_dismissed)
    : width_(width), height_(height),
      on_activate_(std::move(on_activate)), on_kill_(std::move(on_kill)),
      on_dismissed_(std::move(on_dismissed)) {
    // Dim backdrop. Starts transparent and fades in with the bloom.
    layer_ = lv_obj_create(parent);
    lv_obj_remove_style_all(layer_);
    lv_obj_set_size(layer_, width_, height_);
    lv_obj_set_pos(layer_, 0, 0);
    lv_obj_set_style_bg_color(layer_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(layer_, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(layer_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(layer_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(layer_, OnBackdropClicked, LV_EVENT_CLICKED, this);
    lv_obj_move_foreground(layer_);

    // Scaled content. Its transform pivot sits at the island position so the
    // scale animation visually grows out of / shrinks into the island.
    content_ = lv_obj_create(layer_);
    lv_obj_remove_style_all(content_);
    lv_obj_set_size(content_, width_, height_);
    lv_obj_set_pos(content_, 0, 0);
    lv_obj_set_style_bg_opa(content_, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(content_, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_set_style_transform_pivot_x(content_, island_cx, 0);
    lv_obj_set_style_transform_pivot_y(content_, island_cy, 0);

    // Horizontal card row: drag to scroll (LVGL scrolls the row when a press
    // on a card turns into a horizontal drag), click on empty row = dismiss.
    row_ = lv_obj_create(content_);
    lv_obj_remove_style_all(row_);
    lv_obj_set_size(row_, width_, kCardH + 24);
    lv_obj_align(row_, LV_ALIGN_CENTER, 0, 14);
    lv_obj_set_style_pad_left(row_, 28, 0);
    lv_obj_set_style_pad_right(row_, 28, 0);
    lv_obj_set_style_pad_column(row_, 20, 0);
    lv_obj_set_flex_flow(row_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_, cards.size() <= 2 ? LV_FLEX_ALIGN_CENTER : LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(row_, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(row_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(row_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row_, OnBackdropClicked, LV_EVENT_CLICKED, this);

    // Hint above the cards, mirroring the island the UI just grew out of.
    auto *hint = lv_label_create(content_);
    lv_obj_set_style_text_font(hint, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(hint, lv_color_white(), 0);
    lv_obj_set_style_text_opa(hint, LV_OPA_70, 0);
    lv_label_set_text(hint, "Đa nhiệm");
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 52);

    for (const auto &card : cards) BuildCard(row_, card);
    card_count_ = cards.size();

    AnimateOpen();
}

AppSwitcher::~AppSwitcher() {
    LvglLockGuard lock;
    if (layer_) {
        lv_anim_delete(content_, nullptr);
        lv_anim_delete(layer_, nullptr);
        lv_obj_del(layer_);
        layer_ = nullptr;
    }
}

void AppSwitcher::BuildCard(lv_obj_t *row, const Card &card) {
    const auto &p = jetson::UiTheme::Instance().Palette();

    auto *cell = lv_obj_create(row);
    lv_obj_remove_style_all(cell);
    lv_obj_set_size(cell, kCardW, kCardH);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

    // Title row: icon + app name (like the iPhone switcher's header).
    lv_obj_t *title_anchor = nullptr;
    if (card.icon_src) {
        auto *icon = lv_image_create(cell);
        lv_image_set_src(icon, card.icon_src);
        lv_image_set_scale(icon, card.icon_scale);
        lv_obj_align(icon, LV_ALIGN_TOP_LEFT, 4, 0);
        lv_obj_clear_flag(icon, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
        title_anchor = icon;
    }
    auto *title = lv_label_create(cell);
    lv_obj_set_style_text_font(title, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_label_set_text(title, card.title.c_str());
    if (title_anchor) lv_obj_align_to(title, title_anchor, LV_ALIGN_OUT_RIGHT_MID, 8, 0);
    else lv_obj_align(title, LV_ALIGN_TOP_LEFT, 6, 2);

    // Thumbnail: the app's last snapshot, scaled 800x480 -> 300x180. Fallback
    // when no snapshot exists: a themed plate with the app icon centered.
    auto *thumb = lv_obj_create(cell);
    lv_obj_remove_style_all(thumb);
    lv_obj_set_size(thumb, kCardW, kThumbH);
    lv_obj_align(thumb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(thumb, 16, 0);
    lv_obj_set_style_clip_corner(thumb, true, 0);
    lv_obj_set_style_bg_color(thumb, Color(p.panel), 0);
    lv_obj_set_style_bg_opa(thumb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(thumb, 1, 0);
    lv_obj_set_style_border_color(thumb, lv_color_white(), 0);
    lv_obj_set_style_border_opa(thumb, LV_OPA_20, 0);
    lv_obj_set_style_shadow_color(thumb, lv_color_black(), 0);
    lv_obj_set_style_shadow_width(thumb, 18, 0);
    lv_obj_set_style_shadow_opa(thumb, LV_OPA_40, 0);
    lv_obj_clear_flag(thumb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(thumb, LV_OBJ_FLAG_CLICKABLE);

    if (card.snapshot) {
        auto *shot = lv_image_create(thumb);
        lv_image_set_src(shot, card.snapshot);
        lv_image_set_scale(shot, (uint16_t)(kCardW * 256 / (width_ > 0 ? width_ : kCardW)));
        lv_obj_center(shot);
        lv_obj_clear_flag(shot, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    } else if (card.icon_src) {
        auto *big = lv_image_create(thumb);
        lv_image_set_src(big, card.icon_src);
        // icon_scale targets ~22 px; x3 gives a ~66 px centered fallback mark.
        lv_image_set_scale(big, (uint16_t)LV_MIN(card.icon_scale * 3, 1024));
        lv_obj_center(big);
        lv_obj_clear_flag(big, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    }

    auto *ctx = new CardCtx{this, card.app_id};
    lv_obj_add_event_cb(thumb, OnCardClicked, LV_EVENT_CLICKED, ctx);
    lv_obj_add_event_cb(thumb, OnCardCtxDeleted, LV_EVENT_DELETE, ctx);

    // Kill badge (top-right of the thumbnail).
    auto *badge = lv_obj_create(thumb);
    lv_obj_remove_style_all(badge);
    lv_obj_set_size(badge, 30, 30);
    lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(badge, Color(0x3a3a3c), 0);
    lv_obj_set_style_bg_opa(badge, (lv_opa_t)220, 0);
    lv_obj_align(badge, LV_ALIGN_TOP_RIGHT, -6, 6);
    lv_obj_add_flag(badge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(badge, 4);
    auto *x = lv_label_create(badge);
    lv_obj_set_style_text_font(x, &BUILTIN_ICON_FONT, 0);
    lv_obj_set_style_text_color(x, lv_color_white(), 0);
    lv_label_set_text(x, LV_SYMBOL_CLOSE);
    lv_obj_center(x);
    auto *kctx = new CardCtx{this, card.app_id};
    lv_obj_add_event_cb(badge, OnKillClicked, LV_EVENT_CLICKED, kctx);
    lv_obj_add_event_cb(badge, OnCardCtxDeleted, LV_EVENT_DELETE, kctx);
}

void AppSwitcher::AnimateOpen() {
    // Bloom out of the island: scale 10% -> 100% (ease-out) + fade in, while
    // the backdrop dims underneath.
    lv_obj_set_style_transform_scale(content_, kMinScale, 0);
    lv_obj_set_style_opa(content_, LV_OPA_0, 0);

    lv_anim_t s;
    lv_anim_init(&s);
    lv_anim_set_var(&s, content_);
    lv_anim_set_values(&s, kMinScale, 256);
    lv_anim_set_time(&s, kOpenMs);
    lv_anim_set_exec_cb(&s, OnScaleAnim);
    lv_anim_set_path_cb(&s, lv_anim_path_ease_out);
    lv_anim_start(&s);

    lv_anim_t o;
    lv_anim_init(&o);
    lv_anim_set_var(&o, content_);
    lv_anim_set_values(&o, 0, 255);
    lv_anim_set_time(&o, kOpenMs - 60);
    lv_anim_set_exec_cb(&o, OnOpaAnim);
    lv_anim_set_path_cb(&o, lv_anim_path_ease_out);
    lv_anim_start(&o);

    lv_anim_t b;
    lv_anim_init(&b);
    lv_anim_set_var(&b, layer_);
    lv_anim_set_values(&b, 0, kBackdropOpa);
    lv_anim_set_time(&b, kOpenMs);
    lv_anim_set_exec_cb(&b, OnBgOpaAnim);
    lv_anim_set_path_cb(&b, lv_anim_path_ease_out);
    lv_anim_start(&b);
}

void AppSwitcher::Dismiss() {
    if (closing_ || !layer_) return;
    closing_ = true;

    // Shrink back into the island (ease-in), then tear down.
    lv_anim_t s;
    lv_anim_init(&s);
    lv_anim_set_var(&s, content_);
    lv_anim_set_values(&s, lv_obj_get_style_transform_scale_x(content_, 0), kMinScale);
    lv_anim_set_time(&s, kCloseMs);
    lv_anim_set_exec_cb(&s, OnScaleAnim);
    lv_anim_set_path_cb(&s, lv_anim_path_ease_in);
    lv_anim_set_completed_cb(&s, OnDismissDone);
    lv_anim_set_user_data(&s, this);
    lv_anim_start(&s);

    lv_anim_t o;
    lv_anim_init(&o);
    lv_anim_set_var(&o, content_);
    lv_anim_set_values(&o, lv_obj_get_style_opa(content_, 0), 0);
    lv_anim_set_time(&o, kCloseMs);
    lv_anim_set_exec_cb(&o, OnOpaAnim);
    lv_anim_set_path_cb(&o, lv_anim_path_ease_in);
    lv_anim_start(&o);

    lv_anim_t b;
    lv_anim_init(&b);
    lv_anim_set_var(&b, layer_);
    lv_anim_set_values(&b, lv_obj_get_style_bg_opa(layer_, 0), 0);
    lv_anim_set_time(&b, kCloseMs);
    lv_anim_set_exec_cb(&b, OnBgOpaAnim);
    lv_anim_set_path_cb(&b, lv_anim_path_ease_in);
    lv_anim_start(&b);
}

void AppSwitcher::OnScaleAnim(void *var, int32_t v) {
    lv_obj_set_style_transform_scale(static_cast<lv_obj_t *>(var), v, 0);
}

void AppSwitcher::OnOpaAnim(void *var, int32_t v) {
    lv_obj_set_style_opa(static_cast<lv_obj_t *>(var), (lv_opa_t)v, 0);
}

void AppSwitcher::OnBgOpaAnim(void *var, int32_t v) {
    lv_obj_set_style_bg_opa(static_cast<lv_obj_t *>(var), (lv_opa_t)v, 0);
}

void AppSwitcher::OnDismissDone(lv_anim_t *a) {
    auto *self = static_cast<AppSwitcher *>(lv_anim_get_user_data(a));
    if (!self || !self->layer_) return;
    // Deferred delete + deferred owner callback: never destroy ourselves from
    // inside our own animation callback stack.
    lv_obj_del_async(self->layer_);
    self->layer_ = nullptr;
    self->content_ = nullptr;
    self->row_ = nullptr;
    lv_timer_t *t = lv_timer_create(OnDismissTimer, 0, self);
    lv_timer_set_repeat_count(t, 1);
}

void AppSwitcher::OnDismissTimer(lv_timer_t *t) {
    auto *self = static_cast<AppSwitcher *>(lv_timer_get_user_data(t));
    lv_timer_del(t);
    if (!self) return;
    // The callback typically destroys `self` (owner resets its unique_ptr), so
    // move it out first: never run a std::function that lives in freed memory.
    VoidCb cb = std::move(self->on_dismissed_);
    if (cb) cb();
}

void AppSwitcher::OnBackdropClicked(lv_event_t *e) {
    LvglLockGuard lock;
    auto *self = static_cast<AppSwitcher *>(lv_event_get_user_data(e));
    // Only direct clicks: a click on a card bubbles here too, skip those.
    if (lv_event_get_target(e) != lv_event_get_current_target(e)) return;
    self->Dismiss();
}

void AppSwitcher::OnCardClicked(lv_event_t *e) {
    LvglLockGuard lock;
    auto *ctx = static_cast<CardCtx *>(lv_event_get_user_data(e));
    auto *self = ctx->self;
    if (self->closing_) return;
    if (self->on_activate_) self->on_activate_(ctx->app_id);
    // The restored app moved above us; float the switcher back on top so the
    // collapse-into-the-island animation plays over it.
    if (self->layer_) lv_obj_move_foreground(self->layer_);
    self->Dismiss();
}

void AppSwitcher::OnKillClicked(lv_event_t *e) {
    LvglLockGuard lock;
    auto *ctx = static_cast<CardCtx *>(lv_event_get_user_data(e));
    auto *self = ctx->self;
    const int id = ctx->app_id;
    if (self->closing_) return;
    // Remove the card first (it references the app's snapshot buffer), then
    // let the owner close the app and free that buffer.
    lv_obj_t *badge = lv_event_get_current_target_obj(e);
    lv_obj_t *thumb = lv_obj_get_parent(badge);
    lv_obj_t *cell = lv_obj_get_parent(thumb);
    lv_obj_del(cell); // deletes ctx via LV_EVENT_DELETE; don't touch it below
    if (self->card_count_ > 0) --self->card_count_;
    if (self->on_kill_) self->on_kill_(id);
    if (self->card_count_ == 0) self->Dismiss();
}

void AppSwitcher::OnCardCtxDeleted(lv_event_t *e) {
    delete static_cast<CardCtx *>(lv_event_get_user_data(e));
}

} // namespace home
