#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// Candidate detector only. This signal-processing code is not a clinical
// classifier and does not estimate systolic/diastolic pressure.
struct KorotkoffEvent {
  uint32_t timeMs;
  float pressureMmHg, strength, threshold, noiseFloor;
  uint16_t durationMs;
};

class KorotkoffDetector {
  struct PressurePoint { uint32_t timeMs; float mmHg; };
  struct Biquad { float b0,b1,b2,a1,a2,z1=0,z2=0;
    float step(float x){float y=b0*x+z1;z1=b1*x-a1*y+z2;z2=b2*x-a2*y;return y;}
  } hp{0.9565f,-1.9130f,0.9565f,-1.9112f,0.9148f},
    lp{0.1311f,0.2622f,0.1311f,-0.7478f,0.2722f};
  static constexpr uint16_t CALIBRATION_CAPACITY=800, NOISE_CAPACITY=400;
  float calibration[CALIBRATION_CAPACITY]{}, noiseHistory[NOISE_CAPACITY]{}, scratch[CALIBRATION_CAPACITY]{};
  PressurePoint pressureHistory[64]{};
  uint8_t pressureWrite=0,pressureCount=0;
  uint32_t lastPressureTimestamp=UINT32_MAX;
  float lastEventPressure=NAN;
  uint16_t calibrationCount=0,noiseCount=0,noiseWrite=0;
  uint16_t calibrationTick=0,noiseTick=0,belowCount=0,recoveryCount=0;
  uint32_t sampleIndex=0,lastEventIndex=0,burstStartSample=0;
  float previousRawAdc=2048.0f;
  bool havePreviousRaw=false;
  float noiseSigma=0.25f;
  bool inArtifact=false;
  void updateAdaptiveNoise(){
    if(noiseCount<100)return;
    for(uint16_t i=0;i<noiseCount;i++)scratch[i]=noiseHistory[i];
    std::sort(scratch,scratch+noiseCount);
    const float median=scratch[noiseCount/2];
    const float p85=scratch[(uint32_t)(noiseCount-1)*85/100];
    for(uint16_t i=0;i<noiseCount;i++)scratch[i]=fabsf(noiseHistory[i]-median);
    std::sort(scratch,scratch+noiseCount);
    noiseSigma=fmaxf(0.25f,1.4826f*scratch[noiseCount/2]);
    const float estimate=fmaxf(p85,median+2.5f*noiseSigma);
    noiseFloor += (estimate>noiseFloor?0.12f:0.025f)*(estimate-noiseFloor);
    const float target=fmaxf(0.75f,noiseFloor*1.25f);
    threshold += (target>threshold?0.12f:0.025f)*(target-threshold);
    // Keep adaptation bounded so a single noisy interval cannot hide weak bursts.
    threshold=fminf(fmaxf(threshold,0.75f),fmaxf(0.75f,noiseFloor*2.2f));
  }
  void captureNoise(){
    noiseHistory[noiseWrite]=envelope;noiseWrite=(noiseWrite+1)%NOISE_CAPACITY;
    if(noiseCount<NOISE_CAPACITY)noiseCount++;
    if(noiseCount==NOISE_CAPACITY&&noiseWrite%50==0)updateAdaptiveNoise();
  }
 public:
  static constexpr uint8_t MAX_EVENTS=64;
  static constexpr uint16_t SAMPLE_RATE_HZ=2000;
  enum class CalibrationMode:uint8_t { UNKNOWN,UNPRESSURIZED_PUMP_STOPPED,PRESSURIZED_PUMP_STOPPED,ACTIVE_DEFLATION,PUMP_INFLATING };
  KorotkoffEvent events[MAX_EVENTS]{};
  uint8_t eventCount=0;
  bool pressureValid=false,systolicCandidateValid=false,disappearanceCandidateValid=false;
  bool calibrated=false;
  float currentPressure=NAN,systolicCandidate=NAN,disappearanceCandidate=NAN;
  uint32_t lastEventMs=0,burstStartMs=0,lastAboveThresholdMs=0;
  uint16_t lastEventDurationMs=0;
  float lastEventStrength=0.0f;
  float envelope=0.0f,noiseFloor=0.0f,threshold=0.75f,filtered=0.0f,adcBaseline=2048.0f,burstPeak=0.0f;
  bool inBurst=false,deflationActive=false;
  uint8_t consistentEvents=0;
  CalibrationMode calibrationMode=CalibrationMode::UNKNOWN;
  static constexpr uint32_t minEventSamples=8,maxEventSamples=440,endHoldSamples=30,refractorySamples=240,artifactRecoverySamples=160;

