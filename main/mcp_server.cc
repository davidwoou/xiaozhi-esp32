/*
 * MCP Server Implementation
 * Reference: https://modelcontextprotocol.io/specification/2024-11-05
 */

#include "mcp_server.h"
#include <esp_log.h>
#include <esp_app_desc.h>
#include <esp_random.h>
#include <algorithm>
#include <cctype>
#include <climits>
#include <cstddef>
#include <cstring>
#include <esp_pthread.h>

#include "application.h"
#include "display.h"
#include "oled_display.h"
#include "board.h"
#include "settings.h"
#include "lvgl_theme.h"
#include "lvgl_display.h"

#define TAG "MCP"

namespace {

struct CustomMusicItem {
    std::string id;
    std::string music_name;
    std::string author_name;
    std::string url;
};

std::vector<CustomMusicItem> g_custom_music_catalog;
std::optional<CustomMusicItem> g_current_custom_music;

struct MusicSessionState {
    std::vector<CustomMusicItem> playlist;
    std::vector<size_t> history;
    int current_index = -1;
    bool mode_active = false;
    bool paused = false;
    bool expected_stop = false;
    bool last_playing = false;
    std::string last_error;
};

MusicSessionState g_music_session;

int SafeSizeToInt(size_t value) {
    if (value > static_cast<size_t>(INT_MAX)) {
        return INT_MAX;
    }
    return static_cast<int>(value);
}

std::string TrimCopy(const std::string& value) {
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) {
        ++start;
    }

    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return value.substr(start, end - start);
}

std::string ToLowerAsciiCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool ContainsIgnoreCase(const std::string& text, const std::string& keyword) {
    if (keyword.empty()) {
        return true;
    }
    auto lower_text = ToLowerAsciiCopy(text);
    auto lower_keyword = ToLowerAsciiCopy(keyword);
    return lower_text.find(lower_keyword) != std::string::npos;
}

std::string JsonValueToString(const cJSON* value) {
    if (cJSON_IsString(value) && value->valuestring != nullptr) {
        return value->valuestring;
    }
    if (cJSON_IsNumber(value)) {
        return std::to_string(value->valueint);
    }
    return "";
}

std::string GetJsonStringByKeys(const cJSON* object, const std::initializer_list<const char*>& keys) {
    if (!cJSON_IsObject(object)) {
        return "";
    }
    for (auto* key : keys) {
        auto* value = cJSON_GetObjectItem(object, key);
        auto string_value = TrimCopy(JsonValueToString(value));
        if (!string_value.empty()) {
            return string_value;
        }
    }
    return "";
}

cJSON* BuildCustomMusicJson(const CustomMusicItem& item) {
    auto* json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "id", item.id.c_str());
    cJSON_AddStringToObject(json, "music_name", item.music_name.c_str());
    cJSON_AddStringToObject(json, "author_name", item.author_name.c_str());
    cJSON_AddStringToObject(json, "url", item.url.c_str());
    return json;
}

void UpsertCustomMusicItem(const CustomMusicItem& item) {
    auto iter = std::find_if(g_custom_music_catalog.begin(), g_custom_music_catalog.end(),
        [&item](const CustomMusicItem& existing) {
            return existing.id == item.id;
        });
    if (iter == g_custom_music_catalog.end()) {
        g_custom_music_catalog.push_back(item);
    } else {
        *iter = item;
    }
}

const CustomMusicItem* FindCustomMusicById(const std::string& id) {
    auto iter = std::find_if(g_custom_music_catalog.begin(), g_custom_music_catalog.end(),
        [&id](const CustomMusicItem& item) {
            return item.id == id;
        });
    if (iter == g_custom_music_catalog.end()) {
        return nullptr;
    }
    return &(*iter);
}

std::vector<std::string> ParseIdList(const std::string& raw_id_list) {
    std::vector<std::string> ids;
    auto trimmed = TrimCopy(raw_id_list);
    if (trimmed.empty()) {
        return ids;
    }

    auto add_unique = [&ids](const std::string& id_value) {
        auto id = TrimCopy(id_value);
        if (id.empty()) {
            return;
        }
        if (std::find(ids.begin(), ids.end(), id) == ids.end()) {
            ids.push_back(id);
        }
    };

    if (!trimmed.empty() && trimmed.front() == '[') {
        auto* array_json = cJSON_Parse(trimmed.c_str());
        if (cJSON_IsArray(array_json)) {
            cJSON* item = nullptr;
            cJSON_ArrayForEach(item, array_json) {
                add_unique(JsonValueToString(item));
            }
            cJSON_Delete(array_json);
            return ids;
        }
        cJSON_Delete(array_json);
    }

    std::string token;
    for (char ch : trimmed) {
        if (ch == ',' || ch == ';' || std::isspace(static_cast<unsigned char>(ch))) {
            add_unique(token);
            token.clear();
        } else {
            token.push_back(ch);
        }
    }
    add_unique(token);
    return ids;
}

bool IsPlayableHttpUrl(const std::string& url) {
    return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
}

bool StartPlaylistTrackByIndex(size_t index, bool enter_mode, bool append_history, std::string* error_message = nullptr) {
    if (index >= g_music_session.playlist.size()) {
        if (error_message != nullptr) {
            *error_message = "Track index out of range";
        }
        return false;
    }

    auto& app = Application::GetInstance();
    if (app.IsMusicPlaying()) {
        g_music_session.expected_stop = true;
    }

    const auto selected = g_music_session.playlist[index];
    if (!app.PlayMusicUrl(selected.url, selected.music_name)) {
        g_music_session.expected_stop = false;
        if (error_message != nullptr) {
            *error_message = "Failed to start playback for selected track";
        }
        return false;
    }

    g_music_session.expected_stop = false;
    g_music_session.current_index = static_cast<int>(index);
    g_music_session.paused = false;
    if (enter_mode) {
        g_music_session.mode_active = true;
    }
    if (append_history && (g_music_session.history.empty() || g_music_session.history.back() != index)) {
        g_music_session.history.push_back(index);
    }
    if (g_music_session.history.size() > 128) {
        size_t overflow = g_music_session.history.size() - 128;
        g_music_session.history.erase(g_music_session.history.begin(), g_music_session.history.begin() + overflow);
    }
    g_music_session.last_playing = true;
    g_music_session.last_error.clear();
    g_current_custom_music = selected;
    return true;
}

bool StopMusicForSession(bool exit_mode, bool paused) {
    auto& app = Application::GetInstance();
    bool stopped = false;
    if (app.IsMusicPlaying()) {
        g_music_session.expected_stop = true;
        stopped = app.StopMusic();
    }
    if (exit_mode) {
        g_music_session.mode_active = false;
    }
    g_music_session.paused = paused;
    return stopped;
}

