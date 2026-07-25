#include "audio/sherpa_engine.h"

#include "esp_log.h"
#include "settings.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

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

std::string EnvOr(const char *key, const std::string &fallback) {
    const char *value = std::getenv(key);
    return value && *value ? value : fallback;
}

int EnvInt(const char *key, int fallback) {
    const char *value = std::getenv(key);
    if (!value || !*value) return fallback;
    char *end = nullptr;
    long parsed = std::strtol(value, &end, 10);
    return end && *end == '\0' ? static_cast<int>(parsed) : fallback;
}

float EnvFloat(const char *key, float fallback) {
    const char *value = std::getenv(key);
    if (!value || !*value) return fallback;
    char *end = nullptr;
    float parsed = std::strtof(value, &end);
    return end && *end == '\0' ? parsed : fallback;
}

size_t CurlWrite(void *data, size_t size, size_t count, void *user) {
    const size_t bytes = size * count;
    auto *out = static_cast<std::string *>(user);
    if (out->size() + bytes > 1024 * 1024) return 0;
    out->append(static_cast<const char *>(data), bytes);
    return bytes;
}

void AppendLe16(std::vector<uint8_t> &out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
}

void AppendLe32(std::vector<uint8_t> &out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
}

std::vector<uint8_t> MakePcmWav(const int16_t *samples, size_t count) {
    const uint32_t data_bytes =
        static_cast<uint32_t>(std::min<size_t>(count, UINT32_MAX / 2) * 2);
    std::vector<uint8_t> wav;
    wav.reserve(static_cast<size_t>(data_bytes) + 44);
    const char riff[] = "RIFF";
    wav.insert(wav.end(), riff, riff + 4);
    AppendLe32(wav, 36 + data_bytes);
    const char wave_fmt[] = "WAVEfmt ";
    wav.insert(wav.end(), wave_fmt, wave_fmt + 8);
    AppendLe32(wav, 16);
    AppendLe16(wav, 1);
    AppendLe16(wav, 1);
    AppendLe32(wav, 16000);
    AppendLe32(wav, 16000 * 2);
    AppendLe16(wav, 2);
    AppendLe16(wav, 16);
    const char data[] = "data";
    wav.insert(wav.end(), data, data + 4);
    AppendLe32(wav, data_bytes);
    const auto *bytes = reinterpret_cast<const uint8_t *>(samples);
    wav.insert(wav.end(), bytes, bytes + data_bytes);
    return wav;
}

bool SendAll(int fd, const void *data, size_t size) {
    const auto *p = static_cast<const uint8_t *>(data);
    while (size > 0) {
        const ssize_t sent = send(fd, p, size, MSG_NOSIGNAL);
        if (sent <= 0) return false;
        p += sent;
        size -= static_cast<size_t>(sent);
    }
    return true;
}

bool RecvAll(int fd, void *data, size_t size) {
    auto *p = static_cast<uint8_t *>(data);
    while (size > 0) {
        const ssize_t received = recv(fd, p, size, 0);
        if (received <= 0) return false;
        p += received;
        size -= static_cast<size_t>(received);
    }
    return true;
}

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

// Letters and digits only, lowercased. Used to compare a transcript with the
// prompt without tripping over punctuation or capitalisation.
std::string SquashForCompare(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (u >= 0x80) {                       // keep UTF-8 payload bytes as-is
            out.push_back(c);
        } else if (std::isalnum(u)) {
            out.push_back(static_cast<char>(std::tolower(u)));
        }
    }
    return out;
}

/* True when the API handed back its own prompt instead of a transcript.
 *
 * gpt-4o-mini-transcribe echoes the `prompt` field when the audio holds no
 * intelligible speech -- a silent follow-up capture therefore yields a fluent
 * Vietnamese sentence that would otherwise be forwarded to the LLM as a real
 * command. Reject the exact echo, and reject a fragment of it too: the model
 * often returns only the first clause. Short results are left alone because a
 * genuine one-word reply can coincidentally appear inside the prompt. */
