#include "radio_mcp_tool.h"

#include <algorithm>
#include <cctype>

#include <cJSON.h>
#include <esp_log.h>

#include "application.h"
#include "vov_radio_service.h"

#define TAG "RadioMcpTool"

namespace {

std::string BuildStationListDescription() {
    std::string desc;
    for (const auto& station : VovRadioService::Stations()) {
        if (!desc.empty()) desc += ", ";
        desc += "`" + station.id + "` (" + station.name + ")";
    }
    return desc;
}

}  // namespace

void RadioMcpTool::Initialize() {
    auto& mcp_server = McpServer::GetInstance();

    mcp_server.AddTool(
        "self.radio.play_station",
        "Play a live internet radio station (VOV, Vietnam's national broadcaster) through the "
        "device speaker. Use this when the user asks to play the radio, nghe dai, nghe VOV, etc. "
        "Keeps streaming in the background until self.radio.stop is called or a new voice "
        "conversation starts (which stops it automatically).\n"
        "Args:\n"
        "  `station`: One of " +
            BuildStationListDescription() + ". Defaults to `vov1`.\n"
        "Return:\n"
        "  A JSON object with the `station` id and name now playing.",
        PropertyList({
            Property("station", kPropertyTypeString, std::string("vov1")),
        }),
        [](const PropertyList& properties) -> ToolResult { return HandlePlayStation(properties); });

    mcp_server.AddTool(
        "self.radio.stop",
        "Stop the internet radio if it is currently playing. Use this when the user asks to stop "
        "the radio, tat dai, dung phat, etc.",
        PropertyList(), [](const PropertyList& properties) -> ToolResult { return HandleStop(properties); });

    mcp_server.AddTool(
        "self.radio.get_status",
        "Get whether internet radio is currently playing and, if so, which station.",
        PropertyList(),
        [](const PropertyList& properties) -> ToolResult { return HandleGetStatus(properties); });

    ESP_LOGI(TAG, "RadioMcpTool initialized");
}

ToolResult RadioMcpTool::HandlePlayStation(const PropertyList& properties) {
    auto station_id = properties["station"].value<std::string>();
    std::transform(station_id.begin(), station_id.end(), station_id.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    const RadioStation* station = VovRadioService::FindStation(station_id);
    if (station == nullptr) {
        return std::unexpected("Unknown radio station: " + station_id);
    }

    auto& radio = Application::GetInstance().GetRadioService();
    if (!radio.Play(station->url, station->name)) {
        return std::unexpected("Failed to start radio playback for " + station->name);
    }

    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return std::unexpected("Failed to allocate JSON result");
    }
    cJSON_AddStringToObject(root, "station", station->id.c_str());
    cJSON_AddStringToObject(root, "name", station->name.c_str());
    cJSON_AddStringToObject(root, "status", "playing");
    return root;
}

ToolResult RadioMcpTool::HandleStop(const PropertyList& properties) {
    (void)properties;
    Application::GetInstance().GetRadioService().Stop();
    return true;
}

ToolResult RadioMcpTool::HandleGetStatus(const PropertyList& properties) {
    (void)properties;
    auto& radio = Application::GetInstance().GetRadioService();

    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return std::unexpected("Failed to allocate JSON result");
    }
    bool playing = radio.IsPlaying();
    cJSON_AddBoolToObject(root, "playing", playing);
    cJSON_AddStringToObject(root, "station", playing ? radio.CurrentStationName().c_str() : "");
    return root;
}
