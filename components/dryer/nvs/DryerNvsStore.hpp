#pragma once

#include <cstdint>
#include "LoadCell.hpp"
#include "type_def.h"

class DryerNvsStore {
public:
    bool saveRuntime(const DryerNvsRuntime &value) const;
    bool loadRuntime(DryerNvsRuntime *value) const;
    bool saveCalibration(const DryerNvsCalibration &value) const;
    bool loadCalibration(DryerNvsCalibration *value) const;
    bool saveLoadCellCalibration(const LoadCellCalibration &value) const;
    bool loadLoadCellCalibration(LoadCellCalibration *value) const;
    bool saveCoolingSettings(const DryerNvsCoolingSettings &value) const;
    bool loadCoolingSettings(DryerNvsCoolingSettings *value) const;
    bool saveEquipmentName(const char *name) const;
    bool loadEquipmentName(char *name, size_t size) const;
};
