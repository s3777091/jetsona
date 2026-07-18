#ifndef AUDIO_CODEC_H
#define AUDIO_CODEC_H

#include <string>
#include <atomic>

/* Audio output interface for the Linux port. */
class AudioCodec {
public:
    virtual ~AudioCodec() = default;
    virtual void Init() {}
    virtual void SetOutputVolume(int volume) { output_volume_ = volume; }
    virtual void SetInputVolume(int /*volume*/) {}
    virtual void SetOutputMuted(bool muted) { output_muted_ = muted; }
    virtual void SetOutputState(int volume, bool muted) {
        SetOutputVolume(volume);
        SetOutputMuted(muted);
    }
    virtual int output_volume() const { return output_volume_; }
    virtual bool output_muted() const { return output_muted_; }
    virtual int input_volume() const { return 0; }
    /* For phase-1 compatibility with code that asks for sample rates. */
    int input_sample_rate() const { return 16000; }
    int output_sample_rate() const { return 16000; }
    virtual bool duplex() const { return false; }

protected:
    int output_volume_ = 50;
    bool output_muted_ = false;
};

class DummyAudioCodec : public AudioCodec {
public:
    DummyAudioCodec(int /*in_rate*/ = 16000, int /*out_rate*/ = 16000) {}
};

/* Real Jetson output control. It tries the ALSA mixer first (the usual
 * JetPack path), then PipeWire/PulseAudio for images that use a desktop audio
 * stack. Commands run off the UI thread. */
class LinuxAudioCodec : public AudioCodec {
public:
    LinuxAudioCodec(int /*in_rate*/ = 16000, int /*out_rate*/ = 16000) {}
    void SetOutputVolume(int volume) override;
    void SetOutputMuted(bool muted) override;
    void SetOutputState(int volume, bool muted) override;

private:
    void QueueOutputState();
    std::atomic<unsigned> output_revision_{0};
};

#endif
