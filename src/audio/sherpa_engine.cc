#include "audio/sherpa_engine.h"

#include "esp_log.h"
#include "settings.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#if JETSON_HAVE_SHERPA
#include <sherpa-onnx/c-api/c-api.h>
#endif

#define TAG "SherpaEngine"

namespace jetson::audio {

namespace {

std::string ModelsDir() { return std::string(JETSON_ASSETS_DIR) + "/models"; }

std::string S(const char *key, const std::string &def) {
    return Settings("voice", false).GetString(key, def);
}
int I(const char *key, int def) { return Settings("voice", false).GetInt(key, def); }
float F(const char *key, float def) { return Settings("voice", false).GetFloat(key, def); }

#if JETSON_HAVE_SHERPA
std::vector<float> ToFloat(const int16_t *s, size_t n) {
    std::vector<float> f(n);
    for (size_t i = 0; i < n; ++i) f[i] = static_cast<float>(s[i]) / 32768.0f;
    return f;
}

std::string Trim(std::string s) {
    auto sp = s.find_first_not_of(" \t\r\n");
    if (sp == std::string::npos) return "";
    auto ep = s.find_last_not_of(" \t\r\n");
    return s.substr(sp, ep - sp + 1);
}
#endif

} // namespace

SherpaVoiceEngine::~SherpaVoiceEngine() {
#if JETSON_HAVE_SHERPA
    if (kws_stream_) SherpaOnnxDestroyOnlineStream(
        reinterpret_cast<SherpaOnnxOnlineStream *>(kws_stream_));
    if (kws_) SherpaOnnxDestroyKeywordSpotter(
        reinterpret_cast<SherpaOnnxKeywordSpotter *>(kws_));
    if (stt_) SherpaOnnxDestroyOnlineRecognizer(
        reinterpret_cast<const SherpaOnnxOnlineRecognizer *>(stt_));
    if (tts_) SherpaOnnxDestroyOfflineTts(
        reinterpret_cast<SherpaOnnxOfflineTts *>(tts_));
#endif
}

bool SherpaVoiceEngine::Init() { return EnsureKws(); }
bool SherpaVoiceEngine::Ready() const { return kws_ != nullptr; }

// ---- KWS -----------------------------------------------------------------

bool SherpaVoiceEngine::EnsureKws() {
#if JETSON_HAVE_SHERPA
    if (kws_ || kws_tried_) return kws_ != nullptr;
    kws_tried_ = true;
    const std::string base = ModelsDir() + "/kws/";
    // Hold the path strings for the whole call: the create function copies
    // them internally, but a .c_str() of a temporary S(...) would dangle first.
    std::string enc = S("kws_encoder", base + "encoder.onnx");
    std::string dec = S("kws_decoder", base + "decoder.onnx");
    std::string joi = S("kws_joiner", base + "joiner.onnx");
    std::string tok = S("kws_tokens", base + "tokens.txt");
    std::string kwf = S("kws_keywords_file", base + "keywords.txt");
    std::string kwb = S("kws_keywords", "");  // inline keyword, optional
    std::string prov = S("kws_provider", "cpu");

    SherpaOnnxKeywordSpotterConfig cfg{};
    cfg.feat_config.sample_rate = 16000;
    cfg.feat_config.feature_dim = 80;
    cfg.model_config.transducer.encoder = enc.c_str();
    cfg.model_config.transducer.decoder = dec.c_str();
    cfg.model_config.transducer.joiner = joi.c_str();
    cfg.model_config.tokens = tok.c_str();
    cfg.model_config.num_threads = I("kws_num_threads", 1);
    cfg.model_config.provider = prov.c_str();
    cfg.model_config.debug = 0;
    cfg.max_active_paths = I("kws_max_active_paths", 3);
    cfg.num_trailing_blanks = I("kws_num_trailing_blanks", 3);
    cfg.keywords_score = F("kws_keywords_score", 1.0f);
    cfg.keywords_threshold = F("kws_keywords_threshold", 0.3f);
    if (!kwb.empty()) {
        cfg.keywords_buf = kwb.c_str();
        cfg.keywords_buf_size = static_cast<int32_t>(kwb.size());
    } else {
        cfg.keywords_file = kwf.c_str();
    }

    kws_ = SherpaOnnxCreateKeywordSpotter(&cfg);
    if (!kws_) {
        ESP_LOGE(TAG, "KWS create failed (encoder=%s)", enc.c_str());
        return false;
    }
    kws_stream_ = SherpaOnnxCreateKeywordStream(
        reinterpret_cast<SherpaOnnxKeywordSpotter *>(kws_));
    ESP_LOGI(TAG, "KWS ready (provider=%s)", prov.c_str());
    return true;
#else
    if (!kws_tried_) {
        kws_tried_ = true;
        ESP_LOGW(TAG, "built without sherpa-onnx; voice loop inert. "
                      "Run scripts/fetch_sherpa.sh and rebuild with JETSON_HAVE_SHERPA.");
    }
    return false;
#endif
}

bool SherpaVoiceEngine::FeedWake(const int16_t *samples, size_t n) {
#if JETSON_HAVE_SHERPA
    if (!EnsureKws()) return false;
    auto *spotter = reinterpret_cast<SherpaOnnxKeywordSpotter *>(kws_);
    auto *stream = reinterpret_cast<SherpaOnnxOnlineStream *>(kws_stream_);
    std::vector<float> f = ToFloat(samples, n);
    SherpaOnnxOnlineStreamAcceptWaveform(stream, 16000, f.data(),
                                         static_cast<int32_t>(n));
    bool detected = false;
    while (SherpaOnnxIsKeywordStreamReady(spotter, stream)) {
        SherpaOnnxDecodeKeywordStream(spotter, stream);
        const SherpaOnnxKeywordResult *r = SherpaOnnxGetKeywordResult(spotter, stream);
        if (r && r->keyword && r->keyword[0]) {
            detected = true;
            ESP_LOGI(TAG, "wake word: %s", r->keyword);
            SherpaOnnxDestroyKeywordResult(r);
            // Re-arm: drop the accumulated stream and start a fresh one so the
            // next wake word is detected independently of this utterance.
            SherpaOnnxDestroyOnlineStream(stream);
            kws_stream_ = SherpaOnnxCreateKeywordStream(spotter);
            break;
        }
        if (r) SherpaOnnxDestroyKeywordResult(r);
    }
    return detected;
#else
    return false;
#endif
}

// ---- STT -----------------------------------------------------------------

bool SherpaVoiceEngine::EnsureStt() {
#if JETSON_HAVE_SHERPA
    if (stt_ || stt_tried_) return stt_ != nullptr;
    stt_tried_ = true;
    const std::string base = ModelsDir() + "/stt/";
    std::string enc = S("stt_encoder", base + "encoder.onnx");
    std::string dec = S("stt_decoder", base + "decoder.onnx");
    std::string joi = S("stt_joiner", base + "joiner.onnx");
    std::string ctc = S("stt_ctc_model", "");  // set this to use zipformer2_ctc
    std::string tok = S("stt_tokens", base + "tokens.txt");
    std::string prov = S("stt_provider", "cuda");

    SherpaOnnxOnlineRecognizerConfig cfg{};
    cfg.feat_config.sample_rate = 16000;
    cfg.feat_config.feature_dim = 80;
    if (ctc.empty()) {
        cfg.model_config.transducer.encoder = enc.c_str();
        cfg.model_config.transducer.decoder = dec.c_str();
        cfg.model_config.transducer.joiner = joi.c_str();
    } else {
        cfg.model_config.zipformer2_ctc.model = ctc.c_str();
    }
    cfg.model_config.tokens = tok.c_str();
    cfg.model_config.num_threads = I("stt_num_threads", 2);
    cfg.model_config.provider = prov.c_str();
    cfg.model_config.debug = 0;
    cfg.decoding_method = "greedy_search";
    cfg.max_active_paths = I("stt_max_active_paths", 4);
    cfg.enable_endpoint = 1;
    cfg.rule1_min_trailing_silence = F("stt_rule1_silence", 1.2f);
    cfg.rule2_min_trailing_silence = F("stt_rule2_silence", 1.2f);
    cfg.rule3_min_utterance_length = F("stt_rule3_len", 20.0f);

    stt_ = SherpaOnnxCreateOnlineRecognizer(&cfg);
    if (!stt_) {
        ESP_LOGE(TAG, "STT create failed (tokens=%s, provider=%s)", tok.c_str(),
                 prov.c_str());
        return false;
    }
    ESP_LOGI(TAG, "STT ready (provider=%s, mode=%s)", prov.c_str(),
             ctc.empty() ? "transducer" : "zipformer2_ctc");
    return true;
#else
    return false;
#endif
}

std::string SherpaVoiceEngine::Recognize(const int16_t *samples, size_t n) {
#if JETSON_HAVE_SHERPA
    if (!EnsureStt()) return "";
    auto *rec = reinterpret_cast<const SherpaOnnxOnlineRecognizer *>(stt_);
    const SherpaOnnxOnlineStream *stream = SherpaOnnxCreateOnlineStream(rec);
    if (!stream) return "";
    std::vector<float> f = ToFloat(samples, n);
    SherpaOnnxOnlineStreamAcceptWaveform(stream, 16000, f.data(),
                                         static_cast<int32_t>(n));
    SherpaOnnxOnlineStreamInputFinished(stream);

    std::string text;
    while (SherpaOnnxIsOnlineStreamReady(rec, stream)) {
        SherpaOnnxDecodeOnlineStream(rec, stream);
        const SherpaOnnxOnlineRecognizerResult *r =
            SherpaOnnxGetOnlineStreamResult(rec, stream);
        if (r && r->text) text = r->text;
        if (r) SherpaOnnxDestroyOnlineRecognizerResult(r);
        if (SherpaOnnxOnlineStreamIsEndpoint(rec, stream)) break;
    }
    SherpaOnnxDestroyOnlineStream(stream);
    return Trim(text);
#else
    return "";
#endif
}

// ---- TTS -----------------------------------------------------------------

bool SherpaVoiceEngine::EnsureTts() {
#if JETSON_HAVE_SHERPA
    if (tts_ || tts_tried_) return tts_ != nullptr;
    tts_tried_ = true;
    const std::string base = ModelsDir() + "/tts/";
    std::string model = S("tts_model", base + "vi_VN-vivos-x_low.onnx");
    std::string tokens = S("tts_tokens", base + "tokens.txt");
    std::string lexicon = S("tts_lexicon", "");
    std::string data_dir = S("tts_data_dir", base + "espeak-ng-data");
    std::string dict_dir = S("tts_dict_dir", "");
    std::string prov = S("tts_provider", "cpu");

    SherpaOnnxOfflineTtsConfig cfg{};
    cfg.model.vits.model = model.c_str();
    cfg.model.vits.lexicon = lexicon.c_str();
    cfg.model.vits.tokens = tokens.c_str();
    cfg.model.vits.data_dir = data_dir.c_str();
    cfg.model.vits.dict_dir = dict_dir.c_str();
    cfg.model.vits.noise_scale = F("tts_noise", 0.667f);
    cfg.model.vits.noise_scale_w = F("tts_noise_w", 0.8f);
    cfg.model.vits.length_scale = F("tts_length", 1.0f);
    cfg.model.num_threads = I("tts_num_threads", 2);
    cfg.model.debug = 0;
    cfg.model.provider = prov.c_str();
    cfg.max_num_sentences = I("tts_max_sentences", 2);

    tts_ = SherpaOnnxCreateOfflineTts(&cfg);
    if (!tts_) {
        ESP_LOGE(TAG, "TTS create failed (model=%s)", model.c_str());
        return false;
    }
    ESP_LOGI(TAG, "TTS ready (provider=%s, sr=%d)", prov.c_str(),
             SherpaOnnxOfflineTtsSampleRate(
                 reinterpret_cast<SherpaOnnxOfflineTts *>(tts_)));
    return true;
#else
    return false;
#endif
}

bool SherpaVoiceEngine::Synthesize(const std::string &text, SynthResult &out) {
    if (text.empty()) return false;
#if JETSON_HAVE_SHERPA
    if (!EnsureTts()) return false;
    auto *tts = reinterpret_cast<SherpaOnnxOfflineTts *>(tts_);
    int sid = I("tts_sid", 0);
    float speed = F("tts_speed", 1.0f);
    const SherpaOnnxGeneratedAudio *a =
        SherpaOnnxOfflineTtsGenerate(tts, text.c_str(), sid, speed);
    if (!a || a->n <= 0) {
        if (a) SherpaOnnxDestroyOfflineTtsGeneratedAudio(a);
        ESP_LOGW(TAG, "TTS produced no audio for: %.40s", text.c_str());
        return false;
    }
    out.sample_rate = a->sample_rate ? a->sample_rate
                                     : SherpaOnnxOfflineTtsSampleRate(tts);
    out.samples.assign(a->samples, a->samples + a->n);
    SherpaOnnxDestroyOfflineTtsGeneratedAudio(a);
    return true;
#else
    (void)out;
    return false;
#endif
}

} // namespace jetson::audio