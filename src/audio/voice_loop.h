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

/* The voice loop: mic -> capture utterance (energy VAD) -> STT -> wake-word
 * match ("nova") -> Conversation -> TTS -> speaker -> re-arm. One process-wide
 * instance.
 *
 * Wake detection is done on the Vietnamese STT transcript rather than the
 * dedicated KWS spotter, whose ONNX graph is incompatible with the old
 * ONNX Runtime available on JetPack 4. Every voiced utterance is transcribed;
 * it is acted on only if it contains the wake word, or if we are still inside
 * a short "awake" window after the previous turn.
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

    bool running() const { return running_.load(); }
    bool speaking() const { return speaking_.load(); }

private:
    VoiceLoop();
    ~VoiceLoop();
    VoiceLoop(const VoiceLoop &) = delete;
    VoiceLoop &operator=(const VoiceLoop &) = delete;

    void OnMicChunk(const int16_t *samples, size_t n);
    void SpeakerThread();
    void RecognizeAndSend(const std::vector<int16_t> &utterance);

    // Decide whether a transcript should be acted on. Returns true when the
    // wake word "nova" is present (cmd = text after it) or we are still awake
    // from a recent turn (cmd = whole text). Reads awake_until_ under mtx_.
    bool MatchWake(const std::string &text, std::string &cmd);

    std::unique_ptr<VoiceEngine> engine_;
    std::unique_ptr<MicCapture> mic_;
    std::unique_ptr<AudioOutput> out_;

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

    // Adaptive energy VAD. The first second calibrates the room/device noise
    // floor; idle chunks keep it tracking slowly. Pre-roll preserves the start
    // of softly spoken "Hey Nova" instead of cutting it off at onset.
    double noise_rms_ = 0.0;
    double vad_min_rms_ = 38.0;
    double vad_noise_multiplier_ = 1.6;
    double vad_noise_margin_ = 8.0;
    double active_threshold_ = 38.0;
    double idle_peak_rms_ = 0.0;
    size_t pre_roll_samples_ = 10240;
    uint64_t calibration_chunks_ = 0;
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

    std::atomic<bool> running_{false};
};

} // namespace jetson::audio
