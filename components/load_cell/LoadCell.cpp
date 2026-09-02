#include "LoadCell.hpp"

#include <cmath>

void LoadCell::updateRaw(int32_t raw, TickType_t updated_at)
{
    taskENTER_CRITICAL(&lock_);
    raw_ = raw;
    updated_at_ = updated_at;
    valid_ = true;
    taskEXIT_CRITICAL(&lock_);
}

void LoadCell::updateIndicatorWeight(int32_t raw, float weight_g,
                                     TickType_t updated_at)
{
    taskENTER_CRITICAL(&lock_);
    raw_ = raw;
    indicatorWeightG_ = weight_g;
    indicatorWeightValid_ = true;
    updated_at_ = updated_at;
    valid_ = true;
    taskEXIT_CRITICAL(&lock_);
}

LoadCellReading LoadCell::reading() const
{
    LoadCellReading result{};
    taskENTER_CRITICAL(&lock_);
    result.raw = raw_;
    result.updated_at = updated_at_;
    result.valid = valid_;
    result.weight_g = indicatorWeightValid_ ? indicatorWeightG_ : calculateWeight(raw_);
    taskEXIT_CRITICAL(&lock_);
    return result;
}

void LoadCell::tare()
{
    taskENTER_CRITICAL(&lock_);
    calibration_.zero_raw = raw_;
    taskEXIT_CRITICAL(&lock_);
}

bool LoadCell::calibrate(float reference_weight_g)
{
    if (reference_weight_g <= 0.0f) return false;
    taskENTER_CRITICAL(&lock_);
    const bool usable = valid_ && raw_ != calibration_.zero_raw;
    if (usable) {
        calibration_.span_raw = raw_;
        calibration_.reference_weight_deci_g =
            static_cast<int32_t>(std::lround(reference_weight_g * 10.0f));
    }
    taskEXIT_CRITICAL(&lock_);
    return usable;
}

void LoadCell::setCalibration(const LoadCellCalibration &calibration)
{
    taskENTER_CRITICAL(&lock_);
    calibration_ = calibration;
    taskEXIT_CRITICAL(&lock_);
}

LoadCellCalibration LoadCell::calibration() const
{
    taskENTER_CRITICAL(&lock_);
    const LoadCellCalibration result = calibration_;
    taskEXIT_CRITICAL(&lock_);
    return result;
}

float LoadCell::calculateWeight(int32_t raw) const
{
    const int32_t counts = calibration_.span_raw - calibration_.zero_raw;
    if (counts == 0 || calibration_.reference_weight_deci_g <= 0) return 0.0f;
    return static_cast<float>(raw - calibration_.zero_raw) *
           (calibration_.reference_weight_deci_g / 10.0f) /
           static_cast<float>(counts);
}
