#include "pressure_manager.h"
#include "config.h"
#include "device_state.h"
namespace {uint32_t lastGoodMs=0;bool everGood=false;}
void pressureManagerBegin(){lastGoodMs=0;everGood=false;}
void pressureManagerRecord(bool ready,int32_t raw,uint32_t now){if(!ready)return;if(deviceStateMutex)xSemaphoreTake(deviceStateMutex,portMAX_DELAY);lastGoodMs=now;everGood=true;deviceState.pressureRaw=raw;deviceState.pressureMmhg=pressureMmHgEstimate(raw);deviceState.pressureAvailable=true;deviceState.pressureMmhgAvailable=PRESSURE_CALIBRATION_CONFIGURED;if(deviceStateMutex)xSemaphoreGive(deviceStateMutex);}
bool pressureSensorConnected(uint32_t now){if(deviceStateMutex)xSemaphoreTake(deviceStateMutex,portMAX_DELAY);bool connected=everGood&&(uint32_t)(now-lastGoodMs)<=1000;if(deviceStateMutex)xSemaphoreGive(deviceStateMutex);return connected;}
bool pressureMmHgAvailable(){return PRESSURE_CALIBRATION_CONFIGURED;}
float pressureMmHgEstimate(int32_t raw){const float psi=((float)raw-(float)PRESSURE_ZERO_OFFSET_COUNTS)/PRESSURE_COUNTS_PER_PSI;return psi*51.7149f;}
int32_t latestPressureRaw(){return deviceStateSnapshot().pressureRaw;}
