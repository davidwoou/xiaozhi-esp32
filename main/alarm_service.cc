#include "alarm_service.h"

#include <algorithm>
#include <stdexcept>

#include <esp_log.h>
#include <esp_timer.h>

#include "settings.h"

namespace {

const char* TAG = "AlarmService";

}  // namespace

void AlarmService::Initialize() {
    Load();
}

int AlarmService::SetAlarm(int hour, int minute, const std::string& repeat, const std::string& label,
    int id, int year, int month, int day) {
    if (hour < 0 || hour > 23) {
        throw std::runtime_error("Hour must be between 0 and 23");
    }
    if (minute < 0 || minute > 59) {
        throw std::runtime_error("Minute must be between 0 and 59");
    }

    auto repeat_mode = ParseRepeatMode(repeat);
    time_t now = time(nullptr);

    AlarmInfo alarm;
    if (id > 0) {
        auto* existing = FindAlarm(id);
        if (existing == nullptr) {
            throw std::runtime_error("Alarm not found: " + std::to_string(id));
        }
        alarm = *existing;
        alarm.id = id;
    } else {
        alarm.id = next_alarm_id_++;
    }

    alarm.enabled = true;
    alarm.repeat = repeat_mode;
    alarm.hour = hour;
    alarm.minute = minute;
    alarm.label = label;
    alarm.last_triggered_date = 0;
    alarm.year = 0;
    alarm.month = 0;
    alarm.day = 0;
    alarm.scheduled_epoch = 0;

    if (repeat_mode == AlarmRepeatMode::kDaily) {
        if (year != 0 || month != 0 || day != 0) {
            throw std::runtime_error("Daily alarms do not accept year/month/day");
        }
    } else {
        if (year == 0 && month == 0 && day == 0) {
            if (!GetValidLocalTime(now, nullptr)) {
                throw std::runtime_error("System time is not set");
            }
            alarm.scheduled_epoch = BuildNextOccurrenceEpoch(hour, minute, now);
        } else {
            if (year < 2025 || month < 1 || month > 12 || day < 1 || day > 31) {
                throw std::runtime_error("Invalid year/month/day for one-time alarm");
            }
            alarm.scheduled_epoch = BuildLocalEpoch(year, month, day, hour, minute);
            if (alarm.scheduled_epoch <= 0) {
                throw std::runtime_error("Failed to build alarm time");
            }
            if (GetValidLocalTime(now, nullptr) && alarm.scheduled_epoch <= now) {
                throw std::runtime_error("Alarm time must be in the future");
            }
        }

        struct tm scheduled_tm {};
        if (!GetValidLocalTime(alarm.scheduled_epoch, &scheduled_tm)) {
            throw std::runtime_error("Failed to convert alarm time");
        }
        alarm.year = scheduled_tm.tm_year + 1900;
        alarm.month = scheduled_tm.tm_mon + 1;
        alarm.day = scheduled_tm.tm_mday;
    }

    auto* existing = FindAlarm(alarm.id);
    if (existing != nullptr) {
        *existing = alarm;
    } else {
        alarms_.push_back(alarm);
    }

    std::sort(alarms_.begin(), alarms_.end(), [](const AlarmInfo& lhs, const AlarmInfo& rhs) {
        return lhs.id < rhs.id;
    });

    if (ring_source_ == AlarmEventSource::kAlarm && ringing_alarm_id_ == alarm.id) {
        ResetRingingState();
    }

    Save();
    ESP_LOGI(TAG, "Saved alarm id=%d repeat=%s %02d:%02d", alarm.id, RepeatModeToString(alarm.repeat), alarm.hour, alarm.minute);
    return alarm.id;
}

void AlarmService::SetCountdown(int seconds, const std::string& label) {
    if (seconds <= 0) {
        throw std::runtime_error("Countdown seconds must be positive");
    }

    CountdownInfo countdown;
    countdown.active = true;
    countdown.duration_seconds = seconds;
    countdown.remaining_seconds = seconds;
    countdown.started_at_us = GetMonotonicTimeUs();
    countdown.target_at_us = countdown.started_at_us + static_cast<int64_t>(seconds) * kMicrosPerSecond;
    countdown.label = label;
    countdown_ = countdown;

    if (ring_source_ == AlarmEventSource::kCountdown) {
        ResetRingingState();
    }
}

