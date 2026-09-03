#include "MqttClient.hpp"
#include "MqttConfig.hpp"
#include "config.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <cinttypes>
#include <cstdlib>

#include "esp_log.h"
#include "esp_mac.h"
#include "cJSON.h"

namespace {
constexpr const char *TAG = "MqttClient";

const char *dryStateName(DryState state)
{
    switch (state) {
    case DRY_PREPARE: return "PREPARE";
    case DRY_PREHEAT: return "PREHEAT";
    case DRY_RUN: return "RUN";
    case DRY_COOL: return "COOL";
    case DRY_FINISH: return "FINISH";
    default: return "UNKNOWN";
    }
}

bool getEquipmentCode(char *out, size_t outSize)
{
    if (!out || outSize < MqttConfig::EQUIPMENT_CODE_LENGTH + 1U) return false;
    uint8_t mac[6] = {};
    if (esp_efuse_mac_get_default(mac) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read factory base MAC");
        return false;
    }
    snprintf(out, outSize, "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return true;
}

uint16_t readU16(const uint8_t *p)
{
    return static_cast<uint16_t>(p[0]) |
           (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t readU32(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t readU64(const uint8_t *p)
{
    return static_cast<uint64_t>(readU32(p)) |
           (static_cast<uint64_t>(readU32(p + 4)) << 32);
}

float readFloat(const uint8_t *p)
{
    const uint32_t raw = readU32(p);
    float value;
    memcpy(&value, &raw, sizeof(value));
    return value;
}

bool extractTime(const char *payload, int length, char *out, size_t outSize)
{
    if (!payload || length <= 0 || outSize < MqttConfig::TIME_TEXT_SIZE) {
        return false;
    }

    char input[128] = {};
    const size_t copyLen = static_cast<size_t>(length) < sizeof(input) - 1
                               ? static_cast<size_t>(length)
                               : sizeof(input) - 1;
    memcpy(input, payload, copyLen);

    // Accept either a plain timestamp or a small JSON object containing "time".
    const char *value = input;
    const char *timeKey = strstr(input, "\"time\"");
    if (timeKey) {
        value = strchr(timeKey + 6, ':');
        if (!value) return false;
        ++value;
        while (*value == ' ' || *value == '\t' || *value == '"') ++value;
    }

    int year, month, day, hour, minute, second;
    if (sscanf(value, "%4d-%2d-%2d%*[ T]%2d:%2d:%2d",
               &year, &month, &day, &hour, &minute, &second) != 6) {
        return false;
    }
    if (month < 1 || month > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 59) {
        return false;
    }

    snprintf(out, outSize, "%04d-%02d-%02d %02d:%02d:%02d",
             year, month, day, hour, minute, second);
    return true;
}
}  // namespace

MqttClient::MqttClient()
    : _client(nullptr),
      _lock(nullptr),
      _connected(false),
      _hasServerTime(false),
      _equipmentCode{},
      _clientId{},
      _commandTopic{},
      _commands{},
      _commandRead(0),
      _commandWrite(0),
      _commandCount(0),
      _serverTime{},
      _controlHistory{},
      _controlHistoryCount(0)
{
}

MqttClient::~MqttClient()
{
    stop();
    if (_lock) vSemaphoreDelete(_lock);
}

bool MqttClient::start()
{
    if (_client) return true;
    if (!getEquipmentCode(_equipmentCode, sizeof(_equipmentCode))) return false;
    snprintf(_clientId, sizeof(_clientId), MqttConfig::CLIENT_ID_FORMAT,
             _equipmentCode);
    snprintf(_commandTopic, sizeof(_commandTopic),
             MqttConfig::COMMAND_TOPIC_FORMAT, _equipmentCode);
    if (!_lock) {
        _lock = xSemaphoreCreateMutex();
        if (!_lock) {
            ESP_LOGE(TAG, "Failed to create MQTT mutex");
            return false;
        }
    }

    const esp_mqtt_client_config_t config = {
        .broker = {
            .address = {
                .uri = MqttConfig::BROKER_URI,
            },
        },
        .credentials = {
            .client_id = _clientId,
        },
        .session = {
            .keepalive = MqttConfig::KEEPALIVE_SECONDS,
        },
        .network = {
            .reconnect_timeout_ms = MqttConfig::RECONNECT_TIMEOUT_MS,
        },
    };

    _client = esp_mqtt_client_init(&config);
    if (!_client) {
        ESP_LOGE(TAG, "Failed to create MQTT client");
        return false;
    }
    esp_mqtt_client_register_event(
        _client, static_cast<esp_mqtt_event_id_t>(ESP_EVENT_ANY_ID),
        eventHandler, this);
    if (esp_mqtt_client_start(_client) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client");
        esp_mqtt_client_destroy(_client);
        _client = nullptr;
        return false;
    }
    return true;
}

void MqttClient::stop()
{
    if (!_client) return;
    esp_mqtt_client_stop(_client);
    esp_mqtt_client_destroy(_client);
    _client = nullptr;
    _connected = false;
}

bool MqttClient::getServerTime(char *out, size_t outSize) const
{
    if (!out || outSize == 0 || !_lock) return false;
    xSemaphoreTake(_lock, portMAX_DELAY);
    const bool available = _hasServerTime;
    if (available) {
        snprintf(out, outSize, "%s", _serverTime);
    }
    xSemaphoreGive(_lock);
    return available;
}

bool MqttClient::getControlHistory(char *out, size_t outSize) const
{
    if (!out || outSize == 0 || !_lock) return false;
    xSemaphoreTake(_lock, portMAX_DELAY);
    out[0] = '\0';
    for (size_t i = 0; i < _controlHistoryCount; ++i) {
        const size_t used = strlen(out);
        if (used + 1 >= outSize) break;
        if (i == 0)
            snprintf(out + used,outSize-used,"#00D8E8 %s#",_controlHistory[i]);
        else
            snprintf(out + used,outSize-used,"\n%s",_controlHistory[i]);
    }
    const bool available = _controlHistoryCount > 0;
    xSemaphoreGive(_lock);
    return available;
}

void MqttClient::recordHistoryEntry(const char *message)
{
    if (!_lock || !message || !message[0]) return;
    char timestamp[12] = "--:--:--";
    const time_t now = time(nullptr);
    if (now > 1600000000) {
        struct tm localTime {};
        localtime_r(&now, &localTime);
        strftime(timestamp, sizeof(timestamp), "%H:%M:%S", &localTime);
    }
    char entry[96] = {};
    snprintf(entry,sizeof(entry),"%s %s",timestamp,message);
    xSemaphoreTake(_lock,portMAX_DELAY);
    for(size_t i=CONTROL_HISTORY_DEPTH-1;i>0;--i)
        memcpy(_controlHistory[i],_controlHistory[i-1],sizeof(_controlHistory[i]));
    snprintf(_controlHistory[0],sizeof(_controlHistory[0]),"%s",entry);
    if(_controlHistoryCount<CONTROL_HISTORY_DEPTH)++_controlHistoryCount;
    xSemaphoreGive(_lock);
}

void MqttClient::recordCommandHistory(const MqttCommand &command)
{
    const char *damper = command.damperMode == 0 ? "AUTO" :
                          command.damperMode == 1 ? "OPEN" :
                          command.damperMode == 2 ? "CLOSE" : "KEEP";
    char entry[80] = {};
    switch (command.type) {
    case MqttCommandType::SetTemperature:
        snprintf(entry,sizeof(entry),"<- SET TEMP %ldC",
                 static_cast<long>(command.value)); break;
    case MqttCommandType::SetTime:
        snprintf(entry,sizeof(entry),"<- SET TIME %ldm",
                 static_cast<long>(command.value)); break;
    case MqttCommandType::PreheatStart:
        snprintf(entry,sizeof(entry),"<- PREHEAT %ldC %ldm %s",
                 static_cast<long>(command.temperature),
                 static_cast<long>(command.timeMinutes),damper); break;
    case MqttCommandType::DryStart:
        snprintf(entry,sizeof(entry),"<- DRY START %ldC %ldm %s",
                 static_cast<long>(command.temperature),
                 static_cast<long>(command.timeMinutes),damper); break;
    case MqttCommandType::DryStop:
        snprintf(entry,sizeof(entry),"<- DRY STOP %s",damper); break;
    case MqttCommandType::DamperAuto:
    case MqttCommandType::DamperOpen:
    case MqttCommandType::DamperClose:
        snprintf(entry,sizeof(entry),"<- DAMPER %s",damper); break;
    case MqttCommandType::SetEquipmentName:
        snprintf(entry,sizeof(entry),"<- EQUIPMENT ID %06ld",
                 static_cast<long>(command.equipmentId)); break;
    default: return;
    }
    recordHistoryEntry(entry);
}

bool MqttClient::popCommand(MqttCommand &command)
{
    if (!_lock) return false;
    xSemaphoreTake(_lock, portMAX_DELAY);
    const bool available = _commandCount > 0;
    if (available) {
        command = _commands[_commandRead];
        _commandRead = (_commandRead + 1U) % 8U;
        --_commandCount;
    }
    xSemaphoreGive(_lock);
    return available;
}

bool MqttClient::publishEvent(uint16_t eventValue)
{
    if (!_client || !_connected) {
        ESP_LOGW(TAG, "Event publish skipped: MQTT is disconnected");
        return false;
    }

    char topic[64];
    char payload[512];
    if (_equipmentCode[0] == '\0' &&
        !getEquipmentCode(_equipmentCode, sizeof(_equipmentCode))) return false;
    snprintf(topic, sizeof(topic), MqttConfig::EVENT_TOPIC_FORMAT,
             _equipmentCode);
    const EVENT_INFO event{.data = eventValue};
    snprintf(payload, sizeof(payload),
             "{\"data\":%u,\"doorOpen\":%s,\"controlSensorError\":%s,"
             "\"weightSensorError\":%s,\"thermoState\":%s,\"fanMinError\":%s,"
             "\"fanMaxError\":%s,\"heater1Error\":%s,\"heater2Error\":%s,"
             "\"memError\":%s,\"underHeat\":%s,\"overHeat\":%s,"
             "\"fanRelayOn\":%s,\"heaterRelayOn\":%s,\"damperRelayOn\":%s,"
             "\"reserved14\":%s,\"mqttConnect\":%s}",
             static_cast<unsigned>(event.data),
             event.door_open ? "true" : "false",
             event.control_sensor_error ? "true" : "false",
             event.weight_sensor_error ? "true" : "false",
             event.thermo_state ? "true" : "false",
             event.fan_min_error ? "true" : "false",
             event.fan_max_error ? "true" : "false",
             event.heater1_error ? "true" : "false",
             event.heater2_error ? "true" : "false",
             event.mem_error ? "true" : "false",
             event.under_heat ? "true" : "false",
             event.over_heat ? "true" : "false",
             event.fan_relay_on ? "true" : "false",
             event.heater_relay_on ? "true" : "false",
             event.damper_relay_on ? "true" : "false",
             event.x14 ? "true" : "false",
             event.mqtt_connect ? "true" : "false");

    const int messageId = esp_mqtt_client_publish(
        _client, topic, payload, 0, MqttConfig::QOS, 0);
    if (messageId < 0) {
        ESP_LOGE(TAG, "Event publish failed: topic=%s payload=%s", topic,
                 payload);
        return false;
    }
    ESP_LOGI(TAG, "Event published: topic=%s payload=%s msg_id=%d", topic,
             payload, messageId);
    char history[40];
    snprintf(history,sizeof(history),"-> EVENT %u",static_cast<unsigned>(eventValue));
    recordHistoryEntry(history);
    return true;
}

bool MqttClient::publishTelemetry(const DryerSensorValues &values)
{
    if (!_client || !_connected) {
        ESP_LOGW(TAG, "Telemetry publish skipped: MQTT is disconnected");
        return false;
    }

    char topic[64];
    if (_equipmentCode[0] == '\0' &&
        !getEquipmentCode(_equipmentCode, sizeof(_equipmentCode))) return false;
    snprintf(topic, sizeof(topic), MqttConfig::TELEMETRY_TOPIC_FORMAT,
             _equipmentCode);

    char payload[1536];
    int used = snprintf(payload, sizeof(payload),
        "{\"sequence\":%" PRIu32 ",\"updatedAtTicks\":%" PRIu32
        ",\"temperatureHumidity\":{",
        values.sequence, static_cast<uint32_t>(values.updated_at));
    if (used < 0 || used >= static_cast<int>(sizeof(payload))) return false;

    for (size_t i = 0; i < DRYER_SENSOR_COUNT; ++i) {
        const TemperatureHumidityValue &sensor = values.temperature_humidity[i];
        const int written = snprintf(payload + used, sizeof(payload) - used,
            "%s\"sensor%u\":{\"temperatureC\":%.2f,\"humidityPct\":%.2f,"
            "\"valid\":%s,\"updatedAtTicks\":%" PRIu32 "}",
            i == 0 ? "" : ",", static_cast<unsigned>(i + 1),
            static_cast<double>(sensor.temperature_c),
            static_cast<double>(sensor.humidity_pct), sensor.valid ? "true" : "false",
            static_cast<uint32_t>(sensor.updated_at));
        if (written < 0 || written >= static_cast<int>(sizeof(payload) - used)) return false;
        used += written;
    }

    const int written = snprintf(payload + used, sizeof(payload) - used,
        "},\"loadCell\":{\"raw\":%" PRId32 ",\"weightG\":%" PRId32 ",\"valid\":%s},"
        "\"fanVelocity\":{\"raw\":%d,\"constant\":%" PRId32 ",\"velocity\":%.2f,\"valid\":%s},"
        "\"door\":{\"rawLevel\":%d,\"open\":%s,\"valid\":%s},"
        "\"event\":{\"data\":%u,\"doorOpen\":%s,\"controlSensorError\":%s,"
        "\"weightSensorError\":%s,\"thermoState\":%s,\"fanMinError\":%s,"
        "\"fanMaxError\":%s,\"heater1Error\":%s,\"heater2Error\":%s,"
        "\"memError\":%s,\"underHeat\":%s,\"overHeat\":%s,"
        "\"fanRelayOn\":%s,\"heaterRelayOn\":%s,\"damperRelayOn\":%s,"
        "\"reserved14\":%s,\"mqttConnect\":%s},"
        "\"remainingTimeMinutes\":%" PRId32 ","
        "\"targetTemperatureC\":%.1f,"
        "\"operatingState\":%d,\"operatingStateName\":\"%s\","
        "\"damperPercent\":%.1f}",
        values.load_cell.raw, values.load_cell.weight_g,
        values.load_cell.valid ? "true" : "false", values.fan_velocity.raw,
        values.fan_velocity.reference_adc, static_cast<double>(values.fan_velocity.velocity_ms),
        values.fan_velocity.valid ? "true" : "false", values.door.raw_level,
        values.door.open ? "true" : "false", values.door.valid ? "true" : "false",
        static_cast<unsigned>(values.event.data),
        values.event.door_open ? "true" : "false",
        values.event.control_sensor_error ? "true" : "false",
        values.event.weight_sensor_error ? "true" : "false",
        values.event.thermo_state ? "true" : "false",
        values.event.fan_min_error ? "true" : "false",
        values.event.fan_max_error ? "true" : "false",
        values.event.heater1_error ? "true" : "false",
        values.event.heater2_error ? "true" : "false",
        values.event.mem_error ? "true" : "false",
        values.event.under_heat ? "true" : "false",
        values.event.over_heat ? "true" : "false",
        values.event.fan_relay_on ? "true" : "false",
        values.event.heater_relay_on ? "true" : "false",
        values.event.damper_relay_on ? "true" : "false",
        values.event.x14 ? "true" : "false",
        values.event.mqtt_connect ? "true" : "false",
        values.remaining_time_min,
        static_cast<double>(values.target_temperature_c),
        static_cast<int>(values.operating_state),
        dryStateName(values.operating_state),
        static_cast<double>(values.damper_percent));
    if (written < 0 || written >= static_cast<int>(sizeof(payload) - used)) return false;

    const int messageId = esp_mqtt_client_publish(
        _client, topic, payload, 0, MqttConfig::QOS, 0);
    if (messageId < 0) {
        ESP_LOGE(TAG, "Telemetry publish failed: topic=%s", topic);
        return false;
    }
    ESP_LOGI(TAG, "Telemetry published: topic=%s bytes=%d msg_id=%d",
             topic, used + written, messageId);
    recordHistoryEntry("-> TELEMETRY");
    return true;
}

void MqttClient::eventHandler(void *args, esp_event_base_t,
                              int32_t, void *eventData)
{
    static_cast<MqttClient *>(args)->handleEvent(
        static_cast<esp_mqtt_event_handle_t>(eventData));
}

void MqttClient::handleEvent(esp_mqtt_event_handle_t event)
{
    switch (static_cast<esp_mqtt_event_id_t>(event->event_id)) {
    case MQTT_EVENT_CONNECTED:
        _connected = true;
        esp_mqtt_client_subscribe(_client, _commandTopic, MqttConfig::QOS);
        ESP_LOGI(TAG, "Subscribed command: %s", _commandTopic);
        break;
    case MQTT_EVENT_DISCONNECTED:
        _connected = false;
        ESP_LOGW(TAG, "Disconnected; automatic reconnect pending");
        break;
    case MQTT_EVENT_DATA:
        if (event->topic_len == static_cast<int>(strlen(_commandTopic)) &&
            memcmp(event->topic, _commandTopic,
                   static_cast<size_t>(event->topic_len)) == 0) {
            storeCommand(event->data, event->data_len);
        }
        break;
    default:
        break;
    }
}

void MqttClient::storeCommand(const char *payload, int length)
{
    if (!payload || length <= 0 || length >= 512) return;
    char input[512] = {};
    const size_t n = static_cast<size_t>(length) < sizeof(input) - 1U
                         ? static_cast<size_t>(length) : sizeof(input) - 1U;
    memcpy(input, payload, n);

    MqttCommand command{MqttCommandType::None, 0, 0, 0, -1, false, false, 0, {}};
    cJSON *root = cJSON_ParseWithLengthOpts(input, n + 1U, nullptr, 1);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        ESP_LOGW(TAG, "Ignored malformed command JSON");
        return;
    }
    const cJSON *commandItem = cJSON_GetObjectItemCaseSensitive(root, "command");
    if (!cJSON_IsString(commandItem) || !commandItem->valuestring) {
        cJSON_Delete(root);
        ESP_LOGW(TAG, "Command JSON requires a string 'command'");
        return;
    }
    auto readInteger = [&](const char *key, int32_t &value) -> bool {
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
        if (!cJSON_IsNumber(item)) return false;
        value = static_cast<int32_t>(item->valuedouble);
        return static_cast<double>(value) == item->valuedouble;
    };
    auto readDamper = [&](const char *key) -> int8_t {
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
        if (!cJSON_IsString(item) || !item->valuestring) return -1;
        if (strcmp(item->valuestring, "auto") == 0) return 0;
        if (strcmp(item->valuestring, "open") == 0) return 1;
        if (strcmp(item->valuestring, "close") == 0) return 2;
        return -1;
    };

    const char *name = commandItem->valuestring;
    if (strcmp(name, "set_temp") == 0) {
        command.type = MqttCommandType::SetTemperature;
        if (!readInteger("temp", command.value)) command.type = MqttCommandType::None;
    } else if (strcmp(name, "set_time") == 0) {
        command.type = MqttCommandType::SetTime;
        if (!readInteger("time", command.value)) command.type = MqttCommandType::None;
    } else if (strcmp(name, "preheat_start") == 0) {
        command.type = MqttCommandType::PreheatStart;
        command.hasTemperature = readInteger("preheat_temp", command.temperature);
        command.hasTime = readInteger("preheat_time", command.timeMinutes);
        command.damperMode = readDamper("damper");
    } else if (strcmp(name, "dry_start") == 0) {
        command.type = MqttCommandType::DryStart;
        command.hasTemperature = readInteger("dry_temp", command.temperature);
        command.hasTime = readInteger("dry_time", command.timeMinutes);
        command.damperMode = readDamper("damper");
    } else if (strcmp(name, "dry_stop") == 0) {
        command.type = MqttCommandType::DryStop;
        command.damperMode = readDamper("damper");  // optional on stop
    } else if (strcmp(name, "damper") == 0) {
        const int8_t mode = readDamper("mode");
        command.type = mode == 0 ? MqttCommandType::DamperAuto :
                       mode == 1 ? MqttCommandType::DamperOpen :
                       mode == 2 ? MqttCommandType::DamperClose : MqttCommandType::None;
    } else if (strcmp(name, "set_equipment_name") == 0) {
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(root, "name");
        const bool hasEquipmentId = readInteger("equipment_id", command.equipmentId);
        if (cJSON_IsString(value) && value->valuestring && value->valuestring[0] &&
            strlen(value->valuestring) < sizeof(command.text) &&
            hasEquipmentId && command.equipmentId >= EQUIPMENT_ID_MIN &&
            command.equipmentId <= EQUIPMENT_ID_MAX) {
            snprintf(command.text, sizeof(command.text), "%s", value->valuestring);
            command.type = MqttCommandType::SetEquipmentName;
        }
    }
    cJSON_Delete(root);

    if (command.type == MqttCommandType::None) {
        ESP_LOGW(TAG, "Ignored invalid command: %s", input);
        return;
    }
    xSemaphoreTake(_lock, portMAX_DELAY);
    if (_commandCount == 8U) {
        _commandRead = (_commandRead + 1U) % 8U;
        --_commandCount;
        ESP_LOGW(TAG, "Command queue full; oldest command dropped");
    }
    _commands[_commandWrite] = command;
    _commandWrite = (_commandWrite + 1U) % 8U;
    ++_commandCount;
    xSemaphoreGive(_lock);
    ESP_LOGI(TAG, "Command queued: %s", input);
}

void MqttClient::handleTelemetry(const char *payload, int length)
{
    if (!payload || length != MqttConfig::TELEMETRY_FRAME_SIZE) {
        ESP_LOGW(TAG, "Telemetry size error: received=%d expected=%d",
                 length, MqttConfig::TELEMETRY_FRAME_SIZE);
        if (payload && length > 0) {
            ESP_LOG_BUFFER_HEXDUMP(TAG, payload, length, ESP_LOG_INFO);
        }
        return;
    }

    const auto *frame = reinterpret_cast<const uint8_t *>(payload);
    if (memcmp(frame, "HWDR", 4) != 0) {
        ESP_LOGW(TAG, "Telemetry magic error");
        ESP_LOG_BUFFER_HEXDUMP(TAG, payload, length, ESP_LOG_INFO);
        return;
    }

    const uint16_t headerSize = readU16(frame + 6);
    const uint16_t payloadSize = readU16(frame + 8);
    const uint32_t bootId = readU32(frame + 12);
    const uint32_t sequence = readU32(frame + 16);
    const uint64_t timestampMs = readU64(frame + 20);
    const uint32_t crc32 = readU32(frame + 28);
    const uint8_t *actual = frame + 32;

    ESP_LOGI(TAG,
             "HWDR v%u.%u header=%u payload=%u boot=%" PRIu32
             " seq=%" PRIu32 " timestamp_ms=%" PRIu64 " crc32=%08" PRIx32,
             frame[4], frame[5], headerSize, payloadSize, bootId, sequence,
             timestampMs, crc32);
    ESP_LOGI(TAG,
             "temp[0]=%.1fC hum[0]=%.1f%% preheat=%" PRIu32
             "min dry=%" PRIu32 "min damper=%.1f%% state=%u"
             " door=%u relay=0x%02X",
             readFloat(actual), readFloat(actual + 32),
             readU32(actual + 84), readU32(actual + 88),
             readFloat(actual + 92), actual[96], actual[98], actual[99]);

    // Publisher timestamp is UTC Unix milliseconds. Display it in Korea time.
    time_t seconds = static_cast<time_t>(timestampMs / 1000ULL + 9 * 60 * 60);
    struct tm serverTm {};
    gmtime_r(&seconds, &serverTm);
    char formatted[MqttConfig::TIME_TEXT_SIZE] = {};
    strftime(formatted, sizeof(formatted), "%Y-%m-%d %H:%M:%S", &serverTm);

    xSemaphoreTake(_lock, portMAX_DELAY);
    memcpy(_serverTime, formatted, sizeof(_serverTime));
    _hasServerTime = true;
    for (size_t i = CONTROL_HISTORY_DEPTH - 1; i > 0; --i)
        memcpy(_controlHistory[i], _controlHistory[i - 1], sizeof(_controlHistory[i]));
    snprintf(_controlHistory[0], sizeof(_controlHistory[0]),
             "%02d:%02d:%02d STATE:%u PRE:%" PRIu32 "m DRY:%" PRIu32 "m DAMPER:%.0f%%",
             serverTm.tm_hour, serverTm.tm_min, serverTm.tm_sec,
             actual[96], readU32(actual + 84), readU32(actual + 88), readFloat(actual + 92));
    if (_controlHistoryCount < CONTROL_HISTORY_DEPTH) ++_controlHistoryCount;
    xSemaphoreGive(_lock);
    ESP_LOGI(TAG, "Server time (KST): %s", formatted);
}

void MqttClient::storeServerTime(const char *payload, int length)
{
    char parsed[MqttConfig::TIME_TEXT_SIZE] = {};
    if (!extractTime(payload, length, parsed, sizeof(parsed))) {
        ESP_LOGW(TAG, "Ignored invalid server-time payload");
        return;
    }
    xSemaphoreTake(_lock, portMAX_DELAY);
    memcpy(_serverTime, parsed, sizeof(_serverTime));
    _hasServerTime = true;
    xSemaphoreGive(_lock);
    ESP_LOGI(TAG, "Server time updated: %s", parsed);
}
