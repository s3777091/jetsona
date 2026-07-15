#include "display/core/lvgl_display.h"
#include "esp_log.h"
#include "esp_err.h"

#include <ctime>
#include <cstring>

#define TAG "Display"

LvglDisplay::LvglDisplay() {
    esp_timer_create_args_t args = {
        .callback = [](void *arg) {
            LvglDisplay *d = static_cast<LvglDisplay *>(arg);
            DisplayLockGuard lock(d);
            if (d->notification_label_) lv_obj_add_flag(d->notification_label_, LV_OBJ_FLAG_HIDDEN);
            if (d->status_label_) lv_obj_remove_flag(d->status_label_, LV_OBJ_FLAG_HIDDEN);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "notification_timer",
        .skip_unhandled_events = false,
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &notification_timer_));
    esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "display_update", &pm_lock_);
}

LvglDisplay::~LvglDisplay() {
    if (notification_timer_) {
        esp_timer_stop(notification_timer_);
        esp_timer_delete(notification_timer_);
    }
    if (pm_lock_) esp_pm_lock_delete(pm_lock_);
}

void LvglDisplay::SetStatus(const char *status) {
    if (!setup_ui_called_) { ESP_LOGW(TAG, "SetStatus('%s') before SetupUI", status); }
    DisplayLockGuard lock(this);
    if (!status_label_) return;
    lv_label_set_text(status_label_, status);
    lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
    if (notification_label_) lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
    last_status_update_time_ = std::chrono::system_clock::now();
}

void LvglDisplay::ShowNotification(const std::string &n, int duration_ms) {
    ShowNotification(n.c_str(), duration_ms);
}

void LvglDisplay::ShowNotification(const char *notification, int duration_ms) {
    if (!setup_ui_called_) { ESP_LOGW(TAG, "ShowNotification('%s') before SetupUI", notification); }
    DisplayLockGuard lock(this);
    if (!notification_label_) return;
    lv_label_set_text(notification_label_, notification);
    lv_obj_remove_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
    if (status_label_) lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
    esp_timer_stop(notification_timer_);
    esp_timer_start_once(notification_timer_, (uint64_t)duration_ms * 1000);
}

void LvglDisplay::UpdateStatusBar(bool /*update_all*/) {
    /* Base status-bar update. The DS-02 home display overrides this with its
     * own clock / battery / network refresh, so this only updates the clock
     * label when idle. */
    time_t now = time(nullptr);
    struct tm *tm = localtime(&now);
    if (tm && tm->tm_year >= (2025 - 1900)) {
        char buf[16];
        strftime(buf, sizeof(buf), "%H:%M", tm);
        if (status_label_) {
            DisplayLockGuard lock(this);
            lv_label_set_text(status_label_, buf);
        }
    }
    esp_pm_lock_acquire(pm_lock_);
    esp_pm_lock_release(pm_lock_);
}

void LvglDisplay::SetPreviewImage(std::unique_ptr<LvglImage> /*image*/) {}

void LvglDisplay::SetPowerSaveMode(bool on) {
    SetChatMessage("system", "");
    SetEmotion(on ? "sleepy" : "neutral");
}

bool LvglDisplay::SnapshotToJpeg(std::string & /*jpeg_data*/, int /*quality*/) {
    ESP_LOGW(TAG, "SnapshotToJpeg not implemented on Linux yet");
    return false;
}
