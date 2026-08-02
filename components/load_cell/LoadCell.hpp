#pragma once

#include <cstdint>
#include "freertos/FreeRTOS.h"
#include "type_def.h"

class LoadCell {
public:
    static constexpr uint8_t kModbusSlaveAddress = MODBUS_LOAD_CELL_ADDRESS;
    static constexpr uint16_t kWeightRegister = MODBUS_LOAD_CELL_WEIGHT_REG;

    void updateRaw(int32_t raw, TickType_t updated_at);
    LoadCellReading reading() const;

    void tare();
    bool calibrate(float reference_weight_g);
    void setCalibration(const LoadCellCalibration &calibration);
    LoadCellCalibration calibration() const;

private:
    float calculateWeight(int32_t raw) const;
    mutable portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
    int32_t raw_ = 0;
    TickType_t updated_at_ = 0;
    bool valid_ = false;
    LoadCellCalibration calibration_{};
};
