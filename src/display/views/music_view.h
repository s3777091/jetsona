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
    };

    // Per-row context for the "add to album" modal's album picker.
    struct PickCtx {
        MusicView *self = nullptr;
        std::string album_id;
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

    /* User-owned albums. The list lives where the Zing "Radio" rail used to be,
     * so there is no separate library screen; the "+" on a track row opens a
     * modal to pick (or create) the album to add it to. */
    void RenderUserAlbums();
    void OpenUserAlbum(const std::string &album_id);
    void OpenAddToAlbumModal(size_t index);
    void CloseAddModal();

    lv_obj_t *CreateArtwork(lv_obj_t *parent, const std::string &path,
                            int size, bool circular);
    lv_obj_t *CreateSkeletonCard(lv_obj_t *rail, bool circular);
    /* Pull-to-refresh: dragging the page below its top edge (LVGL's elastic
     * overscroll) reveals a down-arrow badge; releasing past the threshold
     * reloads whatever the page currently shows. */
    void AddPullHint(const char *text, bool with_spacer);
    void PullRefresh();
    void RegisterPullInput(lv_indev_t *indev);
    void PlayTrack(size_t index);
    void PlayAll();
    void RefreshTrackRows();

    static void OnPageScroll(lv_event_t *e);
    static void OnPullInput(lv_event_t *e);
    static void OnCardEvent(lv_event_t *e);
    static void OnCardDeleted(lv_event_t *e);
    static void OnBack(lv_event_t *e);
    static void OnPlayAll(lv_event_t *e);
    static void OnTrackEvent(lv_event_t *e);
    static void OnTrackDeleted(lv_event_t *e);
    static void OnPlayerTimer(lv_timer_t *t);
    static void OnAddToAlbum(lv_event_t *e);
    static void OnAddModalDismiss(lv_event_t *e);
    static void OnAddModalDeleted(lv_event_t *e);
    static void OnPickAlbum(lv_event_t *e);
    static void OnPickDeleted(lv_event_t *e);
    static void OnCreateAlbum(lv_event_t *e);

    lv_obj_t *page_ = nullptr;
    lv_obj_t *loading_label_ = nullptr;
    lv_obj_t *add_modal_ = nullptr;
    lv_obj_t *pull_indicator_ = nullptr;
    lv_obj_t *pull_icon_ = nullptr;
    bool pull_tracking_ = false;
    bool pull_armed_ = false;
    lv_point_t pull_press_point_ {};
    bool pull_refresh_scheduled_ = false;
    std::vector<lv_indev_t *> pull_inputs_;
    lv_timer_t *player_timer_ = nullptr;
    jetson::music::Track pending_add_track_;

    bool loading_ = false;
    bool album_mode_ = false;
    bool started_ = false;
    std::atomic<uint64_t> request_generation_{0};
    uint64_t rendered_player_revision_ = 0;
    jetson::music::CatalogItem pending_item_;
    jetson::music::DiscoverData discovery_;
    jetson::music::Album album_;
    std::vector<TrackCtx *> track_rows_;
    std::vector<lv_obj_t *> image_objects_;
    std::vector<std::unique_ptr<LvglImage>> artwork_;
    // Pre-decoded covers, keyed by "<file path>#<box px>" (one decoded copy
    // per rendered size; see CreateArtwork).
    std::unordered_map<std::string, LvglImage *> artwork_by_path_;
    NotifyCb notify_cb_;
};

} // namespace home
