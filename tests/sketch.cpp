// The sketch as a host translation unit. Arduino compiles TionHomeKit.ino as
// C++; host tests include it unchanged and build it against the doubles in
// tests/stubs (HomeSpan, the Arduino core, ESP-IDF calls and the ESP32-S3
// UART adapter).
#include "TionHomeKit.ino"
