// Packaging guard: the portable part of Tion4SCore must build on any ESP32
// target, without the ESP32-S3 UART adapter and its pin constraints.
#include <Tion4SCore.h>

void setup() {
  std::uint8_t frame[tion4s::kRequestFrameSize];
  tion4s::encode_request(tion4s::FrameType::HeartbeatRequest, frame);
}

void loop() {}
