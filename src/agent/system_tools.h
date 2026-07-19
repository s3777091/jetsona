#pragma once

/* The tools that let Ekko operate the device rather than just talk about it.
 *
 * Everything here runs on Conversation's worker thread. The read-only sources
 * (Settings, WifiManager, PlayerController, Board battery) are already
 * thread-safe and are read inline; anything that changes what is on screen goes
 * through DeviceBridge, which marshals onto the main loop.
 *
 * Calendar and Reminder tools write the same Settings-backed stores that
 * CalendarView and RemindersView read, so an event the agent creates shows up
 * in the app — that is the whole point of duplicating their record formats
 * here. If either format changes, these two must be updated with it. */

#include "tools.h"

namespace jetson {

/* Read-only snapshot the model needs constantly: date/time, battery, Wi-Fi,
 * volume, current track. Cheap enough to be the model's first move. */
class DeviceStatusTool : public Tool {
public:
    DeviceStatusTool();
    std::string Execute(const std::string &arguments_json) override;
};

/* Launch any of DeviceBridge::Apps() by id, label or alias. */
class OpenAppTool : public Tool {
public:
    OpenAppTool();
    std::string Execute(const std::string &arguments_json) override;
};

/* Output volume 0..100 and mute, applied to the media player, the ALSA/Pulse
 * mixer and the status-bar slider at once. */
class VolumeTool : public Tool {
public:
    VolumeTool();
    std::string Execute(const std::string &arguments_json) override;
};

/* Panel brightness 10..100 (software scrim — the HDMI panel has no backlight
 * control, see Ds02HomeDisplay::SetBrightness). */
class BrightnessTool : public Tool {
public:
    BrightnessTool();
    std::string Execute(const std::string &arguments_json) override;
};

/* Wi-Fi: status, radio on/off, scan, connect to an already-saved network,
 * disconnect. Connecting to a NEW network needs a password and stays a
 * deliberate manual step in the Wi-Fi settings app. */
class WifiTool : public Tool {
public:
    WifiTool();
    std::string Execute(const std::string &arguments_json) override;
};

/* Transport control over the current queue: play/pause/next/previous/stop. */
class MusicTool : public Tool {
public:
    MusicTool();
    std::string Execute(const std::string &arguments_json) override;
};

/* Search Zing by name and start playing the best match, with the rest of the
 * results left in the queue behind it so "next" walks the alternatives.
 * Blocking: a search plus one artwork download, on the agent's worker thread. */
class MusicPlayTool : public Tool {
public:
    MusicPlayTool();
    std::string Execute(const std::string &arguments_json) override;
};

/* Calendar events, stored per-day exactly as CalendarView writes them:
 * Settings("calendar")["d_YYYY-MM-DD"] = "HH:MM|done|title" lines, plus the
 * "task_dates" index that draws the dots on the month grid. */
class CalendarTool : public Tool {
public:
    enum Op { Add, List };
    explicit CalendarTool(Op op);
    std::string Execute(const std::string &arguments_json) override;
private:
    Op op_;
};

/* Reminders, stored in RemindersView's record format:
 * Settings("reminders")["items_v1"] = '~'-joined "id|pinned|done|RRGGBB|hex(title)|hex(info)". */
class ReminderTool : public Tool {
public:
    enum Op { Add, List, Complete };
    explicit ReminderTool(Op op);
    std::string Execute(const std::string &arguments_json) override;
private:
    Op op_;
};

} // namespace jetson
