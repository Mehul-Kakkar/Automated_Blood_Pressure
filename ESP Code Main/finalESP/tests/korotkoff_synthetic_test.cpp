#include "../korotkoff_detector.h"
#include <cassert>
#include <cmath>
#include <cstdio>

static uint32_t rng=0x12345678;
static float noise(float amplitude){rng=1664525u*rng+1013904223u;return ((int)((rng>>8)%2001)-1000)*amplitude/1000.0f;}
static void feed(KorotkoffDetector& d,uint32_t i,float x){d.process(2048.0f+x,i/2);}
static void prepare(KorotkoffDetector& d,float noiseAmplitude=1.5f){
  d.reset();d.setCalibrationMode(KorotkoffDetector::CalibrationMode::PRESSURIZED_PUMP_STOPPED);
  for(uint32_t i=0;i<9000;i++)feed(d,i,noise(noiseAmplitude));
  assert(d.calibrated);d.startDeflation();
}
static void burst(KorotkoffDetector& d,uint32_t& i,float amplitude,uint32_t durationMs=40,float hz=90){
  const uint32_t n=durationMs*2;for(uint32_t j=0;j<n;j++,i++)feed(d,i,amplitude*sinf(6.283185307f*hz*j/2000.0f)+noise(1.5f));
  for(int j=0;j<120;j++,i++)feed(d,i,noise(1.5f));
}
int main(){
  {KorotkoffDetector d;prepare(d);uint32_t i=9000;burst(d,i,10);assert(d.eventCount>0);}
  {KorotkoffDetector d;prepare(d);uint32_t i=9000;burst(d,i,80);assert(d.eventCount>0);assert(d.events[0].strength>d.events[0].threshold);assert(d.events[0].durationMs>=4&&d.events[0].durationMs<=220);}
  // Irregular single-sample transients are rejected as artifacts, not sounds.
  {KorotkoffDetector d;prepare(d);uint32_t i=9000;const int gaps[]={200,530,290,710,410,180,960,330,520,240,810,370,600,220,470,740,310,890,260,550};for(int gap:gaps){feed(d,i++,120);for(int j=0;j<gap;j++)feed(d,i++,noise(1.5f));}assert(d.eventCount==0);}
  // Recurring low-frequency bursts remain individual candidates; no BPM or
  // regularity classifier is applied.
  {KorotkoffDetector d;prepare(d);uint32_t i=9000;for(int beat=0;beat<5;beat++){burst(d,i,35,35,45);i+=1500;}assert(d.eventCount>=3);}
  {KorotkoffDetector d;prepare(d);const float before=d.noiseFloor;uint32_t i=9000;for(int n=0;n<14000;n++,i++)feed(d,i,noise(8.0f));const float elevated=d.threshold;assert(d.noiseFloor>before);assert(d.threshold<=d.noiseFloor*2.21f);for(int n=0;n<40000;n++,i++)feed(d,i,noise(0.1f));assert(d.threshold<elevated);}
  // Long periodic vibration enters the disturbance guard; a later short burst
  // is still detected after recovery.
  {KorotkoffDetector d;prepare(d);uint32_t i=9000;for(int n=0;n<700;n++,i++)feed(d,i,45*sinf(6.283185307f*80*n/2000.0f));for(int n=0;n<500;n++,i++)feed(d,i,noise(1.5f));burst(d,i,45);assert(d.eventCount>0);}
  // A sustained cuff-release-like vibration is rejected, then detector recovers.
  {KorotkoffDetector d;prepare(d);uint32_t i=9000;for(int n=0;n<800;n++,i++)feed(d,i,noise(1.5f));for(int n=0;n<700;n++,i++)feed(d,i,220*sinf(6.283185307f*55*n/2000.0f));for(int n=0;n<600;n++,i++)feed(d,i,noise(1.5f));assert(d.eventCount==0);burst(d,i,45);assert(d.eventCount>0);}
  std::puts("Synthetic detector checks passed (not clinical validation).");
}
