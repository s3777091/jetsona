#include "display/views/pods_view.h"
#include "display/common/lvgl_utils.h"
#include "display/core/app_icons.h"
#include "display/theme/ui_theme.h"
#include "fonts.h"
#include "application.h"
#include "esp_log.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <thread>
#include <utility>

#define TAG "PodsView"

namespace home {

using jetson::ui::Color;
using jetson::ui::LvglLockGuard;

namespace {
constexpr uint32_t kRunningGreen = 0x30d158;
constexpr uint32_t kStoppedGray  = 0x8e8e93;
constexpr uint32_t kDangerRed    = 0xff453a;
// Web-IDE password baked into the presets (shown in the detail sheet).
constexpr const char *kIdePassword = "jetsona";

struct Preset {
    const char *label;
    const char *image;
    const char *http_port;   // "8080/http"
    const char *env_key;     // password env var the image understands
};
const Preset kPresets[2] = {
    {"VS Code Studio", "codercom/code-server:latest", "8080/http", "PASSWORD"},
    {"PyTorch + Jupyter",
     "runpod/pytorch:2.1.0-py3.10-cuda11.8.0-devel-ubuntu22.04",
     "8888/http", "JUPYTER_PASSWORD"},
};

lv_obj_t *MakeSmallButton(lv_obj_t *parent, const char *text, bool icon_font) {
    const auto &p = jetson::UiTheme::Instance().Palette();
    auto *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, LV_SIZE_CONTENT, 36);
    lv_obj_set_style_pad_hor(btn, 10, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_bg_color(btn, Color(p.button), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    auto *label = lv_label_create(btn);
    lv_obj_set_style_text_font(label,
        icon_font ? &BUILTIN_ICON_FONT : &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(label, Color(p.text), 0);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return btn;
}

lv_obj_t *ButtonLabel(lv_obj_t *btn) { return lv_obj_get_child(btn, 0); }
} // namespace

PodsView::PodsView(lv_obj_t *parent, int width, int height, ClosedCb on_closed)
    : OverlayView(parent, width, height, "Pods", std::move(on_closed)) {
    BuildBody();
}

PodsView::~PodsView() = default;

void PodsView::OnStart() { Refresh(); }

void PodsView::BuildBody() {
    const auto &p = jetson::UiTheme::Instance().Palette();
    lv_obj_set_flex_flow(body_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(body_, 8, 0);
    lv_obj_set_style_pad_row(body_, 6, 0);
    lv_obj_clear_flag(body_, LV_OBJ_FLAG_SCROLLABLE);

    // ---- Toolbar: status text | "+ Thuê GPU" | refresh ----
    auto *bar = lv_obj_create(body_);
    lv_obj_remove_style_all(bar);
    lv_obj_set_width(bar, lv_pct(100));
    lv_obj_set_height(bar, 40);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 8, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    status_line_ = lv_label_create(bar);
    lv_obj_set_flex_grow(status_line_, 1);
    lv_obj_set_style_text_font(status_line_, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(status_line_, Color(p.sub_text), 0);
    lv_label_set_long_mode(status_line_, LV_LABEL_LONG_DOT);
    lv_label_set_text(status_line_, "");

    // ASCII "+" only: the text TTF has no symbol glyphs (see memory/logs).
    auto *rent = MakeSmallButton(bar, "+ Thuê GPU", false);
    lv_obj_set_style_bg_color(rent, Color(p.accent), 0);
    lv_obj_add_event_cb(rent, OnRent, LV_EVENT_CLICKED, this);

    auto *refresh = MakeSmallButton(bar, "", true);
    auto *reload_ic = jetson::ui::CreateAppIcon(refresh, "reload", 18);
    lv_obj_set_style_image_recolor(reload_ic, Color(p.text), 0);
    lv_obj_set_style_image_recolor_opa(reload_ic, LV_OPA_COVER, 0);
    lv_obj_center(reload_ic);
    lv_obj_delete(ButtonLabel(refresh)); // PNG replaces the empty glyph label
    lv_obj_add_event_cb(refresh, OnRefresh, LV_EVENT_CLICKED, this);

    // ---- Pod list ----
    list_ = lv_obj_create(body_);
    lv_obj_remove_style_all(list_);
    lv_obj_set_width(list_, lv_pct(100));
    lv_obj_set_flex_grow(list_, 1);
    lv_obj_set_flex_flow(list_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list_, 6, 0);
    lv_obj_add_flag(list_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list_, LV_SCROLLBAR_MODE_AUTO);
}

void PodsView::SetStatusLine(const std::string &text) {
    if (status_line_) lv_label_set_text(status_line_, text.c_str());
}

void PodsView::Notify(const char *msg) {
    if (notify_cb_) notify_cb_(msg);
}

const jetson::RunpodPod *PodsView::FindPod(const std::string &id) const {
    for (const auto &pod : pods_)
        if (pod.id == id) return &pod;
    return nullptr;
}

// ---- Async plumbing --------------------------------------------------------

void PodsView::RunAsync(std::function<std::string(jetson::RunpodClient &)> work,
                        std::function<void(PodsView &, const std::string &)> done) {
    auto self = std::static_pointer_cast<PodsView>(shared_from_this());
    std::weak_ptr<PodsView> weak = self;
    std::thread([weak, work = std::move(work), done = std::move(done)]() {
        // The client is small (strings); a private copy keeps the worker safe
        // even if the view is destroyed mid-request.
        jetson::RunpodClient client;
        std::string err = work(client);
        Application::GetInstance().Schedule([weak, done, err]() {
            auto sp = weak.lock();
            if (!sp) return;
            LvglLockGuard lock;
            done(*sp, err);
        });
    }).detach();
}

void PodsView::Refresh() {
    if (busy_) return;
    busy_ = true;
    SetStatusLine("Đang tải danh sách pod...");
    // The worker writes into a shared buffer (never touches the view), the
    // done-callback swaps it in on the LVGL thread.
    auto pods = std::make_shared<std::vector<jetson::RunpodPod>>();
    RunAsync(
        [pods](jetson::RunpodClient &client) -> std::string {
            std::string err;
            return client.ListPods(*pods, err) ? "" : err;
        },
        [pods](PodsView &view, const std::string &err) {
            view.busy_ = false;
            if (!err.empty()) {
                view.SetStatusLine(err);
                return;
            }
            view.pods_ = std::move(*pods);
            view.RenderList();
        });
}

void PodsView::RenderList() {
    const auto &p = jetson::UiTheme::Instance().Palette();
    lv_obj_clean(list_);

    double total = 0;
    int running = 0;
    for (const auto &pod : pods_)
        if (pod.running()) { ++running; total += pod.cost_per_hr; }
    char line[96];
    if (pods_.empty()) {
        snprintf(line, sizeof(line), "Chưa có pod nào — nhấn \"Thuê GPU\"");
    } else {
        snprintf(line, sizeof(line), "%d pod, %d đang chạy • $%.2f/giờ",
                 (int)pods_.size(), running, total);
    }
    SetStatusLine(line);

    // Publish running pods to the Chromium kiosk's island launcher:
    // launch_chromium.sh appends this file ("Name|URL" per line) to the
    // static app list that jetson_kiosk_bar's long-press menu reads, so a
    // rented pod's web IDE is one hold-the-island away inside the browser
    // session. Rewritten on every refresh so stopped/deleted pods drop out.
    if (FILE *f = fopen("/tmp/jetson_kiosk_extra_apps", "w")) {
        for (const auto &pod : pods_) {
            const int port = pod.HttpPort();
            if (!pod.running() || port <= 0) continue;
            std::string name = pod.name.empty() ? pod.id : pod.name;
            for (auto &c : name)
                if (c == '|' || c == '\n') c = ' ';
            fprintf(f, "Pod %s|%s\n", name.c_str(),
                    jetson::RunpodClient::ProxyUrl(pod.id, port).c_str());
        }
        fclose(f);
    }

    for (const auto &pod : pods_) {
        auto *row = lv_obj_create(list_);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, 56);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_bg_color(row, Color(p.row), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(row, 10, 0);
        lv_obj_set_style_pad_column(row, 8, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        auto *dot = lv_obj_create(row);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 10, 10);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(
            dot, lv_color_hex(pod.running() ? kRunningGreen : kStoppedGray), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);

        auto *info = lv_obj_create(row);
        lv_obj_remove_style_all(info);
        lv_obj_set_flex_grow(info, 1);
        lv_obj_set_height(info, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
        lv_obj_clear_flag(info, LV_OBJ_FLAG_SCROLLABLE);

        auto *name = lv_label_create(info);
        lv_obj_set_width(name, lv_pct(100));
        lv_obj_set_style_text_font(name, &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(name, Color(p.text), 0);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_label_set_text(name, pod.name.empty() ? pod.id.c_str() : pod.name.c_str());

        char sub[128];
        snprintf(sub, sizeof(sub), "%s x%d • %s • $%.2f/giờ",
                 pod.gpu_name.empty() ? "GPU" : pod.gpu_name.c_str(),
                 pod.gpu_count > 0 ? pod.gpu_count : 1,
                 pod.running() ? "Đang chạy" : "Đã tắt", pod.cost_per_hr);
        auto *subl = lv_label_create(info);
        lv_obj_set_width(subl, lv_pct(100));
        lv_obj_set_style_text_font(subl, &BUILTIN_SMALL_TEXT_FONT, 0);
        lv_obj_set_style_text_color(subl, Color(p.sub_text), 0);
        lv_label_set_long_mode(subl, LV_LABEL_LONG_DOT);
        lv_label_set_text(subl, sub);

        auto add_ctx = [this, &pod](lv_obj_t *obj, lv_event_cb_t cb) {
            auto *ctx = new RowCtx{this, pod.id};
            lv_obj_add_event_cb(obj, cb, LV_EVENT_CLICKED, ctx);
            lv_obj_add_event_cb(obj, OnRowCtxDeleted, LV_EVENT_DELETE, ctx);
        };

        if (pod.running() && pod.HttpPort() > 0) {
            auto *studio = MakeSmallButton(row, "Studio", false);
            add_ctx(studio, OnRowStudio);
        }
        auto *power = MakeSmallButton(row, LV_SYMBOL_POWER, true);
        add_ctx(power, OnRowPower);
        auto *del = MakeSmallButton(row, LV_SYMBOL_TRASH, true);
        lv_obj_set_style_text_color(ButtonLabel(del), lv_color_hex(kDangerRed), 0);
        add_ctx(del, OnRowDelete);

        add_ctx(row, OnRowClicked);
    }
}

// ---- Row actions ------------------------------------------------------------

void PodsView::OpenStudioFor(const std::string &pod_id) {
    const auto *pod = FindPod(pod_id);
    if (!pod) return;
    if (!pod->running()) { Notify("Pod chưa chạy — bật nguồn trước"); return; }
    const int port = pod->HttpPort();
    if (port <= 0) { Notify("Pod không mở cổng web (http)"); return; }
    if (!open_url_cb_) return;
    Notify("Đang mở Studio trên Chromium...");
    open_url_cb_(jetson::RunpodClient::ProxyUrl(pod->id, port));
}

void PodsView::ToggleStartStop(const std::string &pod_id) {
    const auto *pod = FindPod(pod_id);
    if (!pod || busy_) return;
    CloseSheet(); // the detail sheet would show stale state after the toggle
    const bool start = !pod->running();
    busy_ = true;
    SetStatusLine(start ? "Đang bật pod..." : "Đang tắt pod...");
    RunAsync(
        [pod_id, start](jetson::RunpodClient &client) -> std::string {
            std::string err;
            bool ok = start ? client.StartPod(pod_id, err)
                            : client.StopPod(pod_id, err);
            return ok ? "" : err;
        },
        [start](PodsView &view, const std::string &err) {
            view.busy_ = false;
            if (!err.empty()) {
                view.SetStatusLine(err);
                view.Notify(err.c_str());
                return;
            }
            view.Notify(start ? "Đã bật pod" : "Đã tắt pod (chỉ còn phí ổ đĩa)");
            view.Refresh();
        });
}

void PodsView::RequestTerminate(const std::string &pod_id) {
    // Deleting destroys the pod's disk; require a second tap within 4 s.
    const uint32_t now = lv_tick_get();
    if (pending_delete_id_ != pod_id ||
        lv_tick_elaps(pending_delete_tick_) > 4000) {
        pending_delete_id_ = pod_id;
        pending_delete_tick_ = now;
        Notify("Nhấn thùng rác lần nữa để xoá pod (mất dữ liệu!)");
        return;
    }
    pending_delete_id_.clear();
    if (busy_) return;
    busy_ = true;
    SetStatusLine("Đang xoá pod...");
    RunAsync(
        [pod_id](jetson::RunpodClient &client) -> std::string {
            std::string err;
            return client.TerminatePod(pod_id, err) ? "" : err;
        },
        [](PodsView &view, const std::string &err) {
            view.busy_ = false;
            if (!err.empty()) {
                view.SetStatusLine(err);
                view.Notify(err.c_str());
                return;
            }
            view.Notify("Đã xoá pod");
            view.Refresh();
        });
}

// ---- Detail sheet -----------------------------------------------------------

void PodsView::CloseSheet() {
    // Async delete: CloseSheet is reached from event callbacks of the sheet's
    // own children (Đóng/Huỷ/Tắt buttons), where a synchronous delete would
    // free the object mid-event. Hide it immediately so it vanishes this frame.
    if (sheet_) {
        lv_obj_add_flag(sheet_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_delete_async(sheet_);
        sheet_ = nullptr;
    }
    gpu_list_ = nullptr;
    create_btn_label_ = nullptr;
    preset_btns_[0] = preset_btns_[1] = nullptr;
}

void PodsView::OpenDetailSheet(const std::string &pod_id) {
    const auto *pod = FindPod(pod_id);
    if (!pod) return;
    CloseSheet();
    const auto &p = jetson::UiTheme::Instance().Palette();

    sheet_ = lv_obj_create(overlay_);
    lv_obj_remove_style_all(sheet_);
    lv_obj_set_size(sheet_, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(sheet_, Color(p.scrim), 0);
    lv_obj_set_style_bg_opa(sheet_, LV_OPA_60, 0);
    lv_obj_add_flag(sheet_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sheet_, OnSheetDismiss, LV_EVENT_CLICKED, this);

    auto *card = lv_obj_create(sheet_);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, std::min(width_ - 80, 560), LV_SIZE_CONTENT);
    lv_obj_center(card);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_bg_color(card, Color(p.panel), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_set_style_pad_row(card, 6, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    // Absorb clicks so tapping the card doesn't dismiss the sheet.
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    auto add_line = [&](const char *text, bool title, bool dim) {
        auto *l = lv_label_create(card);
        lv_obj_set_width(l, lv_pct(100));
        lv_obj_set_style_text_font(
            l, title ? &BUILTIN_TEXT_FONT : &BUILTIN_SMALL_TEXT_FONT, 0);
        lv_obj_set_style_text_color(l, Color(dim ? p.sub_text : p.text), 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_label_set_text(l, text);
        return l;
    };

    char buf[256];
    snprintf(buf, sizeof(buf), "%s", pod->name.empty() ? pod->id.c_str()
                                                       : pod->name.c_str());
    add_line(buf, true, false);
    snprintf(buf, sizeof(buf), "%s x%d • %s • $%.2f/giờ",
             pod->gpu_name.c_str(), pod->gpu_count > 0 ? pod->gpu_count : 1,
             pod->running() ? "Đang chạy" : "Đã tắt", pod->cost_per_hr);
    add_line(buf, false, true);
    snprintf(buf, sizeof(buf), "Image: %s", pod->image.c_str());
    add_line(buf, false, true);

    const std::string ssh = pod->SshCommand();
    if (!ssh.empty()) {
        snprintf(buf, sizeof(buf), "SSH: %s", ssh.c_str());
        add_line(buf, false, false);
    }
    const int port = pod->HttpPort();
    if (port > 0) {
        snprintf(buf, sizeof(buf), "Web IDE: %s",
                 jetson::RunpodClient::ProxyUrl(pod->id, port).c_str());
        add_line(buf, false, false);
        snprintf(buf, sizeof(buf), "Mật khẩu IDE (pod thuê từ app): %s",
                 kIdePassword);
        add_line(buf, false, true);
    }

    auto *btn_row = lv_obj_create(card);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_width(btn_row, lv_pct(100));
    lv_obj_set_height(btn_row, 44);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn_row, 8, 0);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    auto add_ctx = [this, pod](lv_obj_t *obj, lv_event_cb_t cb) {
        auto *ctx = new RowCtx{this, pod->id};
        lv_obj_add_event_cb(obj, cb, LV_EVENT_CLICKED, ctx);
        lv_obj_add_event_cb(obj, OnRowCtxDeleted, LV_EVENT_DELETE, ctx);
    };

    if (pod->running() && port > 0) {
        auto *studio = MakeSmallButton(btn_row, "Mở Studio", false);
        lv_obj_set_style_bg_color(studio, Color(p.accent), 0);
        add_ctx(studio, OnRowStudio);
    }
    auto *power = MakeSmallButton(
        btn_row, pod->running() ? "Tắt" : "Bật", false);
    add_ctx(power, OnRowPower);
    auto *close = MakeSmallButton(btn_row, "Đóng", false);
    lv_obj_add_event_cb(close, OnSheetDismiss, LV_EVENT_CLICKED, this);
}

// ---- Rent sheet ---------------------------------------------------------

void PodsView::OpenRentSheet() {
    CloseSheet();
    const auto &p = jetson::UiTheme::Instance().Palette();

    sheet_ = lv_obj_create(overlay_);
    lv_obj_remove_style_all(sheet_);
    lv_obj_set_size(sheet_, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(sheet_, Color(p.scrim), 0);
    lv_obj_set_style_bg_opa(sheet_, LV_OPA_60, 0);
    lv_obj_add_flag(sheet_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sheet_, OnSheetDismiss, LV_EVENT_CLICKED, this);

    auto *card = lv_obj_create(sheet_);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, std::min(width_ - 60, 620), height_ - 90);
    lv_obj_center(card);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_bg_color(card, Color(p.panel), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_set_style_pad_row(card, 8, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    auto *title = lv_label_create(card);
    lv_obj_set_style_text_font(title, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(title, Color(p.text), 0);
    lv_label_set_text(title, "Thuê GPU (Secure Cloud)");

    // Workspace preset toggle.
    auto *presets = lv_obj_create(card);
    lv_obj_remove_style_all(presets);
    lv_obj_set_width(presets, lv_pct(100));
    lv_obj_set_height(presets, 40);
    lv_obj_set_flex_flow(presets, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(presets, 8, 0);
    lv_obj_clear_flag(presets, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 2; ++i) {
        auto *btn = MakeSmallButton(presets, kPresets[i].label, false);
        preset_btns_[i] = btn;
        lv_obj_add_event_cb(btn, OnPresetClicked, LV_EVENT_CLICKED, this);
    }

    gpu_list_ = lv_obj_create(card);
    lv_obj_remove_style_all(gpu_list_);
    lv_obj_set_width(gpu_list_, lv_pct(100));
    lv_obj_set_flex_grow(gpu_list_, 1);
    lv_obj_set_flex_flow(gpu_list_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(gpu_list_, 4, 0);
    lv_obj_add_flag(gpu_list_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(gpu_list_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(gpu_list_, LV_SCROLLBAR_MODE_AUTO);

    auto *btn_row = lv_obj_create(card);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_width(btn_row, lv_pct(100));
    lv_obj_set_height(btn_row, 44);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn_row, 8, 0);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    auto *cancel = MakeSmallButton(btn_row, "Huỷ", false);
    lv_obj_add_event_cb(cancel, OnSheetDismiss, LV_EVENT_CLICKED, this);
    auto *create = MakeSmallButton(btn_row, "Thuê ngay", false);
    lv_obj_set_style_bg_color(create, Color(p.accent), 0);
    create_btn_label_ = ButtonLabel(create);
    lv_obj_add_event_cb(create, OnCreateClicked, LV_EVENT_CLICKED, this);

    // Highlight the current preset + populate the GPU catalog.
    UpdatePresetHighlight();
    if (gpu_types_.empty()) {
        auto *loading = lv_label_create(gpu_list_);
        lv_obj_set_style_text_font(loading, &BUILTIN_SMALL_TEXT_FONT, 0);
        lv_obj_set_style_text_color(loading, Color(p.sub_text), 0);
        lv_label_set_text(loading, "Đang tải bảng giá GPU...");
        auto types = std::make_shared<std::vector<jetson::RunpodGpuType>>();
        RunAsync(
            [types](jetson::RunpodClient &client) -> std::string {
                std::string err;
                client.ListGpuTypes(*types, err);
                return err; // non-fatal: fallback list already substituted
            },
            [types](PodsView &view, const std::string &err) {
                view.gpu_types_ = std::move(*types);
                if (!err.empty())
                    view.SetStatusLine("Giá GPU ngoại tuyến: " + err);
                view.RenderGpuList();
            });
    } else {
        RenderGpuList();
    }
}

void PodsView::UpdatePresetHighlight() {
    const auto &p = jetson::UiTheme::Instance().Palette();
    for (int i = 0; i < 2; ++i) {
        if (!preset_btns_[i]) continue;
        lv_obj_set_style_bg_color(
            preset_btns_[i],
            Color(i == preset_index_ ? p.row_active : p.button), 0);
    }
}

void PodsView::RenderGpuList() {
    if (!gpu_list_) return;
    const auto &p = jetson::UiTheme::Instance().Palette();
    lv_obj_clean(gpu_list_);
    if (selected_gpu_id_.empty() && !gpu_types_.empty())
        selected_gpu_id_ = gpu_types_.front().id;

    for (const auto &gpu : gpu_types_) {
        auto *row = lv_obj_create(gpu_list_);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, 40);
        lv_obj_set_style_radius(row, 8, 0);
        const bool selected = gpu.id == selected_gpu_id_;
        lv_obj_set_style_bg_color(row, Color(selected ? p.row_active : p.row), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(row, 10, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        char text[96];
        snprintf(text, sizeof(text), "%s • %dGB", gpu.display_name.c_str(),
                 gpu.memory_gb);
        auto *name = lv_label_create(row);
        lv_obj_set_style_text_font(name, &BUILTIN_SMALL_TEXT_FONT, 0);
        lv_obj_set_style_text_color(name, Color(p.text), 0);
        lv_label_set_text(name, text);

        if (gpu.secure_price > 0)
            snprintf(text, sizeof(text), "$%.2f/giờ", gpu.secure_price);
        else
            snprintf(text, sizeof(text), "—");
        auto *price = lv_label_create(row);
        lv_obj_set_style_text_font(price, &BUILTIN_SMALL_TEXT_FONT, 0);
        lv_obj_set_style_text_color(price, Color(p.sub_text), 0);
        lv_label_set_text(price, text);

        auto *ctx = new RowCtx{this, gpu.id};
        // user_data lets UpdateGpuSelectionHighlight recover each row's GPU id
        // without rebuilding the list (rebuilding would delete the clicked row
        // inside its own event callback).
        lv_obj_set_user_data(row, ctx);
        lv_obj_add_event_cb(row, OnGpuRowClicked, LV_EVENT_CLICKED, ctx);
        lv_obj_add_event_cb(row, OnRowCtxDeleted, LV_EVENT_DELETE, ctx);
    }
}

void PodsView::UpdateGpuSelectionHighlight() {
    if (!gpu_list_) return;
    const auto &p = jetson::UiTheme::Instance().Palette();
    const uint32_t count = lv_obj_get_child_count(gpu_list_);
    for (uint32_t i = 0; i < count; ++i) {
        auto *row = lv_obj_get_child(gpu_list_, (int32_t)i);
        auto *ctx = static_cast<RowCtx *>(lv_obj_get_user_data(row));
        if (!ctx) continue; // the transient "loading" label
        lv_obj_set_style_bg_color(
            row, Color(ctx->pod_id == selected_gpu_id_ ? p.row_active : p.row), 0);
    }
}

void PodsView::DoCreatePod() {
    if (creating_ || selected_gpu_id_.empty()) return;
    const Preset &preset = kPresets[preset_index_ == 1 ? 1 : 0];
    creating_ = true;
    if (create_btn_label_) lv_label_set_text(create_btn_label_, "Đang tạo...");

    jetson::RunpodCreateOptions opt;
    opt.gpu_type_id = selected_gpu_id_;
    opt.image = preset.image;
    opt.name = preset_index_ == 1 ? "jetsona-jupyter" : "jetsona-studio";
    opt.ports = {preset.http_port, "22/tcp"};
    opt.env[preset.env_key] = kIdePassword;

    RunAsync(
        [opt](jetson::RunpodClient &client) -> std::string {
            std::string err;
            jetson::RunpodPod pod;
            return client.CreatePod(opt, pod, err) ? "" : err;
        },
        [](PodsView &view, const std::string &err) {
            view.creating_ = false;
            if (!err.empty()) {
                if (view.create_btn_label_)
                    lv_label_set_text(view.create_btn_label_, "Thuê ngay");
                view.SetStatusLine(err);
                view.Notify(err.c_str());
                return;
            }
            view.CloseSheet();
            view.Notify("Đã tạo pod — chờ khởi động rồi mở Studio");
            view.Refresh();
        });
}

// ---- Event trampolines ---------------------------------------------------

void PodsView::OnRefresh(lv_event_t *e) {
    static_cast<PodsView *>(lv_event_get_user_data(e))->Refresh();
}

void PodsView::OnRent(lv_event_t *e) {
    static_cast<PodsView *>(lv_event_get_user_data(e))->OpenRentSheet();
}

void PodsView::OnRowClicked(lv_event_t *e) {
    auto *ctx = static_cast<RowCtx *>(lv_event_get_user_data(e));
    ctx->self->OpenDetailSheet(ctx->pod_id);
}

void PodsView::OnRowStudio(lv_event_t *e) {
    auto *ctx = static_cast<RowCtx *>(lv_event_get_user_data(e));
    ctx->self->OpenStudioFor(ctx->pod_id);
}

void PodsView::OnRowPower(lv_event_t *e) {
    auto *ctx = static_cast<RowCtx *>(lv_event_get_user_data(e));
    ctx->self->ToggleStartStop(ctx->pod_id);
}

void PodsView::OnRowDelete(lv_event_t *e) {
    auto *ctx = static_cast<RowCtx *>(lv_event_get_user_data(e));
    ctx->self->RequestTerminate(ctx->pod_id);
}

void PodsView::OnRowCtxDeleted(lv_event_t *e) {
    delete static_cast<RowCtx *>(lv_event_get_user_data(e));
}

void PodsView::OnSheetDismiss(lv_event_t *e) {
    auto *self = static_cast<PodsView *>(lv_event_get_user_data(e));
    // Only dismiss on direct clicks (the scrim or an explicit close button);
    // clicks inside the card are absorbed by the card itself.
    self->CloseSheet();
}

void PodsView::OnGpuRowClicked(lv_event_t *e) {
    auto *ctx = static_cast<RowCtx *>(lv_event_get_user_data(e));
    ctx->self->selected_gpu_id_ = ctx->pod_id; // reused field: gpu id
    ctx->self->UpdateGpuSelectionHighlight();
}

void PodsView::OnPresetClicked(lv_event_t *e) {
    auto *self = static_cast<PodsView *>(lv_event_get_user_data(e));
    lv_obj_t *target = static_cast<lv_obj_t *>(lv_event_get_target(e));
    for (int i = 0; i < 2; ++i)
        if (self->preset_btns_[i] == target) self->preset_index_ = i;
    self->UpdatePresetHighlight();
}

void PodsView::OnCreateClicked(lv_event_t *e) {
    static_cast<PodsView *>(lv_event_get_user_data(e))->DoCreatePod();
}

} // namespace home