bool LooksLikePromptEcho(const std::string &text, const std::string &prompt) {
    const std::string a = SquashForCompare(text);
    const std::string b = SquashForCompare(prompt);
    if (a.empty() || b.empty()) return false;
    if (a == b) return true;
    return a.size() >= 24 && b.find(a) != std::string::npos;
}

} // namespace

SherpaVoiceEngine::~SherpaVoiceEngine() {
    if (oww_fd_ >= 0) close(oww_fd_);
#if JETSON_HAVE_SHERPA
    if (vad_) SherpaOnnxDestroyVoiceActivityDetector(
        reinterpret_cast<SherpaOnnxVoiceActivityDetector *>(vad_));
    if (kws_stream_) SherpaOnnxDestroyOnlineStream(
        reinterpret_cast<SherpaOnnxOnlineStream *>(kws_stream_));
    if (kws_) SherpaOnnxDestroyKeywordSpotter(
        reinterpret_cast<SherpaOnnxKeywordSpotter *>(kws_));
    if (stt_) SherpaOnnxDestroyOfflineRecognizer(
        reinterpret_cast<const SherpaOnnxOfflineRecognizer *>(stt_));
    if (tts_) SherpaOnnxDestroyOfflineTts(
        reinterpret_cast<SherpaOnnxOfflineTts *>(tts_));
#endif
}

// Only Silero remains in-process. openWakeWord is isolated in an offline
// sidecar, transcription is OpenAI, and speech synthesis is Edge TTS.
bool SherpaVoiceEngine::Init() {
    const bool kws_ok = EnsureKws();
    const bool vad_ok = EnsureVad();
    const bool stt_ok = !EnvOr("OPENAI_API_KEY", "").empty();
    ESP_LOGI(TAG, "OpenAI STT %s (model=%s)",
             stt_ok ? "configured" : "MISSING OPENAI_API_KEY",
             EnvOr("OPENAI_TRANSCRIBE_MODEL",
                   "gpt-4o-mini-transcribe").c_str());
    ESP_LOGI(TAG, "Edge TTS configured (voice=%s)",
             EnvOr("EDGE_TTS_VOICE", "vi-VN-HoaiMyNeural").c_str());
    return kws_ok && vad_ok && stt_ok;
}
bool SherpaVoiceEngine::Ready() const {
    return vad_ != nullptr && !EnvOr("OPENAI_API_KEY", "").empty();
}

// ---- KWS -----------------------------------------------------------------

bool SherpaVoiceEngine::EnsureKws() {
    if (oww_fd_ >= 0) return true;

    const std::string socket_path =
        EnvOr("OPENWAKEWORD_SOCKET",
              "/run/jetsona-voice/openwakeword.sock");
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;

    timeval timeout{};
    timeout.tv_sec = 0;
    timeout.tv_usec = 250000;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (socket_path.size() >= sizeof(address.sun_path)) {
        close(fd);
        ESP_LOGE(TAG, "openWakeWord socket path is too long");
        return false;
    }
    std::memcpy(address.sun_path, socket_path.c_str(),
                socket_path.size() + 1);
    if (connect(fd, reinterpret_cast<sockaddr *>(&address),
                sizeof(address)) != 0) {
        close(fd);
        if (!kws_tried_) {
            kws_tried_ = true;
            ESP_LOGW(TAG, "openWakeWord not ready at %s: %s",
                     socket_path.c_str(), std::strerror(errno));
        }
        return false;
    }

    oww_fd_ = fd;
    kws_tried_ = false;
    if (!oww_connected_logged_) {
        oww_connected_logged_ = true;
        ESP_LOGI(TAG,
                 "openWakeWord connected (model=Hey Nova, frame=80 ms, threshold=%.2f x%d)",
                 EnvFloat("OPENWAKEWORD_THRESHOLD", 0.45f),
                 std::max(1, EnvInt("OPENWAKEWORD_MIN_FRAMES", 2)));
    }
    return true;

#if 0  // Replaced by the offline openWakeWord sidecar above.
#if JETSON_HAVE_SHERPA
    if (kws_ || kws_tried_) return kws_ != nullptr;
    kws_tried_ = true;
    const std::string base = ModelsDir() + "/kws_gigaspeech/";
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
    cfg.keywords_score = F("kws_keywords_score", 3.0f);
    cfg.keywords_threshold = F("kws_keywords_threshold", 0.05f);
    if (!kwb.empty()) {
        cfg.keywords_buf = kwb.c_str();
        cfg.keywords_buf_size = static_cast<int32_t>(kwb.size());
    } else {
        cfg.keywords_file = kwf.c_str();
    }

    try {
        kws_ = SherpaOnnxCreateKeywordSpotter(&cfg);
    } catch (const std::exception &e) {
        ESP_LOGE(TAG, "KWS create failed: %s", e.what());
        kws_ = nullptr;
    } catch (...) {
        ESP_LOGE(TAG, "KWS create failed with an unknown exception");
        kws_ = nullptr;
    }
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
#endif
}

