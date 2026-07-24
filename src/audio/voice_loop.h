#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
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

/* The voice loop: mic -> wake word ("Ekko") -> capture utterance (energy VAD) ->
 * STT -> Conversation -> TTS -> speaker -> re-arm. One process-wide instance.
 *
 * Mic chunks arrive on the capture thread; STT/agent run on a detached worker;
 * TTS playback runs on a dedicated speaker thread that also serves out-of-band
 * Speak() calls (battery/alarm). KWS is paused while the speaker is active so
 * the device's own voice output is not heard back as a wake word. */
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

    std::unique_ptr<VoiceEngine> engine_;
    std::unique_ptr<MicCapture> mic_;
    std::unique_ptr<AudioOutput> out_;

    // State machine (guarded by mtx_). kIdle feeds KWS; kCollecting accumulates
    // the utterance; kBusy ignores the mic until the turn finishes + speaks.
    enum State { kIdle, kCollecting, kBusy };
    State state_ = kIdle;
    std::vector<int16_t> utter_;
    int speech_chunks_ = 0;
    int silence_chunks_ = 0;
    int total_chunks_ = 0;

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