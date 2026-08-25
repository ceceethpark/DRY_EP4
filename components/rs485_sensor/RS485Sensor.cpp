#include "RS485Sensor.hpp"
#include "LoadCell.hpp"

#include "driver/uart.h"
#include "esp_log.h"

namespace {
constexpr const char *kTag = "RS485Sensor";
constexpr uart_port_t kUart = static_cast<uart_port_t>(HW_RS485_UART_NUM);
constexpr TickType_t kResponseTimeout = pdMS_TO_TICKS(350);
constexpr TickType_t kCycleDelay = pdMS_TO_TICKS(1000);
}

RS485Sensor::~RS485Sensor() { stop(); }

esp_err_t RS485Sensor::start()
{
    if (task_ != nullptr) return ESP_ERR_INVALID_STATE;
    mutex_ = xSemaphoreCreateMutex();
    if (mutex_ == nullptr) return ESP_ERR_NO_MEM;

    for (size_t i = 0; i < kSensorCount; ++i) {
        readings_[i].slaveAddress = static_cast<uint8_t>(kControlAddress + i);
        readings_[i].role = i == 0 ? Role::DryerTemperatureControl : Role::Monitoring;
    }

    const uart_config_t config = {
        .baud_rate = HW_RS485_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
        .flags = {},
    };
    esp_err_t err = uart_driver_install(kUart, 256, 0, 0, nullptr, 0);
    if (err == ESP_OK) err = uart_param_config(kUart, &config);
    if (err == ESP_OK) {
        err = uart_set_pin(kUart, HW_RS485_TX_GPIO, HW_RS485_RX_GPIO,
                           UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err == ESP_OK) err = uart_set_mode(kUart, UART_MODE_UART);
    if (err != ESP_OK) {
        if (err != ESP_ERR_INVALID_STATE) uart_driver_delete(kUart);
        vSemaphoreDelete(mutex_);
        mutex_ = nullptr;
        return err;
    }

    ESP_LOGI(kTag, "UART%d %d bps TX=GPIO%d RX=GPIO%d automatic DE/RE; control=100, monitoring=101..106",
             HW_RS485_UART_NUM, HW_RS485_BAUD_RATE,
             HW_RS485_TX_GPIO, HW_RS485_RX_GPIO);
    stopRequested_ = false;
    if (xTaskCreate(taskEntry, "rs485_sensor", 4096, this, 5, &task_) != pdPASS) {
        uart_driver_delete(kUart);
        vSemaphoreDelete(mutex_);
        mutex_ = nullptr;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void RS485Sensor::stop()
{
    if (task_ != nullptr) {
        stopRequested_ = true;
        xTaskNotifyGive(task_);
        while (task_ != nullptr) vTaskDelay(pdMS_TO_TICKS(10));
        uart_driver_delete(kUart);
    }
    if (mutex_ != nullptr) {
        vSemaphoreDelete(mutex_);
        mutex_ = nullptr;
    }
}

RS485Sensor::Reading RS485Sensor::controlReading() const
{
    Reading result{};
    reading(kControlAddress, result);
    return result;
}

RS485Sensor::Reading RS485Sensor::monitoringReading(size_t index) const
{
    Reading result{};
    if (index < kMonitorCount) {
        reading(static_cast<uint8_t>(kMonitorFirstAddress + index), result);
    }
    return result;
}

bool RS485Sensor::reading(uint8_t address, Reading &out) const
{
    if (address < kControlAddress || address > kMonitorLastAddress || mutex_ == nullptr) return false;
    if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(20)) != pdTRUE) return false;
    out = readings_[address - kControlAddress];
    xSemaphoreGive(mutex_);
    return true;
}

uint8_t RS485Sensor::controlFailureCount() const
{
    if (mutex_ == nullptr || xSemaphoreTake(mutex_, pdMS_TO_TICKS(20)) != pdTRUE) return 0;
    const uint8_t result = controlFailureCount_;
    xSemaphoreGive(mutex_);
    return result;
}

uint8_t RS485Sensor::loadCellFailureCount() const
{
    if (mutex_ == nullptr || xSemaphoreTake(mutex_, pdMS_TO_TICKS(20)) != pdTRUE) return 0;
    const uint8_t result = loadCellFailureCount_;
    xSemaphoreGive(mutex_);
    return result;
}

void RS485Sensor::taskEntry(void *context)
{
    static_cast<RS485Sensor *>(context)->pollLoop();
}

void RS485Sensor::pollLoop()
{
    while (!stopRequested_) {
        for (uint8_t address = kControlAddress;
             address <= kMonitorLastAddress && !stopRequested_; ++address) {
            Reading value{};
            value.slaveAddress = address;
            value.role = address == kControlAddress
                             ? Role::DryerTemperatureControl
                             : Role::Monitoring;
            const esp_err_t err = poll(address, value);
            if (err != ESP_OK) {
                ESP_LOGW(kTag, "slave %u: %s", address, esp_err_to_name(err));
            }
            if (xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE) {
                readings_[address - kControlAddress] = value;
                if (address == kControlAddress) {
                    controlFailureCount_ = err == ESP_OK ? 0 :
                        (controlFailureCount_ < UINT8_MAX ? controlFailureCount_ + 1 : UINT8_MAX);
                }
                xSemaphoreGive(mutex_);
            }
        }
        if (!stopRequested_ && loadCell_ != nullptr) {
            const esp_err_t err = pollLoadCell();
            if (err != ESP_OK) {
                ESP_LOGW(kTag, "load cell slave %u: %s",
                         LoadCell::kModbusSlaveAddress, esp_err_to_name(err));
            }
            if (xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE) {
                loadCellFailureCount_ = err == ESP_OK ? 0 :
                    (loadCellFailureCount_ < UINT8_MAX ? loadCellFailureCount_ + 1 : UINT8_MAX);
                xSemaphoreGive(mutex_);
            }
        }
        ulTaskNotifyTake(pdTRUE, kCycleDelay);
    }
    task_ = nullptr;
    vTaskDelete(nullptr);
}

esp_err_t RS485Sensor::pollLoadCell()
{
    /* Temperature sensors use 8N1 while AD500A Modbus requires 8E1.
       Switch only for this transaction and always restore 8N1 on return. */
    if (uart_set_parity(kUart, UART_PARITY_EVEN) != ESP_OK) return ESP_FAIL;
    struct RestoreParity {
        ~RestoreParity() { uart_set_parity(kUart, UART_PARITY_DISABLE); }
    } restoreParity;
    const uint8_t address = LoadCell::kModbusSlaveAddress;
    /* AD500A input registers 1..4 (PDU 0..3): decimal position, unit,
       and signed 32-bit displayed weight. */
    const uint16_t reg = LoadCell::kWeightRegister;
    uint8_t request[8] = {address, 0x04,
                          static_cast<uint8_t>(reg >> 8), static_cast<uint8_t>(reg),
                          0, 4, 0, 0};
    const uint16_t requestCrc = crc16(request, 6);
    request[6] = static_cast<uint8_t>(requestCrc);
    request[7] = static_cast<uint8_t>(requestCrc >> 8);
    uart_flush_input(kUart);
    if (uart_write_bytes(kUart, request, sizeof(request)) != sizeof(request)) return ESP_FAIL;
    if (uart_wait_tx_done(kUart, pdMS_TO_TICKS(100)) != ESP_OK) return ESP_ERR_TIMEOUT;
    uint8_t response[13]{};
    const int received = uart_read_bytes(kUart, response, sizeof(response), kResponseTimeout);
    if (received != sizeof(response)) return ESP_ERR_TIMEOUT;
    if (response[0] != address || response[1] != 0x04 || response[2] != 8)
        return ESP_ERR_INVALID_RESPONSE;
    const uint16_t receivedCrc = response[11] | (static_cast<uint16_t>(response[12]) << 8);
    if (crc16(response, 11) != receivedCrc) return ESP_ERR_INVALID_CRC;
    const uint16_t decimalPosition=(static_cast<uint16_t>(response[3])<<8)|response[4];
    const uint16_t unit=(static_cast<uint16_t>(response[5])<<8)|response[6];
    const uint32_t rawUnsigned=(static_cast<uint32_t>(response[7])<<24)|
        (static_cast<uint32_t>(response[8])<<16)|(static_cast<uint32_t>(response[9])<<8)|response[10];
    if(decimalPosition>5||unit>2)return ESP_ERR_INVALID_RESPONSE;
    float weight=static_cast<float>(static_cast<int32_t>(rawUnsigned));
    for(uint16_t i=0;i<decimalPosition;i++)weight*=0.1F;
    const float grams=unit==0?weight*1000.0F:unit==1?weight:weight*1000000.0F;
    loadCell_->updateIndicatorWeight(static_cast<int32_t>(rawUnsigned),grams,xTaskGetTickCount());
    return ESP_OK;
}

esp_err_t RS485Sensor::poll(uint8_t address, Reading &out)
{
    // FC03: manual registers 1..2 (PDU 0..1), humidity then temperature.
    uint8_t request[8] = {address, 0x03, 0, 0, 0, 2, 0, 0};
    const uint16_t requestCrc = crc16(request, 6);
    request[6] = static_cast<uint8_t>(requestCrc);
    request[7] = static_cast<uint8_t>(requestCrc >> 8);
    uart_flush_input(kUart);
    if (uart_write_bytes(kUart, request, sizeof(request)) != sizeof(request)) return ESP_FAIL;
    if (uart_wait_tx_done(kUart, pdMS_TO_TICKS(100)) != ESP_OK) return ESP_ERR_TIMEOUT;

    uint8_t response[9]{};
    const int received = uart_read_bytes(kUart, response, sizeof(response), kResponseTimeout);
    if (received != sizeof(response)) {
        if (received > 0) {
            ESP_LOGW(kTag, "slave %u partial RX: %d bytes", address, received);
            ESP_LOG_BUFFER_HEXDUMP(kTag, response, received, ESP_LOG_WARN);
        }
        return ESP_ERR_TIMEOUT;
    }
    if (response[0] != address || response[1] != 0x03 || response[2] != 4) return ESP_ERR_INVALID_RESPONSE;
    const uint16_t receivedCrc = response[7] | (static_cast<uint16_t>(response[8]) << 8);
    if (crc16(response, 7) != receivedCrc) return ESP_ERR_INVALID_CRC;

    const int16_t humidity = static_cast<int16_t>((response[3] << 8) | response[4]);
    const int16_t temperature = static_cast<int16_t>((response[5] << 8) | response[6]);
    out.humidityPercent = humidity * 0.1F;
    out.temperatureC = temperature * 0.1F;
    out.updatedAt = xTaskGetTickCount();
    out.valid = true;
    return ESP_OK;
}

uint16_t RS485Sensor::crc16(const uint8_t *data, size_t length)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1U) ? static_cast<uint16_t>((crc >> 1) ^ 0xA001U)
                             : static_cast<uint16_t>(crc >> 1);
        }
    }
    return crc;
}
