#include "DaliTimer.h"

DaliTimer::DaliTimer(int timerId) : timer(timerId) {}

void DaliTimer::attachInterrupt(uint32_t interval, const std::function<void()>& isr) {
  #if defined(ARDUINO_ARCH_RP2040)
  timer.attachInterrupt(interval, [isr](repeating_timer *t) -> bool {
    isr();
    return true;
  });
  #elif defined(ARDUINO_ARCH_ESP32) || defined(ARDUINO_ARCH_ESP8266)
  timer.attachInterrupt(interval, [isr](void * timer) -> bool {
    isr();
    return true;
  });
  #elif defined(ARDUINO_ARCH_AVR)
  timer.init();
  timer.attachInterrupt(interval, [isr](unsigned int outputPin) {
    isr();
  });
  #endif
}

void DaliTimer::restartTimer() {
  timer.restartTimer();
}