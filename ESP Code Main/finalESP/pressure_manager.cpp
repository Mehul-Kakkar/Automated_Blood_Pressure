#include "pressure_manager.h"
#include "config.h"
#include "device_state.h"
namespace {uint32_t lastGoodMs=0;bool everGood=false;int32_t rawCounts=0;}
void pressureManagerBegin(){lastGoodMs=0;everGood=false;rawCounts=0;}
void pressureManagerRecord(bool ready,int32_t raw,uint32_t now){if(!ready)return;lastGoodMs=now;everGood=true;rawCounts=raw;if(deviceStateMutex){xSemaphoreTake(deviceStateMutex,portMAX_DELAY);deviceState.pressureRaw=raw;deviceState.pressureAvailable=true;deviceState.pressureMmhgAvailable=false;xSemaphoreGive(deviceStateMutex);}}
bool pressureSensorConnected(uint32_t now){return everGood&&(uint32_t)(now-lastGoodMs)<=1000;}
bool pressureMmHgAvailable(){return PRESSURE_CALIBRATION_CONFIRMED;}
int32_t latestPressureRaw(){return rawCounts;}
