#include "url_audio_player.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>

#include <esp_log.h>
#include <esp_timer.h>

#include "audio_service.h"
#include "board.h"
#include "http.h"
#include "ogg_demuxer.h"
#include "protocol.h"

#include "esp_audio_dec_default.h"
#include "esp_audio_dec_reg.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_audio_simple_dec_reg.h"
#include "esp_audio_types.h"

namespace {

const char* TAG = "UrlAudioPlayer";

std::string ToLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string GetUrlExtension(const std::string& url) {
    size_t path_end = url.find_first_of("?#");
    std::string path = path_end == std::string::npos ? url : url.substr(0, path_end);
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= path.size()) {
        return "";
    }
    return ToLowerCopy(path.substr(dot + 1));
}

int GetOpusSamplesPerFrame(const uint8_t* packet, int sample_rate) {
    if (packet == nullptr || sample_rate <= 0) {
        return 0;
    }

    if (packet[0] & 0x80) {
        return ((packet[0] >> 3) & 0x3) * sample_rate / 400;
    }
    if ((packet[0] & 0x60) == 0x60) {
        return (packet[0] & 0x08) ? sample_rate / 50 : sample_rate / 100;
    }
    int frame_idx = (packet[0] >> 3) & 0x3;
    if (frame_idx == 3) {
        return sample_rate * 60 / 1000;
    }
    return (sample_rate << frame_idx) / 100;
}

int GetOpusFrameCount(const uint8_t* packet, size_t length) {
    if (packet == nullptr || length == 0) {
        return 0;
    }

    int code = packet[0] & 0x3;
    if (code == 0) {
        return 1;
    }
    if (code == 1 || code == 2) {
        return 2;
    }
    if (length < 2) {
        return 1;
    }
    return packet[1] & 0x3F;
}

int NormalizeOpusFrameDurationMs(int duration_ms) {
    static constexpr int kSupported[] = {5, 10, 20, 40, 60, 80, 100, 120};
    if (duration_ms <= 0) {
        return 60;
    }
    int best = kSupported[0];
    int best_delta = std::abs(duration_ms - best);
    for (int value : kSupported) {
        int delta = std::abs(duration_ms - value);
        if (delta < best_delta) {
            best = value;
            best_delta = delta;
        }
    }
    return best;
}

int GetOpusPacketDurationMs(const uint8_t* packet, size_t length, int sample_rate) {
    int per_frame = GetOpusSamplesPerFrame(packet, sample_rate);
    int frame_count = GetOpusFrameCount(packet, length);
    if (per_frame <= 0 || frame_count <= 0) {
        return 60;
    }
    int duration_ms = (per_frame * frame_count * 1000) / sample_rate;
    return NormalizeOpusFrameDurationMs(duration_ms);
}

const char* FormatToString(UrlAudioPlayer::Format format) {
    switch (format) {
        case UrlAudioPlayer::Format::kMp3:
            return "mp3";
        case UrlAudioPlayer::Format::kWav:
            return "wav";
        case UrlAudioPlayer::Format::kOgg:
            return "ogg";
        default:
            return "unknown";
    }
}

esp_audio_simple_dec_type_t ToSimpleDecoderType(UrlAudioPlayer::Format format) {
    switch (format) {
        case UrlAudioPlayer::Format::kMp3:
            return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
        case UrlAudioPlayer::Format::kWav:
            return ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
        default:
            return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
    }
}

void RegisterSimpleDecoderDefaultsOnce() {
    static std::once_flag once;
    std::call_once(once, []() {
        esp_audio_dec_register_default();
        esp_audio_simple_dec_register_default();
    });
}

}  // namespace

UrlAudioPlayer::UrlAudioPlayer(AudioService& audio_service)
    : audio_service_(audio_service) {
}

UrlAudioPlayer::~UrlAudioPlayer() {
    Stop();
    if (resampler_ != nullptr) {
        esp_ae_rate_cvt_close(resampler_);
        resampler_ = nullptr;
    }
}

bool UrlAudioPlayer::Play(const std::string& url) {
    if (url.empty()) {
        SetError("URL is empty");
        return false;
    }

    Stop();
    audio_service_.ResetDecoder();

    std::lock_guard<std::mutex> lock(mutex_);
    stop_requested_.store(false);
    playing_ = true;
    stopping_ = false;
    current_url_ = url;
    current_format_ = "unknown";
    state_ = "starting";
    last_error_.clear();

    BaseType_t task_ret = xTaskCreate(
        &UrlAudioPlayer::PlaybackTaskEntry,
        "url_audio",
        10240,
        this,
        3,
        &task_handle_);
    if (task_ret != pdPASS) {
        task_handle_ = nullptr;
        playing_ = false;
        stopping_ = false;
        state_ = "error";
        last_error_ = "Failed to create playback task";
        return false;
    }
    return true;
}