bool AlarmService::CancelCountdown() {
    bool had_countdown = countdown_.has_value();
    countdown_.reset();
    if (ring_source_ == AlarmEventSource::kCountdown) {
        ResetRingingState();
        return true;
    }
    return had_countdown;
}

bool AlarmService::DeleteAlarm(int id) {
    auto it = std::remove_if(alarms_.begin(), alarms_.end(), [id](const AlarmInfo& alarm) {
        return alarm.id == id;
    });
    if (it == alarms_.end()) {
        return false;
    }
    alarms_.erase(it, alarms_.end());
    if (ringing_alarm_id_ == id) {
        ResetRingingState();
    }
    Save();
    return true;
}

bool AlarmService::SetAlarmEnabled(int id, bool enabled) {
    auto* alarm = FindAlarm(id);
    if (alarm == nullptr) {
        return false;
    }

    alarm->enabled = enabled;
    if (!enabled && ringing_alarm_id_ == id) {
        ResetRingingState();
    }
    Save();
    return true;
}

int AlarmService::SnoozeRingingAlarm(int minutes) {
    if (minutes <= 0) {
        throw std::runtime_error("Snooze minutes must be positive");
    }

    auto* ringing_alarm = FindAlarm(ringing_alarm_id_);
    if (ringing_alarm == nullptr) {
        throw std::runtime_error("No ringing alarm to snooze");
    }

    time_t now = time(nullptr);
    if (!GetValidLocalTime(now, nullptr)) {
        throw std::runtime_error("System time is not set");
    }

    time_t scheduled_epoch = now + minutes * 60;
    struct tm scheduled_tm {};
    if (!GetValidLocalTime(scheduled_epoch, &scheduled_tm)) {
        throw std::runtime_error("Failed to build snooze time");
    }

    AlarmInfo snooze_alarm;
    snooze_alarm.id = next_alarm_id_++;
    snooze_alarm.enabled = true;
    snooze_alarm.repeat = AlarmRepeatMode::kOnce;
    snooze_alarm.hour = scheduled_tm.tm_hour;
    snooze_alarm.minute = scheduled_tm.tm_min;
    snooze_alarm.year = scheduled_tm.tm_year + 1900;
    snooze_alarm.month = scheduled_tm.tm_mon + 1;
    snooze_alarm.day = scheduled_tm.tm_mday;
    snooze_alarm.scheduled_epoch = scheduled_epoch;
    snooze_alarm.label = ringing_alarm->label;

    alarms_.push_back(snooze_alarm);
    std::sort(alarms_.begin(), alarms_.end(), [](const AlarmInfo& lhs, const AlarmInfo& rhs) {
        return lhs.id < rhs.id;
    });

    ResetRingingState();
    Save();
    ESP_LOGI(TAG, "Snoozed alarm id=%d by %d minutes -> new id=%d", ringing_alarm->id, minutes, snooze_alarm.id);
    return snooze_alarm.id;
}

bool AlarmService::StopRinging() {
    if (!IsRinging()) {
        return false;
    }
    if (ring_source_ == AlarmEventSource::kCountdown) {
        countdown_.reset();
    }
    ResetRingingState();
    return true;
}

bool AlarmService::HasActiveCountdown() const {
    return countdown_.has_value() && countdown_->active;
}

bool AlarmService::IsCountdownRinging() const {
    return ring_source_ == AlarmEventSource::kCountdown;
}

std::optional<AlarmInfo> AlarmService::GetNextAlarm() const {
    std::optional<AlarmInfo> next_alarm;
    time_t next_epoch = 0;
    time_t now = time(nullptr);
    bool time_valid = GetValidLocalTime(now, nullptr);

    for (const auto& alarm : alarms_) {
        if (!alarm.enabled) {
            continue;
        }

        time_t candidate_epoch = 0;
        if (alarm.repeat == AlarmRepeatMode::kDaily) {
            if (!time_valid) {
                continue;
            }
            candidate_epoch = BuildNextOccurrenceEpoch(alarm.hour, alarm.minute, now);
        } else {
            candidate_epoch = alarm.scheduled_epoch;
            if (candidate_epoch <= 0) {
                continue;
            }
        }

        if (!next_alarm.has_value() || candidate_epoch < next_epoch) {
            next_epoch = candidate_epoch;
            next_alarm = alarm;
        }
    }

    return next_alarm;
}

