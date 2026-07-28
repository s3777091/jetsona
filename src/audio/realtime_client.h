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
 * again, synthesise. Here audio goes up as it is captured and the reply plays
 * as it arrives. The capture device stays open throughout the session, though
 * microphone frames are withheld while reply audio is physically audible
 * because this enclosure has no usable acoustic echo canceller.
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

    /* True while reply audio is physically coming out of the speaker. Local
     * TTS (alarms, timers, battery warnings) checks this so it waits for a gap
     * instead of landing on top of the session in a different voice. */
    bool assistant_speaking() const { return assistant_audio_active_.load(); }

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
     * device, the microphone hears the speaker louder than it was played,
     * because the two sit centimetres apart. An amplitude gate proved unsafe:
     * room coupling varies enough that Nova's own voice regularly crossed the
     * "user is louder" threshold and interrupted her.
     *
     * Use strict half duplex instead. The microphone remains captured locally,
     * but no samples leave the device while assistant audio is playing or
     * during the short acoustic tail after ALSA has drained. This deliberately
     * trades barge-in for the invariant that Nova can never hear herself. */
    bool EchoActive() const;
    void FinishPlayback(bool drain);

    double out_gain_ = 0.35;
    double echo_tail_sec_ = 0.65;
    std::atomic<bool> assistant_audio_active_{false};
    double echo_until_ = 0.0;          // guarded by activity_mtx_

    /* Set when Gemini reports an interruption, cleared when the turn ends. The
     * model can keep generating briefly after it, and without this the next
     * audio frame would resume the abandoned sentence halfway through. */
    std::atomic<bool> turn_muted_{false};

    // Transcripts arrive a word or two at a time. Held here and logged as whole
    // sentences, because one line per fragment buries everything else.
    std::string user_line_;
    std::string assistant_line_;
    std::vector<int16_t> gain_buffer_;

    mutable std::mutex activity_mtx_;
    double last_activity_ = 0.0;
    // Wall time the session opened, for the daily ledger.
    double session_started_ = 0.0;
};

} // namespace jetson::audio
