#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace jetson::audio {

class AudioOutput;

/* Firmware end of the Gemini Live session (see scripts/gemini_live_runtime.py).
 *
 * The old path was four serial network stages -- transcribe, think, think
 * again, synthesise -- with the microphone switched off for the last of them.
 * That is why the device could not be interrupted and why anything said while
 * it worked was lost. Here the microphone keeps running the whole time, audio
 * goes up as it is captured, and the reply plays as it arrives.
 *
 * A session lasts exactly as long as the socket connection: Start() when the
 * wake word fires, Stop() when the conversation goes quiet. Nothing is billed
 * in between, which is the whole reason the wake word stays local.
 *
 * Threads: SendAudio runs on the ALSA capture thread and must never block on
 * the network, so it only writes to the socket. A reader thread owns
 * everything coming back, and dispatches tool calls to a short-lived worker so
 * a slow tool cannot stall playback. */
class RealtimeClient {
public:
    // Runs a tool by name with raw JSON arguments; returns the result string.
    using ToolExecutor =
        std::function<std::string(const std::string &name,
                                  const std::string &arguments_json)>;
    // Fires when the model finishes a turn, so the caller can start the idle
    // timer that eventually closes the session.
    using TurnDoneCb = std::function<void()>;

    RealtimeClient();
    ~RealtimeClient();

    RealtimeClient(const RealtimeClient &) = delete;
    RealtimeClient &operator=(const RealtimeClient &) = delete;

    /* Connects, declares the prompt and tool registry, and starts streaming.
     * Returns false (and logs) if the sidecar is not reachable, leaving the
     * caller free to fall back to the older path. */
    bool Start(const std::string &system_prompt, const std::string &tools_json,
               AudioOutput *out, ToolExecutor executor, TurnDoneCb on_turn_done);

    /* Ends the session and the billing with it. Safe to call when not running. */
    void Stop();

    bool running() const { return running_.load(); }

    /* Microphone audio, 16 kHz mono S16LE. Called from the capture thread. */
    void SendAudio(const int16_t *samples, size_t n);

    /* Seconds since the last audio arrived from the model or the user was
     * heard speaking -- what the idle timeout is measured against. */
    double IdleSeconds() const;

private:
    void ReaderThread();
    bool SendFrame(uint8_t type, const void *payload, size_t size);
    void HandleToolCall(const std::string &json_payload);
    void MarkActivity();

    int fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread reader_;
    std::mutex send_mtx_;

    AudioOutput *out_ = nullptr;
    ToolExecutor executor_;
    TurnDoneCb on_turn_done_;

    // Playback is opened lazily on the first audio frame so an empty turn does
    // not leave the speaker device claimed.
    bool playing_ = false;

    /* Echo control. This board has no working canceller: measured on the
     * device, the microphone hears the speaker at 1.2x the level it was played
     * at, because the two sit centimetres apart. Sending that back up made the
     * model transcribe Nova's own words as the user's, answer itself, and
     * interrupt itself. So playback is attenuated, and while it is audible the
     * microphone is only forwarded when it clearly beats the echo -- which is
     * what still lets a real interruption through.
     *
     * echo_rms_ decays rather than dropping to zero, because audio handed to
     * ALSA is still on its way out of the speaker for another buffer's worth
     * of time. */
    double out_gain_ = 0.35;
    double echo_coupling_ = 1.2;
    double echo_margin_ = 1.6;
    std::atomic<double> echo_rms_{0.0};
    std::vector<int16_t> gain_buffer_;

    mutable std::mutex activity_mtx_;
    double last_activity_ = 0.0;
};

} // namespace jetson::audio
