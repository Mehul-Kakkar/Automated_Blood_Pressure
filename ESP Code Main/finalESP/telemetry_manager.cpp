#include "telemetry_manager.h"
#include "config.h"
#include "sensor_manager.h"
#include "device_state.h"
#include "uuid_util.h"
#include <ArduinoJson.h>
#include <freertos/queue.h>
namespace {uint32_t lost=0;StaticJsonDocument<8192> doc;}
size_t telemetryBuildNextBatch(const char* deviceId,const char* sessionId,char* output,size_t capacity){
  if(!validUuid(deviceId)||!validUuid(sessionId)||!output||capacity<1024)return 0;
  SensorSample samples[MAX_TELEMETRY_BATCH_SAMPLES];size_t count=0;QueueHandle_t q=sensorSampleQueue();while(q&&count<MAX_TELEMETRY_BATCH_SAMPLES&&xQueueReceive(q,&samples[count],0)==pdTRUE)count++;if(!count)return 0;
  doc.clear();doc["schema_version"]="1";JsonObject payload=doc.createNestedObject("payload");char batchId[37];makeUuidV4(batchId);payload["batch_id"]=batchId;payload["device_id"]=deviceId;payload["session_id"]=sessionId;payload["schema_version"]="1";payload["firmware_version"]=FIRMWARE_VERSION;payload["processing_algorithm_version"]=PROCESSING_ALGORITHM_VERSION;payload["data_provenance"]="device_live";
  JsonArray records=payload.createNestedArray("samples"),events=payload.createNestedArray("acoustic_events");
  for(size_t i=0;i<count;i++){char sampleId[37];makeUuidV4(sampleId);uint32_t sequence; if(deviceStateMutex){xSemaphoreTake(deviceStateMutex,portMAX_DELAY);sequence=++deviceState.sampleSequence;xSemaphoreGive(deviceStateMutex);}else sequence=(uint32_t)i+1;JsonObject s=records.createNestedObject();s["sample_id"]=sampleId;s["sample_sequence"]=sequence;s["device_timestamp_ms"]=samples[i].timestampMs;s["raw_adc"]=samples[i].microphoneRaw;s["filtered"]=samples[i].filtered;s["acoustic_envelope"]=samples[i].envelope;s["noise_floor"]=samples[i].noiseFloor;s["threshold"]=samples[i].threshold;s["adc_baseline"]=samples[i].adcBaseline;s["sample_rate_hz"]=SENSOR_SAMPLE_RATE_HZ;const char* modes[]={"unknown","unpressurized_pump_stopped","pressurized_pump_stopped","active_deflation","pump_inflating"};s["calibration_mode"]=samples[i].calibrationMode<5?modes[samples[i].calibrationMode]:"unknown";s["calibrated"]=samples[i].calibrated;s["event_detected"]=samples[i].eventDetected;s["is_valid"]=!samples[i].adcClipped;JsonArray flags=s.createNestedArray("quality_flags");if(samples[i].adcClipped)flags.add("microphone_adc_clipped");float telemetryPressure=samples[i].eventDetected?samples[i].eventPressureMmhg:samples[i].pressureMmhg;bool pressureValid=samples[i].pressureReady&&isfinite(telemetryPressure)&&telemetryPressure>=0.0f&&telemetryPressure<=400.0f;if(pressureValid)s["pressure_mmhg"]=telemetryPressure;else flags.add("pressure_sample_unavailable");
    if(samples[i].eventDetected){char eventId[37];makeUuidV4(eventId);JsonObject e=events.createNestedObject();e["event_id"]=eventId;e["event_sequence"]=sequence;e["event_type"]="acoustic_candidate";e["device_timestamp_ms"]=samples[i].timestampMs;e["strength"]=samples[i].eventStrength;e["is_valid"]=true;JsonArray eventFlags=e.createNestedArray("quality_flags");JsonObject metadata=e.createNestedObject("metadata");metadata["pressure_calibrated"]=PRESSURE_CALIBRATION_CONFIGURED;metadata["duration_ms"]=samples[i].eventDurationMs;metadata["threshold"]=samples[i].threshold;metadata["noise_floor"]=samples[i].noiseFloor;metadata["sample_rate_hz"]=SENSOR_SAMPLE_RATE_HZ;if(isfinite(samples[i].eventPressureMmhg)){e["pressure_mmhg"]=samples[i].eventPressureMmhg;metadata["pressure_mmhg"]=samples[i].eventPressureMmhg;}else eventFlags.add("pressure_interpolation_unavailable");}
  }
  size_t bytes=serializeJson(doc,output,capacity);if(!bytes||bytes>=capacity){lost+=count;return 0;}return count;
}
void telemetryRecordPublishFailure(size_t n){lost+=(uint32_t)n;}
uint32_t telemetryLostSamples(){return lost+sensorDroppedCount();}
