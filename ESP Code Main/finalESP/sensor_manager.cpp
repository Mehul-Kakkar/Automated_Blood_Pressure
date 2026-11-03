#include "sensor_manager.h"
#include "config.h"
#include "korotkoff_detector.h"
#include "device_state.h"
#include "pressure_manager.h"
#include <freertos/queue.h>

namespace {
QueueHandle_t samples=nullptr;
KorotkoffDetector detector;
float previousInput=0.0f,highPassOutput=0.0f,lowPassOutput=0.0f;
constexpr float HP_ALPHA=0.9409f,LP_ALPHA=0.4852f;
uint32_t lost=0,lastPressurePoll=0,sampleCount=0;

bool readHx710bIfReady(int32_t& raw){
  // Never wait for DOUT. A not-ready conversion is skipped so the microphone
  // loop cannot stall while awaiting the pressure converter.
  if(digitalRead(PRESSURE_DATA_PIN)!=LOW)return false;
  uint32_t value=0;
  for(uint8_t bit=0;bit<24;bit++){digitalWrite(PRESSURE_CLOCK_PIN,HIGH);delayMicroseconds(1);value=(value<<1)|(digitalRead(PRESSURE_DATA_PIN)?1UL:0UL);digitalWrite(PRESSURE_CLOCK_PIN,LOW);delayMicroseconds(1);}
  // Existing HX710B sketch uses the 25th pulse (Channel A, gain 128).
  digitalWrite(PRESSURE_CLOCK_PIN,HIGH);delayMicroseconds(1);digitalWrite(PRESSURE_CLOCK_PIN,LOW);delayMicroseconds(1);
  if(value&0x800000UL)value|=0xFF000000UL;raw=(int32_t)value;return true;
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
    if((uint32_t)(nowMs-lastPressurePoll)>=PRESSURE_POLL_INTERVAL_MS){lastPressurePoll=nowMs;sample.pressureReady=readHx710bIfReady(sample.pressureRaw);pressureManagerRecord(sample.pressureReady,sample.pressureRaw,nowMs);}
    sampleCount++;
    // Store a compact downsample at 10 Hz; raw acoustic data is never streamed
    // continuously. A latched event flag is carried into the next batch sample.
    if(sampleCount%200==0){DeviceState current=deviceStateSnapshot();if(current.state==MeasurementState::MEASURING||current.state==MeasurementState::DEFLATING){if(xQueueSend(samples,&sample,0)!=pdTRUE)lost++;else eventSinceTelemetry=false;}}
  }
}
void sensorManagerBegin(){pinMode(MIC_PIN,INPUT);pinMode(PRESSURE_DATA_PIN,INPUT);pinMode(PRESSURE_CLOCK_PIN,OUTPUT);digitalWrite(PRESSURE_CLOCK_PIN,LOW);analogReadResolution(12);analogSetPinAttenuation(MIC_PIN,ADC_11db);detector.reset();samples=xQueueCreate(TELEMETRY_QUEUE_CAPACITY,sizeof(SensorSample));nextSampleUs=micros();}
void sensorManagerPoll(){acquireOne();}
bool sensorManagerReady(){return samples!=nullptr;}
QueueHandle_t sensorSampleQueue(){return samples;}
uint32_t sensorDroppedCount(){return lost;}
