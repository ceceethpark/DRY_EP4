#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "type_def.h"
#include "config.h"

class LoadCell;

// JC1060P470C-I-W-Y board wiring: GPIO26 -> TX1, GPIO27 <- RX1.
// The 74LVC1G132/Q3 circuit generates MAX485 DE/RE automatically from TX1_1.

class RS485Sensor {
public:
    static constexpr uint8_t kControlAddress = MODBUS_CONTROL_ADDRESS;
    static constexpr uint8_t kMonitorFirstAddress = MODBUS_MONITOR_FIRST_ADDRESS;
    static constexpr uint8_t kMonitorLastAddress = MODBUS_MONITOR_LAST_ADDRESS;
    static constexpr size_t kMonitorCount = DRYER_MONITOR_COUNT;
    static constexpr size_t kSensorCount = 1 + kMonitorCount;

    enum class Role : uint8_t {
        DryerTemperatureControl,
        Monitoring,
    };

    struct Reading {
        uint8_t slaveAddress = 0;
        Role role = Role::Monitoring;
        float temperatureC = 0.0F;
        float humidityPercent = 0.0F;
        TickType_t updatedAt = 0;
        bool valid = false;
    };

    RS485Sensor() = default;
    ~RS485Sensor();

    RS485Sensor(const RS485Sensor &) = delete;
    RS485Sensor &operator=(const RS485Sensor &) = delete;

    // Installs UART1 in RS485 half-duplex mode and starts background polling.
    esp_err_t start();
    void stop();
    bool isRunning() const { return task_ != nullptr; }

    // Address 100: the only reading intended for dryer temperature control.
    Reading controlReading() const;

    // index 0..5 maps to monitoring addresses 101..106.
    Reading monitoringReading(size_t index) const;

    // Direct address lookup for addresses 100..106.
    bool reading(uint8_t slaveAddress, Reading &out) const;
    void setLoadCell(LoadCell *loadCell) { loadCell_ = loadCell; }
    uint8_t controlFailureCount() const;
    uint8_t loadCellFailureCount() const;

private:
    static void taskEntry(void *context);
    void pollLoop();
    esp_err_t poll(uint8_t slaveAddress, Reading &out);
    esp_err_t pollLoadCell();
    static uint16_t crc16(const uint8_t *data, size_t length);

    mutable SemaphoreHandle_t mutex_ = nullptr;
    TaskHandle_t task_ = nullptr;
    volatile bool stopRequested_ = false;
    Reading readings_[kSensorCount]{};
    LoadCell *loadCell_ = nullptr;
    uint8_t controlFailureCount_ = 0;
    uint8_t loadCellFailureCount_ = 0;
};
