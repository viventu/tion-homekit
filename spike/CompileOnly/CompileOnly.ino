#ifndef TION_COMPILE_SPIKE_ONLY
#error "This sketch is a compile-only probe and must not be built as operational firmware."
#endif

#include <Arduino.h>
#include <HardwareSerial.h>
#include <HomeSpan.h>

// This function checks API compatibility but is deliberately never called.
void compileOnlyBoundary() {
  HardwareSerial tionUart(1);
  tionUart.begin(9600, SERIAL_8N1, 19, 20);
  homeSpan.begin(Category::Fans, "Tion Compile Probe");
  new SpanAccessory();
  new Service::AccessoryInformation();
  new Characteristic::Identify();
  new Service::Fan();
  new Characteristic::Active();
  new Characteristic::RotationSpeed();
}

void setup() {}
void loop() {}
