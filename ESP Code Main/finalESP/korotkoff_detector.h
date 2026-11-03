#pragma once
#include <Arduino.h>
#include <math.h>

// Port of korotkoffTest/korotkoff_detector.h's acquisition/event thresholds.
// Pressure endpoints are deliberately not inferred without validated mmHg.
class KorotkoffDetector {
 public:
  uint32_t lastEventMs=0,burstStartMs=0,lastAboveThresholdMs=0;
  float envelope=0.0f,noiseFloor=3.0f,burstPeak=0.0f;
  bool inBurst=false;
  const float envelopeAlpha=0.08f,noiseAlpha=0.002f,thresholdFloor=12.0f,thresholdMultiplier=3.0f;
  const uint32_t minEventIntervalMs=280,minBurstMs=8,maxBurstMs=180;
  void reset(){lastEventMs=burstStartMs=lastAboveThresholdMs=0;envelope=0;noiseFloor=3;burstPeak=0;inBurst=false;}
  bool process(float bandpassed,uint32_t nowMs){bool detected=false;float magnitude=fabsf(bandpassed);envelope+=envelopeAlpha*(magnitude-envelope);float threshold=fmaxf(thresholdFloor,noiseFloor*thresholdMultiplier);if(!inBurst)noiseFloor+=noiseAlpha*(envelope-noiseFloor);if(envelope>=threshold){if(!inBurst){inBurst=true;burstStartMs=nowMs;burstPeak=envelope;}lastAboveThresholdMs=nowMs;burstPeak=fmaxf(burstPeak,envelope);}else if(inBurst&&(uint32_t)(nowMs-lastAboveThresholdMs)>=20){uint32_t duration=lastAboveThresholdMs-burstStartMs;inBurst=false;if(duration>=minBurstMs&&duration<=maxBurstMs&&(lastEventMs==0||(uint32_t)(nowMs-lastEventMs)>=minEventIntervalMs)){lastEventMs=nowMs;detected=true;}burstPeak=0;}return detected;}
};
