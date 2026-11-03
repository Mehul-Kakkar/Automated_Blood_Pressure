#pragma once
#include <Arduino.h>
#include <esp_system.h>
inline void makeUuidV4(char out[37]){uint32_t a=esp_random(),b=esp_random(),c=esp_random(),d=esp_random();snprintf(out,37,"%08lx-%04lx-4%03lx-%04lx-%08lx%04lx",(unsigned long)a,(unsigned long)(b>>16),(unsigned long)(c&0xfff),(unsigned long)((d>>16&0x3fff)|0x8000),(unsigned long)d,(unsigned long)(c>>16));}
