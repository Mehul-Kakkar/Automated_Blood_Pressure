#include "korotkoff_detector.h"

KorotkoffDetector detector;

void setup() {
    Serial.begin(115200);
    detector.reset();

    Serial.println("Korotkoff detector module initialized.");
}

void loop() {
    // Module compilation test only.
    delay(1000);
}