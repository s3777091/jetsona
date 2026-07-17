#include "display/views/music_view.h"

#include "application.h"
#include "display/common/lvgl_utils.h"
#include "display/theme/ui_theme.h"
#include "fonts.h"
#include "media/player_controller.h"
#include "net/zing_music_client.h"

#include <lvgl.h>

#include <algorithm>
#include <cstdio>
#include <exception>
#include <memory>
#include <thread>
#include <utility>

namespace home {

using jetson::music::CatalogItem;
using jetson::music::CatalogKind;
using jetson::music::PlaybackStatus;
using jetson::music::PlayerController;
using jetson::music::Track;
using jetson::ui::Color;
using jetson::ui::LvglLockGuard;

namespace {
constexpr int kPlayerBarHeight = 64;
constexpr int kCardWidth = 124;
constexpr int kCoverSize = 112;
constexpr int kRailHeight = 178;

void RemoveInteraction(lv_obj_t *obj) {
    if (!obj) return;
    lv_obj_clear_flag(obj,
        (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
}

void ImageDimensions(const lv_img_dsc_t *dsc, int &w, int &h) {
    w = h = 240;
    if (!dsc || !dsc->data || dsc->data_size < 24) return;
    const uint8_t *d = dsc->data;
    const size_t n = dsc->data_size;
    if (d[0] == 0x89 && d[1] == 0x50) {
        w = (d[16] << 24) | (d[17] << 16) | (d[18] << 8) | d[19];
        h = (d[20] << 24) | (d[21] << 16) | (d[22] << 8) | d[23];
        return;
    }
    if (d[0] != 0xff || d[1] != 0xd8) return;
    size_t p = 2;
    while (p + 8 < n) {
        if (d[p] != 0xff) { ++p; continue; }
        while (p < n && d[p] == 0xff) ++p;
        if (p >= n) break;
        const uint8_t marker = d[p++];
        if (marker == 0xd8 || marker == 0xd9) continue;
        if (p + 2 > n) break;
        const size_t len = (static_cast<size_t>(d[p]) << 8) | d[p + 1];
        if (len < 2 || p + len > n) break;
        const bool sof = (marker >= 0xc0 && marker <= 0xc3) ||
                         (marker >= 0xc5 && marker <= 0xc7) ||
                         (marker >= 0xc9 && marker <= 0xcb) ||
                         (marker >= 0xcd && marker <= 0xcf);
        if (sof && len >= 7) {
            h = (d[p + 3] << 8) | d[p + 4];
            w = (d[p + 5] << 8) | d[p + 6];
            return;
        }
        p += len;
    }
}

lv_obj_t *MakeIconButton(lv_obj_t *parent, const char *symbol,
                         int size, lv_event_cb_t cb, void *user) {
    const auto &p = jetson::UiTheme::Instance().Palette();
    auto *button = lv_obj_create(parent);
    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, size, size);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(button, Color(p.button), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(button, Color(p.accent), LV_STATE_PRESSED);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    auto *label = lv_label_create(button);
    lv_obj_set_style_text_font(label, &BUILTIN_ICON_FONT, 0);
    lv_obj_set_style_text_color(label, Color(p.text), 0);
    lv_label_set_text(label, symbol);
    lv_obj_center(label);
    RemoveInteraction(label);
    if (cb) lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, user);
    return button;
}

std::string DurationText(int64_t duration_ms) {
    const int total = static_cast<int>(std::max<int64_t>(0, duration_ms) / 1000);
    char out[16];
    std::snprintf(out, sizeof(out), "%d:%02d", total / 60, total % 60);
    return out;
}

void SkeletonOpacity(void *object, int32_t value) {
    lv_obj_set_style_bg_opa(static_cast<lv_obj_t *>(object),
                            static_cast<lv_opa_t>(value), 0);
}
} // namespace

MusicView::MusicView(lv_obj_t *parent, int width, int height, ClosedCb on_closed)
    : OverlayView(parent, width, height, "Music", std::move(on_closed)) {
    BuildBody();
    BuildSkeleton();
}

MusicView::~MusicView() {
    request_generation_.fetch_add(1);
    LvglLockGuard lock;
    if (player_timer_) { lv_timer_del(player_timer_); player_timer_ = nullptr; }
    ClearArtwork();
    if (player_art_obj_) lv_image_set_src(player_art_obj_, nullptr);
    if (player_artwork_)
        lv_image_cache_drop(player_artwork_->image_dsc());
    player_artwork_.reset();
}

void MusicView::OnStart() {
    if (started_) return;
    started_ = true;
    LoadDiscovery();
}

void MusicView::BuildBody() {
    const auto &p = jetson::UiTheme::Instance().Palette();
    lv_obj_clear_flag(body_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(body_, 0, 0);

    page_ = lv_obj_create(body_);
    lv_obj_remove_style_all(page_);
    lv_obj_set_size(page_, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(page_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(page_, 12, 0);
    lv_obj_set_style_pad_row(page_, 12, 0);
    lv_obj_add_flag(page_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(page_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(page_, LV_SCROLLBAR_MODE_AUTO);

    player_bar_ = lv_obj_create(overlay_);
    lv_obj_remove_style_all(player_bar_);
    lv_obj_set_size(player_bar_, width_, kPlayerBarHeight);
    lv_obj_set_pos(player_bar_, 0, height_ - kPlayerBarHeight);
    lv_obj_set_style_bg_color(player_bar_, Color(0x111216), 0);
    lv_obj_set_style_bg_opa(player_bar_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(player_bar_, 1, 0);
    lv_obj_set_style_border_side(player_bar_, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(player_bar_, Color(p.border), 0);
    lv_obj_clear_flag(player_bar_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(player_bar_, LV_OBJ_FLAG_HIDDEN);

    player_art_host_ = lv_obj_create(player_bar_);
    lv_obj_remove_style_all(player_art_host_);
    lv_obj_set_size(player_art_host_, 46, 46);
    lv_obj_set_pos(player_art_host_, 10, 7);
    lv_obj_set_style_radius(player_art_host_, 8, 0);
    lv_obj_set_style_clip_corner(player_art_host_, true, 0);
    lv_obj_set_style_bg_color(player_art_host_, Color(0x273047), 0);
    lv_obj_set_style_bg_opa(player_art_host_, LV_OPA_COVER, 0);
    RemoveInteraction(player_art_host_);

    player_title_ = lv_label_create(player_bar_);
    lv_obj_set_size(player_title_, 300, 24);
    lv_obj_set_pos(player_title_, 68, 8);
    lv_obj_set_style_text_font(player_title_, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(player_title_, Color(p.text), 0);
    lv_label_set_long_mode(player_title_, LV_LABEL_LONG_DOT);

    player_artist_ = lv_label_create(player_bar_);
    lv_obj_set_size(player_artist_, 300, 22);
    lv_obj_set_pos(player_artist_, 68, 32);
    lv_obj_set_style_text_font(player_artist_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(player_artist_, Color(p.sub_text), 0);
    lv_label_set_long_mode(player_artist_, LV_LABEL_LONG_DOT);

    auto *previous = MakeIconButton(player_bar_, LV_SYMBOL_PREV, 40,
                                    OnPlayerPrevious, this);
    lv_obj_set_pos(previous, width_ - 174, 10);
    auto *toggle = MakeIconButton(player_bar_, LV_SYMBOL_PLAY, 44,
                                  OnPlayerToggle, this);
    lv_obj_set_pos(toggle, width_ - 122, 8);
    player_toggle_label_ = lv_obj_get_child(toggle, 0);
    auto *next = MakeIconButton(player_bar_, LV_SYMBOL_NEXT, 40,
                                OnPlayerNext, this);
    lv_obj_set_pos(next, width_ - 66, 10);

    player_progress_ = lv_bar_create(player_bar_);
    lv_obj_set_size(player_progress_, width_, 3);
    lv_obj_align(player_progress_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_bar_set_range(player_progress_, 0, 1000);
    lv_bar_set_value(player_progress_, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(player_progress_, Color(0x303138), LV_PART_MAIN);
    lv_obj_set_style_bg_color(player_progress_, Color(p.accent), LV_PART_INDICATOR);
    lv_obj_set_style_radius(player_progress_, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(player_progress_, 0, LV_PART_INDICATOR);
    RemoveInteraction(player_progress_);

    player_timer_ = lv_timer_create(OnPlayerTimer, 350, this);
}

void MusicView::ClearArtwork() {
    for (auto *obj : image_objects_)
        if (obj) lv_image_set_src(obj, nullptr);
    image_objects_.clear();
    for (const auto &image : artwork_)
        if (image) lv_image_cache_drop(image->image_dsc());
    artwork_by_path_.clear();
    artwork_.clear();
}

void MusicView::ClearPage() {
    ClearArtwork();
    track_rows_.clear();
    loading_label_ = nullptr;
    if (page_) lv_obj_clean(page_);
}

lv_obj_t *MusicView::CreateArtwork(lv_obj_t *parent, const std::string &path,
                                   int size, bool circular) {
    const auto &p = jetson::UiTheme::Instance().Palette();
    auto *host = lv_obj_create(parent);
    lv_obj_remove_style_all(host);
    lv_obj_set_size(host, size, size);
    lv_obj_set_style_radius(host, circular ? LV_RADIUS_CIRCLE : 10, 0);
    lv_obj_set_style_clip_corner(host, true, 0);
    lv_obj_set_style_bg_color(host, Color(0x252b3a), 0);
    lv_obj_set_style_bg_grad_color(host, Color(0x111827), 0);
    lv_obj_set_style_bg_grad_dir(host, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(host, LV_OPA_COVER, 0);
    RemoveInteraction(host);

    auto *fallback = lv_label_create(host);
    lv_obj_set_style_text_font(fallback, &BUILTIN_ICON_FONT, 0);
    lv_obj_set_style_text_color(fallback, Color(p.sub_text), 0);
    lv_label_set_text(fallback, LV_SYMBOL_AUDIO);
    lv_obj_center(fallback);
    RemoveInteraction(fallback);

    if (path.empty()) return host;
    LvglImage *image = nullptr;
    auto cached = artwork_by_path_.find(path);
    if (cached != artwork_by_path_.end()) {
        image = cached->second;
    } else {
        auto owner = LvglImageFromFile(path);
        if (!owner) return host;
        image = owner.get();
        artwork_by_path_.emplace(path, image);
        artwork_.push_back(std::move(owner));
    }
    auto *obj = lv_image_create(host);
    const auto *dsc = image->image_dsc();
    int w, h;
    ImageDimensions(dsc, w, h);
    lv_image_set_src(obj, dsc);
    lv_obj_set_size(obj, w, h);
    const int shorter = std::max(1, std::min(w, h));
    lv_image_set_scale(obj, static_cast<uint32_t>(size * 256 / shorter));
    lv_image_set_pivot(obj, w / 2, h / 2);
    lv_obj_center(obj);
    RemoveInteraction(obj);
    image_objects_.push_back(obj);
    return host;
}

lv_obj_t *MusicView::CreateSkeletonCard(lv_obj_t *rail, bool circular) {
    const auto &p = jetson::UiTheme::Instance().Palette();
    auto *card = lv_obj_create(rail);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, kCardWidth, kRailHeight - 4);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    auto *cover = lv_obj_create(card);
    lv_obj_remove_style_all(cover);
    lv_obj_set_size(cover, kCoverSize, kCoverSize);
    lv_obj_align(cover, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_radius(cover, circular ? LV_RADIUS_CIRCLE : 10, 0);
    lv_obj_set_style_bg_color(cover, Color(p.button), 0);
    lv_obj_set_style_bg_opa(cover, LV_OPA_70, 0);
    lv_anim_t pulse;
    lv_anim_init(&pulse);
    lv_anim_set_var(&pulse, cover);
    lv_anim_set_exec_cb(&pulse, SkeletonOpacity);
    lv_anim_set_values(&pulse, LV_OPA_40, LV_OPA_80);
    lv_anim_set_duration(&pulse, 720);
    lv_anim_set_playback_duration(&pulse, 720);
    lv_anim_set_repeat_count(&pulse, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&pulse, lv_anim_path_ease_in_out);
    lv_anim_start(&pulse);
    auto *line = lv_obj_create(card);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, 88, 10);
    lv_obj_align(line, LV_ALIGN_BOTTOM_MID, 0, -28);
    lv_obj_set_style_radius(line, 5, 0);
    lv_obj_set_style_bg_color(line, Color(p.button), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_70, 0);
    auto *subline = lv_obj_create(card);
    lv_obj_remove_style_all(subline);
    lv_obj_set_size(subline, 64, 8);
    lv_obj_align(subline, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_radius(subline, 4, 0);
    lv_obj_set_style_bg_color(subline, Color(p.button), 0);
    lv_obj_set_style_bg_opa(subline, LV_OPA_50, 0);
    return card;
}

void MusicView::BuildSkeleton() {
    ClearPage();
    const auto &p = jetson::UiTheme::Instance().Palette();
    static const char *titles[] = {
        "Trending songs", "Popular artists", "Radio", "Top 100"
    };
    for (int section = 0; section < 4; ++section) {
        auto *title = lv_label_create(page_);
        lv_obj_set_style_text_font(title, &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(title, Color(p.text), 0);
        lv_label_set_text(title, titles[section]);
        auto *rail = lv_obj_create(page_);
        lv_obj_remove_style_all(rail);
        lv_obj_set_size(rail, lv_pct(100), kRailHeight);
        lv_obj_set_flex_flow(rail, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(rail, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_column(rail, 12, 0);
        lv_obj_add_flag(rail, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE |
                                              LV_OBJ_FLAG_SCROLL_CHAIN_VER));
        lv_obj_set_scroll_dir(rail, LV_DIR_HOR);
        lv_obj_set_scrollbar_mode(rail, LV_SCROLLBAR_MODE_OFF);
        for (int i = 0; i < 6; ++i) CreateSkeletonCard(rail, section == 1);
    }
}

void MusicView::LoadDiscovery() {
    if (loading_) return;
    loading_ = true;
    album_mode_ = false;
    const uint64_t generation = request_generation_.fetch_add(1) + 1;
    BuildSkeleton();

    auto result = std::make_shared<jetson::music::DiscoverData>();
    std::weak_ptr<MusicView> weak =
        std::static_pointer_cast<MusicView>(shared_from_this());
    std::thread([weak, result, generation]() {
        std::string error;
        bool ok = false;
        try {
            jetson::ZingMusicClient client;
            ok = client.FetchDiscover(*result, error);
        } catch (const std::exception &exception) {
            error = std::string("Không thể đọc dữ liệu Zing: ") + exception.what();
        } catch (...) {
            error = "Không thể đọc dữ liệu Zing";
        }
        Application::GetInstance().Schedule([weak, result, ok, error, generation]() {
            auto self = weak.lock();
            if (!self) return;
            LvglLockGuard lock;
            if (generation != self->request_generation_.load()) return;
            self->loading_ = false;
            if (!ok && result->trending.empty() && result->artists.empty() &&
                result->radio.empty() && result->top100.empty()) {
                self->ClearPage();
                const auto &p = jetson::UiTheme::Instance().Palette();
                auto *message = lv_label_create(self->page_);
                lv_obj_set_width(message, lv_pct(100));
                lv_obj_set_style_text_font(message, &BUILTIN_TEXT_FONT, 0);
                lv_obj_set_style_text_color(message, Color(p.sub_text), 0);
                lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
                lv_label_set_long_mode(message, LV_LABEL_LONG_WRAP);
                const std::string text = error.empty()
                    ? "Không tải được thư viện nhạc."
                    : "Không tải được thư viện nhạc\n" + error;
                lv_label_set_text(message, text.c_str());
                auto *retry = MakeIconButton(self->page_, LV_SYMBOL_REFRESH, 46,
                                             OnRetry, self.get());
                lv_obj_set_style_align(retry, LV_ALIGN_CENTER, 0);
                self->Notify(text);
                return;
            }
            self->discovery_ = std::move(*result);
            self->RenderDiscovery();
            if (!error.empty()) self->Notify(error);
        });
    }).detach();
}

void MusicView::RenderSection(const char *title,
                              const std::vector<CatalogItem> &items,
                              bool circular) {
    if (items.empty()) return;
    const auto &p = jetson::UiTheme::Instance().Palette();
    auto *heading = lv_label_create(page_);
    lv_obj_set_style_text_font(heading, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(heading, Color(p.text), 0);
    lv_label_set_text(heading, title);

    auto *rail = lv_obj_create(page_);
    lv_obj_remove_style_all(rail);
    lv_obj_set_size(rail, lv_pct(100), kRailHeight);
    lv_obj_set_flex_flow(rail, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(rail, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(rail, 12, 0);
    lv_obj_add_flag(rail, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE |
                                          LV_OBJ_FLAG_SCROLL_CHAIN_VER));
    lv_obj_set_scroll_dir(rail, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(rail, LV_SCROLLBAR_MODE_OFF);

    for (const auto &item : items) {
        auto *card = lv_obj_create(rail);
        lv_obj_remove_style_all(card);
        lv_obj_set_size(card, kCardWidth, kRailHeight - 2);
        lv_obj_set_style_radius(card, 12, 0);
        lv_obj_set_style_bg_color(card, Color(p.row_active), LV_STATE_HOVERED);
        lv_obj_set_style_bg_opa(card, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_50, LV_STATE_HOVERED);
        lv_obj_set_style_pad_all(card, 6, 0);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        auto *art = CreateArtwork(card, item.thumbnail_path, kCoverSize, circular);
        lv_obj_align(art, LV_ALIGN_TOP_MID, 0, 0);

        auto *hover = lv_obj_create(art);
        lv_obj_remove_style_all(hover);
        lv_obj_set_size(hover, lv_pct(100), lv_pct(100));
        lv_obj_set_style_bg_color(hover, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(hover, LV_OPA_50, 0);
        lv_obj_set_style_radius(hover, circular ? LV_RADIUS_CIRCLE : 10, 0);
        RemoveInteraction(hover);
        auto *play = lv_obj_create(hover);
        lv_obj_remove_style_all(play);
        lv_obj_set_size(play, 44, 44);
        lv_obj_set_style_radius(play, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(play, Color(0x22d3ee), 0);
        lv_obj_set_style_bg_opa(play, LV_OPA_COVER, 0);
        lv_obj_center(play);
        auto *play_icon = lv_label_create(play);
        lv_obj_set_style_text_font(play_icon, &BUILTIN_ICON_FONT, 0);
        lv_obj_set_style_text_color(play_icon, lv_color_white(), 0);
        lv_label_set_text(play_icon, LV_SYMBOL_PLAY);
        lv_obj_center(play_icon);
        RemoveInteraction(play);
        RemoveInteraction(play_icon);
        lv_obj_add_flag(hover, LV_OBJ_FLAG_HIDDEN);

        auto *name = lv_label_create(card);
        lv_obj_set_size(name, kCardWidth - 10, 23);
        lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, -23);
        lv_obj_set_style_text_font(name, &BUILTIN_SMALL_TEXT_FONT, 0);
        lv_obj_set_style_text_color(name, Color(p.text), 0);
        lv_obj_set_style_text_align(name, circular ? LV_TEXT_ALIGN_CENTER
                                                   : LV_TEXT_ALIGN_LEFT, 0);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_label_set_text(name, item.title.c_str());
        RemoveInteraction(name);

        auto *subtitle = lv_label_create(card);
        lv_obj_set_size(subtitle, kCardWidth - 10, 19);
        lv_obj_align(subtitle, LV_ALIGN_BOTTOM_MID, 0, -3);
        lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(subtitle, Color(p.sub_text), 0);
        lv_obj_set_style_text_align(subtitle, circular ? LV_TEXT_ALIGN_CENTER
                                                       : LV_TEXT_ALIGN_LEFT, 0);
        lv_label_set_long_mode(subtitle, LV_LABEL_LONG_DOT);
        lv_label_set_text(subtitle,
            circular ? "Artist" : item.subtitle.c_str());
        RemoveInteraction(subtitle);

        auto *ctx = new CardCtx{this, item, hover};
        lv_obj_add_event_cb(card, OnCardEvent, LV_EVENT_ALL, ctx);
        lv_obj_add_event_cb(card, OnCardDeleted, LV_EVENT_DELETE, ctx);
    }
}

void MusicView::RenderDiscovery() {
    album_mode_ = false;
    ClearPage();
    RenderSection("Trending songs", discovery_.trending, false);
    RenderSection("Popular artists", discovery_.artists, true);
    RenderSection("Radio", discovery_.radio, false);
    RenderSection("Top 100", discovery_.top100, false);
    lv_obj_scroll_to_y(page_, 0, LV_ANIM_OFF);
}

void MusicView::OpenItem(const CatalogItem &item) {
    if (item.kind == CatalogKind::Song) {
        if (item.premium) {
            Notify("Bài hát này cần tài khoản Zing MP3 Premium");
            return;
        }
        if (item.id.rfind("demo:", 0) == 0) {
            Notify("Zing đang từ chối phiên hiện tại; hãy cập nhật ZING_COOKIES rồi thử lại");
            return;
        }
        std::vector<Track> queue;
        size_t selected = 0;
        for (const auto &candidate : discovery_.trending) {
            if (candidate.kind != CatalogKind::Song || candidate.premium) continue;
            if (candidate.id == item.id) selected = queue.size();
            Track track;
            track.id = candidate.id;
            track.title = candidate.title;
            track.artist = candidate.subtitle;
            track.artwork_url = candidate.thumbnail_url;
            track.artwork_path = candidate.thumbnail_path;
            track.duration_ms = static_cast<int64_t>(candidate.duration_seconds) * 1000;
            queue.push_back(std::move(track));
        }
        if (!queue.empty()) PlayerController::Instance().PlayQueue(std::move(queue), selected);
        return;
    }
    if (item.kind == CatalogKind::Radio) {
        if (item.premium) {
            Notify("Kênh Radio này cần tài khoản Zing MP3 Premium");
            return;
        }
        if (item.id.rfind("demo:", 0) == 0) {
            Notify("Radio mẫu không thể phát; hãy cập nhật ZING_COOKIES rồi thử lại");
            return;
        }
        Track station;
        station.id = item.id;
        station.title = item.title;
        station.artist = item.subtitle.empty() ? "Zing Radio" : item.subtitle;
        station.artwork_url = item.thumbnail_url;
        station.artwork_path = item.thumbnail_path;
        PlayerController::Instance().PlayQueue({std::move(station)}, 0);
        return;
    }
    pending_item_ = item;
    LoadAlbum(item);
}

void MusicView::LoadAlbum(const CatalogItem &item) {
    if (loading_) return;
    loading_ = true;
    album_mode_ = true;
    const uint64_t generation = request_generation_.fetch_add(1) + 1;
    pending_item_ = item;
    ClearPage();
    const auto &p = jetson::UiTheme::Instance().Palette();

    auto *toolbar = lv_obj_create(page_);
    lv_obj_remove_style_all(toolbar);
    lv_obj_set_size(toolbar, lv_pct(100), 42);
    auto *back = MakeIconButton(toolbar, LV_SYMBOL_LEFT, 40, OnBack, this);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 0, 0);
    loading_label_ = lv_label_create(toolbar);
    lv_obj_set_size(loading_label_, 500, 28);
    lv_obj_align(loading_label_, LV_ALIGN_LEFT_MID, 56, 0);
    lv_obj_set_style_text_font(loading_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(loading_label_, Color(p.text), 0);
    lv_label_set_long_mode(loading_label_, LV_LABEL_LONG_DOT);
    lv_label_set_text(loading_label_, item.title.c_str());
    auto *header_skeleton = lv_obj_create(page_);
    lv_obj_set_size(header_skeleton, lv_pct(100), 142);
    lv_obj_set_style_radius(header_skeleton, 16, 0);
    lv_obj_set_style_bg_color(header_skeleton, Color(p.button), 0);
    lv_obj_set_style_bg_opa(header_skeleton, LV_OPA_70, 0);
    for (int i = 0; i < 5; ++i) {
        auto *row = lv_obj_create(page_);
        lv_obj_set_size(row, lv_pct(100), 52);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_bg_color(row, Color(p.button), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_50, 0);
    }

    auto album = std::make_shared<jetson::music::Album>();
    std::weak_ptr<MusicView> weak =
        std::static_pointer_cast<MusicView>(shared_from_this());
    std::thread([weak, album, id = item.id, generation]() {
        std::string error;
        bool ok = false;
        try {
            jetson::ZingMusicClient client;
            ok = client.FetchAlbum(id, *album, error);
        } catch (const std::exception &exception) {
            error = std::string("Không thể đọc album Zing: ") + exception.what();
        } catch (...) {
            error = "Không thể đọc album Zing";
        }
        Application::GetInstance().Schedule([weak, album, ok, error, generation]() {
            auto self = weak.lock();
            if (!self) return;
            LvglLockGuard lock;
            if (generation != self->request_generation_.load()) return;
            self->loading_ = false;
            if (!ok) {
                self->ClearPage();
                const auto &p = jetson::UiTheme::Instance().Palette();
                auto *bar = lv_obj_create(self->page_);
                lv_obj_remove_style_all(bar);
                lv_obj_set_size(bar, lv_pct(100), 46);
                auto *back = MakeIconButton(bar, LV_SYMBOL_LEFT, 40,
                                            OnBack, self.get());
                lv_obj_align(back, LV_ALIGN_LEFT_MID, 0, 0);
                auto *message = lv_label_create(self->page_);
                lv_obj_set_width(message, lv_pct(100));
                lv_obj_set_style_text_font(message, &BUILTIN_TEXT_FONT, 0);
                lv_obj_set_style_text_color(message, Color(p.sub_text), 0);
                lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
                lv_label_set_long_mode(message, LV_LABEL_LONG_WRAP);
                lv_label_set_text(message, error.empty()
                    ? "Không tải được album." : error.c_str());
                auto *retry = MakeIconButton(self->page_, LV_SYMBOL_REFRESH, 46,
                                             OnRetry, self.get());
                lv_obj_set_style_align(retry, LV_ALIGN_CENTER, 0);
                self->Notify(error.empty() ? "Không tải được album" : error);
                return;
            }
            self->album_ = std::move(*album);
            self->RenderAlbum();
        });
    }).detach();
}

void MusicView::RenderAlbum() {
    ClearPage();
    album_mode_ = true;
    const auto &p = jetson::UiTheme::Instance().Palette();

    auto *toolbar = lv_obj_create(page_);
    lv_obj_remove_style_all(toolbar);
    lv_obj_set_size(toolbar, lv_pct(100), 42);
    auto *back = MakeIconButton(toolbar, LV_SYMBOL_LEFT, 40, OnBack, this);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 0, 0);
    auto *toolbar_title = lv_label_create(toolbar);
    lv_obj_set_size(toolbar_title, 600, 30);
    lv_obj_align(toolbar_title, LV_ALIGN_LEFT_MID, 56, 0);
    lv_obj_set_style_text_font(toolbar_title, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(toolbar_title, Color(p.text), 0);
    lv_label_set_long_mode(toolbar_title, LV_LABEL_LONG_DOT);
    lv_label_set_text(toolbar_title, album_.title.c_str());

    auto *header = lv_obj_create(page_);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, lv_pct(100), 150);
    lv_obj_set_style_radius(header, 16, 0);
    lv_obj_set_style_bg_color(header, Color(0x5b2677), 0);
    lv_obj_set_style_bg_grad_color(header, Color(0x17131f), 0);
    lv_obj_set_style_bg_grad_dir(header, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_clip_corner(header, true, 0);
    RemoveInteraction(header);

    auto *cover = CreateArtwork(header, album_.artwork_path, 126, false);
    lv_obj_align(cover, LV_ALIGN_LEFT_MID, 12, 0);
    auto *kind = lv_label_create(header);
    lv_obj_set_pos(kind, 156, 18);
    lv_obj_set_style_text_font(kind, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(kind, Color(0xd8b4fe), 0);
    lv_label_set_text(kind, "ALBUM / PLAYLIST");
    auto *title = lv_label_create(header);
    lv_obj_set_pos(title, 156, 44);
    lv_obj_set_size(title, width_ - 330, 42);
    lv_obj_set_style_text_font(title, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_label_set_text(title, album_.title.c_str());
    auto *creator = lv_label_create(header);
    lv_obj_set_pos(creator, 156, 91);
    lv_obj_set_size(creator, width_ - 330, 24);
    lv_obj_set_style_text_font(creator, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(creator, Color(0xd1d5db), 0);
    lv_label_set_long_mode(creator, LV_LABEL_LONG_DOT);
    std::string details = album_.creator;
    if (!details.empty() && !album_.tracks.empty()) details += "  •  ";
    if (!album_.tracks.empty()) details += std::to_string(album_.tracks.size()) + " bài hát";
    lv_label_set_text(creator, details.c_str());
    auto *play_all = MakeIconButton(header, LV_SYMBOL_PLAY, 54,
                                    OnPlayAll, this);
    lv_obj_set_style_bg_color(play_all, Color(0x22d3ee), 0);
    lv_obj_align(play_all, LV_ALIGN_RIGHT_MID, -20, 0);

    auto *columns = lv_obj_create(page_);
    lv_obj_remove_style_all(columns);
    lv_obj_set_size(columns, lv_pct(100), 30);
    auto add_col = [&](const char *text, int x, int w) {
        auto *label = lv_label_create(columns);
        lv_obj_set_pos(label, x, 4);
        lv_obj_set_size(label, w, 22);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(label, Color(p.sub_text), 0);
        lv_label_set_text(label, text);
    };
    add_col("#", 12, 28);
    add_col("Bài hát", 58, 340);
    add_col("Album", 430, 210);
    add_col(LV_SYMBOL_AUDIO, width_ - 82, 36);

    track_rows_.clear();
    for (size_t i = 0; i < album_.tracks.size(); ++i) {
        const auto &track = album_.tracks[i];
        auto *row = lv_obj_create(page_);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), 56);
        lv_obj_set_style_radius(row, 9, 0);
        lv_obj_set_style_bg_color(row, Color(p.row_active), LV_STATE_HOVERED);
        lv_obj_set_style_bg_color(row, Color(p.row_active), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_HOVERED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        char number[12];
        std::snprintf(number, sizeof(number), "%zu", i + 1);
        auto *index = lv_label_create(row);
        lv_obj_set_pos(index, 12, 17);
        lv_obj_set_size(index, 30, 22);
        lv_obj_set_style_text_font(index, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(index, Color(p.sub_text), 0);
        lv_obj_set_style_text_align(index, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(index, number);
        RemoveInteraction(index);

        auto *art = CreateArtwork(row, track.artwork_path, 40, false);
        lv_obj_set_pos(art, 52, 8);
        auto *song = lv_label_create(row);
        lv_obj_set_pos(song, 104, 6);
        lv_obj_set_size(song, 310, 24);
        lv_obj_set_style_text_font(song, &BUILTIN_SMALL_TEXT_FONT, 0);
        lv_obj_set_style_text_color(song, Color(p.text), 0);
        lv_label_set_long_mode(song, LV_LABEL_LONG_DOT);
        lv_label_set_text(song, track.title.c_str());
        RemoveInteraction(song);
        auto *artist = lv_label_create(row);
        lv_obj_set_pos(artist, 104, 30);
        lv_obj_set_size(artist, 310, 20);
        lv_obj_set_style_text_font(artist, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(artist, Color(p.sub_text), 0);
        lv_label_set_long_mode(artist, LV_LABEL_LONG_DOT);
        lv_label_set_text(artist, track.artist.c_str());
        RemoveInteraction(artist);
        auto *album_name = lv_label_create(row);
        lv_obj_set_pos(album_name, 430, 17);
        lv_obj_set_size(album_name, 205, 22);
        lv_obj_set_style_text_font(album_name, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(album_name, Color(p.sub_text), 0);
        lv_label_set_long_mode(album_name, LV_LABEL_LONG_DOT);
        lv_label_set_text(album_name, album_.title.c_str());
        RemoveInteraction(album_name);
        auto *duration = lv_label_create(row);
        lv_obj_set_size(duration, 54, 22);
        lv_obj_align(duration, LV_ALIGN_RIGHT_MID, -10, 0);
        lv_obj_set_style_text_font(duration, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(duration, Color(p.sub_text), 0);
        lv_obj_set_style_text_align(duration, LV_TEXT_ALIGN_RIGHT, 0);
        const std::string duration_text = DurationText(track.duration_ms);
        lv_label_set_text(duration, duration_text.c_str());
        RemoveInteraction(duration);
        auto *action = lv_label_create(row);
        lv_obj_set_size(action, 24, 24);
        lv_obj_align(action, LV_ALIGN_RIGHT_MID, -76, 0);
        lv_obj_set_style_text_font(action, &BUILTIN_ICON_FONT, 0);
        lv_obj_set_style_text_color(action, Color(p.text), 0);
        lv_label_set_text(action, LV_SYMBOL_PLUS);
        lv_obj_add_flag(action, LV_OBJ_FLAG_HIDDEN);
        RemoveInteraction(action);

        auto *ctx = new TrackCtx{this, i, row, action};
        track_rows_.push_back(ctx);
        lv_obj_add_event_cb(row, OnTrackEvent, LV_EVENT_ALL, ctx);
        lv_obj_add_event_cb(row, OnTrackDeleted, LV_EVENT_DELETE, ctx);
    }
    lv_obj_scroll_to_y(page_, 0, LV_ANIM_OFF);
    RefreshTrackRows();
}

void MusicView::ShowDiscovery() {
    if (loading_) {
        request_generation_.fetch_add(1);
        loading_ = false;
    }
    if (discovery_.trending.empty() && discovery_.artists.empty() &&
        discovery_.radio.empty() && discovery_.top100.empty()) {
        LoadDiscovery();
        return;
    }
    RenderDiscovery();
}

void MusicView::PlayTrack(size_t index) {
    if (index >= album_.tracks.size()) return;
    if (album_.tracks[index].premium) {
        Notify("Bài hát này cần tài khoản Zing MP3 Premium");
        return;
    }
    if (album_.tracks[index].id.rfind("demo:", 0) == 0 ||
        album_.tracks[index].id.find(":track:") != std::string::npos) {
        Notify("Đây là dữ liệu mẫu ngoại tuyến; cần ZING_COOKIES hợp lệ để phát nhạc");
        return;
    }
    std::vector<Track> playable;
    size_t selected = 0;
    for (size_t i = 0; i < album_.tracks.size(); ++i) {
        if (album_.tracks[i].premium) continue;
        if (i == index) selected = playable.size();
        playable.push_back(album_.tracks[i]);
    }
    if (!playable.empty())
        PlayerController::Instance().PlayQueue(std::move(playable), selected);
}

void MusicView::PlayAll() {
    if (album_.tracks.empty()) return;
    std::vector<Track> playable;
    for (const auto &track : album_.tracks) {
        if (!track.premium) playable.push_back(track);
    }
    if (playable.empty()) {
        Notify("Album này không có bài hát miễn phí");
        return;
    }
    if (playable.front().id.rfind("demo:", 0) == 0 ||
        playable.front().id.find(":track:") != std::string::npos) {
        Notify("Đây là dữ liệu mẫu ngoại tuyến; cần ZING_COOKIES hợp lệ để phát nhạc");
        return;
    }
    PlayerController::Instance().PlayQueue(std::move(playable), 0);
}

void MusicView::Notify(const std::string &message) {
    if (notify_cb_ && !message.empty()) notify_cb_(message.c_str());
}

void MusicView::RefreshTrackRows() {
    if (!album_mode_) return;
    const auto snapshot = PlayerController::Instance().Snapshot();
    const auto &p = jetson::UiTheme::Instance().Palette();
    for (auto *ctx : track_rows_) {
        if (!ctx || !ctx->row || ctx->index >= album_.tracks.size()) continue;
        const bool current = snapshot.has_current &&
                             snapshot.current.id == album_.tracks[ctx->index].id;
        lv_obj_set_style_bg_color(ctx->row,
                                  Color(current ? p.row_active : p.row), 0);
        lv_obj_set_style_bg_opa(ctx->row,
                                current ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
}

void MusicView::RefreshPlayerBar() {
    const auto snapshot = PlayerController::Instance().Snapshot();
    if (!snapshot.has_current || snapshot.status == PlaybackStatus::Idle) {
        if (!lv_obj_has_flag(player_bar_, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(player_bar_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_height(body_, height_ - kHeaderHeight);
        }
        return;
    }
    if (lv_obj_has_flag(player_bar_, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_clear_flag(player_bar_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(body_, height_ - kHeaderHeight - kPlayerBarHeight);
    }

    lv_label_set_text(player_title_, snapshot.current.title.c_str());
    if (snapshot.status == PlaybackStatus::Error && !snapshot.error.empty()) {
        lv_label_set_text(player_artist_, snapshot.error.c_str());
        if (snapshot.revision != notified_player_error_revision_) {
            notified_player_error_revision_ = snapshot.revision;
            Notify(snapshot.error);
        }
    } else {
        lv_label_set_text(player_artist_, snapshot.current.artist.c_str());
    }
    const bool playing = snapshot.status == PlaybackStatus::Playing ||
                         snapshot.status == PlaybackStatus::Buffering ||
                         snapshot.status == PlaybackStatus::Resolving;
    lv_label_set_text(player_toggle_label_, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    const int progress = snapshot.duration_ms > 0
        ? static_cast<int>(std::clamp<int64_t>(
              snapshot.position_ms * 1000 / snapshot.duration_ms, 0, 1000))
        : 0;
    lv_bar_set_value(player_progress_, progress, LV_ANIM_OFF);

    if (rendered_player_art_ != snapshot.current.artwork_path) {
        rendered_player_art_ = snapshot.current.artwork_path;
        if (player_art_obj_) {
            lv_image_set_src(player_art_obj_, nullptr);
            lv_obj_del(player_art_obj_);
            player_art_obj_ = nullptr;
        }
        if (player_artwork_)
            lv_image_cache_drop(player_artwork_->image_dsc());
        player_artwork_.reset();
        if (!rendered_player_art_.empty())
            player_artwork_ = LvglImageFromFile(rendered_player_art_);
        if (player_artwork_) {
            player_art_obj_ = lv_image_create(player_art_host_);
            int w, h;
            ImageDimensions(player_artwork_->image_dsc(), w, h);
            lv_image_set_src(player_art_obj_, player_artwork_->image_dsc());
            lv_obj_set_size(player_art_obj_, w, h);
            lv_image_set_scale(player_art_obj_,
                static_cast<uint32_t>(46 * 256 / std::max(1, std::min(w, h))));
            lv_image_set_pivot(player_art_obj_, w / 2, h / 2);
            lv_obj_center(player_art_obj_);
            RemoveInteraction(player_art_obj_);
        }
    }
    if (snapshot.revision != rendered_player_revision_) {
        rendered_player_revision_ = snapshot.revision;
        RefreshTrackRows();
    }
}

void MusicView::OnCardEvent(lv_event_t *e) {
    auto *ctx = static_cast<CardCtx *>(lv_event_get_user_data(e));
    if (!ctx) return;
    const auto code = lv_event_get_code(e);
    if (code == LV_EVENT_HOVER_OVER || code == LV_EVENT_PRESSED) {
        if (ctx->overlay) lv_obj_clear_flag(ctx->overlay, LV_OBJ_FLAG_HIDDEN);
    } else if (code == LV_EVENT_HOVER_LEAVE || code == LV_EVENT_PRESS_LOST) {
        if (ctx->overlay) lv_obj_add_flag(ctx->overlay, LV_OBJ_FLAG_HIDDEN);
    } else if (code == LV_EVENT_RELEASED) {
        auto *target = static_cast<lv_obj_t *>(lv_event_get_target(e));
        if (ctx->overlay && (!target || !lv_obj_has_state(target, LV_STATE_HOVERED)))
            lv_obj_add_flag(ctx->overlay, LV_OBJ_FLAG_HIDDEN);
    } else if (code == LV_EVENT_CLICKED) {
        LvglLockGuard lock;
        ctx->self->OpenItem(ctx->item);
    }
}

void MusicView::OnCardDeleted(lv_event_t *e) {
    delete static_cast<CardCtx *>(lv_event_get_user_data(e));
}

void MusicView::OnTrackEvent(lv_event_t *e) {
    auto *ctx = static_cast<TrackCtx *>(lv_event_get_user_data(e));
    if (!ctx) return;
    const auto code = lv_event_get_code(e);
    if (code == LV_EVENT_HOVER_OVER || code == LV_EVENT_PRESSED) {
        if (ctx->action) lv_obj_clear_flag(ctx->action, LV_OBJ_FLAG_HIDDEN);
    } else if (code == LV_EVENT_HOVER_LEAVE || code == LV_EVENT_PRESS_LOST) {
        if (ctx->action) lv_obj_add_flag(ctx->action, LV_OBJ_FLAG_HIDDEN);
    } else if (code == LV_EVENT_RELEASED) {
        auto *target = static_cast<lv_obj_t *>(lv_event_get_target(e));
        if (ctx->action && (!target || !lv_obj_has_state(target, LV_STATE_HOVERED)))
            lv_obj_add_flag(ctx->action, LV_OBJ_FLAG_HIDDEN);
    } else if (code == LV_EVENT_CLICKED) {
        LvglLockGuard lock;
        ctx->self->PlayTrack(ctx->index);
    }
}

void MusicView::OnTrackDeleted(lv_event_t *e) {
    delete static_cast<TrackCtx *>(lv_event_get_user_data(e));
}

void MusicView::OnBack(lv_event_t *e) {
    LvglLockGuard lock;
    static_cast<MusicView *>(lv_event_get_user_data(e))->ShowDiscovery();
}

void MusicView::OnRetry(lv_event_t *e) {
    LvglLockGuard lock;
    auto *self = static_cast<MusicView *>(lv_event_get_user_data(e));
    if (self->album_mode_) self->LoadAlbum(self->pending_item_);
    else self->LoadDiscovery();
}

void MusicView::OnPlayAll(lv_event_t *e) {
    LvglLockGuard lock;
    static_cast<MusicView *>(lv_event_get_user_data(e))->PlayAll();
}

void MusicView::OnPlayerToggle(lv_event_t *e) {
    PlayerController::Instance().Toggle();
}

void MusicView::OnPlayerPrevious(lv_event_t *e) {
    PlayerController::Instance().Previous();
}

void MusicView::OnPlayerNext(lv_event_t *e) {
    PlayerController::Instance().Next();
}

void MusicView::OnPlayerTimer(lv_timer_t *t) {
    auto *self = static_cast<MusicView *>(lv_timer_get_user_data(t));
    LvglLockGuard lock;
    self->RefreshPlayerBar();
}

} // namespace home