std::optional<AlarmInfo> AlarmService::GetRingingAlarm() const {
    if (auto* alarm = FindRingingAlarm(); alarm != nullptr) {
        return *alarm;
    }
    return std::nullopt;
}

std::optional<CountdownInfo> AlarmService::GetCountdown() const {
    return BuildCountdownSnapshot();
}

cJSON* AlarmService::GetClockJson() const {
    auto root = cJSON_CreateObject();
    time_t now = time(nullptr);
    struct tm local_tm {};
    bool time_valid = GetValidLocalTime(now, &local_tm);
    cJSON_AddBoolToObject(root, "time_valid", time_valid);
    if (time_valid) {
        cJSON_AddNumberToObject(root, "unix_time", static_cast<double>(now));
        cJSON_AddStringToObject(root, "local_time", FormatLocalTime(now, "%Y-%m-%d %H:%M:%S").c_str());
        cJSON_AddStringToObject(root, "date", FormatLocalTime(now, "%Y-%m-%d").c_str());
        cJSON_AddStringToObject(root, "time", FormatLocalTime(now, "%H:%M").c_str());
    }
    return root;
}

cJSON* AlarmService::GetStatusJson() const {
    auto root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ringing", ring_source_ == AlarmEventSource::kAlarm);
    cJSON_AddNumberToObject(root, "count", static_cast<int>(GetEnabledAlarmCount()));
    if (auto* ringing_alarm = FindRingingAlarm(); ringing_alarm != nullptr) {
        cJSON_AddNumberToObject(root, "ringing_alarm_id", ringing_alarm->id);
        cJSON_AddItemToObject(root, "ringing_alarm", AlarmToJson(*ringing_alarm));
    }
    if (auto next_alarm = GetNextAlarm(); next_alarm.has_value()) {
        cJSON_AddItemToObject(root, "next_alarm", AlarmToJson(*next_alarm));
    }

    auto alarms = cJSON_CreateArray();
    for (const auto& alarm : alarms_) {
        if (alarm.enabled) {
            cJSON_AddItemToArray(alarms, AlarmToJson(alarm));
        }
    }
    cJSON_AddItemToObject(root, "alarms", alarms);
    return root;
}

cJSON* AlarmService::GetCountdownJson() const {
    if (auto countdown = BuildCountdownSnapshot(); countdown.has_value()) {
        return CountdownToJson(*countdown);
    }
    auto root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "active", false);
    cJSON_AddBoolToObject(root, "ringing", false);
    return root;
}

AlarmTickResult AlarmService::Tick() {
    AlarmTickResult result;
    int64_t monotonic_now_us = GetMonotonicTimeUs();
    time_t now = time(nullptr);

    if (IsRinging()) {
        if ((monotonic_now_us - ring_started_at_us_) >= kMaxRingDurationSeconds * kMicrosPerSecond) {
            result.source = ring_source_;
            if (ring_source_ == AlarmEventSource::kCountdown) {
                countdown_.reset();
            }
            ResetRingingState();
            result.auto_stopped = true;
            return result;
        }

        if ((monotonic_now_us - last_ring_at_us_) >= kRingIntervalSeconds * kMicrosPerSecond) {
            last_ring_at_us_ = monotonic_now_us;
            result.play_sound = true;
            result.source = ring_source_;
            if (auto* alarm = FindRingingAlarm(); alarm != nullptr) {
                result.alarm = *alarm;
            } else if (auto countdown = BuildCountdownSnapshot(); countdown.has_value()) {
                result.countdown = *countdown;
            }
        }
        return result;
    }

    if (countdown_.has_value() && countdown_->active && monotonic_now_us >= countdown_->target_at_us) {
        countdown_->active = false;
        countdown_->remaining_seconds = 0;
        ring_source_ = AlarmEventSource::kCountdown;
        ring_started_at_us_ = monotonic_now_us;
        last_ring_at_us_ = monotonic_now_us;
        result.triggered = true;
        result.play_sound = true;
        result.source = AlarmEventSource::kCountdown;
        result.countdown = BuildCountdownSnapshot();
        return result;
    }

    struct tm local_tm {};
    if (!GetValidLocalTime(now, &local_tm)) {
        return result;
    }

    int today = GetDateKey(local_tm);
    bool dirty = false;

    for (auto& alarm : alarms_) {
        if (!alarm.enabled) {
            continue;
        }

        bool due = false;
        if (alarm.repeat == AlarmRepeatMode::kDaily) {
            due = local_tm.tm_hour == alarm.hour &&
                local_tm.tm_min == alarm.minute &&
                alarm.last_triggered_date != today;
        } else if (alarm.scheduled_epoch > 0) {
            due = now >= alarm.scheduled_epoch;
        }

        if (!due) {
            continue;
        }

        alarm.last_triggered_date = today;
        if (alarm.repeat == AlarmRepeatMode::kOnce) {
            alarm.enabled = false;
        }

        ringing_alarm_id_ = alarm.id;
        ring_source_ = AlarmEventSource::kAlarm;
        ring_started_at_us_ = monotonic_now_us;
        last_ring_at_us_ = monotonic_now_us;
        result.triggered = true;
        result.play_sound = true;
        result.source = AlarmEventSource::kAlarm;
        result.alarm = alarm;
        dirty = true;
        break;
    }

    if (dirty) {
        Save();
    }

    return result;
}