bool SherpaVoiceEngine::FeedWake(const int16_t *samples, size_t n) {
    if (!samples || n == 0) return false;
    oww_audio_.insert(oww_audio_.end(), samples, samples + n);

    constexpr size_t kFrameSamples = 1280;  // openWakeWord native 80 ms frame
    const float threshold =
        std::max(0.001f, std::min(0.99f,
                                 EnvFloat("OPENWAKEWORD_THRESHOLD", 0.45f)));
    const int min_frames =
        std::max(1, std::min(5,
                            EnvInt("OPENWAKEWORD_MIN_FRAMES", 2)));
    bool detected = false;

    while (oww_audio_.size() >= kFrameSamples) {
        if (!EnsureKws()) {
            // Bound memory while the sidecar is restarting. The next call
            // retries the connection without starving continuous capture.
            if (oww_audio_.size() > kFrameSamples * 3)
                oww_audio_.erase(oww_audio_.begin(),
                                 oww_audio_.end() - kFrameSamples * 3);
            return false;
        }

        float score = 0.0f;
        const bool ok =
            SendAll(oww_fd_, oww_audio_.data(), kFrameSamples * sizeof(int16_t)) &&
            RecvAll(oww_fd_, &score, sizeof(score));
        oww_audio_.erase(oww_audio_.begin(),
                         oww_audio_.begin() + kFrameSamples);
        if (!ok || !std::isfinite(score)) {
            close(oww_fd_);
            oww_fd_ = -1;
            oww_score_frames_ = 0;
            ESP_LOGW(TAG, "openWakeWord connection lost; reconnecting");
            continue;
        }

        if (score >= threshold) {
            ++oww_score_frames_;
            ESP_LOGI(TAG, "Hey Nova candidate score=%.3f frame=%d/%d",
                     score, oww_score_frames_, min_frames);
        } else {
            // One sub-threshold frame is tolerated so a narrow score peak
            // across adjacent 80 ms windows is not discarded immediately.
            oww_score_frames_ = std::max(0, oww_score_frames_ - 1);
        }
        if (oww_score_frames_ >= min_frames) {
            detected = true;
            oww_score_frames_ = 0;
            break;
        }
    }
    return detected;
}

// ---- VAD -----------------------------------------------------------------

bool SherpaVoiceEngine::EnsureVad() {
#if JETSON_HAVE_SHERPA
    if (vad_ || vad_tried_) return vad_ != nullptr;
    vad_tried_ = true;
    const std::string base = ModelsDir() + "/vad/";
    std::string model = S("vad_model", base + "silero_vad_v4.onnx");
    std::string provider = S("vad_provider", "cpu");

    SherpaOnnxVadModelConfig cfg{};
    cfg.silero_vad.model = model.c_str();
    cfg.silero_vad.threshold = F("vad_speech_threshold", 0.3f);
    cfg.silero_vad.min_silence_duration =
        F("vad_min_silence_duration", 0.3f);
    cfg.silero_vad.min_speech_duration =
        F("vad_min_speech_duration", 0.15f);
    cfg.silero_vad.max_speech_duration =
        F("vad_max_speech_duration", 20.0f);
    cfg.silero_vad.window_size = 512;
    cfg.sample_rate = 16000;
    cfg.num_threads = I("vad_num_threads", 1);
    cfg.provider = provider.c_str();
    cfg.debug = 0;

    try {
        vad_ = SherpaOnnxCreateVoiceActivityDetector(&cfg, 30.0f);
    } catch (const std::exception &e) {
        ESP_LOGE(TAG, "VAD create failed: %s", e.what());
        vad_ = nullptr;
    } catch (...) {
        ESP_LOGE(TAG, "VAD create failed with an unknown exception");
        vad_ = nullptr;
    }
    if (!vad_) {
        ESP_LOGW(TAG, "neural VAD unavailable (model=%s); using energy fallback",
                 model.c_str());
        return false;
    }
    ESP_LOGI(TAG, "Silero VAD ready (threshold=%.2f, provider=%s)",
             cfg.silero_vad.threshold, provider.c_str());
    return true;
#else
    return false;
#endif
}

