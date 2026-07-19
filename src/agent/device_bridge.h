#pragma once

/* The agent's only door into the running UI.
 *
 * Agent tools execute on Conversation's worker thread, where touching LVGL is
 * forbidden (see tools.h). Anything that opens an app, moves a slider or shows
 * a notification therefore cannot be done inline. DeviceBridge is the seam: the
 * home display registers handlers once at startup, tools call the request
 * methods from the worker thread, and each request is marshalled onto the main
 * loop with Application::Schedule before the handler runs.
 *
 * Requests are deliberately fire-and-forget. Waiting on a promise from the
 * worker thread would deadlock the agent whenever the main loop is busy (an app
 * animating, a wallpaper decoding), and the actions here cannot meaningfully
 * fail once their arguments validate — so validation happens synchronously on
 * the worker thread and the UI work is queued. Read-only state (volume, battery,
 * Wi-Fi) does not come through here at all: those sources are already
 * thread-safe and the tools read them directly.
 *
 * Unregistered handlers make the matching request return false rather than
 * crash, which keeps the agent usable in builds that never construct the home
 * display (tests, headless runs). */

#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace jetson {

/* One launchable surface, as the model sees it. `aliases` exists because the
 * model paraphrases: "mo trinh duyet", "open browser", "web" all mean chromium. */
struct AgentApp {
    const char *id;       // canonical id the tool schema advertises
    const char *label;    // Vietnamese display name, used in tool replies
    const char *aliases;  // space-separated extra names matched case-insensitively
};

class DeviceBridge {
public:
    static DeviceBridge &Instance();

    // ---- Registration (main thread, once, at home-display startup) --------
    using AppOpener = std::function<void(const std::string &app_id)>;
    using Notifier = std::function<void(const std::string &text)>;
    using VolumeSetter = std::function<void(int volume, bool muted)>;
    using BrightnessSetter = std::function<void(int percent)>;

    /* Reminders and Calendar keep their whole list in memory and rewrite the
     * store wholesale on every save. If one of those apps is open when the
     * agent writes a row, its next save would drop it — so the agent asks the
     * live view to re-read the store instead. No-op when the app is closed. */
    using StoreReloader = std::function<void()>;

    void SetAppOpener(AppOpener fn);
    void SetNotifier(Notifier fn);
    void SetVolumeSetter(VolumeSetter fn);
    void SetBrightnessSetter(BrightnessSetter fn);
    void SetReminderReloader(StoreReloader fn);
    void SetCalendarReloader(StoreReloader fn);

    // ---- Requests (worker thread; queued onto the main loop) --------------
    /* Resolves `name` through the app table (id, label or alias). Returns false
     * with `out_err` set when nothing matches or no opener is registered;
     * otherwise queues the open and reports the canonical label in `out_label`. */
    bool OpenApp(const std::string &name, std::string &out_label, std::string &out_err);
    bool Notify(const std::string &text);
    bool SetVolume(int volume, bool muted);
    bool SetBrightness(int percent);
    void ReloadReminders();
    void ReloadCalendar();

    // The catalogue the tool schema enumerates. Stable order.
    static const std::vector<AgentApp> &Apps();
    // Canonical id for a user/model-supplied name, or "" when unmatched.
    static std::string ResolveAppId(const std::string &name);

private:
    DeviceBridge() = default;

    /* Handlers are written on the main thread (registered at startup, cleared
     * when the display is torn down) and read on the agent's worker thread, so
     * every access is guarded. Held only long enough to copy the std::function
     * out -- the handler itself runs later, on the main loop. */
    mutable std::mutex mtx_;
    AppOpener app_opener_;
    Notifier notifier_;
    VolumeSetter volume_setter_;
    BrightnessSetter brightness_setter_;
    StoreReloader reminder_reloader_;
    StoreReloader calendar_reloader_;
};

} // namespace jetson
