#ifndef URL_AUDIO_PLAYER_H
#define URL_AUDIO_PLAYER_H

#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_ae_rate_cvt.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

class AudioService;
class Http;

class UrlAudioPlayer {
public:
    enum class Format {
        kUnknown,
        kMp3,
        kWav,
        kOgg,
    };

    explicit UrlAudioPlayer(AudioService& audio_service);
    ~UrlAudioPlayer();

    bool Play(const std::string& url, const std::string& track_name = "");
    bool Stop();
    bool IsPlaying() const;
    cJSON* GetStatusJson() const;

private:
    AudioService& audio_service_;
    mutable std::mutex mutex_;
    std::atomic<bool> stop_requested_{false};
    TaskHandle_t task_handle_ = nullptr;
    Http* active_http_ = nullptr;
    bool playing_ = false;
    bool stopping_ = false;
    std::string current_url_;
    std::string current_track_name_;
    std::string current_format_ = "unknown";
    std::string state_ = "idle";
    std::string last_error_;

    esp_ae_rate_cvt_handle_t resampler_ = nullptr;
    int resampler_src_rate_ = 0;
    int resampler_dst_rate_ = 0;
    int resampler_channels_ = 0;

    static void PlaybackTaskEntry(void* arg);
    void PlaybackTask();

    void SetError(const std::string& message);
    Format DetectFormat(const std::string& url, const std::string& content_type) const;
    bool DecodeWithSimpleDecoder(Format format, const uint8_t* initial_data, size_t initial_size);
    bool DecodeOggOpus(const uint8_t* initial_data, size_t initial_size);

    bool EnsureResampler(int src_rate, int dst_rate, int channels);
    bool ConvertToOutputPcm(const uint8_t* buffer, size_t buffer_size,
        int sample_rate, int channels, int bits_per_sample,
        std::vector<int16_t>& output);
    void ApplyPlaybackAttenuation(std::vector<int16_t>& pcm) const;
};

#endif  // URL_AUDIO_PLAYER_H
