#include "display/core/display.h"
#include "esp_log.h"

#include <cstring>

#define TAG "Display"

Display::Display() {}
Display::~Display() {}

void Display::SetStatus(const char *status) { ESP_LOGW(TAG, "SetStatus: %s", status); }

void Display::ShowNotification(const std::string &n, int duration_ms) {
    ShowNotification(n.c_str(), duration_ms);
}
void Display::ShowNotification(const char *notification, int /*duration_ms*/) {
    ESP_LOGW(TAG, "ShowNotification: %s", notification);
}

void Display::ShowWifiConfigPrompt(const char *ssid, const char *password, const char *url) {
    ESP_LOGW(TAG, "ShowWifiConfigPrompt: ssid=%s pw_set=%d url=%s",
             ssid ? ssid : "", password && password[0] != '\0', url ? url : "");
}
void Display::HideWifiConfigPrompt() {}
void Display::UpdateStatusBar(bool /*update_all*/) {}
void Display::SetEmotion(const char *emotion) { ESP_LOGW(TAG, "SetEmotion: %s", emotion); }
void Display::SetChatMessage(const char *role, const char *content) {
    ESP_LOGW(TAG, "Role:%s", role);
    ESP_LOGW(TAG, "     %s", content);
}
void Display::ClearChatMessages() {}
void Display::SetTheme(Theme *theme) { current_theme_ = theme; }
void Display::SetPowerSaveMode(bool on) { ESP_LOGW(TAG, "SetPowerSaveMode: %d", on); }
