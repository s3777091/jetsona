#pragma once

#include "display/core/lvgl_image.h"
#include "display/views/overlay_view.h"
#include "media/music_types.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace home {

/* Native music browser/player for the 800x480 desktop.
 *
 * Discovery is one vertically scrolling page containing six horizontally
 * scrolling rails (Dành riêng cho bạn, Mới phát hành, Chill, Top 100,
 * Nghệ sĩ, Radio). Album details replace that page in-place, so Music remains
 * one warm multitasking app. Zing requests and artwork downloads always run
 * off the LVGL thread; PlayerController owns playback after the view is hidden
 * or closed. */
class MusicView : public OverlayView {
public:
    using NotifyCb = std::function<void(const char *)>;

    MusicView(lv_obj_t *parent, int width, int height, ClosedCb on_closed);
    ~MusicView() override;

    void SetNotifyCb(NotifyCb cb) { notify_cb_ = std::move(cb); }

protected:
    void OnStart() override;

private:
    struct CardCtx {
        MusicView *self = nullptr;
        jetson::music::CatalogItem item;
        lv_obj_t *overlay = nullptr;
    };

    struct TrackCtx {
        MusicView *self = nullptr;
        size_t index = 0;
        lv_obj_t *row = nullptr;
        lv_obj_t *action = nullptr;
    };

    void BuildBody();
    void BuildSkeleton();
    void LoadDiscovery();
    void RenderDiscovery();
    void RenderSection(const char *title,
                       const std::vector<jetson::music::CatalogItem> &items,
                       bool circular);
    void OpenItem(const jetson::music::CatalogItem &item);
    void LoadAlbum(const jetson::music::CatalogItem &item);
    void RenderAlbum();
    void ShowDiscovery();
    void ClearPage();
    void ClearArtwork();
    void Notify(const std::string &message);

    lv_obj_t *CreateArtwork(lv_obj_t *parent, const std::string &path,
                            int size, bool circular);
    lv_obj_t *CreateSkeletonCard(lv_obj_t *rail, bool circular);
    void PlayTrack(size_t index);
    void PlayAll();
    void RefreshPlayerBar();
    void RefreshTrackRows();

    static void OnCardEvent(lv_event_t *e);
    static void OnCardDeleted(lv_event_t *e);
    static void OnBack(lv_event_t *e);
    static void OnRetry(lv_event_t *e);
    static void OnPlayAll(lv_event_t *e);
    static void OnTrackEvent(lv_event_t *e);
    static void OnTrackDeleted(lv_event_t *e);
    static void OnPlayerToggle(lv_event_t *e);
    static void OnPlayerPrevious(lv_event_t *e);
    static void OnPlayerNext(lv_event_t *e);
    static void OnPlayerTimer(lv_timer_t *t);

    lv_obj_t *page_ = nullptr;
    lv_obj_t *player_bar_ = nullptr;
    lv_obj_t *player_art_host_ = nullptr;
    lv_obj_t *player_art_obj_ = nullptr;
    lv_obj_t *player_title_ = nullptr;
    lv_obj_t *player_artist_ = nullptr;
    lv_obj_t *player_toggle_label_ = nullptr;
    lv_obj_t *player_progress_ = nullptr;
    lv_obj_t *loading_label_ = nullptr;
    lv_timer_t *player_timer_ = nullptr;

    bool loading_ = false;
    bool album_mode_ = false;
    bool started_ = false;
    std::atomic<uint64_t> request_generation_{0};
    uint64_t rendered_player_revision_ = 0;
    uint64_t notified_player_error_revision_ = 0;
    std::string rendered_player_art_;
    jetson::music::CatalogItem pending_item_;
    jetson::music::DiscoverData discovery_;
    jetson::music::Album album_;
    std::vector<TrackCtx *> track_rows_;
    std::vector<lv_obj_t *> image_objects_;
    std::vector<std::unique_ptr<LvglImage>> artwork_;
    std::unordered_map<std::string, LvglImage *> artwork_by_path_;
    std::unique_ptr<LvglImage> player_artwork_;
    NotifyCb notify_cb_;
};

} // namespace home
