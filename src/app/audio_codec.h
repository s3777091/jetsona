#ifndef AUDIO_CODEC_H
#define AUDIO_CODEC_H

#include <string>

/* Minimal AudioCodec interface for the Linux port. Phase 1 uses the dummy
 * codec (no ALSA). Phase 2 replaces this with an ALSA-backed implementation. */
class AudioCodec {
public:
    virtual ~AudioCodec() = default;
    virtual void Init() {}
    virtual void SetOutputVolume(int volume) { output_volume_ = volume; }
    virtual void SetInputVolume(int /*volume*/) {}
    virtual int output_volume() const { return output_volume_; }
    virtual int input_volume() const { return 0; }
    /* For phase-1 compatibility with code that asks for sample rates. */
    int input_sample_rate() const { return 16000; }
    int output_sample_rate() const { return 16000; }
    virtual bool duplex() const { return false; }

protected:
    int output_volume_ = 50;
};

class DummyAudioCodec : public AudioCodec {
public:
    DummyAudioCodec(int /*in_rate*/ = 16000, int /*out_rate*/ = 16000) {}
};

#endif