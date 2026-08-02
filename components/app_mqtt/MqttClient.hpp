#pragma once

#include <cstddef>
#include <cstdint>
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "type_def.h"

enum class MqttCommandType : uint8_t {
    None, SetTemperature, SetTime, PreheatStart, DryStart, DryStop,
    DamperAuto, DamperOpen, DamperClose, SetEquipmentName
};

struct MqttCommand {
    MqttCommandType type;
    int32_t value;
    int32_t temperature;
    int32_t timeMinutes;
    int8_t damperMode;  // 0=auto, 1=open, 2=close, -1=not supplied
    bool hasTemperature;
    bool hasTime;
    char text[33];
};

class MqttClient {
public:
    MqttClient();
    ~MqttClient();

    bool start();
    void stop();
    bool getServerTime(char *out, size_t outSize) const;
    bool getControlHistory(char *out, size_t outSize) const;
    bool isConnected() const { return _connected; }
    bool publishEvent(uint16_t eventValue);
    bool publishTelemetry(const DryerSensorValues &values);
    bool popCommand(MqttCommand &command);

private:
    esp_mqtt_client_handle_t _client;
    mutable SemaphoreHandle_t _lock;
    volatile bool _connected;
    bool _hasServerTime;
    char _equipmentCode[13];
    char _clientId[32];
    char _commandTopic[64];
    MqttCommand _commands[8];
    size_t _commandRead;
    size_t _commandWrite;
    size_t _commandCount;
    char _serverTime[20];
    char _controlHistory[3][96];
    size_t _controlHistoryCount;

    static void eventHandler(void *args, esp_event_base_t base,
                             int32_t eventId, void *eventData);
    void handleEvent(esp_mqtt_event_handle_t event);
    void storeServerTime(const char *payload, int length);
    void handleTelemetry(const char *payload, int length);
    void storeCommand(const char *payload, int length);
};