bool SherpaVoiceEngine::VadReady() const { return vad_ != nullptr; }

bool SherpaVoiceEngine::FeedVad(const int16_t *samples, size_t n) {
#if JETSON_HAVE_SHERPA
    if (!EnsureVad()) return false;
    auto *vad =
        reinterpret_cast<SherpaOnnxVoiceActivityDetector *>(vad_);
    std::vector<float> f = ToFloat(samples, n);
    SherpaOnnxVoiceActivityDetectorAcceptWaveform(
        vad, f.data(), static_cast<int32_t>(f.size()));
    const bool detected = SherpaOnnxVoiceActivityDetectorDetected(vad) != 0;
    // We retain audio in VoiceLoop's pre-roll/utterance buffers, so discard
    // sherpa's completed segment copies to keep its circular buffer bounded.
    while (!SherpaOnnxVoiceActivityDetectorEmpty(vad)) {
        const SherpaOnnxSpeechSegment *segment =
            SherpaOnnxVoiceActivityDetectorFront(vad);
        if (segment) SherpaOnnxDestroySpeechSegment(segment);
        SherpaOnnxVoiceActivityDetectorPop(vad);
    }
    return detected;
#else
    (void)samples;
    (void)n;
    return false;
#endif
}

void SherpaVoiceEngine::ResetVad() {
#if JETSON_HAVE_SHERPA
    if (vad_) SherpaOnnxVoiceActivityDetectorReset(
        reinterpret_cast<SherpaOnnxVoiceActivityDetector *>(vad_));
#endif
}

// ---- STT -----------------------------------------------------------------

bool SherpaVoiceEngine::EnsureStt() {
#if JETSON_HAVE_SHERPA
    if (stt_ || stt_tried_) return stt_ != nullptr;
    stt_tried_ = true;
    const std::string base = ModelsDir() + "/stt/";
    // The bundled model is the Vietnamese-only offline int8 transducer. The
    // voice loop already hands us a complete, VAD-endpointed utterance.
    std::string enc = S("stt_encoder", base + "encoder.onnx");
    std::string dec = S("stt_decoder", base + "decoder.onnx");
    std::string joi = S("stt_joiner", base + "joiner.onnx");
    std::string tok = S("stt_tokens", base + "tokens.txt");
    std::string prov = S("stt_provider", "cpu");

    SherpaOnnxOfflineRecognizerConfig cfg{};
    cfg.feat_config.sample_rate = 16000;
    cfg.feat_config.feature_dim = 80;
    cfg.model_config.transducer.encoder = enc.c_str();
    cfg.model_config.transducer.decoder = dec.c_str();
    cfg.model_config.transducer.joiner = joi.c_str();
    cfg.model_config.tokens = tok.c_str();
    cfg.model_config.num_threads = I("stt_num_threads", 2);
    cfg.model_config.provider = prov.c_str();
    cfg.model_config.debug = 0;
    cfg.decoding_method = "greedy_search";
    cfg.max_active_paths = I("stt_max_active_paths", 4);

    try {
        stt_ = SherpaOnnxCreateOfflineRecognizer(&cfg);
    } catch (const std::exception &e) {
        ESP_LOGE(TAG, "STT create failed: %s", e.what());
        stt_ = nullptr;
    } catch (...) {
        ESP_LOGE(TAG, "STT create failed with an unknown exception");
        stt_ = nullptr;
    }
    if (!stt_) {
        ESP_LOGE(TAG, "STT create failed (encoder=%s, provider=%s)", enc.c_str(),
                 prov.c_str());
        return false;
    }
    ESP_LOGI(TAG, "STT ready (Vietnamese offline int8 transducer, provider=%s)",
             prov.c_str());
    return true;
#else
    return false;
#endif
}