void AlarmService::Load() {
    Settings settings("alarm");
    auto raw = settings.GetString("data");
    if (raw.empty()) {
        alarms_.clear();
        next_alarm_id_ = 1;
        ResetRingingState();
        return;
    }

    auto* root = cJSON_Parse(raw.c_str());
    if (root == nullptr) {
        ESP_LOGW(TAG, "Failed to parse stored alarm data");
        alarms_.clear();
        next_alarm_id_ = 1;
        ResetRingingState();
        return;
    }

    auto* version = cJSON_GetObjectItem(root, "version");
    auto* next_id = cJSON_GetObjectItem(root, "next_id");
    auto* alarms = cJSON_GetObjectItem(root, "alarms");
    if (!cJSON_IsNumber(version) || version->valueint != kStorageVersion || !cJSON_IsArray(alarms)) {
        ESP_LOGW(TAG, "Unsupported stored alarm data");
        cJSON_Delete(root);
        alarms_.clear();
        next_alarm_id_ = 1;
        ResetRingingState();
        return;
    }

    alarms_.clear();
    next_alarm_id_ = cJSON_IsNumber(next_id) ? std::max(1, next_id->valueint) : 1;
    ResetRingingState();

    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, alarms) {
        auto* id = cJSON_GetObjectItem(item, "id");
        auto* enabled = cJSON_GetObjectItem(item, "enabled");
        auto* repeat = cJSON_GetObjectItem(item, "repeat");
        auto* hour = cJSON_GetObjectItem(item, "hour");
        auto* minute = cJSON_GetObjectItem(item, "minute");
        if (!cJSON_IsNumber(id) || !cJSON_IsString(repeat) || !cJSON_IsNumber(hour) || !cJSON_IsNumber(minute)) {
            continue;
        }

        AlarmInfo alarm;
        alarm.id = id->valueint;
        alarm.enabled = enabled == nullptr || cJSON_IsTrue(enabled);
        try {
            alarm.repeat = ParseRepeatMode(repeat->valuestring);
        } catch (const std::exception&) {
            ESP_LOGW(TAG, "Skip invalid alarm repeat mode for id=%d", alarm.id);
            continue;
        }
        alarm.hour = hour->valueint;
        alarm.minute = minute->valueint;

        auto* label = cJSON_GetObjectItem(item, "label");
        auto* year = cJSON_GetObjectItem(item, "year");
        auto* month = cJSON_GetObjectItem(item, "month");
        auto* day = cJSON_GetObjectItem(item, "day");
        auto* last_triggered_date = cJSON_GetObjectItem(item, "last_triggered_date");
        auto* scheduled_epoch = cJSON_GetObjectItem(item, "scheduled_epoch");

        if (cJSON_IsString(label)) {
            alarm.label = label->valuestring;
        }
        if (cJSON_IsNumber(year)) {
            alarm.year = year->valueint;
        }
        if (cJSON_IsNumber(month)) {
            alarm.month = month->valueint;
        }
        if (cJSON_IsNumber(day)) {
            alarm.day = day->valueint;
        }
        if (cJSON_IsNumber(last_triggered_date)) {
            alarm.last_triggered_date = last_triggered_date->valueint;
        }
        if (cJSON_IsNumber(scheduled_epoch)) {
            alarm.scheduled_epoch = static_cast<time_t>(scheduled_epoch->valuedouble);
        }

        alarms_.push_back(alarm);
        next_alarm_id_ = std::max(next_alarm_id_, alarm.id + 1);
    }

    cJSON_Delete(root);
}

