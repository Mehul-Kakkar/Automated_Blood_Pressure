#include "sensor_manager.h"
#include "config.h"
#include "korotkoff_detector.h"
#include "device_state.h"
#include "pressure_manager.h"
#include <freertos/queue.h>
#include <HX710B.h>
#include <freertos/task.h>

namespace {
QueueHandle_t samples=nullptr;
KorotkoffDetector detector;
float previousInput=0.0f,highPassOutput=0.0f,lowPassOutput=0.0f;
constexpr float HP_ALPHA=0.9409f,LP_ALPHA=0.4852f;
uint32_t lost=0,sampleCount=0;
HX710B pressureAdc(PRESSURE_CLOCK_PIN,PRESSURE_DATA_PIN,HX710B_DIFF1);
bool pressureTaskStarted=false;

bool readHx710bIfReady(int32_t& raw){
  // Called only by pressureTask, never by the microphone acquisition loop.
  uint32_t libraryRaw=0;
  // This installed library uses read(data, timeoutMs) and returns HX710B_OK.
  // A zero timeout bounds each readiness check; a missed conversion is retried.
  if(pressureAdc.read(&libraryRaw,0)!=HX710B_OK)return false;
  if(libraryRaw==0||libraryRaw==0xFFFFFFUL)return false; // reject saturation endpoints
  uint32_t twosComplement=libraryRaw^0x800000UL; // undo the library's offset-binary transform
  if(twosComplement&0x800000UL)twosComplement|=0xFF000000UL;
  raw=(int32_t)twosComplement;
  return true;
}
void pressureTask(void*){
  // This library initializes the converter and waits briefly for startup; keep
  // that work outside the time-sensitive microphone acquisition loop.
  pressureAdc.init();
  for(;;){int32_t raw=0;uint32_t now=millis();bool ready=readHx710bIfReady(raw);pressureManagerRecord(ready,raw,now);vTaskDelay(pdMS_TO_TICKS(5));}
}
uint32_t nextSampleUs=0;bool eventSinceTelemetry=false;
void acquireOne(){
    uint32_t nowUs=micros();if((int32_t)(nowUs-nextSampleUs)<0)return;
    nextSampleUs+=SENSOR_SAMPLE_INTERVAL_US;
    if((int32_t)(nowUs-nextSampleUs)>(int32_t)(5*SENSOR_SAMPLE_INTERVAL_US))nextSampleUs=nowUs+SENSOR_SAMPLE_INTERVAL_US;
    uint16_t raw=(uint16_t)analogRead(MIC_PIN);float input=(float)raw;
    highPassOutput=HP_ALPHA*(highPassOutput+input-previousInput);previousInput=input;
    lowPassOutput+=LP_ALPHA*(highPassOutput-lowPassOutput);
    uint32_t nowMs=millis();bool event=detector.process(lowPassOutput,nowMs);eventSinceTelemetry|=event;
    SensorSample sample{};sample.timestampMs=nowMs;sample.microphoneRaw=raw;sample.filtered=lowPassOutput;sample.envelope=detector.envelope;sample.noiseFloor=detector.noiseFloor;sample.threshold=fmaxf(detector.thresholdFloor,detector.noiseFloor*detector.thresholdMultiplier);sample.eventDetected=eventSinceTelemetry;
    sample.pressureReady=pressureSensorConnected(nowMs);if(sample.pressureReady)sample.pressureRaw=latestPressureRaw();
    sampleCount++;
    // Store a compact downsample at 10 Hz; raw acoustic data is never streamed
    // continuously. A latched event flag is carried into the next batch sample.
    if(sampleCount%200==0){DeviceState current=deviceStateSnapshot();if(current.state==MeasurementState::MEASURING||current.state==MeasurementState::DEFLATING){if(xQueueSend(samples,&sample,0)!=pdTRUE)lost++;else eventSinceTelemetry=false;}}
  }
}
void sensorManagerBegin(){pinMode(MIC_PIN,INPUT);analogReadResolution(12);analogSetPinAttenuation(MIC_PIN,ADC_11db);detector.reset();samples=xQueueCreate(TELEMETRY_QUEUE_CAPACITY,sizeof(SensorSample));nextSampleUs=micros();pressureTaskStarted=xTaskCreatePinnedToCore(pressureTask,"hx710b-read",3072,nullptr,1,nullptr,0)==pdPASS;}
void sensorManagerPoll(){acquireOne();}
bool sensorManagerReady(){return samples!=nullptr&&pressureTaskStarted;}
QueueHandle_t sensorSampleQueue(){return samples;}
uint32_t sensorDroppedCount(){return lost;}
