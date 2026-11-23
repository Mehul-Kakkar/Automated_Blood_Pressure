#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <ctype.h>

enum class MeasurementState:uint8_t { BOOT,IDLE,PRECHECK,INFLATING,MEASURING,DEFLATING,COMPLETED,FAULT,EMERGENCY_STOP };
enum class FaultCode:uint8_t { NONE,PRESSURE_UNCALIBRATED,PINMAP_UNVERIFIED,VALVE_UNVERIFIED,SENSOR_DISCONNECTED,INFLATION_TIMEOUT,PRESSURE_LIMIT,AUTO_INFLATION_DISABLED,STOP_RELEASE_UNVERIFIED,COMMAND_QUEUE_FULL,ADC_SATURATION,IMPLAUSIBLE_PRESSURE };
struct DeviceState {
  MeasurementState state=MeasurementState::BOOT;
  FaultCode fault=FaultCode::NONE;
  uint32_t uptimeMs=0,statusSequence=0,lostSamples=0,sampleSequence=0,pressureUpdatedAtMs=0;
  int32_t pressureRaw=0;
  float pressureMmhg=0.0f;
  bool pressureAvailable=false,microphoneAvailable=true,timedPumpTest=false,pressureSaturated=false;
  bool pressureMmhgAvailable=false;
  uint32_t timedTestReleaseStartedAtMs=0;
  uint32_t deflationStartedAtMs=0;
  char sessionId[37]={0};
  char startRequestId[37]={0};
};
extern DeviceState deviceState;
extern SemaphoreHandle_t deviceStateMutex;

inline DeviceState deviceStateSnapshot(){DeviceState out{};if(deviceStateMutex){xSemaphoreTake(deviceStateMutex,portMAX_DELAY);out=deviceState;xSemaphoreGive(deviceStateMutex);}return out;}
inline bool validUuid(const char*s){if(!s||strlen(s)!=36)return false;for(size_t i=0;i<36;i++){if(i==8||i==13||i==18||i==23){if(s[i]!='-')return false;}else if(!isxdigit((unsigned char)s[i]))return false;}return true;}
inline bool measurementActive(MeasurementState s){return s==MeasurementState::PRECHECK||s==MeasurementState::INFLATING||s==MeasurementState::MEASURING||s==MeasurementState::DEFLATING;}
inline const char* backendState(MeasurementState s){switch(s){case MeasurementState::BOOT:case MeasurementState::IDLE:return "idle";case MeasurementState::PRECHECK:case MeasurementState::INFLATING:case MeasurementState::MEASURING:case MeasurementState::DEFLATING:return "measuring";case MeasurementState::COMPLETED:return "stopped";default:return "fault";}}
