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
uint32_t lost=0,sampleCount=0;bool acousticMeasurementActive=false,acousticDeflationActive=false;
SensorSample latestSample{},latestEventSample{};bool haveLatestSample=false,bleEventLatched=false;
constexpr size_t WAVEFORM_RING_CAPACITY=4096;
struct WaveformFrame{uint32_t sequence,timestampUs;WaveformSample sample;};
WaveformFrame waveformRing[WAVEFORM_RING_CAPACITY]{};
uint64_t waveformWriteSequence=0,waveformReadSequence=0;
bool waveformCaptureActive=false;
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
  // HX710B::read() already applies raw ^ 0x800000, producing an unsigned
  // offset-binary count. The calibration sketch records centered counts as
  // libraryRaw - 0x800000, so center once without applying the XOR again.
  raw=(int32_t)libraryRaw-0x800000L;
  // Check saturation in the centered signed 24-bit representation used by
  // calibration, before converting the sample to pressure.
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
  uint32_t invalidSamples=0;bool saturationReported=false;
  int32_t lastRaw=0,lastRejectedRaw=0;
  Serial.printf("HX710B init %s; DOUT/GPIO%u=%d\n",initialized?"ok":"failed",PRESSURE_DATA_PIN,digitalRead(PRESSURE_DATA_PIN));
  for(;;){
    int32_t raw=0;uint32_t now=millis();bool ready=readHx710bIfReady(raw);
    // Saturated conversions still contain a useful raw diagnostic count.
    // Keep the sensor link fresh and publish the count, while the pressure
    // manager marks its mmHg value unavailable.
    if(!ready&&adcSaturated){pressureManagerRecord(true,raw,now);lastGoodMs=now;lastRaw=raw;}
    if(!adcSaturated)saturationReported=false;
    if(ready&&raw<MIN_PLAUSIBLE_PRESSURE_RAW){
      // Treat below-zero centered counts as noise. Do not publish them as
      // pressure and do not turn a single transient into an inflation fault.
      invalidSamples++;lastRejectedRaw=raw;
    } else if(!ready&&deviceStateSnapshot().state==MeasurementState::INFLATING){
      // Saturation is the inflation cutoff, not a sensor fault. The main
      // control loop stops the pump and enters the passive-backflow phase.
      if(adcSaturated&&!saturationReported){
        Serial.printf("HX710B pressure saturation cutoff: centered_raw=%ld raw24=0x%06lX; pump cutoff requested.\n",(long)raw,(unsigned long)((uint32_t)raw&0xFFFFFFUL));
        saturationReported=true;lastRejectedRaw=raw;invalidSamples++;
      }
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
uint32_t nextSampleUs=0;
void acquireOne(){
    uint32_t nowUs=micros();if((int32_t)(nowUs-nextSampleUs)<0)return;
    nextSampleUs+=SENSOR_SAMPLE_INTERVAL_US;
    if((int32_t)(nowUs-nextSampleUs)>(int32_t)(5*SENSOR_SAMPLE_INTERVAL_US))nextSampleUs=nowUs+SENSOR_SAMPLE_INTERVAL_US;
    DeviceState pressure=deviceStateSnapshot();
    if(!::measurementActive(pressure.state)){waveformCaptureActive=false;acousticMeasurementActive=false;if(acousticDeflationActive)detector.stopDeflation();acousticDeflationActive=false;return;}
    uint16_t raw=(uint16_t)analogRead(MIC_PIN);const bool clipped=raw<=2||raw>=4093;float input=(float)raw;
    uint32_t nowMs=millis();const bool pressureValid=pressureSensorConnected(nowMs)&&pressure.pressureMmhgAvailable&&isfinite(pressure.pressureMmhg)&&pressure.pressureMmhg>=0.0f&&pressure.pressureMmhg<=400.0f;
    const bool collecting=pressure.state==MeasurementState::MEASURING||pressure.state==MeasurementState::DEFLATING;
    const bool waveformActive=::measurementActive(pressure.state);
    if(waveformActive&&!waveformCaptureActive)waveformReadSequence=waveformWriteSequence;
    waveformCaptureActive=waveformActive;
    if(pressure.state==MeasurementState::PRECHECK)detector.setCalibrationMode(KorotkoffDetector::CalibrationMode::UNPRESSURIZED_PUMP_STOPPED);
    else if(pressure.state==MeasurementState::INFLATING)detector.setCalibrationMode(KorotkoffDetector::CalibrationMode::PUMP_INFLATING);
    else if(pressure.state==MeasurementState::MEASURING&&!acousticMeasurementActive)detector.setCalibrationMode(KorotkoffDetector::CalibrationMode::PRESSURIZED_PUMP_STOPPED);
    // Pressure falls during MEASURING (pump off, exhaust closed). Start
    // Korotkoff tracking there, immediately after the 4 s noise calibration;
    // waiting for DEFLATING means waiting until the cuff is already below 60.
    const bool releasePhase=(pressure.state==MeasurementState::MEASURING||pressure.state==MeasurementState::DEFLATING)&&pressure.deflationStartedAtMs!=0;
    if(releasePhase&&!acousticDeflationActive&&detector.calibrated){detector.startDeflation();acousticDeflationActive=true;}
    if(!releasePhase&&acousticDeflationActive){detector.stopDeflation();acousticDeflationActive=false;}
    acousticMeasurementActive=collecting;detector.setPressure(pressure.pressureMmhg,pressureValid,pressure.pressureUpdatedAtMs);
    const bool event=detector.process(clipped?2048.0f:input,nowMs);
    if(!event)detector.hasPossibleDisappearance(nowMs);
    SensorSample sample{};sample.eventPressureMmhg=NAN;sample.timestampMs=event?detector.lastEventMs:nowMs;sample.microphoneRaw=raw;sample.filtered=detector.filtered;sample.envelope=detector.envelope;sample.noiseFloor=detector.noiseFloor;sample.threshold=detector.threshold;sample.adcBaseline=detector.adcBaseline;sample.eventDurationMs=event?detector.lastEventDurationMs:0;sample.eventStrength=event?detector.lastEventStrength:0;sample.calibrationMode=(uint8_t)detector.calibrationMode;sample.calibrated=detector.calibrated;sample.eventDetected=event;sample.adcClipped=clipped;if(event&&detector.eventCount)sample.eventPressureMmhg=detector.events[detector.eventCount-1].pressureMmHg;
    sample.pressureReady=pressureValid;if(sample.pressureReady){sample.pressureRaw=pressure.pressureRaw;sample.pressureMmhg=pressure.pressureMmhg;}
    latestSample=sample;haveLatestSample=true;if(event){latestEventSample=sample;bleEventLatched=true;}
    if(waveformCaptureActive){long filteredQ4=lroundf(detector.filtered*16.0f);if(filteredQ4>32767)filteredQ4=32767;if(filteredQ4<-32768)filteredQ4=-32768;long envelopeQ2=lroundf(detector.envelope*4.0f),thresholdQ2=lroundf(detector.threshold*4.0f);if(envelopeQ2<0)envelopeQ2=0;if(envelopeQ2>65535)envelopeQ2=65535;if(thresholdQ2<0)thresholdQ2=0;if(thresholdQ2>65535)thresholdQ2=65535;WaveformSample frame{static_cast<int16_t>(filteredQ4),static_cast<uint16_t>(envelopeQ2),static_cast<uint16_t>(thresholdQ2)};waveformRing[waveformWriteSequence%WAVEFORM_RING_CAPACITY]={static_cast<uint32_t>(waveformWriteSequence),nowUs,frame};waveformWriteSequence++;}
    sampleCount++;
    // Preserve each event at its detector timestamp and pressure. The backend
    // receives a decimated 50 Hz signal trace; BLE carries the 2 kHz trace.
    DeviceState current=deviceStateSnapshot();const bool shouldCollect=current.state==MeasurementState::MEASURING||current.state==MeasurementState::DEFLATING;
    if(event&&shouldCollect){SensorSample eventSample=sample;eventSample.eventDetected=true;if(xQueueSend(samples,&eventSample,0)!=pdTRUE)lost++;}
    if(sampleCount%40==0&&shouldCollect){sample.eventDetected=false;if(xQueueSend(samples,&sample,0)!=pdTRUE)lost++;}
  }
}
void sensorManagerBegin(){pinMode(MIC_PIN,INPUT);analogReadResolution(12);analogSetPinAttenuation(MIC_PIN,ADC_11db);detector.reset();samples=xQueueCreate(TELEMETRY_QUEUE_CAPACITY,sizeof(SensorSample));nextSampleUs=micros();pressureTaskStarted=xTaskCreatePinnedToCore(pressureTask,"hx710b-read",3072,nullptr,1,nullptr,0)==pdPASS;}
void sensorManagerPoll(){acquireOne();}
bool sensorManagerReady(){return samples!=nullptr&&pressureTaskStarted;}
QueueHandle_t sensorSampleQueue(){return samples;}
uint32_t sensorDroppedCount(){return lost;}
bool sensorManagerLatestSample(SensorSample& sample){if(!haveLatestSample)return false;if(bleEventLatched){sample=latestEventSample;bleEventLatched=false;}else sample=latestSample;return true;}
bool sensorManagerBloodPressureEstimate(float& systolicMmHg,float& diastolicMmHg){if(!detector.hasPossibleSystolic()||!detector.disappearanceCandidateValid)return false;systolicMmHg=detector.systolicCandidate;diastolicMmHg=detector.disappearanceCandidate;return isfinite(systolicMmHg)&&isfinite(diastolicMmHg)&&systolicMmHg>diastolicMmHg&&diastolicMmHg>=0.0f&&systolicMmHg<=400.0f;}
size_t sensorManagerReadWaveformBatch(WaveformSample* output,size_t capacity,uint32_t& firstSequence,uint32_t& firstTimestampUs){if(!output||capacity==0)return 0;if(waveformWriteSequence-waveformReadSequence>WAVEFORM_RING_CAPACITY)waveformReadSequence=waveformWriteSequence-WAVEFORM_RING_CAPACITY;const size_t available=static_cast<size_t>(waveformWriteSequence-waveformReadSequence);const size_t count=available<capacity?available:capacity;if(!count)return 0;const WaveformFrame& first=waveformRing[waveformReadSequence%WAVEFORM_RING_CAPACITY];firstSequence=first.sequence;firstTimestampUs=first.timestampUs;for(size_t i=0;i<count;i++)output[i]=waveformRing[(waveformReadSequence+i)%WAVEFORM_RING_CAPACITY].sample;waveformReadSequence+=count;return count;}
