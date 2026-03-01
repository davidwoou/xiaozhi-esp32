#ifndef ALARM_SERVICE_H
#define ALARM_SERVICE_H

#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <vector>

#include <cJSON.h>

enum class AlarmRepeatMode {
    kOnce,
    kDaily,
};

struct AlarmInfo {
    int id = 0;
    bool enabled = true;
    AlarmRepeatMode repeat = AlarmRepeatMode::kOnce;
    int hour = 0;
    int minute = 0;
    int year = 0;
    int month = 0;
    int day = 0;
    int last_triggered_date = 0;
    time_t scheduled_epoch = 0;
    std::string label;
};

struct CountdownInfo {
    bool active = false;
    bool ringing = false;
    int duration_seconds = 0;
    int remaining_seconds = 0;
    int64_t started_at_us = 0;
    int64_t target_at_us = 0;
    std::string label;
};

enum class AlarmEventSource {
    kNone,
    kAlarm,
    kCountdown,
};

struct AlarmTickResult {
    bool triggered = false;
    bool play_sound = false;
    bool auto_stopped = false;
    AlarmEventSource source = AlarmEventSource::kNone;
    std::optional<AlarmInfo> alarm;
    std::optional<CountdownInfo> countdown;
};

class AlarmService {
public:
    void Initialize();

    int SetAlarm(int hour, int minute, const std::string& repeat, const std::string& label,
        int id = 0, int year = 0, int month = 0, int day = 0);
    bool DeleteAlarm(int id);
    bool SetAlarmEnabled(int id, bool enabled);
    int SnoozeRingingAlarm(int minutes);
    void SetCountdown(int seconds, const std::string& label);
    bool CancelCountdown();
    bool StopRinging();
    bool IsRinging() const { return ring_source_ != AlarmEventSource::kNone; }
    bool HasActiveCountdown() const;
    bool IsCountdownRinging() const;
    std::optional<AlarmInfo> GetNextAlarm() const;
    std::optional<AlarmInfo> GetRingingAlarm() const;
    std::optional<CountdownInfo> GetCountdown() const;

    cJSON* GetClockJson() const;
    cJSON* GetStatusJson() const;
    cJSON* GetCountdownJson() const;
    AlarmTickResult Tick();

private:
    static constexpr int kStorageVersion = 1;
    static constexpr time_t kRingIntervalSeconds = 4;
    static constexpr time_t kMaxRingDurationSeconds = 60;
    static constexpr int64_t kMicrosPerSecond = 1000000;

    std::vector<AlarmInfo> alarms_;
    std::optional<CountdownInfo> countdown_;
    int next_alarm_id_ = 1;
    int ringing_alarm_id_ = 0;
    AlarmEventSource ring_source_ = AlarmEventSource::kNone;
    int64_t ring_started_at_us_ = 0;
    int64_t last_ring_at_us_ = 0;

    void Load();
    void Save() const;

    AlarmInfo* FindAlarm(int id);
    const AlarmInfo* FindAlarm(int id) const;
    const AlarmInfo* FindRingingAlarm() const;
    size_t GetEnabledAlarmCount() const;

    static bool GetValidLocalTime(time_t now, struct tm* out_tm);
    static int64_t GetMonotonicTimeUs();
    static int GetDateKey(const struct tm& tm);
    static time_t BuildLocalEpoch(int year, int month, int day, int hour, int minute);
    static time_t BuildNextOccurrenceEpoch(int hour, int minute, time_t now);
    static std::string FormatLocalTime(time_t timestamp, const char* format);
    static const char* RepeatModeToString(AlarmRepeatMode repeat);
    static AlarmRepeatMode ParseRepeatMode(const std::string& repeat);

    cJSON* AlarmToJson(const AlarmInfo& alarm) const;
    cJSON* CountdownToJson(const CountdownInfo& countdown) const;
    std::optional<CountdownInfo> BuildCountdownSnapshot() const;
    void ResetRingingState();
};

#endif  // ALARM_SERVICE_H