bool UrlAudioPlayer::Stop() {
    TaskHandle_t task = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!playing_ && task_handle_ == nullptr) {
            return false;
        }
        stop_requested_.store(true);
        stopping_ = true;
        state_ = "stopping";
        if (active_http_ != nullptr) {
            active_http_->Close();
        }
        task = task_handle_;
    }

    if (task != nullptr && task != xTaskGetCurrentTaskHandle()) {
        for (int i = 0; i < 150; ++i) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (task_handle_ == nullptr) {
                    break;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    audio_service_.ResetDecoder();
    return true;
}

bool UrlAudioPlayer::IsPlaying() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return playing_;
}

cJSON* UrlAudioPlayer::GetStatusJson() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "playing", playing_);
    cJSON_AddBoolToObject(root, "stopping", stopping_);
    cJSON_AddStringToObject(root, "state", state_.c_str());
    cJSON_AddStringToObject(root, "format", current_format_.c_str());
    cJSON_AddStringToObject(root, "url", current_url_.c_str());
    cJSON_AddBoolToObject(root, "streaming_reserved", true);
    if (!last_error_.empty()) {
        cJSON_AddStringToObject(root, "last_error", last_error_.c_str());
    }
    return root;
}

void UrlAudioPlayer::PlaybackTaskEntry(void* arg) {
    auto* self = static_cast<UrlAudioPlayer*>(arg);
    self->PlaybackTask();
    {
        std::lock_guard<std::mutex> lock(self->mutex_);
        self->task_handle_ = nullptr;
    }
    vTaskDelete(nullptr);
}

void UrlAudioPlayer::PlaybackTask() {
    std::string url;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        url = current_url_;
        state_ = "connecting";
    }

    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        SetError("Network interface unavailable");
        std::lock_guard<std::mutex> lock(mutex_);
        playing_ = false;
        stopping_ = false;
        state_ = "error";
        return;
    }

    auto http = network->CreateHttp(2);
    if (!http) {
        SetError("Failed to create HTTP client");
        std::lock_guard<std::mutex> lock(mutex_);
        playing_ = false;
        stopping_ = false;
        state_ = "error";
        return;
    }
    http->SetTimeout(10000);
    http->SetKeepAlive(false);

    if (!http->Open("GET", url)) {
        SetError("Failed to open URL");
        std::lock_guard<std::mutex> lock(mutex_);
        playing_ = false;
        stopping_ = false;
        state_ = "error";
        return;
    }

    std::string content_type = ToLowerCopy(http->GetResponseHeader("Content-Type"));
    Format format = DetectFormat(url, content_type);
    if (format == Format::kUnknown) {
        SetError("Unsupported audio format. Only wav/mp3/ogg are supported");
        http->Close();
        std::lock_guard<std::mutex> lock(mutex_);
        playing_ = false;
        stopping_ = false;
        state_ = "error";
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        active_http_ = http.get();
        current_format_ = FormatToString(format);
        state_ = "playing";
    }

    bool success = false;
    if (format == Format::kOgg) {
        success = DecodeOggOpus();
    } else {
        success = DecodeWithSimpleDecoder(format);
    }

    http->Close();

    bool stopped = stop_requested_.load();
    if (!stopped && success) {
        audio_service_.WaitForPlaybackQueueEmpty();
    }
    if (stopped) {
        audio_service_.ResetDecoder();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    active_http_ = nullptr;
    playing_ = false;
    stopping_ = false;
    if (stopped) {
        state_ = "idle";
    } else if (success) {
        state_ = "idle";
    } else {
        state_ = "error";
    }
}

void UrlAudioPlayer::SetError(const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_error_ = message;
    ESP_LOGE(TAG, "%s", message.c_str());
}

UrlAudioPlayer::Format UrlAudioPlayer::DetectFormat(const std::string& url, const std::string& content_type) const {
    auto ext = GetUrlExtension(url);
    if (ext == "mp3") {
        return Format::kMp3;
    }
    if (ext == "wav") {
        return Format::kWav;
    }
    if (ext == "ogg") {
        return Format::kOgg;
    }

    if (content_type.find("audio/mpeg") != std::string::npos || content_type.find("audio/mp3") != std::string::npos) {
        return Format::kMp3;
    }
    if (content_type.find("audio/wav") != std::string::npos ||
        content_type.find("audio/wave") != std::string::npos ||
        content_type.find("audio/x-wav") != std::string::npos) {
        return Format::kWav;
    }
    if (content_type.find("audio/ogg") != std::string::npos ||
        content_type.find("application/ogg") != std::string::npos) {
        return Format::kOgg;
    }

    return Format::kUnknown;
}

