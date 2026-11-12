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
SensorSample latestSample{};bool haveLatestSample=false,bleEventLatched=false;
// Match the known-working test sketch, which uses the library default mode
// (HX710B_DIFF2 / 40 Hz) by omitting the optional mode argument.
HX710B pressureAdc(PRESSURE_CLOCK_PIN,PRESSURE_DATA_PIN);
bool pressureTaskStarted=false;
constexpr unsigned long PRESSURE_READ_TIMEOUT_MS=100UL;
bool adcSaturated=false;
bool readHx710bIfReady(int32_t& raw){
  // Called only by pressureTask, never by the microphone acquisition loop.
  uint32_t libraryRaw=0;
  // This installed library uses read(data, timeoutMs) and returns HX710B_OK.
  // Use the same wait behavior as the standalone test sketch. This task is
  // dedicated to pressure acquisition, so waiting for a conversion is safe.
  if(pressureAdc.read(&libraryRaw,PRESSURE_READ_TIMEOUT_MS)!=HX710B_OK){adcSaturated=false;return false;}
  uint32_t twosComplement=libraryRaw^0x800000UL; // undo the library's offset-binary transform
  if(twosComplement&0x800000UL)twosComplement|=0xFF000000UL;
  raw=(int32_t)twosComplement;
  // Signed 24-bit positive saturation is 0x7fffff (8,388,607), which appears
  // as 0xffffff after this library's offset-binary transform.
  adcSaturated=raw==HX710B_SIGNED_POSITIVE_SATURATION||raw==HX710B_SIGNED_NEGATIVE_SATURATION;
  if(adcSaturated)return false;
  return true;
}
bool initializePressureAdc(){
  // Match the working pump/valve bench setup before library initialization.
  pinMode(PRESSURE_DATA_PIN,INPUT);
  pinMode(PRESSURE_CLOCK_PIN,OUTPUT);
  digitalWrite(PRESSURE_CLOCK_PIN,LOW);
  delay(10);
  return pressureAdc.init()==HX710B_OK;
}
void pressureTask(void*){
  // This library initializes the converter and waits briefly for startup; keep
  // that work outside the time-sensitive microphone acquisition loop.
  bool initialized=initializePressureAdc();
  uint32_t lastInitMs=millis(),lastDiagnosticMs=lastInitMs,lastGoodMs=0;
  uint32_t invalidSamples=0;
  int32_t lastRaw=0,lastRejectedRaw=0;
  Serial.printf("HX710B init %s; DOUT/GPIO%u=%d\n",initialized?"ok":"failed",PRESSURE_DATA_PIN,digitalRead(PRESSURE_DATA_PIN));
  for(;;){
    int32_t raw=0;uint32_t now=millis();bool ready=readHx710bIfReady(raw);
    if(ready&&raw<MIN_PLAUSIBLE_PRESSURE_RAW){
      invalidSamples++;lastRejectedRaw=raw;
      DeviceState s=deviceStateSnapshot();if(s.state==MeasurementState::INFLATING){xSemaphoreTake(deviceStateMutex,portMAX_DELAY);deviceState.fault=FaultCode::IMPLAUSIBLE_PRESSURE;xSemaphoreGive(deviceStateMutex);}
    } else if(!ready&&deviceStateSnapshot().state==MeasurementState::INFLATING){
      // Saturation / a failed conversion is never represented as pressure.
      // Escalate immediately; the main control loop releases the actuators.
      if(adcSaturated){xSemaphoreTake(deviceStateMutex,portMAX_DELAY);deviceState.fault=FaultCode::ADC_SATURATION;xSemaphoreGive(deviceStateMutex);}
    } else if(ready){pressureManagerRecord(true,raw,now);lastGoodMs=now;lastRaw=raw;}
    if((uint32_t)(now-lastDiagnosticMs)>=1000){
      lastDiagnosticMs=now;
      const bool connected=pressureSensorConnected(now);
      Serial.printf("HX710B %s; last valid raw=%ld age=%lu ms; rejected=%lu last rejected raw=%ld; DOUT/GPIO%u=%d SCK/GPIO%u=%d\n",connected?"connected":"no fresh conversion",(long)lastRaw,(unsigned long)(lastGoodMs?(uint32_t)(now-lastGoodMs):UINT32_MAX),(unsigned long)invalidSamples,(long)lastRejectedRaw,PRESSURE_DATA_PIN,digitalRead(PRESSURE_DATA_PIN),PRESSURE_CLOCK_PIN,digitalRead(PRESSURE_CLOCK_PIN));
    }
    // Re-run library initialization after a sustained read outage so a sensor
    // brownout or reconnection can recover without resetting the controller.
    if(!pressureSensorConnected(now)&&(uint32_t)(now-lastInitMs)>=3000){
      lastInitMs=now;initialized=initializePressureAdc();
      Serial.printf("HX710B reinit %s; DOUT/GPIO%u=%d\n",initialized?"ok":"failed",PRESSURE_DATA_PIN,digitalRead(PRESSURE_DATA_PIN));
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
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
    DeviceState pressure=deviceStateSnapshot();sample.pressureReady=pressureSensorConnected(nowMs)&&pressure.pressureMmhgAvailable;if(sample.pressureReady){sample.pressureRaw=pressure.pressureRaw;sample.pressureMmhg=pressure.pressureMmhg;}
    latestSample=sample;latestSample.eventDetected=event;haveLatestSample=true;bleEventLatched|=event;
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
bool sensorManagerLatestSample(SensorSample& sample){if(!haveLatestSample)return false;sample=latestSample;sample.eventDetected=bleEventLatched;bleEventLatched=false;return true;}