void AlarmService::Save() const {
    Settings settings("alarm", true);

    auto* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", kStorageVersion);
    cJSON_AddNumberToObject(root, "next_id", next_alarm_id_);

    auto* alarms = cJSON_CreateArray();
    for (const auto& alarm : alarms_) {
        auto* item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "id", alarm.id);
        cJSON_AddBoolToObject(item, "enabled", alarm.enabled);
        cJSON_AddStringToObject(item, "repeat", RepeatModeToString(alarm.repeat));
        cJSON_AddNumberToObject(item, "hour", alarm.hour);
        cJSON_AddNumberToObject(item, "minute", alarm.minute);
        cJSON_AddStringToObject(item, "label", alarm.label.c_str());
        cJSON_AddNumberToObject(item, "year", alarm.year);
        cJSON_AddNumberToObject(item, "month", alarm.month);
        cJSON_AddNumberToObject(item, "day", alarm.day);
        cJSON_AddNumberToObject(item, "last_triggered_date", alarm.last_triggered_date);
        cJSON_AddNumberToObject(item, "scheduled_epoch", static_cast<double>(alarm.scheduled_epoch));
        cJSON_AddItemToArray(alarms, item);
    }
    cJSON_AddItemToObject(root, "alarms", alarms);

    char* raw = cJSON_PrintUnformatted(root);
    settings.SetString("data", raw);
    cJSON_free(raw);
    cJSON_Delete(root);
}

AlarmInfo* AlarmService::FindAlarm(int id) {
    auto it = std::find_if(alarms_.begin(), alarms_.end(), [id](const AlarmInfo& alarm) {
        return alarm.id == id;
    });
    return it == alarms_.end() ? nullptr : &(*it);
}

const AlarmInfo* AlarmService::FindAlarm(int id) const {
    auto it = std::find_if(alarms_.begin(), alarms_.end(), [id](const AlarmInfo& alarm) {
        return alarm.id == id;
    });
    return it == alarms_.end() ? nullptr : &(*it);
}

const AlarmInfo* AlarmService::FindRingingAlarm() const {
    if (ring_source_ != AlarmEventSource::kAlarm) {
        return nullptr;
    }
    return FindAlarm(ringing_alarm_id_);
}

size_t AlarmService::GetEnabledAlarmCount() const {
    return static_cast<size_t>(std::count_if(alarms_.begin(), alarms_.end(), [](const AlarmInfo& alarm) {
        return alarm.enabled;
    }));
}

bool AlarmService::GetValidLocalTime(time_t now, struct tm* out_tm) {
    struct tm local_tm {};
    if (localtime_r(&now, &local_tm) == nullptr) {
        return false;
    }
    if (local_tm.tm_year < (2025 - 1900)) {
        return false;
    }
    if (out_tm != nullptr) {
        *out_tm = local_tm;
    }
    return true;
}

int64_t AlarmService::GetMonotonicTimeUs() {
    return esp_timer_get_time();
}