bool UrlAudioPlayer::DecodeWithSimpleDecoder(Format format) {
    RegisterSimpleDecoderDefaultsOnce();

    auto dec_type = ToSimpleDecoderType(format);
    if (dec_type == ESP_AUDIO_SIMPLE_DEC_TYPE_NONE) {
        SetError("Unsupported decoder type");
        return false;
    }

    esp_audio_simple_dec_cfg_t dec_cfg = {
        .dec_type = dec_type,
        .dec_cfg = nullptr,
        .cfg_size = 0,
        .use_frame_dec = false,
    };
    esp_audio_simple_dec_handle_t decoder = nullptr;
    auto open_ret = esp_audio_simple_dec_open(&dec_cfg, &decoder);
    if (open_ret != ESP_AUDIO_ERR_OK || decoder == nullptr) {
        SetError("Failed to open simple decoder");
        return false;
    }

    std::vector<uint8_t> input_buffer(2048);
    std::vector<uint8_t> output_buffer(4096);
    esp_audio_simple_dec_info_t dec_info = {};
    bool have_info = false;

    while (!stop_requested_.load()) {
        Http* http = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            http = active_http_;
        }
        if (http == nullptr) {
            esp_audio_simple_dec_close(decoder);
            SetError("HTTP session closed unexpectedly");
            return false;
        }

        int read_size = http->Read(reinterpret_cast<char*>(input_buffer.data()), input_buffer.size());
        if (read_size < 0) {
            esp_audio_simple_dec_close(decoder);
            SetError("HTTP read failed");
            return false;
        }
        bool eos = (read_size == 0);

        esp_audio_simple_dec_raw_t raw = {
            .buffer = input_buffer.data(),
            .len = static_cast<uint32_t>(read_size),
            .eos = eos,
            .consumed = 0,
            .frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE,
        };

        bool first_process = true;
        while (!stop_requested_.load() && (first_process || raw.len > 0)) {
            first_process = false;
            esp_audio_simple_dec_out_t out_frame = {
                .buffer = output_buffer.data(),
                .len = static_cast<uint32_t>(output_buffer.size()),
                .needed_size = 0,
                .decoded_size = 0,
            };

            auto ret = esp_audio_simple_dec_process(decoder, &raw, &out_frame);
            if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                output_buffer.resize(out_frame.needed_size);
                continue;
            }
            if (ret != ESP_AUDIO_ERR_OK) {
                esp_audio_simple_dec_close(decoder);
                SetError("Audio decode failed");
                return false;
            }

            if (out_frame.decoded_size > 0) {
                auto info_ret = esp_audio_simple_dec_get_info(decoder, &dec_info);
                if (info_ret == ESP_AUDIO_ERR_OK) {
                    have_info = true;
                }
                if (!have_info) {
                    esp_audio_simple_dec_close(decoder);
                    SetError("Decoder info not ready");
                    return false;
                }

                std::vector<int16_t> converted;
                if (!ConvertToOutputPcm(out_frame.buffer, out_frame.decoded_size,
                        dec_info.sample_rate, dec_info.channel, dec_info.bits_per_sample, converted)) {
                    esp_audio_simple_dec_close(decoder);
                    if (last_error_.empty()) {
                        SetError("Failed to convert decoded PCM");
                    }
                    return false;
                }
                if (!converted.empty() && !audio_service_.PushPcmToPlaybackQueue(std::move(converted), true)) {
                    esp_audio_simple_dec_close(decoder);
                    SetError("Failed to queue PCM for playback");
                    return false;
                }
            }

            uint32_t consumed = raw.consumed;
            if (consumed > raw.len) {
                consumed = raw.len;
            }
            raw.len -= consumed;
            raw.buffer += consumed;
            raw.consumed = 0;

            if (raw.len > 0 && consumed == 0 && out_frame.decoded_size == 0) {
                // Decoder consumed nothing and produced nothing; wait for next input chunk.
                raw.len = 0;
            }
        }

        if (eos) {
            break;
        }
    }

    esp_audio_simple_dec_close(decoder);
    return !stop_requested_.load();
}