std::vector<CustomMusicItem> FilterCatalogByKeyword(const std::string& music_name, const std::string& author_name) {
    std::vector<CustomMusicItem> result;
    for (const auto& item : g_custom_music_catalog) {
        if (!ContainsIgnoreCase(item.music_name, music_name)) {
            continue;
        }
        if (!author_name.empty() && !ContainsIgnoreCase(item.author_name, author_name)) {
            continue;
        }
        result.push_back(item);
    }
    return result;
}

std::vector<CustomMusicItem> BuildPlaylistFromIdList(const std::vector<std::string>& id_list, const std::string& requested_name) {
    std::vector<CustomMusicItem> candidates;
    candidates.reserve(id_list.size());
    for (const auto& id : id_list) {
        auto* item = FindCustomMusicById(id);
        if (item == nullptr) {
            continue;
        }
        if (!requested_name.empty() && !ContainsIgnoreCase(item->music_name, requested_name)) {
            continue;
        }
        candidates.push_back(*item);
    }
    return candidates;
}

int FindFirstMatchIndex(const std::vector<CustomMusicItem>& playlist, const std::string& keyword) {
    for (size_t i = 0; i < playlist.size(); ++i) {
        if (ContainsIgnoreCase(playlist[i].music_name, keyword)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::string NormalizeMusicControlAction(const std::string& raw_action) {
    auto action = ToLowerAsciiCopy(TrimCopy(raw_action));
    if (action.empty()) {
        return action;
    }

    if (action == "pause" || action == "暂停") {
        return "pause";
    }
    if (action == "resume" || action == "continue" || action == "继续" || action == "恢复") {
        return "resume";
    }
    if (action == "next" || action == "下一首" || action == "下一曲") {
        return "next";
    }
    if (action == "prev" || action == "previous" || action == "上一首" || action == "上一曲") {
        return "prev";
    }
    if (action == "play_index" || action == "按序号播放") {
        return "play_index";
    }
    if (action == "play_name" || action == "按名称播放") {
        return "play_name";
    }
    if (action == "status" || action == "状态") {
        return "status";
    }
    if (action == "exit" || action == "stop" || action == "退出" || action == "退出音乐模式") {
        return "exit";
    }
    if (action == "enter_dialog" || action == "dialog" || action == "chat" ||
        action == "进入对话模式" || action == "进入聊天模式") {
        return "enter_dialog";
    }

    return action;
}

cJSON* BuildMusicSessionJson() {
    auto* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "mode_active", g_music_session.mode_active);
    cJSON_AddBoolToObject(root, "paused", g_music_session.paused);
    cJSON_AddNumberToObject(root, "playlist_size", SafeSizeToInt(g_music_session.playlist.size()));
    cJSON_AddNumberToObject(root, "history_size", SafeSizeToInt(g_music_session.history.size()));
    cJSON_AddNumberToObject(root, "current_index", g_music_session.current_index);
    cJSON_AddNumberToObject(root, "current_position", g_music_session.current_index >= 0 ? g_music_session.current_index + 1 : 0);
    cJSON_AddBoolToObject(root, "expected_stop", g_music_session.expected_stop);
    if (!g_music_session.last_error.empty()) {
        cJSON_AddStringToObject(root, "last_error", g_music_session.last_error.c_str());
    }
    if (g_music_session.current_index >= 0 &&
        static_cast<size_t>(g_music_session.current_index) < g_music_session.playlist.size()) {
        cJSON_AddItemToObject(root, "current_track",
            BuildCustomMusicJson(g_music_session.playlist[static_cast<size_t>(g_music_session.current_index)]));
    }
    return root;
}

void AddCustomMusicMetaToJson(cJSON* root) {
    if (root == nullptr) {
        return;
    }
    cJSON_AddNumberToObject(root, "custom_music_catalog_size", SafeSizeToInt(g_custom_music_catalog.size()));
    cJSON_AddItemToObject(root, "music_session", BuildMusicSessionJson());
    if (g_current_custom_music.has_value()) {
        cJSON_AddItemToObject(root, "current_custom_music", BuildCustomMusicJson(g_current_custom_music.value()));
    }
}

cJSON* BuildDeviceStatusJson(Board& board, Application& app) {
    auto* root = cJSON_Parse(board.GetDeviceStatusJson().c_str());
    if (root == nullptr) {
        throw std::runtime_error("Failed to parse device status JSON");
    }
    cJSON_AddItemToObject(root, "clock", app.GetAlarmService().GetClockJson());
    cJSON_AddItemToObject(root, "alarm", app.GetAlarmService().GetStatusJson());
    cJSON_AddItemToObject(root, "countdown", app.GetAlarmService().GetCountdownJson());
    cJSON_AddItemToObject(root, "music", app.GetMusicStatusJson());
    AddCustomMusicMetaToJson(root);
    return root;
}

}  // namespace

McpServer::McpServer() {
}

McpServer::~McpServer() {
    for (auto tool : tools_) {
        delete tool;
    }
    tools_.clear();
}

void McpServer::OnClockTick() {
    auto& app = Application::GetInstance();
    bool playing = app.IsMusicPlaying();
    if (playing) {
        g_music_session.last_playing = true;
        return;
    }

    if (!g_music_session.last_playing) {
        return;
    }
    g_music_session.last_playing = false;

    if (g_music_session.expected_stop) {
        g_music_session.expected_stop = false;
        return;
    }

    auto* status = app.GetMusicStatusJson();
    auto* state_json = cJSON_GetObjectItem(status, "state");
    auto* error_json = cJSON_GetObjectItem(status, "last_error");
    std::string state = cJSON_IsString(state_json) && state_json->valuestring != nullptr ? state_json->valuestring : "";
    std::string error = cJSON_IsString(error_json) && error_json->valuestring != nullptr ? error_json->valuestring : "";
    cJSON_Delete(status);

    if (state == "error") {
        g_music_session.last_error = error.empty() ? "Track playback failed" : error;
        g_music_session.paused = false;
        return;
    }

    if (!g_music_session.mode_active || g_music_session.paused || g_music_session.playlist.empty() ||
        g_music_session.current_index < 0 || static_cast<size_t>(g_music_session.current_index) >= g_music_session.playlist.size()) {
        return;
    }

    size_t current_index = static_cast<size_t>(g_music_session.current_index);
    size_t next_index = current_index + 1;
    if (next_index >= g_music_session.playlist.size()) {
        g_music_session.mode_active = false;
        g_music_session.paused = false;
        Application::GetInstance().RefreshIdleDisplay();
        return;
    }

    std::string error_message;
    if (!StartPlaylistTrackByIndex(next_index, true, true, &error_message)) {
        g_music_session.last_error = error_message.empty() ? "Failed to auto play next track" : error_message;
    }
}

void McpServer::OnWakeWordInterruption() {
    auto& app = Application::GetInstance();
    bool is_playing = app.IsMusicPlaying();
    if (!is_playing && !g_music_session.mode_active) {
        return;
    }

    g_music_session.mode_active = false;
    g_music_session.paused = is_playing;
    if (is_playing) {
        g_music_session.expected_stop = true;
        app.StopMusic();
    }
}

void McpServer::AddCommonTools() {
    // *Important* To speed up the response time, we add the common tools to the beginning of
    // the tools list to utilize the prompt cache.
    // **重要** 为了提升响应速度，我们把常用的工具放在前面，利用 prompt cache 的特性。

    // Backup the original tools list and restore it after adding the common tools.
    auto original_tools = std::move(tools_);
    auto& board = Board::GetInstance();

    // Do not add custom tools here.
    // Custom tools must be added in the board's InitializeTools function.

    AddTool("self.get_device_status",
        "Provides the real-time information of the device, including the current status of the audio speaker, screen, battery, network, etc.\n"
        "Use this tool for: \n"
        "1. Answering questions about current condition (e.g. what is the current volume of the audio speaker?)\n"
        "2. As the first step to control the device (e.g. turn up / down the volume of the audio speaker, etc.)",
        PropertyList(),
        [&board](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            return BuildDeviceStatusJson(board, app);
        });

    AddTool("self.alarm.get",
        "Get local clock information and all local alarms stored on the device.",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            auto* root = cJSON_CreateObject();
            cJSON_AddItemToObject(root, "clock", app.GetAlarmService().GetClockJson());
            cJSON_AddItemToObject(root, "alarm", app.GetAlarmService().GetStatusJson());
            return root;
        });

    AddTool("self.countdown.get",
        "Get the current local countdown state on the device.",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            auto* root = cJSON_CreateObject();
            cJSON_AddItemToObject(root, "countdown", app.GetAlarmService().GetCountdownJson());
            return root;
        });

    AddTool("self.countdown.set",
        "Start or replace a local countdown. `seconds` is the total countdown duration in seconds.",
        PropertyList({
            Property("seconds", kPropertyTypeInteger, 1, 86400),
            Property("label", kPropertyTypeString, std::string(""))
        }),
        [](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            bool was_ringing = app.GetAlarmService().IsCountdownRinging();
            app.GetAlarmService().SetCountdown(
                properties["seconds"].value<int>(),
                properties["label"].value<std::string>());
            if (was_ringing) {
                app.GetAudioService().ResetDecoder();
                app.DismissAlert();
            }
            app.RefreshIdleDisplay();

            auto* root = cJSON_CreateObject();
            cJSON_AddItemToObject(root, "countdown", app.GetAlarmService().GetCountdownJson());
            return root;
        });

    auto cancel_countdown = [](const PropertyList& properties) -> ReturnValue {
        auto& app = Application::GetInstance();
        bool was_ringing = app.GetAlarmService().IsCountdownRinging();
        bool cancelled = app.GetAlarmService().CancelCountdown();
        if (was_ringing) {
            app.GetAudioService().ResetDecoder();
            app.DismissAlert();
        }
        app.RefreshIdleDisplay();

        auto* root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "cancelled", cancelled);
        cJSON_AddItemToObject(root, "countdown", app.GetAlarmService().GetCountdownJson());
        return root;
    };

    AddTool("self.countdown.cancel",
        "Cancel the current local countdown. If the countdown is ringing, this also stops the sound.",
        PropertyList(),
        cancel_countdown);

    AddTool("self.countdown.stop",
        "Stop the current local countdown. If the countdown is active it will be cancelled; if it is ringing it will also stop the sound.",
        PropertyList(),
        cancel_countdown);

    AddTool("self.music.get",
        "Get local URL music playback status on the device.",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            auto* root = cJSON_CreateObject();
            cJSON_AddItemToObject(root, "music", app.GetMusicStatusJson());
            AddCustomMusicMetaToJson(root);
            return root;
        });

    AddTool("self.music.play",
        "Play one direct URL track and enter music mode. Supports http/https mp3/wav/ogg URLs.",
        PropertyList({
            Property("url", kPropertyTypeString),
            Property("name", kPropertyTypeString, std::string("")),
            Property("streaming", kPropertyTypeBoolean, false)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            if (properties["streaming"].value<bool>()) {
                throw std::runtime_error("Streaming playback is reserved and not implemented yet");
            }

            auto url = TrimCopy(properties["url"].value<std::string>());
            if (!IsPlayableHttpUrl(url)) {
                throw std::runtime_error("url must start with http:// or https://");
            }

            CustomMusicItem item;
            item.id = "direct_url_1";
            item.music_name = TrimCopy(properties["name"].value<std::string>());
            if (item.music_name.empty()) {
                item.music_name = url;
            }
            item.url = url;

            g_music_session.playlist = {item};
            g_music_session.history.clear();
            g_music_session.current_index = -1;
            g_music_session.paused = false;
            g_music_session.last_error.clear();

            std::string error_message;
            if (!StartPlaylistTrackByIndex(0, true, true, &error_message)) {
                throw std::runtime_error(error_message.empty() ? "Failed to start local URL music playback" : error_message);
            }

            auto& app = Application::GetInstance();
            auto* root = cJSON_CreateObject();
            cJSON_AddItemToObject(root, "music", app.GetMusicStatusJson());
            AddCustomMusicMetaToJson(root);
            return root;
        });

    AddTool("self.music.stop",
        "Stop local URL music playback.",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            bool stopped = StopMusicForSession(true, false);

            auto* root = cJSON_CreateObject();
            cJSON_AddBoolToObject(root, "stopped", stopped);
            cJSON_AddItemToObject(root, "music", app.GetMusicStatusJson());
            AddCustomMusicMetaToJson(root);
            return root;
        });

    AddTool("self.music.catalog.push",
        "Push or replace a custom music catalog to the device.\n"
        "Pass `music_list_json` as a JSON array string. Each item should include id/music_id, music_name/name, author_name/author, and url.\n"
        "After pushing, call `search_custom_music` and then `play_custom_music`.",
        PropertyList({
            Property("music_list_json", kPropertyTypeString),
            Property("replace", kPropertyTypeBoolean, true)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            const auto music_list_json = properties["music_list_json"].value<std::string>();
            auto* parsed = cJSON_Parse(music_list_json.c_str());
            if (!cJSON_IsArray(parsed)) {
                cJSON_Delete(parsed);
                throw std::runtime_error("music_list_json must be a JSON array string");
            }

            if (properties["replace"].value<bool>()) {
                g_custom_music_catalog.clear();
                g_current_custom_music.reset();
            }

            int accepted = 0;
            int skipped = 0;
            cJSON* item = nullptr;
            cJSON_ArrayForEach(item, parsed) {
                CustomMusicItem music_item;
                music_item.id = GetJsonStringByKeys(item, {"id", "music_id"});
                music_item.music_name = GetJsonStringByKeys(item, {"music_name", "name", "title"});
                music_item.author_name = GetJsonStringByKeys(item, {"author_name", "author", "artist"});
                music_item.url = GetJsonStringByKeys(item, {"url", "play_url", "audio_url"});
                if (music_item.id.empty() || music_item.music_name.empty() || music_item.url.empty() || !IsPlayableHttpUrl(music_item.url)) {
                    ++skipped;
                    continue;
                }
                UpsertCustomMusicItem(music_item);
                ++accepted;
            }
            cJSON_Delete(parsed);

            auto* root = cJSON_CreateObject();
            cJSON_AddBoolToObject(root, "ok", true);
            cJSON_AddNumberToObject(root, "accepted", accepted);
            cJSON_AddNumberToObject(root, "skipped", skipped);
            AddCustomMusicMetaToJson(root);
            return root;
        });

    AddTool("search_custom_music",
        "Search music and get music IDs from the custom music catalog on the device.\n"
        "Use this tool when the user asks to search or play music.\n"
        "Args:\n"
        "  `music_name`: the music name keyword to search\n"
        "  `author_name`: optional author keyword to filter",
        PropertyList({
            Property("music_name", kPropertyTypeString),
            Property("author_name", kPropertyTypeString, std::string(""))
        }),
        [](const PropertyList& properties) -> ReturnValue {
            const auto music_name = TrimCopy(properties["music_name"].value<std::string>());
            const auto author_name = TrimCopy(properties["author_name"].value<std::string>());
            if (music_name.empty()) {
                throw std::runtime_error("music_name cannot be empty");
            }

            auto* root = cJSON_CreateObject();
            cJSON_AddStringToObject(root, "music_name", music_name.c_str());
            cJSON_AddStringToObject(root, "author_name", author_name.c_str());
            cJSON_AddNumberToObject(root, "catalog_size", SafeSizeToInt(g_custom_music_catalog.size()));

            auto* result_list = cJSON_CreateArray();
            auto result = FilterCatalogByKeyword(music_name, author_name);
            for (const auto& item : result) {
                cJSON_AddItemToArray(result_list, BuildCustomMusicJson(item));
            }
            cJSON_AddNumberToObject(root, "result_count", cJSON_GetArraySize(result_list));
            cJSON_AddItemToObject(root, "music_list", result_list);
            return root;
        });

    AddTool("play_custom_music",
        "Play music from custom catalog using IDs from `search_custom_music`.\n"
        "Call `search_custom_music` first.\n"
        "Args:\n"
        "  `id_list`: music ID list string (comma/space-separated or JSON array string). Keep the list order.\n"
        "  `music_name`: optional music name for fuzzy filter. If multiple tracks match, the first one is selected.",
        PropertyList({
            Property("id_list", kPropertyTypeString),
            Property("music_name", kPropertyTypeString, std::string(""))
        }),
        [](const PropertyList& properties) -> ReturnValue {
            auto id_list = ParseIdList(properties["id_list"].value<std::string>());
            if (id_list.empty()) {
                throw std::runtime_error("id_list is empty. Please provide IDs from search_custom_music");
            }

            auto requested_name = TrimCopy(properties["music_name"].value<std::string>());
            auto candidates = BuildPlaylistFromIdList(id_list, requested_name);

            if (candidates.empty()) {
                throw std::runtime_error("No playable tracks found in local catalog for given id_list");
            }

            g_music_session.playlist = candidates;
            g_music_session.history.clear();
            g_music_session.current_index = -1;
            g_music_session.paused = false;
            g_music_session.last_error.clear();

            std::string error_message;
            if (!StartPlaylistTrackByIndex(0, true, true, &error_message)) {
                throw std::runtime_error(error_message.empty() ? "Failed to play selected custom music URL" : error_message);
            }

            auto& app = Application::GetInstance();
            auto* root = cJSON_CreateObject();
            cJSON_AddBoolToObject(root, "ok", true);
            cJSON_AddNumberToObject(root, "candidate_count", SafeSizeToInt(candidates.size()));
            cJSON_AddItemToObject(root, "selected_music", BuildCustomMusicJson(candidates.front()));
            cJSON_AddItemToObject(root, "music", app.GetMusicStatusJson());
            AddCustomMusicMetaToJson(root);
            return root;
        });

    AddTool("self.music.control",
        "Control music mode and the current playlist.\n"
        "Supported actions:\n"
        "  `pause` / `暂停`: stop current track but keep music mode and current playlist.\n"
        "  `resume` / `continue` / `继续` / `恢复`: continue previous playlist from the current track beginning.\n"
        "  `next` / `下一首`: play next track in order.\n"
        "  `prev` / `上一首`: play previously played track from history.\n"
        "  `play_index`: play Nth track in current playlist (1-based).\n"
        "  `play_name`: fuzzy match track name in current playlist (or latest catalog if no active playlist), select first hit.\n"
        "  `exit` / `退出音乐模式`: stop playback and exit music mode.\n"
        "  `enter_dialog` / `进入对话模式`: exit music mode and enter dialogue listening mode.\n"
        "  `status`: return current music status only.",
        PropertyList({
            Property("action", kPropertyTypeString),
            Property("index", kPropertyTypeInteger, 1, 1, 9999),
            Property("music_name", kPropertyTypeString, std::string(""))
        }),
        [](const PropertyList& properties) -> ReturnValue {
            auto action = NormalizeMusicControlAction(properties["action"].value<std::string>());
            auto& app = Application::GetInstance();

            if (action == "status") {
                auto* root = cJSON_CreateObject();
                cJSON_AddStringToObject(root, "action", "status");
                cJSON_AddItemToObject(root, "music", app.GetMusicStatusJson());
                AddCustomMusicMetaToJson(root);
                return root;
            } else if (action == "pause") {
                StopMusicForSession(false, true);
                g_music_session.mode_active = true;
            } else if (action == "resume" || action == "continue") {
                if (g_music_session.playlist.empty() || g_music_session.current_index < 0 ||
                    static_cast<size_t>(g_music_session.current_index) >= g_music_session.playlist.size()) {
                    throw std::runtime_error("没有可继续播放的歌曲，请先点歌。");
                }
                std::string error_message;
                if (!StartPlaylistTrackByIndex(static_cast<size_t>(g_music_session.current_index), true, false, &error_message)) {
                    throw std::runtime_error(error_message.empty() ? "继续播放失败" : error_message);
                }
            } else if (action == "next") {
                if (g_music_session.playlist.empty()) {
                    throw std::runtime_error("当前没有可播放歌单，请先点歌。");
                }
                size_t target_index = 0;
                if (g_music_session.current_index >= 0 &&
                    static_cast<size_t>(g_music_session.current_index) < g_music_session.playlist.size()) {
                    target_index = static_cast<size_t>(g_music_session.current_index) + 1;
                    if (target_index >= g_music_session.playlist.size()) {
                        throw std::runtime_error("当前已是最后一首");
                    }
                }
                std::string error_message;
                if (!StartPlaylistTrackByIndex(target_index, true, true, &error_message)) {
                    throw std::runtime_error(error_message.empty() ? "下一首播放失败" : error_message);
                }
            } else if (action == "prev" || action == "previous") {
                if (g_music_session.history.size() < 2) {
                    throw std::runtime_error("没有上一首");
                }
                g_music_session.history.pop_back();
                size_t target_index = g_music_session.history.back();
                std::string error_message;
                if (!StartPlaylistTrackByIndex(target_index, true, false, &error_message)) {
                    throw std::runtime_error(error_message.empty() ? "上一首播放失败" : error_message);
                }
            } else if (action == "play_index") {
                if (g_music_session.playlist.empty()) {
                    throw std::runtime_error("当前没有可播放歌单，请先点歌。");
                }
                int index = properties["index"].value<int>();
                if (index < 1 || static_cast<size_t>(index) > g_music_session.playlist.size()) {
                    throw std::runtime_error("没有第" + std::to_string(index) + "首，请说1到" +
                        std::to_string(g_music_session.playlist.size()) + "之间的数字");
                }
                std::string error_message;
                if (!StartPlaylistTrackByIndex(static_cast<size_t>(index - 1), true, true, &error_message)) {
                    throw std::runtime_error(error_message.empty() ? "按序号播放失败" : error_message);
                }
            } else if (action == "play_name") {
                auto music_name = TrimCopy(properties["music_name"].value<std::string>());
                if (music_name.empty()) {
                    throw std::runtime_error("music_name cannot be empty");
                }

                int index = FindFirstMatchIndex(g_music_session.playlist, music_name);
                if (index >= 0) {
                    std::string error_message;
                    if (!StartPlaylistTrackByIndex(static_cast<size_t>(index), true, true, &error_message)) {
                        throw std::runtime_error(error_message.empty() ? "按名称播放失败" : error_message);
                    }
                } else {
                    auto matches = FilterCatalogByKeyword(music_name, "");
                    if (matches.empty()) {
                        throw std::runtime_error("当前列表没有这首，请说出正确歌曲名称。");
                    }
                    g_music_session.playlist = matches;
                    g_music_session.history.clear();
                    g_music_session.current_index = -1;
                    std::string error_message;
                    if (!StartPlaylistTrackByIndex(0, true, true, &error_message)) {
                        throw std::runtime_error(error_message.empty() ? "按名称播放失败" : error_message);
                    }
                }
            } else if (action == "exit") {
                StopMusicForSession(true, false);
            } else if (action == "enter_dialog") {
                StopMusicForSession(true, false);
                app.StartListening();
            } else {
                throw std::runtime_error("Unsupported action for self.music.control");
            }

            auto* root = cJSON_CreateObject();
            cJSON_AddStringToObject(root, "action", action.c_str());
            cJSON_AddItemToObject(root, "music", app.GetMusicStatusJson());
            AddCustomMusicMetaToJson(root);
            return root;
        });

    AddTool("self.alarm.set",
        "Set or update a local alarm using the device's local time.\n"
        "Use repeat=`once` for a one-time alarm and repeat=`daily` for a recurring alarm.\n"
        "If the user gives a specific date, pass year/month/day. If updating an existing alarm, pass alarm_id.\n"
        "Before converting relative times like tomorrow morning, call `self.get_device_status` or `self.alarm.get` to read the current local time.",
        PropertyList({
            Property("hour", kPropertyTypeInteger, 0, 23),
            Property("minute", kPropertyTypeInteger, 0, 59),
            Property("repeat", kPropertyTypeString, std::string("once")),
            Property("label", kPropertyTypeString, std::string("")),
            Property("alarm_id", kPropertyTypeInteger, 0, 0, 9999),
            Property("year", kPropertyTypeInteger, 0, 0, 2099),
            Property("month", kPropertyTypeInteger, 0, 0, 12),
            Property("day", kPropertyTypeInteger, 0, 0, 31)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            app.GetAlarmService().SetAlarm(
                properties["hour"].value<int>(),
                properties["minute"].value<int>(),
                properties["repeat"].value<std::string>(),
                properties["label"].value<std::string>(),
                properties["alarm_id"].value<int>(),
                properties["year"].value<int>(),
                properties["month"].value<int>(),
                properties["day"].value<int>());
            app.RefreshIdleDisplay();

            auto* root = cJSON_CreateObject();
            cJSON_AddItemToObject(root, "clock", app.GetAlarmService().GetClockJson());
            cJSON_AddItemToObject(root, "alarm", app.GetAlarmService().GetStatusJson());
            return root;
        });

    AddTool("self.alarm.enable",
        "Enable a local alarm by alarm_id.",
        PropertyList({
            Property("alarm_id", kPropertyTypeInteger, 1, 9999)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            if (!app.GetAlarmService().SetAlarmEnabled(properties["alarm_id"].value<int>(), true)) {
                throw std::runtime_error("Alarm not found");
            }
            app.RefreshIdleDisplay();

            auto* root = cJSON_CreateObject();
            cJSON_AddItemToObject(root, "alarm", app.GetAlarmService().GetStatusJson());
            return root;
        });

    AddTool("self.alarm.disable",
        "Disable a local alarm by alarm_id.",
        PropertyList({
            Property("alarm_id", kPropertyTypeInteger, 1, 9999)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            auto ringing_alarm = app.GetAlarmService().GetRingingAlarm();
            bool was_ringing = ringing_alarm.has_value() &&
                ringing_alarm->id == properties["alarm_id"].value<int>();
            if (!app.GetAlarmService().SetAlarmEnabled(properties["alarm_id"].value<int>(), false)) {
                throw std::runtime_error("Alarm not found");
            }
            if (was_ringing) {
                app.GetAudioService().ResetDecoder();
            }
            app.DismissAlert();
            app.RefreshIdleDisplay();

            auto* root = cJSON_CreateObject();
            cJSON_AddItemToObject(root, "alarm", app.GetAlarmService().GetStatusJson());
            return root;
        });

    AddTool("self.alarm.snooze",
        "Snooze the currently ringing local alarm. Supported snooze values are 5 or 10 minutes.",
        PropertyList({
            Property("minutes", kPropertyTypeInteger, 5, 5, 10)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            auto minutes = properties["minutes"].value<int>();
            if (minutes != 5 && minutes != 10) {
                throw std::runtime_error("Snooze minutes must be 5 or 10");
            }

            auto& app = Application::GetInstance();
            app.GetAlarmService().SnoozeRingingAlarm(minutes);
            app.GetAudioService().ResetDecoder();
            app.DismissAlert();
            app.RefreshIdleDisplay();

            auto* root = cJSON_CreateObject();
            cJSON_AddNumberToObject(root, "snoozed_minutes", minutes);
            cJSON_AddItemToObject(root, "alarm", app.GetAlarmService().GetStatusJson());
            return root;
        });

    AddTool("self.alarm.delete",
        "Delete a local alarm by alarm_id.",
        PropertyList({
            Property("alarm_id", kPropertyTypeInteger, 1, 9999)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            auto ringing_alarm = app.GetAlarmService().GetRingingAlarm();
            bool was_ringing = ringing_alarm.has_value() &&
                ringing_alarm->id == properties["alarm_id"].value<int>();
            if (!app.GetAlarmService().DeleteAlarm(properties["alarm_id"].value<int>())) {
                throw std::runtime_error("Alarm not found");
            }
            if (was_ringing) {
                app.GetAudioService().ResetDecoder();
                app.DismissAlert();
            }
            app.RefreshIdleDisplay();

            auto* root = cJSON_CreateObject();
            cJSON_AddItemToObject(root, "alarm", app.GetAlarmService().GetStatusJson());
            return root;
        });

    AddTool("self.alarm.stop",
        "Stop the currently ringing local alarm.",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            bool stopped = app.GetAlarmService().StopRinging();
            if (stopped) {
                app.GetAudioService().ResetDecoder();
                app.DismissAlert();
            }
            app.RefreshIdleDisplay();

            auto* root = cJSON_CreateObject();
            cJSON_AddBoolToObject(root, "stopped", stopped);
            cJSON_AddItemToObject(root, "alarm", app.GetAlarmService().GetStatusJson());
            return root;
        });

    AddTool("self.audio_speaker.set_volume", 
        "Set the volume of the audio speaker. If the current volume is unknown, you must call `self.get_device_status` tool first and then call this tool.",
        PropertyList({
            Property("volume", kPropertyTypeInteger, 0, 100)
        }), 
        [&board](const PropertyList& properties) -> ReturnValue {
            auto codec = board.GetAudioCodec();
            codec->SetOutputVolume(properties["volume"].value<int>());
            return true;
        });
    
    auto backlight = board.GetBacklight();
    if (backlight) {
        AddTool("self.screen.set_brightness",
            "Set the brightness of the screen.",
            PropertyList({
                Property("brightness", kPropertyTypeInteger, 0, 100)
            }),
            [backlight](const PropertyList& properties) -> ReturnValue {
                uint8_t brightness = static_cast<uint8_t>(properties["brightness"].value<int>());
                backlight->SetBrightness(brightness, true);
                return true;
            });
    }

#ifdef HAVE_LVGL
    auto display = board.GetDisplay();
    if (display && display->GetTheme() != nullptr) {
        AddTool("self.screen.set_theme",
            "Set the theme of the screen. The theme can be `light` or `dark`.",
            PropertyList({
                Property("theme", kPropertyTypeString)
            }),
            [display](const PropertyList& properties) -> ReturnValue {
                auto theme_name = properties["theme"].value<std::string>();
                auto& theme_manager = LvglThemeManager::GetInstance();
                auto theme = theme_manager.GetTheme(theme_name);
                if (theme != nullptr) {
                    display->SetTheme(theme);
                    return true;
                }
                return false;
            });
    }

    auto camera = board.GetCamera();
    if (camera) {
        AddTool("self.camera.take_photo",
            "Always remember you have a camera. If the user asks you to see something, use this tool to take a photo and then explain it.\n"
            "Args:\n"
            "  `question`: The question that you want to ask about the photo.\n"
            "Return:\n"
            "  A JSON object that provides the photo information.",
            PropertyList({
                Property("question", kPropertyTypeString)
            }),
            [camera](const PropertyList& properties) -> ReturnValue {
                // Lower the priority to do the camera capture
                TaskPriorityReset priority_reset(1);

                if (!camera->Capture()) {
                    throw std::runtime_error("Failed to capture photo");
                }
                auto question = properties["question"].value<std::string>();
                return camera->Explain(question);
            });
    }
#endif

    // Restore the original tools list to the end of the tools list
    tools_.insert(tools_.end(), original_tools.begin(), original_tools.end());
}

void McpServer::AddUserOnlyTools() {
    // System tools
    AddUserOnlyTool("self.get_system_info",
        "Get the system information",
        PropertyList(),
        [this](const PropertyList& properties) -> ReturnValue {
            auto& board = Board::GetInstance();
            return board.GetSystemInfoJson();
        });

    AddUserOnlyTool("self.reboot", "Reboot the system",
        PropertyList(),
        [this](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            app.Schedule([&app]() {
                ESP_LOGW(TAG, "User requested reboot");
                vTaskDelay(pdMS_TO_TICKS(1000));

                app.Reboot();
            });
            return true;
        });

    // Firmware upgrade
    AddUserOnlyTool("self.upgrade_firmware", "Upgrade firmware from a specific URL. This will download and install the firmware, then reboot the device.",
        PropertyList({
            Property("url", kPropertyTypeString, "The URL of the firmware binary file to download and install")
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            auto url = properties["url"].value<std::string>();
            ESP_LOGI(TAG, "User requested firmware upgrade from URL: %s", url.c_str());
            
            auto& app = Application::GetInstance();
            app.Schedule([url, &app]() {
                bool success = app.UpgradeFirmware(url);
                if (!success) {
                    ESP_LOGE(TAG, "Firmware upgrade failed");
                }
            });
            
            return true;
        });

    // Display control
#ifdef HAVE_LVGL
    auto display = dynamic_cast<LvglDisplay*>(Board::GetInstance().GetDisplay());
    if (display) {
        AddUserOnlyTool("self.screen.get_info", "Information about the screen, including width, height, etc.",
            PropertyList(),
            [display](const PropertyList& properties) -> ReturnValue {
                cJSON *json = cJSON_CreateObject();
                cJSON_AddNumberToObject(json, "width", display->width());
                cJSON_AddNumberToObject(json, "height", display->height());
                if (dynamic_cast<OledDisplay*>(display)) {
                    cJSON_AddBoolToObject(json, "monochrome", true);
                } else {
                    cJSON_AddBoolToObject(json, "monochrome", false);
                }
                return json;
            });

#if CONFIG_LV_USE_SNAPSHOT
        AddUserOnlyTool("self.screen.snapshot", "Snapshot the screen and upload it to a specific URL",
            PropertyList({
                Property("url", kPropertyTypeString),
                Property("quality", kPropertyTypeInteger, 80, 1, 100)
            }),
            [display](const PropertyList& properties) -> ReturnValue {
                auto url = properties["url"].value<std::string>();
                auto quality = properties["quality"].value<int>();

                std::string jpeg_data;
                if (!display->SnapshotToJpeg(jpeg_data, quality)) {
                    throw std::runtime_error("Failed to snapshot screen");
                }

                ESP_LOGI(TAG, "Upload snapshot %u bytes to %s", jpeg_data.size(), url.c_str());
                
                // 构造multipart/form-data请求体
                std::string boundary = "----ESP32_SCREEN_SNAPSHOT_BOUNDARY";
                
                auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
                http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
                if (!http->Open("POST", url)) {
                    throw std::runtime_error("Failed to open URL: " + url);
                }
                {
                    // 文件字段头部
                    std::string file_header;
                    file_header += "--" + boundary + "\r\n";
                    file_header += "Content-Disposition: form-data; name=\"file\"; filename=\"screenshot.jpg\"\r\n";
                    file_header += "Content-Type: image/jpeg\r\n";
                    file_header += "\r\n";
                    http->Write(file_header.c_str(), file_header.size());
                }

                // JPEG数据
                http->Write((const char*)jpeg_data.data(), jpeg_data.size());

                {
                    // multipart尾部
                    std::string multipart_footer;
                    multipart_footer += "\r\n--" + boundary + "--\r\n";
                    http->Write(multipart_footer.c_str(), multipart_footer.size());
                }
                http->Write("", 0);

                if (http->GetStatusCode() != 200) {
                    throw std::runtime_error("Unexpected status code: " + std::to_string(http->GetStatusCode()));
                }
                std::string result = http->ReadAll();
                http->Close();
                ESP_LOGI(TAG, "Snapshot screen result: %s", result.c_str());
                return true;
            });
        
        AddUserOnlyTool("self.screen.preview_image", "Preview an image on the screen",
            PropertyList({
                Property("url", kPropertyTypeString)
            }),
            [display](const PropertyList& properties) -> ReturnValue {
                auto url = properties["url"].value<std::string>();
                auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);

                if (!http->Open("GET", url)) {
                    throw std::runtime_error("Failed to open URL: " + url);
                }
                int status_code = http->GetStatusCode();
                if (status_code != 200) {
                    throw std::runtime_error("Unexpected status code: " + std::to_string(status_code));
                }

                size_t content_length = http->GetBodyLength();
                char* data = (char*)heap_caps_malloc(content_length, MALLOC_CAP_8BIT);
                if (data == nullptr) {
                    throw std::runtime_error("Failed to allocate memory for image: " + url);
                }
                size_t total_read = 0;
                while (total_read < content_length) {
                    int ret = http->Read(data + total_read, content_length - total_read);
                    if (ret < 0) {
                        heap_caps_free(data);
                        throw std::runtime_error("Failed to download image: " + url);
                    }
                    if (ret == 0) {
                        break;
                    }
                    total_read += ret;
                }
                http->Close();

                auto image = std::make_unique<LvglAllocatedImage>(data, content_length);
                display->SetPreviewImage(std::move(image));
                return true;
            });
#endif // CONFIG_LV_USE_SNAPSHOT
    }
#endif // HAVE_LVGL

    // Assets download url
    auto& assets = Assets::GetInstance();
    if (assets.partition_valid()) {
        AddUserOnlyTool("self.assets.set_download_url", "Set the download url for the assets",
            PropertyList({
                Property("url", kPropertyTypeString)
            }),
            [](const PropertyList& properties) -> ReturnValue {
                auto url = properties["url"].value<std::string>();
                Settings settings("assets", true);
                settings.SetString("download_url", url);
                return true;
            });
    }
}

void McpServer::AddTool(McpTool* tool) {
    // Prevent adding duplicate tools
    if (std::find_if(tools_.begin(), tools_.end(), [tool](const McpTool* t) { return t->name() == tool->name(); }) != tools_.end()) {
        ESP_LOGW(TAG, "Tool %s already added", tool->name().c_str());
        return;
    }

    ESP_LOGI(TAG, "Add tool: %s%s", tool->name().c_str(), tool->user_only() ? " [user]" : "");
    tools_.push_back(tool);
}

void McpServer::AddTool(const std::string& name, const std::string& description, const PropertyList& properties, std::function<ReturnValue(const PropertyList&)> callback) {
    AddTool(new McpTool(name, description, properties, callback));
}

void McpServer::AddUserOnlyTool(const std::string& name, const std::string& description, const PropertyList& properties, std::function<ReturnValue(const PropertyList&)> callback) {
    auto tool = new McpTool(name, description, properties, callback);
    tool->set_user_only(true);
    AddTool(tool);
}

void McpServer::ParseMessage(const std::string& message) {
    cJSON* json = cJSON_Parse(message.c_str());
    if (json == nullptr) {
        ESP_LOGE(TAG, "Failed to parse MCP message: %s", message.c_str());
        return;
    }
    ParseMessage(json);
    cJSON_Delete(json);
}

void McpServer::ParseCapabilities(const cJSON* capabilities) {
    auto vision = cJSON_GetObjectItem(capabilities, "vision");
    if (cJSON_IsObject(vision)) {
        auto url = cJSON_GetObjectItem(vision, "url");
        auto token = cJSON_GetObjectItem(vision, "token");
        if (cJSON_IsString(url)) {
            auto camera = Board::GetInstance().GetCamera();
            if (camera) {
                std::string url_str = std::string(url->valuestring);
                std::string token_str;
                if (cJSON_IsString(token)) {
                    token_str = std::string(token->valuestring);
                }
                camera->SetExplainUrl(url_str, token_str);
            }
        }
    }
}

void McpServer::ParseMessage(const cJSON* json) {
    // Check JSONRPC version
    auto version = cJSON_GetObjectItem(json, "jsonrpc");
    if (version == nullptr || !cJSON_IsString(version) || strcmp(version->valuestring, "2.0") != 0) {
        ESP_LOGE(TAG, "Invalid JSONRPC version: %s", version ? version->valuestring : "null");
        return;
    }
    
    // Check method
    auto method = cJSON_GetObjectItem(json, "method");
    if (method == nullptr || !cJSON_IsString(method)) {
        ESP_LOGE(TAG, "Missing method");
        return;
    }
    
    auto method_str = std::string(method->valuestring);
    if (method_str.find("notifications") == 0) {
        return;
    }
    
    // Check params
    auto params = cJSON_GetObjectItem(json, "params");
    if (params != nullptr && !cJSON_IsObject(params)) {
        ESP_LOGE(TAG, "Invalid params for method: %s", method_str.c_str());
        return;
    }

    auto id = cJSON_GetObjectItem(json, "id");
    if (id == nullptr || !cJSON_IsNumber(id)) {
        ESP_LOGE(TAG, "Invalid id for method: %s", method_str.c_str());
        return;
    }
    auto id_int = id->valueint;
    
    if (method_str == "initialize") {
        if (cJSON_IsObject(params)) {
            auto capabilities = cJSON_GetObjectItem(params, "capabilities");
            if (cJSON_IsObject(capabilities)) {
                ParseCapabilities(capabilities);
            }
        }
        auto app_desc = esp_app_get_description();
        std::string message = "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"" BOARD_NAME "\",\"version\":\"";
        message += app_desc->version;
        message += "\"}}";
        ReplyResult(id_int, message);
    } else if (method_str == "tools/list") {
        std::string cursor_str = "";
        bool list_user_only_tools = false;
        if (params != nullptr) {
            auto cursor = cJSON_GetObjectItem(params, "cursor");
            if (cJSON_IsString(cursor)) {
                cursor_str = std::string(cursor->valuestring);
            }
            auto with_user_tools = cJSON_GetObjectItem(params, "withUserTools");
            if (cJSON_IsBool(with_user_tools)) {
                list_user_only_tools = with_user_tools->valueint == 1;
            }
        }
        GetToolsList(id_int, cursor_str, list_user_only_tools);
    } else if (method_str == "tools/call") {
        if (!cJSON_IsObject(params)) {
            ESP_LOGE(TAG, "tools/call: Missing params");
            ReplyError(id_int, "Missing params");
            return;
        }
        auto tool_name = cJSON_GetObjectItem(params, "name");
        if (!cJSON_IsString(tool_name)) {
            ESP_LOGE(TAG, "tools/call: Missing name");
            ReplyError(id_int, "Missing name");
            return;
        }
        auto tool_arguments = cJSON_GetObjectItem(params, "arguments");
        if (tool_arguments != nullptr && !cJSON_IsObject(tool_arguments)) {
            ESP_LOGE(TAG, "tools/call: Invalid arguments");
            ReplyError(id_int, "Invalid arguments");
            return;
        }
        DoToolCall(id_int, std::string(tool_name->valuestring), tool_arguments);
    } else {
        ESP_LOGE(TAG, "Method not implemented: %s", method_str.c_str());
        ReplyError(id_int, "Method not implemented: " + method_str);
    }
}

void McpServer::ReplyResult(int id, const std::string& result) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id) + ",\"result\":";
    payload += result;
    payload += "}";
    Application::GetInstance().SendMcpMessage(payload);
}

void McpServer::ReplyError(int id, const std::string& message) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id);
    payload += ",\"error\":{\"message\":\"";
    payload += message;
    payload += "\"}}";
    Application::GetInstance().SendMcpMessage(payload);
}

