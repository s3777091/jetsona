#ifndef DISPLAY_H
#define DISPLAY_H

#include <lvgl.h>

#include <string>
#include <chrono>

class Theme {
public:
    Theme(const std::string &name) : name_(name) {}
    virtual ~Theme() = default;
    inline std::string name() const { return name_; }
private:
    std::string name_;
};

class Display {
public:
    Display();
    virtual ~Display();

    virtual void SetStatus(const char *status);
    virtual void ShowNotification(const char *notification, int duration_ms = 3000);
    virtual void ShowNotification(const std::string &notification, int duration_ms = 3000);
    virtual void ShowWifiConfigPrompt(const char *ssid, const char *password, const char *url);
    virtual void HideWifiConfigPrompt();
    virtual void SetEmotion(const char *emotion);
    virtual void SetChatMessage(const char *role, const char *content);
    virtual void ClearChatMessages();
    virtual void SetTheme(Theme *theme);
    virtual Theme *GetTheme() { return current_theme_; }
    virtual void UpdateStatusBar(bool update_all = false);
    virtual void SetPowerSaveMode(bool on);
    virtual void SetupUI() { setup_ui_called_ = true; }

    inline int width() const { return width_; }
    inline int height() const { return height_; }
    inline bool IsSetupUICalled() const { return setup_ui_called_; }

protected:
    int width_ = 0;
    int height_ = 0;
    bool setup_ui_called_ = false;
    Theme *current_theme_ = nullptr;

    friend class DisplayLockGuard;
    virtual bool Lock(int timeout_ms = 0) = 0;
    virtual void Unlock() = 0;
};

class DisplayLockGuard {
public:
    DisplayLockGuard(Display *display) : display_(display) {
        if (!display_->Lock(30000)) {
            fprintf(stderr, "Display: failed to lock display\n");
        }
    }
    ~DisplayLockGuard() { display_->Unlock(); }
private:
    Display *display_;
};

class NoDisplay : public Display {
private:
    virtual bool Lock(int /*timeout_ms*/ = 0) override { return true; }
    virtual void Unlock() override {}
};

#endif