bool UrlAudioPlayer::DecodeOggOpus() {
    OggDemuxer demuxer;
    demuxer.OnDemuxerFinished([this](const uint8_t* data, int sample_rate, size_t len) {
        if (stop_requested_.load() || data == nullptr || len == 0) {
            return;
        }
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->sample_rate = sample_rate > 0 ? sample_rate : 48000;
        packet->frame_duration = GetOpusPacketDurationMs(data, len, packet->sample_rate);
        packet->payload.assign(data, data + len);
        audio_service_.PushPacketToDecodeQueue(std::move(packet), true);
    });

    std::vector<uint8_t> buffer(2048);
    while (!stop_requested_.load()) {
        Http* http = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            http = active_http_;
        }
        if (http == nullptr) {
            SetError("HTTP session closed unexpectedly");
            return false;
        }

        int read_size = http->Read(reinterpret_cast<char*>(buffer.data()), buffer.size());
        if (read_size < 0) {
            SetError("HTTP read failed");
            return false;
        }
        if (read_size == 0) {
            break;
        }
        demuxer.Process(buffer.data(), static_cast<size_t>(read_size));
    }

    return !stop_requested_.load();
}

bool UrlAudioPlayer::EnsureResampler(int src_rate, int dst_rate, int channels) {
    if (src_rate == dst_rate) {
        return true;
    }

    if (resampler_ != nullptr &&
        resampler_src_rate_ == src_rate &&
        resampler_dst_rate_ == dst_rate &&
        resampler_channels_ == channels) {
        return true;
    }

    if (resampler_ != nullptr) {
        esp_ae_rate_cvt_close(resampler_);
        resampler_ = nullptr;
    }

    esp_ae_rate_cvt_cfg_t cfg = {
        .src_rate = static_cast<uint32_t>(src_rate),
        .dest_rate = static_cast<uint32_t>(dst_rate),
        .channel = static_cast<uint8_t>(channels),
        .bits_per_sample = ESP_AUDIO_BIT16,
        .complexity = 2,
        .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,
    };
    esp_ae_rate_cvt_open(&cfg, &resampler_);
    if (resampler_ == nullptr) {
        SetError("Failed to create audio resampler");
        return false;
    }

    resampler_src_rate_ = src_rate;
    resampler_dst_rate_ = dst_rate;
    resampler_channels_ = channels;
    return true;
}

bool UrlAudioPlayer::ConvertToOutputPcm(const uint8_t* buffer, size_t buffer_size,
    int sample_rate, int channels, int bits_per_sample,
    std::vector<int16_t>& output) {
    output.clear();
    if (buffer == nullptr || buffer_size == 0) {
        return true;
    }
    if (bits_per_sample != 16) {
        SetError("Only 16-bit PCM is supported");
        return false;
    }
    if (channels <= 0 || channels > 2) {
        SetError("Only mono/stereo audio is supported");
        return false;
    }

    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        SetError("Audio codec unavailable");
        return false;
    }

    std::vector<int16_t> pcm(buffer_size / sizeof(int16_t));
    std::memcpy(pcm.data(), buffer, buffer_size);

    int target_channels = codec->output_channels();
    if (target_channels <= 0 || target_channels > 2) {
        target_channels = 1;
    }

    std::vector<int16_t> channel_converted;
    if (channels == target_channels) {
        channel_converted = std::move(pcm);
    } else if (channels == 2 && target_channels == 1) {
        size_t frames = pcm.size() / 2;
        channel_converted.resize(frames);
        for (size_t i = 0; i < frames; ++i) {
            int left = pcm[i * 2];
            int right = pcm[i * 2 + 1];
            channel_converted[i] = static_cast<int16_t>((left + right) / 2);
        }
    } else if (channels == 1 && target_channels == 2) {
        channel_converted.resize(pcm.size() * 2);
        for (size_t i = 0; i < pcm.size(); ++i) {
            channel_converted[i * 2] = pcm[i];
            channel_converted[i * 2 + 1] = pcm[i];
        }
    } else {
        SetError("Unsupported channel conversion");
        return false;
    }

    int target_rate = codec->output_sample_rate();
    if (target_rate <= 0 || sample_rate == target_rate) {
        output = std::move(channel_converted);
        return true;
    }

    if (!EnsureResampler(sample_rate, target_rate, target_channels)) {
        return false;
    }

    uint32_t input_samples = static_cast<uint32_t>(channel_converted.size() / target_channels);
    uint32_t output_samples = 0;
    esp_ae_rate_cvt_get_max_out_sample_num(resampler_, input_samples, &output_samples);
    if (output_samples == 0) {
        output.clear();
        return true;
    }

    std::vector<int16_t> resampled(output_samples * target_channels);
    uint32_t actual_output = output_samples;
    esp_ae_rate_cvt_process(resampler_,
        reinterpret_cast<esp_ae_sample_t>(channel_converted.data()), input_samples,
        reinterpret_cast<esp_ae_sample_t>(resampled.data()), &actual_output);
    resampled.resize(actual_output * target_channels);
    output = std::move(resampled);
    return true;
}
