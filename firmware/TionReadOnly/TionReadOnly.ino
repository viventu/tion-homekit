#include <Arduino.h>
#include <Tion4SCore.h>
#include <tion4s/esp32_uart_service.h>

tion4s::Esp32UartService tion_uart;
bool uart_task_started = false;

void setup() {
  uart_task_started = tion_uart.begin();
}

void loop() {
  if (!uart_task_started) {
    tion_uart.service_fallback();
    tion_uart.supervise();
    delay(10);
  } else {
    tion_uart.supervise();
    delay(1000);
  }
}
