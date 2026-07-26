#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace jetson::audio {

class VoiceEngine;
class MicCapture;
class AudioOutput;
class RealtimeClient;

/* The voice loop: mic -> continuous openWakeWord -> Silero confirmation ->
 * capture through trailing silence -> OpenAI STT -> Conversation -> Edge TTS.
 *
 * KWS always receives PCM while idle/collecting. Silero never gates audio sent
 * to KWS; it only confirms a wake candidate and endpoints an active command.
 * Ambient speech without a wake match is kept only in the short ring buffer
 * and never sent to cloud STT.
 *
 * Mic chunks arrive on the capture thread; STT/agent run on a detached worker;
 * TTS playback runs on a dedicated speaker thread that also serves out-of-band
 * Speak() calls (battery/alarm). Capture is paused while the speaker is active so
 * the device's own voice output is not transcribed back as a command. */
class VoiceLoop {
public:
    static VoiceLoop &Instance();

    /* Builds the engine, opens mic + speaker, starts the threads. Idempotent;
     * returns false (and logs) if the mic won't open -- the rest of Ekko still
     * runs (agent is reachable if something else feeds it text). */
    bool Start();
    void Stop();

    /* Speak text out of band (battery low, alarm, agent reply). Thread-safe;
     * enqueues to the speaker thread. No-op if the loop isn't running. */
    void Speak(const std::string &text);

    /* Called when the agent starts a tool. Speaks a short "working on it" line,
     * at most once per turn, so a tool-using turn does not answer the user with
     * many seconds of silence. Thread-safe: runs on the agent worker. */
    void NotifyToolStarted();

    bool running() const { return running_.load(); }
    bool speaking() const { return speaking_.load(); }

private:
    VoiceLoop();
    ~VoiceLoop();
    VoiceLoop(const VoiceLoop &) = delete;
    VoiceLoop &operator=(const VoiceLoop &) = delete;

    void OnMicChunk(const int16_t *samples, size_t n);
    void SpeakerThread();
    void RecognizeAndSend(const std::vector<int16_t> &utterance,
                          bool explicit_wake, bool follow_up);

    // Decide whether a transcript should be acted on. Returns true when the
    // wake word is present (cmd = text after it), when the local model already
    // matched it, or when capture began inside the awake window (cmd = whole
    // text). The last two are decided at capture time and passed in, because
    // endpointing plus cloud STT can outlast the window on their own.
    bool MatchWake(const std::string &text, bool explicit_wake, bool follow_up,
                   std::string &cmd);

    // Opens a streaming Gemini Live session on a wake word and closes it when
    // the conversation goes quiet. While it is open the mic keeps feeding it,
    // including while Nova is talking -- that is what allows interrupting her.
    // Null when JETSON_VOICE_REALTIME is off, which leaves the older
    // STT/LLM/TTS path in charge.
    bool StartRealtime();

    std::unique_ptr<VoiceEngine> engine_;
    std::unique_ptr<MicCapture> mic_;
    std::unique_ptr<AudioOutput> out_;
    std::unique_ptr<RealtimeClient> realtime_;
    double realtime_idle_sec_ = 20.0;

    // State machine (guarded by mtx_). kIdle waits for speech onset (energy);
    // kCollecting accumulates the utterance; kBusy ignores the mic until the
    // turn finishes + speaks.
    enum State { kIdle, kCollecting, kBusy };
    State state_ = kIdle;
    std::vector<int16_t> utter_;
    std::deque<int16_t> pre_roll_;
    int speech_chunks_ = 0;
    int silence_chunks_ = 0;
    int total_chunks_ = 0;
    int onset_chunks_ = 0;
    int recent_vad_chunks_ = 0;
    bool wake_latched_ = false;
    // Set when a capture starts inside the awake window, so the follow-up is
    // still honoured after STT returns however long that took.
    bool followup_latched_ = false;

    // Adaptive energy VAD. Startup calibration keeps the per-chunk RMS samples
    // and uses a low percentile, so steady fan noise becomes part of the floor
    // while a person speaking during calibration does not inflate it. Idle
    // chunks keep the estimate tracking slowly.
    double noise_rms_ = 0.0;
    double noise_high_rms_ = 0.0;
    double vad_min_rms_ = 38.0;
    double vad_noise_multiplier_ = 1.6;
    double vad_noise_margin_ = 8.0;
    double active_threshold_ = 38.0;
    double idle_peak_rms_ = 0.0;
    size_t pre_roll_samples_ = 10240;
    uint64_t calibration_chunks_ = 0;
    std::vector<double> calibration_rms_;
    uint64_t idle_log_chunks_ = 0;

    // After a turn we stay "awake" briefly so a follow-up needn't repeat the
    // wake word. Guarded by mtx_.
    std::chrono::steady_clock::time_point awake_until_{};

    std::mutex mtx_;

    // Speaker thread: drains speech_ and TTS-plays each, one at a time.
    std::queue<std::string> speech_;
    std::mutex speech_mtx_;
    std::condition_variable speech_cv_;
    std::thread speaker_thread_;
    std::atomic<bool> speaking_{false};

    // Cleared when a turn is handed to the agent; set by the first tool of that
    // turn so only the first one speaks a filler.
    std::atomic<bool> filler_spoken_{false};

    std::atomic<bool> running_{false};
};

} // namespace jetson::audio