  void reset(){
    eventCount=0;pressureValid=false;currentPressure=NAN;systolicCandidateValid=false;systolicCandidate=NAN;
    disappearanceCandidateValid=false;disappearanceCandidate=NAN;lastEventPressure=NAN;pressureWrite=pressureCount=0;lastPressureTimestamp=UINT32_MAX;lastEventMs=burstStartMs=lastAboveThresholdMs=0;
    envelope=noiseFloor=filtered=0;threshold=0.75f;adcBaseline=2048.0f;burstPeak=lastEventStrength=0;lastEventDurationMs=0;inBurst=false;
    deflationActive=false;consistentEvents=0;calibrated=false;calibrationMode=CalibrationMode::UNKNOWN;
    calibrationCount=noiseCount=noiseWrite=calibrationTick=noiseTick=belowCount=recoveryCount=0;sampleIndex=lastEventIndex=burstStartSample=0;previousRawAdc=2048.0f;havePreviousRaw=false;
    hp.z1=hp.z2=lp.z1=lp.z2=0;inArtifact=false;
  }
  void setPressure(float mmHg,bool valid,uint32_t timestampMs=0){pressureValid=valid&&std::isfinite(mmHg)&&mmHg>=0.0f&&mmHg<=400.0f;currentPressure=pressureValid?mmHg:NAN;if(!pressureValid||timestampMs==lastPressureTimestamp)return;lastPressureTimestamp=timestampMs;pressureHistory[pressureWrite]={timestampMs,mmHg};pressureWrite=(pressureWrite+1)%64;if(pressureCount<64)pressureCount++;}
  bool pressureAt(uint32_t timestampMs,float& output)const{
    if(pressureCount<2)return false;
    const uint8_t oldest=(pressureWrite+64-pressureCount)%64;
    const PressurePoint* before=nullptr;const PressurePoint* after=nullptr;
    for(uint8_t i=0;i<pressureCount;i++){const PressurePoint& point=pressureHistory[(oldest+i)%64];const int32_t relative=(int32_t)(point.timeMs-timestampMs);if(relative<=0)before=&point;if(relative>=0){after=&point;break;}}
    if(!before||!after)return false;
    const int32_t span=(int32_t)(after->timeMs-before->timeMs);
    const int32_t offset=(int32_t)(timestampMs-before->timeMs);
    if(span<0||span>1000||offset<0||offset>span)return false;
    const float ratio=span==0?0.0f:(float)offset/(float)span;
    output=before->mmHg+(after->mmHg-before->mmHg)*ratio;
    return std::isfinite(output)&&output>=0.0f&&output<=400.0f;
  }
  void setCalibrationMode(CalibrationMode mode){
    if(mode==calibrationMode)return;
    calibrationMode=mode;calibrationCount=0;calibrationTick=0;
    // Distinct environment calibration: do not carry an inflated-cuff estimate
    // into baseline or active-deflation noise tracking.
    if(mode!=CalibrationMode::ACTIVE_DEFLATION){calibrated=false;noiseCount=noiseWrite=0;noiseFloor=0.0f;threshold=0.75f;systolicCandidateValid=false;systolicCandidate=NAN;disappearanceCandidateValid=false;disappearanceCandidate=NAN;lastEventPressure=NAN;}
  }
  void startDeflation(){eventCount=0;systolicCandidateValid=false;systolicCandidate=NAN;disappearanceCandidateValid=false;disappearanceCandidate=NAN;lastEventPressure=NAN;lastEventMs=burstStartMs=lastAboveThresholdMs=0;inBurst=false;burstPeak=0;consistentEvents=0;deflationActive=true;setCalibrationMode(CalibrationMode::ACTIVE_DEFLATION);}
  void stopDeflation(){deflationActive=false;inBurst=false;}

