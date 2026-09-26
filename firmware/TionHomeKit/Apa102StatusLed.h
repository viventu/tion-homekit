#pragma once

#include <Arduino.h>
#include <HomeSpan.h>

// T-Dongle S3 has one APA102 LED on GPIO40 (data) and GPIO39 (clock).
// HomeSpan owns its blink pattern; this adapter supplies a dim blue pixel.
// A steady red pixel marks a start failure while HomeSpan does not run.
class Apa102StatusLed final : public Blinkable {
 public:
  void begin() {
    pinMode(kDataPin, OUTPUT);
    pinMode(kClockPin, OUTPUT);
    off();
  }

  void on() override { write_pixel(0, 0, 32); }
  void off() override { write_pixel(0, 0, 0); }
  void fault() { write_pixel(32, 0, 0); }
  int getPin() override { return kDataPin; }

 private:
  static constexpr int kDataPin = 40;
  static constexpr int kClockPin = 39;

  void write_byte(std::uint8_t value) {
    for (int bit = 7; bit >= 0; --bit) {
      digitalWrite(kClockPin, LOW);
      digitalWrite(kDataPin, (value >> bit) & 1);
      digitalWrite(kClockPin, HIGH);
    }
  }

  void write_pixel(std::uint8_t red, std::uint8_t green, std::uint8_t blue) {
    for (int i = 0; i < 4; ++i) {
      write_byte(0);
    }
    write_byte(0xE1);  // APA102 global brightness: 1 of 31.
    write_byte(blue);
    write_byte(green);
    write_byte(red);
    for (int i = 0; i < 4; ++i) {
      write_byte(0xFF);
    }
    digitalWrite(kClockPin, LOW);
  }
};