void McpServer::GetToolsList(int id, const std::string& cursor, bool list_user_only_tools) {
    const int max_payload_size = 8000;
    std::string json = "{\"tools\":[";
    
    bool found_cursor = cursor.empty();
    auto it = tools_.begin();
    std::string next_cursor = "";
    
    while (it != tools_.end()) {
        // 如果我们还没有找到起始位置，继续搜索
        if (!found_cursor) {
            if ((*it)->name() == cursor) {
                found_cursor = true;
            } else {
                ++it;
                continue;
            }
        }

        if (!list_user_only_tools && (*it)->user_only()) {
            ++it;
            continue;
        }
        
        // 添加tool前检查大小
        std::string tool_json = (*it)->to_json() + ",";
        if (json.length() + tool_json.length() + 30 > max_payload_size) {
            // 如果添加这个tool会超出大小限制，设置next_cursor并退出循环
            next_cursor = (*it)->name();
            break;
        }
        
        json += tool_json;
        ++it;
    }
    
    if (json.back() == ',') {
        json.pop_back();
    }
    
    if (json.back() == '[' && !tools_.empty()) {
        // 如果没有添加任何tool，返回错误
        ESP_LOGE(TAG, "tools/list: Failed to add tool %s because of payload size limit", next_cursor.c_str());
        ReplyError(id, "Failed to add tool " + next_cursor + " because of payload size limit");
        return;
    }

    if (next_cursor.empty()) {
        json += "]}";
    } else {
        json += "],\"nextCursor\":\"" + next_cursor + "\"}";
    }
    
    ReplyResult(id, json);
}