  // Accept raw 12-bit ADC counts. Bandpass is 2nd-order Butterworth HP 20 Hz
  // followed by LP 300 Hz, at 2 kHz; there is no per-sample millis() timing.
  bool process(float rawAdc,uint32_t nowMs){
    ++sampleIndex;
    const bool impulse=havePreviousRaw&&fabsf(rawAdc-previousRawAdc)>96.0f;previousRawAdc=rawAdc;havePreviousRaw=true;
    adcBaseline += 0.0005f*(rawAdc-adcBaseline);
    const float centered=rawAdc-adcBaseline;
    filtered=lp.step(hp.step(centered));
    const float alpha=1.0f-expf(-1.0f/(SAMPLE_RATE_HZ*0.008f));
    envelope+=alpha*(fabsf(filtered)-envelope);
    if(impulse){inBurst=false;inArtifact=true;recoveryCount=0;belowCount=0;}

    if(!calibrated && calibrationMode==CalibrationMode::PRESSURIZED_PUMP_STOPPED){
      if(++calibrationTick>=10){calibrationTick=0;if(calibrationCount<CALIBRATION_CAPACITY)calibration[calibrationCount++]=envelope;}
      if(calibrationCount>=CALIBRATION_CAPACITY){
        std::copy(calibration,calibration+calibrationCount,scratch);std::sort(scratch,scratch+calibrationCount);
        const float med=scratch[calibrationCount/2];
        for(uint16_t i=0;i<calibrationCount;i++)scratch[i]=fabsf(calibration[i]-med);
        std::sort(scratch,scratch+calibrationCount);noiseSigma=fmaxf(0.25f,1.4826f*scratch[calibrationCount/2]);
        noiseFloor=fmaxf(calibration[0],0.0f);
        std::copy(calibration,calibration+calibrationCount,scratch);std::sort(scratch,scratch+calibrationCount);
        noiseFloor=fmaxf(scratch[(uint32_t)(calibrationCount-1)*85/100],med+2.5f*noiseSigma);
        threshold=fmaxf(0.75f,fmaxf(noiseFloor*1.25f,med+3.0f*noiseSigma));calibrated=true;
        for(uint16_t i=0;i<NOISE_CAPACITY;i++)noiseHistory[i]=calibration[calibrationCount-NOISE_CAPACITY+i];
        noiseCount=NOISE_CAPACITY;noiseWrite=0;
      }
    }
    if(!calibrated){threshold=fmaxf(threshold,0.75f);return false;}
    if(calibrationMode!=CalibrationMode::ACTIVE_DEFLATION){
      if(++noiseTick>=10){noiseTick=0;captureNoise();}
      return false;
    }

    if(inArtifact){if(++noiseTick>=10){noiseTick=0;captureNoise();}if(envelope<threshold*0.55f){if(++recoveryCount>=artifactRecoverySamples){inArtifact=false;recoveryCount=0;}}else recoveryCount=0;return false;}
    if(!inBurst){
      if(envelope>=threshold && sampleIndex-lastEventIndex>=refractorySamples){inBurst=true;burstStartMs=nowMs;burstStartSample=sampleIndex;burstPeak=envelope;belowCount=0;}
      else if(++noiseTick>=10){noiseTick=0;captureNoise();}
      return false;
    }
    if(envelope>burstPeak)burstPeak=envelope;
    if(envelope<fmaxf(threshold*0.25f,burstPeak*0.15f))++belowCount;else belowCount=0;
    const uint32_t elapsedSamples=sampleIndex-burstStartSample;
    if(elapsedSamples>=maxEventSamples){inBurst=false;inArtifact=true;recoveryCount=0;return false;}
    if(belowCount<endHoldSamples)return false;
    const uint32_t duration=elapsedSamples-belowCount;
    inBurst=false;belowCount=0;
    if(duration>=minEventSamples&&duration<=maxEventSamples&&sampleIndex-lastEventIndex>=refractorySamples){const uint16_t durationMs=(uint16_t)(duration/2);recordEvent(burstStartMs,burstPeak,durationMs);lastEventIndex=sampleIndex;return true;}
    return false;
  }
  void recordEvent(uint32_t nowMs,float strength,uint16_t durationMs=4){
    float eventPressure=NAN;const bool eventPressureValid=pressureAt(nowMs,eventPressure);
    lastEventMs=nowMs;lastEventStrength=strength;lastEventDurationMs=durationMs;if(eventCount<MAX_EVENTS)events[eventCount++]={nowMs,eventPressureValid?eventPressure:NAN,strength,threshold,noiseFloor,durationMs};
    if(!eventPressureValid||!deflationActive)return;
    lastEventPressure=eventPressure;
    if(!systolicCandidateValid){systolicCandidate=eventPressure;systolicCandidateValid=true;consistentEvents=1;}else if(consistentEvents<255)consistentEvents++;
    disappearanceCandidateValid=false;
  }
  bool hasPossibleSystolic()const{return systolicCandidateValid&&consistentEvents>=3;}
  bool hasPossibleDisappearance(uint32_t nowMs){if(!deflationActive||!pressureValid||!std::isfinite(lastEventPressure)||lastEventMs==0||(uint32_t)(nowMs-lastEventMs)<4000)return false;disappearanceCandidate=lastEventPressure;disappearanceCandidateValid=true;return true;}
};