std::string SherpaVoiceEngine::Recognize(const int16_t *samples, size_t n) {
    if (!samples || n == 0) return "";
    const std::string api_key = EnvOr("OPENAI_API_KEY", "");
    if (api_key.empty()) {
        ESP_LOGE(TAG, "OpenAI STT unavailable: OPENAI_API_KEY is missing");
        return "";
    }

    static std::once_flag curl_once;
    std::call_once(curl_once,
                   [] { curl_global_init(CURL_GLOBAL_DEFAULT); });

    CURL *curl = curl_easy_init();
    if (!curl) {
        ESP_LOGE(TAG, "OpenAI STT: curl init failed");
        return "";
    }

    const std::vector<uint8_t> wav = MakePcmWav(samples, n);
    const std::string base =
        EnvOr("OPENAI_TRANSCRIBE_BASE_URL", "https://api.openai.com/v1");
    const std::string url = base + "/audio/transcriptions";
    const std::string model =
        EnvOr("OPENAI_TRANSCRIBE_MODEL", "gpt-4o-mini-transcribe");
    const std::string language =
        EnvOr("OPENAI_TRANSCRIBE_LANGUAGE", "vi");
    const std::string prompt =
        "Đây là lệnh tiếng Việt gửi cho trợ lý Nova. Cụm từ đánh thức là "
        "\"Hey Nova\"; hãy chép chính xác tên Nova khi được nói.";

    std::string response;
    curl_mime *mime = curl_mime_init(curl);
    auto add_text = [mime](const char *name, const std::string &value) {
        curl_mimepart *part = curl_mime_addpart(mime);
        curl_mime_name(part, name);
        curl_mime_data(part, value.c_str(), CURL_ZERO_TERMINATED);
    };
    curl_mimepart *file = curl_mime_addpart(mime);
    curl_mime_name(file, "file");
    curl_mime_filename(file, "utterance.wav");
    curl_mime_type(file, "audio/wav");
    curl_mime_data(file, reinterpret_cast<const char *>(wav.data()),
                   wav.size());
    add_text("model", model);
    add_text("language", language);
    add_text("prompt", prompt);
    add_text("response_format", "json");
    add_text("temperature", "0");

    struct curl_slist *headers = nullptr;
    const std::string authorization = "Authorization: Bearer " + api_key;
    headers = curl_slist_append(headers, authorization.c_str());
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, "Expect:");
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    ESP_LOGI(TAG, "OpenAI STT request (model=%s, audio=%.2f s)",
             model.c_str(), static_cast<double>(n) / 16000.0);
    const CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_mime_free(mime);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        ESP_LOGE(TAG, "OpenAI STT transport failed: %s",
                 curl_easy_strerror(rc));
        return "";
    }

    const auto root = nlohmann::json::parse(response, nullptr, false);
    if (status < 200 || status >= 300) {
        std::string message = "unknown API error";
        if (root.is_object() && root.contains("error") &&
            root["error"].is_object())
            message = root["error"].value("message", message);
        ESP_LOGE(TAG, "OpenAI STT HTTP %ld: %.180s", status,
                 message.c_str());
        return "";
    }
    if (!root.is_object() || !root.contains("text") ||
        !root["text"].is_string()) {
        ESP_LOGE(TAG, "OpenAI STT returned invalid JSON");
        return "";
    }

    std::string text = root["text"].get<std::string>();
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    const size_t last = text.find_last_not_of(" \t\r\n");
    text = text.substr(first, last - first + 1);
    if (LooksLikePromptEcho(text, prompt)) {
        ESP_LOGW(TAG,
                 "OpenAI STT echoed the prompt (no speech in %.2f s of audio); discarding",
                 static_cast<double>(n) / 16000.0);
        return "";
    }
    ESP_LOGI(TAG, "OpenAI STT transcript: %s", text.c_str());
    return text;
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
    // Keep Edge's network-enabled container in a directory separate from the
    // offline KWS socket. It must never be able to open the raw-PCM channel.
    if (mkdir("/run/jetsona-edge-tts", 0700) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "Edge TTS: cannot create runtime directory: %s",
                 std::strerror(errno));
        return false;
    }

    char text_path[] = "/run/jetsona-edge-tts/tts-text-XXXXXX";
    char pcm_path[] = "/run/jetsona-edge-tts/tts-pcm-XXXXXX";
    const int text_fd = mkstemp(text_path);
    const int pcm_fd = mkstemp(pcm_path);
    if (text_fd < 0 || pcm_fd < 0) {
        if (text_fd >= 0) close(text_fd);
        if (pcm_fd >= 0) close(pcm_fd);
        if (text_fd >= 0) unlink(text_path);
        if (pcm_fd >= 0) unlink(pcm_path);
        ESP_LOGE(TAG, "Edge TTS: cannot create temporary files");
        return false;
    }
    close(pcm_fd);

    size_t written = 0;
    bool write_ok = true;
    while (written < text.size()) {
        const ssize_t n =
            write(text_fd, text.data() + written, text.size() - written);
        if (n <= 0) {
            write_ok = false;
            break;
        }
        written += static_cast<size_t>(n);
    }
    close(text_fd);
    if (!write_ok) {
        unlink(text_path);
        unlink(pcm_path);
        ESP_LOGE(TAG, "Edge TTS: cannot write input text");
        return false;
    }

    const std::string script =
        EnvOr("EDGE_TTS_SCRIPT",
              "/opt/jetson-fw/scripts/edge_tts_synthesize.sh");
    const std::string voice =
        EnvOr("EDGE_TTS_VOICE", "vi-VN-HoaiMyNeural");
    const std::string rate = EnvOr("EDGE_TTS_RATE", "+0%");
    const int sample_rate =
        std::max(8000, std::min(48000,
                                EnvInt("EDGE_TTS_SAMPLE_RATE", 24000)));
    const std::string sample_rate_arg = std::to_string(sample_rate);
    const std::string image =
        EnvOr("JETSON_VOICE_RUNTIME_IMAGE",
              "jetsona/voice-runtime:oww-0.6-edge-7.2.8");

    const pid_t child = fork();
    if (child == 0) {
        execl(script.c_str(), script.c_str(), text_path, pcm_path,
              voice.c_str(), rate.c_str(), sample_rate_arg.c_str(),
              image.c_str(), static_cast<char *>(nullptr));
        _exit(127);
    }
    int status = 0;
    const bool child_ok =
        child > 0 && waitpid(child, &status, 0) == child &&
        WIFEXITED(status) && WEXITSTATUS(status) == 0;
    unlink(text_path);
    if (!child_ok) {
        unlink(pcm_path);
        ESP_LOGE(TAG, "Edge TTS failed (status=%d)", status);
        return false;
    }

    std::ifstream pcm(pcm_path, std::ios::binary);
    std::vector<int16_t> s16;
    if (pcm) {
        pcm.seekg(0, std::ios::end);
        const std::streamoff bytes = pcm.tellg();
        pcm.seekg(0, std::ios::beg);
        if (bytes > 1) {
            s16.resize(static_cast<size_t>(bytes) / sizeof(int16_t));
            pcm.read(reinterpret_cast<char *>(s16.data()),
                     static_cast<std::streamsize>(
                         s16.size() * sizeof(int16_t)));
        }
    }
    unlink(pcm_path);
    if (s16.empty()) {
        ESP_LOGE(TAG, "Edge TTS produced no PCM");
        return false;
    }

    out.sample_rate = sample_rate;
    out.samples.resize(s16.size());
    std::transform(s16.begin(), s16.end(), out.samples.begin(),
                   [](int16_t value) {
                       return static_cast<float>(value) / 32768.0f;
                   });
    ESP_LOGI(TAG, "Edge TTS rendered %.2f s (voice=%s)",
             static_cast<double>(out.samples.size()) / sample_rate,
             voice.c_str());
    return true;
}

} // namespace jetson::audio
