#include "glass2_usage_http.h"

#include "glass2.h"

#include <array>
#include <cstdint>
#include <mutex>

#include <ArduinoJson.h>
#include <esp_err.h>
#include <esp_http_server.h>
#include <esp_log.h>

namespace {

constexpr char kTag[] = "GLASS2_HTTP";
constexpr std::uint16_t kPort = 8767;
constexpr std::size_t kMaxBodySize = 256;

httpd_handle_t s_server = nullptr;
std::mutex s_server_mutex;

esp_err_t send_json(httpd_req_t* request, const char* status, const char* body)
{
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, body);
}

esp_err_t usage_handler(httpd_req_t* request)
{
    if (request->content_len == 0 || request->content_len > kMaxBodySize) {
        return send_json(request, "400 Bad Request", R"({"ok":false})");
    }

    std::array<char, kMaxBodySize + 1> body{};
    std::size_t received = 0;
    while (received < request->content_len) {
        const int result =
            httpd_req_recv(request, body.data() + received, request->content_len - received);
        if (result <= 0) {
            ESP_LOGW(kTag, "usage http: failed to read request body");
            return send_json(request, "400 Bad Request", R"({"ok":false})");
        }
        received += static_cast<std::size_t>(result);
    }

    ArduinoJson::JsonDocument document;
    const auto json_error = ArduinoJson::deserializeJson(document, body.data(), received);
    if (json_error || !document.is<ArduinoJson::JsonObject>()) {
        ESP_LOGW(kTag, "usage http: invalid JSON");
        return send_json(request, "400 Bad Request", R"({"ok":false})");
    }

    const ArduinoJson::JsonVariantConst percent_value = document["percent"];
    ArduinoJson::JsonVariantConst updated_at_value = document["updatedAt"];
    if (updated_at_value.isNull()) {
        updated_at_value = document["updated_at"];
    }

    if (!percent_value.is<int>() || !updated_at_value.is<std::int64_t>()) {
        ESP_LOGW(kTag, "usage http: percent and updatedAt must be integers");
        return send_json(request, "400 Bad Request", R"({"ok":false})");
    }

    const int percent = percent_value.as<int>();
    const std::int64_t updated_at = updated_at_value.as<std::int64_t>();
    if (percent < 0 || percent > 100) {
        ESP_LOGW(kTag, "usage http: rejected percent %d", percent);
        return send_json(request, "400 Bad Request", R"({"ok":false})");
    }

    if (!glass2_update_usage(percent, updated_at)) {
        ESP_LOGW(kTag, "usage http: Glass2 unavailable");
        return send_json(request, "503 Service Unavailable", R"({"ok":false})");
    }

    ESP_LOGI(kTag, "usage http: updated percent=%d updatedAt=%lld", percent,
             static_cast<long long>(updated_at));
    return send_json(request, "200 OK", R"({"ok":true})");
}

}  // namespace

bool glass2_usage_http_start()
{
    std::lock_guard<std::mutex> lock(s_server_mutex);
    if (s_server != nullptr) {
        return true;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = kPort;
    config.ctrl_port = 32770;
    config.max_uri_handlers = 2;

    esp_err_t error = httpd_start(&s_server, &config);
    if (error != ESP_OK) {
        s_server = nullptr;
        ESP_LOGE(kTag, "usage http: failed to listen on port %u: %s", kPort, esp_err_to_name(error));
        return false;
    }

    const httpd_uri_t usage_uri = {
        .uri = "/usage",
        .method = HTTP_POST,
        .handler = usage_handler,
        .user_ctx = nullptr,
    };
    error = httpd_register_uri_handler(s_server, &usage_uri);
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "usage http: failed to register endpoint: %s", esp_err_to_name(error));
        httpd_stop(s_server);
        s_server = nullptr;
        return false;
    }

    ESP_LOGI(kTag, "usage http: listening on 0.0.0.0:%u POST /usage", kPort);
    return true;
}

void glass2_usage_http_stop()
{
    std::lock_guard<std::mutex> lock(s_server_mutex);
    if (s_server == nullptr) {
        return;
    }

    httpd_stop(s_server);
    s_server = nullptr;
    ESP_LOGI(kTag, "usage http: stopped");
}
