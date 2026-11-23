// Host-side replay for a raw ADC capture: one 0..4095 count per line.
// Build: g++ -std=c++17 -O2 korotkoff_replay.cpp -o korotkoff_replay
// Run:   korotkoff_replay capture.csv > replay.csv
#include "../korotkoff_detector.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

int main(int argc,char** argv){
  if(argc!=2){std::fprintf(stderr,"Usage: %s raw_adc_capture.csv\n",argv[0]);return 2;}
  std::ifstream input(argv[1]);if(!input){std::perror(argv[1]);return 2;}
  KorotkoffDetector detector;
  detector.setCalibrationMode(KorotkoffDetector::CalibrationMode::PRESSURIZED_PUMP_STOPPED);
  std::puts("sequence,timestamp_ms,raw_adc,filtered,envelope,noise_floor,threshold,candidate,duration_ms");
  std::string line;uint32_t sequence=0;
  while(std::getline(input,line)){
    const char* start=line.c_str();while(*start==' '||*start=='\t'||*start=='\r')++start;
    if(!*start||*start=='#')continue;
    errno=0;char* end=nullptr;long raw=std::strtol(start,&end,10);
    while(end&&(*end==' '||*end=='\t'||*end=='\r'))++end;
    if(errno||end==start||(end&&*end)||raw<0||raw>4095){std::fprintf(stderr,"Invalid ADC count at input sample %lu\n",(unsigned long)sequence);return 2;}
    if(sequence==8000)detector.startDeflation(); // first four seconds calibrate; capture then enters deflation
    const uint32_t timeMs=sequence/2;
    const bool event=detector.process((float)raw,timeMs);
    std::printf("%lu,%lu,%ld,%.4f,%.4f,%.4f,%.4f,%u,%u\n",(unsigned long)sequence,(unsigned long)timeMs,raw,
      detector.filtered,detector.envelope,detector.noiseFloor,detector.threshold,event?1U:0U,event?detector.lastEventDurationMs:0U);
    ++sequence;
  }
  return input.bad()?1:0;
}
