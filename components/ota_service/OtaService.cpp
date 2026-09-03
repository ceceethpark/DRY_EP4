#include "OtaService.hpp"

#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

namespace {
constexpr const char *TAG = "OtaService";
constexpr const char *PRODUCT = "EP6";
constexpr uint16_t DISCOVERY_PORT = 3232;
constexpr uint16_t HTTP_PORT = 3233;

void getDeviceMac(char *text, size_t size)
{
    uint8_t mac[6]{};
    if (esp_efuse_mac_get_default(mac) == ESP_OK) {
        snprintf(text, size, "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        snprintf(text, size, "00:00:00:00:00:00");
    }
}

bool requestTargetsThisDevice(httpd_req_t *request)
{
    char requestedMac[24]{};
    char deviceMac[24]{};
    if (httpd_req_get_hdr_value_str(request, "X-Device-MAC", requestedMac,
                                    sizeof(requestedMac)) != ESP_OK) {
        return false;
    }
    getDeviceMac(deviceMac, sizeof(deviceMac));
    return strcasecmp(requestedMac, deviceMac) == 0;
}
}

bool OtaService::start()
{
    if (_started) return true;

    const esp_err_t validResult = esp_ota_mark_app_valid_cancel_rollback();
    if (validResult != ESP_OK && validResult != ESP_ERR_NOT_SUPPORTED &&
        validResult != ESP_ERR_OTA_ROLLBACK_INVALID_STATE) {
        ESP_LOGW(TAG, "Could not mark running app valid: %s",
                 esp_err_to_name(validResult));
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = HTTP_PORT;
    config.ctrl_port = HTTP_PORT + 1;
    config.stack_size = 8192;
    config.recv_wait_timeout = 15;
    config.send_wait_timeout = 15;
    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "OTA HTTP server start failed");
        return false;
    }

    const httpd_uri_t infoUri = {
        .uri = "/ota/info", .method = HTTP_GET,
        .handler = reinterpret_cast<esp_err_t (*)(httpd_req_t *)>(infoHandler),
        .user_ctx = this};
    const httpd_uri_t updateUri = {
        .uri = "/ota/update", .method = HTTP_POST,
        .handler = reinterpret_cast<esp_err_t (*)(httpd_req_t *)>(updateHandler),
        .user_ctx = this};
    if (httpd_register_uri_handler(server, &infoUri) != ESP_OK ||
        httpd_register_uri_handler(server, &updateUri) != ESP_OK) {
        httpd_stop(server);
        ESP_LOGE(TAG, "OTA HTTP handlers registration failed");
        return false;
    }

    _httpServer = server;
    if (xTaskCreate(discoveryTask, "ota_discovery", 4096, this, 5, nullptr) != pdPASS) {
        httpd_stop(server);
        _httpServer = nullptr;
        ESP_LOGE(TAG, "OTA discovery task start failed");
        return false;
    }
    _started = true;
    ESP_LOGI(TAG, "EP6 OTA ready: UDP %u, HTTP %u, MAC-target protected",
             DISCOVERY_PORT, HTTP_PORT);
    return true;
}

void OtaService::discoveryTask(void *)
{
    const int socketFd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (socketFd < 0) {
        ESP_LOGE(TAG, "Discovery socket creation failed");
        vTaskDelete(nullptr);
        return;
    }
    int reuse = 1;
    setsockopt(socketFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(DISCOVERY_PORT);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(socketFd, reinterpret_cast<sockaddr *>(&local), sizeof(local)) != 0) {
        ESP_LOGE(TAG, "Discovery UDP bind failed");
        close(socketFd);
        vTaskDelete(nullptr);
        return;
    }

    char deviceMac[24]{};
    getDeviceMac(deviceMac, sizeof(deviceMac));
    while (true) {
        char request[96]{};
        sockaddr_in sender{};
        socklen_t senderLength = sizeof(sender);
        const int received = recvfrom(socketFd, request, sizeof(request) - 1, 0,
                                      reinterpret_cast<sockaddr *>(&sender),
                                      &senderLength);
        if (received <= 0) continue;
        request[received] = '\0';
        char product[16]{};
        char requestedMac[24]{};
        if (sscanf(request, "DY_OTA_DISCOVER %15s %23s", product, requestedMac) != 2 ||
            strcmp(product, PRODUCT) != 0 || strcasecmp(requestedMac, deviceMac) != 0) {
            continue;
        }
        char response[160]{};
        snprintf(response, sizeof(response),
                 "{\"product\":\"%s\",\"mac\":\"%s\",\"ota_port\":%u,\"version\":\"%s\"}",
                 PRODUCT, deviceMac, HTTP_PORT, esp_app_get_description()->version);
        sendto(socketFd, response, strlen(response), 0,
               reinterpret_cast<sockaddr *>(&sender), senderLength);
    }
}

int OtaService::infoHandler(void *rawRequest)
{
    auto *request = static_cast<httpd_req_t *>(rawRequest);
    char deviceMac[24]{};
    char response[160]{};
    getDeviceMac(deviceMac, sizeof(deviceMac));
    snprintf(response, sizeof(response),
             "{\"product\":\"%s\",\"mac\":\"%s\",\"version\":\"%s\"}",
             PRODUCT, deviceMac, esp_app_get_description()->version);
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, response);
}

int OtaService::updateHandler(void *rawRequest)
{
    auto *request = static_cast<httpd_req_t *>(rawRequest);
    if (!requestTargetsThisDevice(request)) {
        httpd_resp_send_err(request, HTTPD_403_FORBIDDEN, "Target MAC mismatch");
        return ESP_FAIL;
    }

    const esp_partition_t *partition = esp_ota_get_next_update_partition(nullptr);
    if (!partition || request->content_len <= 0 ||
        static_cast<size_t>(request->content_len) > partition->size) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid firmware size");
        return ESP_FAIL;
    }

    esp_ota_handle_t handle = 0;
    esp_err_t result = esp_ota_begin(partition, request->content_len, &handle);
    if (result != ESP_OK) {
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_FAIL;
    }

    char *buffer = static_cast<char *>(malloc(16384));
    if (!buffer) {
        esp_ota_abort(handle);
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA buffer memory");
        return ESP_FAIL;
    }
    int remaining = request->content_len;
    while (remaining > 0) {
        const int chunk = httpd_req_recv(request, buffer,
                                         remaining < 16384 ? remaining : 16384);
        if (chunk == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (chunk <= 0 || esp_ota_write(handle, buffer, chunk) != ESP_OK) {
            result = ESP_FAIL;
            break;
        }
        remaining -= chunk;
    }
    free(buffer);

    if (result == ESP_OK) result = esp_ota_end(handle);
    else esp_ota_abort(handle);
    if (result == ESP_OK) result = esp_ota_set_boot_partition(partition);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "OTA update failed: %s", esp_err_to_name(result));
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA validation failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA complete, restarting");
    httpd_resp_set_type(request, "application/json");
    httpd_resp_sendstr(request, "{\"ok\":true,\"restarting\":true}");
    xTaskCreate(restartTask, "ota_restart", 2048, nullptr, 5, nullptr);
    return ESP_OK;
}

void OtaService::restartTask(void *)
{
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}