int AlarmService::GetDateKey(const struct tm& tm) {
    return (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
}

time_t AlarmService::BuildLocalEpoch(int year, int month, int day, int hour, int minute) {
    struct tm scheduled_tm {};
    scheduled_tm.tm_year = year - 1900;
    scheduled_tm.tm_mon = month - 1;
    scheduled_tm.tm_mday = day;
    scheduled_tm.tm_hour = hour;
    scheduled_tm.tm_min = minute;
    scheduled_tm.tm_sec = 0;
    scheduled_tm.tm_isdst = -1;
    return mktime(&scheduled_tm);
}

time_t AlarmService::BuildNextOccurrenceEpoch(int hour, int minute, time_t now) {
    struct tm local_tm {};
    localtime_r(&now, &local_tm);
    local_tm.tm_hour = hour;
    local_tm.tm_min = minute;
    local_tm.tm_sec = 0;
    local_tm.tm_isdst = -1;
    time_t scheduled = mktime(&local_tm);
    if (scheduled <= now) {
        local_tm.tm_mday += 1;
        scheduled = mktime(&local_tm);
    }
    return scheduled;
}

std::string AlarmService::FormatLocalTime(time_t timestamp, const char* format) {
    struct tm local_tm {};
    if (localtime_r(&timestamp, &local_tm) == nullptr) {
        return "";
    }

    char buffer[32];
    if (strftime(buffer, sizeof(buffer), format, &local_tm) == 0) {
        return "";
    }
    return buffer;
}

const char* AlarmService::RepeatModeToString(AlarmRepeatMode repeat) {
    return repeat == AlarmRepeatMode::kDaily ? "daily" : "once";
}

AlarmRepeatMode AlarmService::ParseRepeatMode(const std::string& repeat) {
    if (repeat == "daily") {
        return AlarmRepeatMode::kDaily;
    }
    if (repeat == "once") {
        return AlarmRepeatMode::kOnce;
    }
    throw std::runtime_error("Unsupported repeat mode: " + repeat);
}

cJSON* AlarmService::AlarmToJson(const AlarmInfo& alarm) const {
    auto* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", alarm.id);
    cJSON_AddBoolToObject(root, "enabled", alarm.enabled);
    cJSON_AddStringToObject(root, "repeat", RepeatModeToString(alarm.repeat));
    cJSON_AddNumberToObject(root, "hour", alarm.hour);
    cJSON_AddNumberToObject(root, "minute", alarm.minute);
    cJSON_AddStringToObject(root, "label", alarm.label.c_str());

    if (alarm.repeat == AlarmRepeatMode::kDaily) {
        time_t now = time(nullptr);
        if (GetValidLocalTime(now, nullptr)) {
            auto next_trigger = BuildNextOccurrenceEpoch(alarm.hour, alarm.minute, now);
            if (GetValidLocalTime(next_trigger, nullptr)) {
                cJSON_AddStringToObject(root, "next_trigger_local", FormatLocalTime(next_trigger, "%Y-%m-%d %H:%M").c_str());
            }
        }
    } else if (alarm.scheduled_epoch > 0) {
        cJSON_AddNumberToObject(root, "year", alarm.year);
        cJSON_AddNumberToObject(root, "month", alarm.month);
        cJSON_AddNumberToObject(root, "day", alarm.day);
        cJSON_AddNumberToObject(root, "scheduled_epoch", static_cast<double>(alarm.scheduled_epoch));
        cJSON_AddStringToObject(root, "scheduled_local", FormatLocalTime(alarm.scheduled_epoch, "%Y-%m-%d %H:%M").c_str());
    }

    return root;
}

cJSON* AlarmService::CountdownToJson(const CountdownInfo& countdown) const {
    auto* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "active", countdown.active);
    cJSON_AddBoolToObject(root, "ringing", countdown.ringing);
    cJSON_AddNumberToObject(root, "duration_seconds", countdown.duration_seconds);
    cJSON_AddNumberToObject(root, "remaining_seconds", countdown.remaining_seconds);
    cJSON_AddStringToObject(root, "label", countdown.label.c_str());
    return root;
}

std::optional<CountdownInfo> AlarmService::BuildCountdownSnapshot() const {
    if (!countdown_.has_value()) {
        return std::nullopt;
    }

    auto countdown = *countdown_;
    countdown.ringing = ring_source_ == AlarmEventSource::kCountdown;
    if (countdown.ringing) {
        countdown.active = false;
        countdown.remaining_seconds = 0;
        return countdown;
    }

    if (countdown.active) {
        int64_t remaining_us = std::max<int64_t>(0, countdown.target_at_us - GetMonotonicTimeUs());
        countdown.remaining_seconds = static_cast<int>((remaining_us + kMicrosPerSecond - 1) / kMicrosPerSecond);
    }

    return countdown;
}

void AlarmService::ResetRingingState() {
    ring_source_ = AlarmEventSource::kNone;
    ringing_alarm_id_ = 0;
    ring_started_at_us_ = 0;
    last_ring_at_us_ = 0;
}
