#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sherpa-onnx/c-api/c-api.h"

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s MODEL THRESHOLD WAV...\n", argv[0]);
        return 2;
    }
    const float threshold = strtof(argv[2], NULL);

    SherpaOnnxVadModelConfig config;
    memset(&config, 0, sizeof(config));
    config.silero_vad.model = argv[1];
    config.silero_vad.threshold = threshold;
    config.silero_vad.min_silence_duration = 0.3f;
    config.silero_vad.min_speech_duration = 0.15f;
    config.silero_vad.max_speech_duration = 20.0f;
    config.silero_vad.window_size = 512;
    config.sample_rate = 16000;
    config.num_threads = 1;
    config.provider = "cpu";

    for (int file_index = 3; file_index < argc; ++file_index) {
        const SherpaOnnxWave *wave = SherpaOnnxReadWave(argv[file_index]);
        if (!wave || wave->sample_rate != 16000) {
            fprintf(stderr, "%s: invalid 16 kHz mono WAV\n", argv[file_index]);
            if (wave) SherpaOnnxFreeWave(wave);
            continue;
        }

        SherpaOnnxVoiceActivityDetector *vad =
            SherpaOnnxCreateVoiceActivityDetector(&config, 30.0f);
        if (!vad) {
            fprintf(stderr, "failed to create VAD\n");
            SherpaOnnxFreeWave(wave);
            return 3;
        }

        int offset = 0;
        int segments = 0;
        double speech_seconds = 0.0;
        while (offset + config.silero_vad.window_size <= wave->num_samples) {
            SherpaOnnxVoiceActivityDetectorAcceptWaveform(
                vad, wave->samples + offset, config.silero_vad.window_size);
            offset += config.silero_vad.window_size;
            while (!SherpaOnnxVoiceActivityDetectorEmpty(vad)) {
                const SherpaOnnxSpeechSegment *segment =
                    SherpaOnnxVoiceActivityDetectorFront(vad);
                printf("%s segment=%d start=%.3f duration=%.3f\n",
                       argv[file_index], segments,
                       segment->start / 16000.0,
                       segment->n / 16000.0);
                speech_seconds += segment->n / 16000.0;
                ++segments;
                SherpaOnnxDestroySpeechSegment(segment);
                SherpaOnnxVoiceActivityDetectorPop(vad);
            }
        }
        SherpaOnnxVoiceActivityDetectorFlush(vad);
        while (!SherpaOnnxVoiceActivityDetectorEmpty(vad)) {
            const SherpaOnnxSpeechSegment *segment =
                SherpaOnnxVoiceActivityDetectorFront(vad);
            printf("%s segment=%d start=%.3f duration=%.3f\n",
                   argv[file_index], segments,
                   segment->start / 16000.0,
                   segment->n / 16000.0);
            speech_seconds += segment->n / 16000.0;
            ++segments;
            SherpaOnnxDestroySpeechSegment(segment);
            SherpaOnnxVoiceActivityDetectorPop(vad);
        }
        printf("%s summary segments=%d speech_seconds=%.3f\n",
               argv[file_index], segments, speech_seconds);
        SherpaOnnxDestroyVoiceActivityDetector(vad);
        SherpaOnnxFreeWave(wave);
    }
    return 0;
}
