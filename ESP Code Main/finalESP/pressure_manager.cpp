#include "pressure_manager.h"
#include "config.h"
#include "device_state.h"
namespace {uint32_t lastGoodMs=0;bool everGood=false;}
void pressureManagerBegin(){lastGoodMs=0;everGood=false;}
void pressureManagerRecord(bool ready,int32_t raw,uint32_t now){if(!ready)return;if(deviceStateMutex)xSemaphoreTake(deviceStateMutex,portMAX_DELAY);lastGoodMs=now;everGood=true;deviceState.pressureRaw=raw;deviceState.pressureMmhg=pressureMmHgEstimate(raw);deviceState.pressureUpdatedAtMs=now;deviceState.pressureAvailable=true;deviceState.pressureSaturated=raw==HX710B_SIGNED_POSITIVE_SATURATION||raw==HX710B_SIGNED_NEGATIVE_SATURATION;deviceState.pressureMmhgAvailable=PRESSURE_CALIBRATION_CONFIGURED&&isfinite(deviceState.pressureMmhg);if(deviceStateMutex)xSemaphoreGive(deviceStateMutex);}
bool pressureSensorConnected(uint32_t now){if(deviceStateMutex)xSemaphoreTake(deviceStateMutex,portMAX_DELAY);bool connected=everGood&&(uint32_t)(now-lastGoodMs)<=250;if(deviceStateMutex)xSemaphoreGive(deviceStateMutex);return connected;}
bool pressureMmHgAvailable(){return PRESSURE_CALIBRATION_CONFIGURED;}
float pressureMmHgEstimate(int32_t raw){if(raw<MIN_PLAUSIBLE_PRESSURE_RAW||raw==HX710B_SIGNED_POSITIVE_SATURATION||raw==HX710B_SIGNED_NEGATIVE_SATURATION)return NAN;return PRESSURE_CALIBRATION_INTERCEPT_MMHG+(PRESSURE_CALIBRATION_MMHG_PER_COUNT*(float)raw);}
int32_t latestPressureRaw(){return deviceStateSnapshot().pressureRaw;}
