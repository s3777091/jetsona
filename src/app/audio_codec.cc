#include "audio_codec.h"

#include "platform/shell_command.h"

#include <algorithm>
#include <string>
#include <thread>
#include <chrono>

namespace {

void ApplyLinuxVolume(int volume, bool muted) {
    std::string out;
    const std::string pct = std::to_string(volume) + "%";
    const std::string mute = muted ? "mute" : "unmute";
    // Master is present on most USB/HDMI ALSA cards; PCM covers JetPack
    // images whose HDMI codec exposes no Master control. Desktop-oriented
    // images fall through to PipeWire or PulseAudio.
    const std::string command =
        "amixer -q sset Master " + pct + " " + mute + " 2>/dev/null || "
        "amixer -q sset PCM " + pct + " " + mute + " 2>/dev/null || "
        "(wpctl set-volume @DEFAULT_AUDIO_SINK@ " + std::to_string(volume / 100.0) +
            " 2>/dev/null && wpctl set-mute @DEFAULT_AUDIO_SINK@ " +
            (muted ? "1" : "0") + " 2>/dev/null) || "
        "(pactl set-sink-volume @DEFAULT_SINK@ " + pct +
            " 2>/dev/null && pactl set-sink-mute @DEFAULT_SINK@ " +
            (muted ? "1" : "0") + " 2>/dev/null)";
    (void)jetson::platform::RunShellCommand(command, out);
}

} // namespace

void LinuxAudioCodec::SetOutputVolume(int volume) {
    output_volume_ = std::max(0, std::min(volume, 100));
    QueueOutputState();
}

void LinuxAudioCodec::SetOutputMuted(bool muted) {
    output_muted_ = muted;
    QueueOutputState();
}

void LinuxAudioCodec::SetOutputState(int volume, bool muted) {
    output_volume_ = std::max(0, std::min(volume, 100));
    output_muted_ = muted;
    QueueOutputState();
}

void LinuxAudioCodec::QueueOutputState() {
    const int volume = output_volume_;
    const bool muted = output_muted_;
    const unsigned revision = output_revision_.fetch_add(1) + 1;
    std::thread([this, revision, volume, muted]() {
        // Sliders can emit many changes per drag. Apply only the newest state,
        // avoiding stale mixer commands finishing after the final value.
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        if (output_revision_.load() == revision)
            ApplyLinuxVolume(volume, muted);
    }).detach();
}
