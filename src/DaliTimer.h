#pragma once

#include <cstdint>
#include <functional>

#include <Arduino.h>

#if defined(ARDUINO_ARCH_RP2040)
#include "RPI_PICO_Timer.h"
#elif defined(ARDUINO_ARCH_ESP32)
#include "TimerInterrupt_Generic.h"
#elif defined(ARDUINO_ARCH_ESP8266)
#include "ESP8266Timer.h"
#elif defined(ARDUINO_ARCH_AVR)
#include "ITimer1.h"
#include "ITimer2.h"
#include "ITimer3.h"
#endif

class DaliTimer {
public:
  #if defined(ARDUINO_ARCH_RP2040)
  RPI_PICO_Timer timer;
  #elif defined(ARDUINO_ARCH_ESP32)
  ESP32Timer timer;
  #elif defined(ARDUINO_ARCH_ESP8266)
  ESP8266Timer timer;
  #elif defined(ARDUINO_ARCH_AVR)
  #if DALI_TIMER==1
  ITimer1 timer;
  #elif DALI_TIMER==2
  ITimer2 timer;
  #else
  ITimer3 timer;
  #endif
  #endif

  DaliTimer(int timerId);

  void attachInterrupt(uint32_t interval, const std::function<void()>& isr);

  void restartTimer();
};
