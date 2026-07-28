#pragma once

/* The tools that let Ekko operate the device rather than just talk about it.
 *
 * Everything here runs on Conversation's worker thread. The read-only sources
 * (Settings, PlayerController, Board battery) are already thread-safe and are
 * read inline; anything that changes device state goes through DeviceBridge,
 * which marshals onto the main loop.
 *
 * Calendar and Reminder tools write Settings-backed stores; with no UI the
 * "reload" nudge is a no-op, but the record formats are kept identical so a
 * future view or export reads them back unchanged. */

#include "tools.h"

#include <string>

namespace jetson {

/* Pidfile the alarm scheduler writes when it launches the ringtone mpv, so
 * AlarmTool 'stop' (running on the agent worker thread) can kill the ringing
 * alarm. Fixed path on a single-user device. */
inline std::string AlarmPidFile() { return "/tmp/ekko-alarm.pid"; }

/* Read-only snapshot the model needs constantly: date/time, battery,
 * volume, current track. Cheap enough to be the model's first move. */
class DeviceStatusTool : public Tool {
public:
    DeviceStatusTool();
    std::string Execute(const std::string &arguments_json) override;
};

/* Output volume 0..100 and mute, applied to the media player and the
 * ALSA/PipeWire mixer through the codec. */
class VolumeTool : public Tool {
public:
    VolumeTool();
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

/* Wake-on-LAN: turn the home PC on. POSTs to JETSON_WOL_URL with an
 * X-WoL-Token header. Ekko runs on the Jetson, which is the box that actually
 * fires the magic packet, so the default URL is the local /wake endpoint. */
class PcPowerTool : public Tool {
public:
    PcPowerTool();
    std::string Execute(const std::string &arguments_json) override;
};

/* The room air conditioner (LG ThinQ), reached through the local bridge
 * service scripts/jetsona_ac.py at JETSON_AC_URL with an X-AC-Token header.
 *
 * Besides the obvious on/off/setpoint actions it exposes 'comfort', which
 * takes how the user *feels* ("lạnh quá", "nóng quá") rather than a number.
 * That path deliberately does its own read-decide-write inside the bridge:
 * the right answer to "I'm cold" depends on the current mode, setpoint and
 * fan, and making the model fetch status and reason about it first costs a
 * round trip and gets it wrong more often than a fixed ladder does. */
class AirConditionerTool : public Tool {
public:
    AirConditionerTool();
    std::string Execute(const std::string &arguments_json) override;
};

/* Current weather at the device location (pinned to Đà Nẵng in config.yaml).
 * Used by the morning alarm briefing and any "thời tiết hôm nay" question. */
class WeatherTool : public Tool {
public:
    WeatherTool();
    std::string Execute(const std::string &arguments_json) override;
};

/* Alarm ringtone picker: list the ringtones synced from S3, set the active one
 * (persisted to Settings("alarm","ringtone")), and play a short preview.
 * The alarm scheduler plays the active ringtone when it fires. */
class RingtoneTool : public Tool {
public:
    RingtoneTool();
    std::string Execute(const std::string &arguments_json) override;
};

/* The user's own album ("Album của tôi"): save the track now playing, list the
 * saved albums, and play one back. Backed by UserLibrary (~/.jetson-fw/
 * music_albums.json). This is the back half of the music flow --
 * music_play finds a song, music_album keeps the ones the user says
 * "lưu bài này đi" and replays them when they say "chơi nhạc trong album". */
class MusicAlbumTool : public Tool {
public:
    MusicAlbumTool();
    std::string Execute(const std::string &arguments_json) override;
};

/* Morning alarm. 'set' arms it for a wall-clock time (HH:MM) the scheduler in
 * Application watches; 'cancel' disarms it; 'status' reads it back; 'stop'
 * silences a currently ringing alarm (kills the ringtone mpv). The fire
 * itself -- ringtone + Da Nang weather briefing + pending morning notes --
 * happens in Application::TickClock, not here. */
class AlarmTool : public Tool {
public:
    AlarmTool();
    std::string Execute(const std::string &arguments_json) override;
};

} // namespace jetson