void McpServer::DoToolCall(int id, const std::string& tool_name, const cJSON* tool_arguments) {
    auto tool_iter = std::find_if(tools_.begin(), tools_.end(), 
                                 [&tool_name](const McpTool* tool) { 
                                     return tool->name() == tool_name; 
                                 });
    
    if (tool_iter == tools_.end()) {
        ESP_LOGE(TAG, "tools/call: Unknown tool: %s", tool_name.c_str());
        ReplyError(id, "Unknown tool: " + tool_name);
        return;
    }

    PropertyList arguments = (*tool_iter)->properties();
    try {
        for (auto& argument : arguments) {
            bool found = false;
            if (cJSON_IsObject(tool_arguments)) {
                auto value = cJSON_GetObjectItem(tool_arguments, argument.name().c_str());
                if (argument.type() == kPropertyTypeBoolean && cJSON_IsBool(value)) {
                    argument.set_value<bool>(value->valueint == 1);
                    found = true;
                } else if (argument.type() == kPropertyTypeInteger && cJSON_IsNumber(value)) {
                    argument.set_value<int>(value->valueint);
                    found = true;
                } else if (argument.type() == kPropertyTypeString && cJSON_IsString(value)) {
                    argument.set_value<std::string>(value->valuestring);
                    found = true;
                }
            }

            if (!argument.has_default_value() && !found) {
                ESP_LOGE(TAG, "tools/call: Missing valid argument: %s", argument.name().c_str());
                ReplyError(id, "Missing valid argument: " + argument.name());
                return;
            }
        }
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "tools/call: %s", e.what());
        ReplyError(id, e.what());
        return;
    }

    // Use main thread to call the tool
    auto& app = Application::GetInstance();
    app.Schedule([this, id, tool_iter, arguments = std::move(arguments)]() {
        try {
            ReplyResult(id, (*tool_iter)->Call(arguments));
        } catch (const std::exception& e) {
            ESP_LOGE(TAG, "tools/call: %s", e.what());
            ReplyError(id, e.what());
        }
    });
}
