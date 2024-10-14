/*
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301  USA
*/

#include "DaliBus.h"
#include "esp_system.h"
#include "rom/ets_sys.h"

void DaliBus_wrapper_pinchangeISR(void* arg) {
  DaliBusClass* bus = reinterpret_cast<DaliBusClass*>(arg);
  bus->pinchangeISR();
}


void DaliBus_wrapper_timerInterrupt(void* arg) {
  DaliBusClass* bus = reinterpret_cast<DaliBusClass*>(arg);
  bus->timerISR();
}

void DaliBusClass::begin(byte tx_pin, byte rx_pin, bool active_low) {
  txPin = tx_pin;
  rxPin = rx_pin;
  activeLow = active_low;

  timer = timerBegin(10000000);

  // init bus state
  busState = IDLE;

  // TX pin setup
  pinMode(txPin, OUTPUT);
  setBusLevel(HIGH);

  // RX pin setup
  pinMode(rxPin, INPUT);

  attachInterruptArg(digitalPinToInterrupt(rxPin), DaliBus_wrapper_pinchangeISR, this, CHANGE);

  timerAttachInterruptArg(timer, DaliBus_wrapper_timerInterrupt, this);
}

daliReturnValue DaliBusClass::sendRaw(const byte * message, uint8_t bits) {
  if(bits > 25) return DALI_INVALID_PARAMETER;
  if(bits != 25 && bits % 8 != 0) return DALI_INVALID_PARAMETER;
  uint8_t length = (bits - (bits % 8)) / 8;
  if(bits % 8 != 0) length++;
  if (busState != IDLE) return DALI_BUSY;

  // prepare variables for sending
  for (byte i = 0; i < length; i++)
    txMessage[i] = message[i];

  if(bits == 25) {
    txMessage[3] = (txMessage[2] & 1) << 7;
    txMessage[2] = (txMessage[2] >> 1) | 0b10000000;
  }

  txLength = bits;
  txCollision = 0;
  rxMessage = DALI_RX_EMPTY;
  rxLength = 0;

  // initiate transmission
  busState = TX_START_1ST;
  return DALI_SENT;
}

bool DaliBusClass::busIsIdle() {
  return (busState == IDLE);
}

int DaliBusClass::getLastResponse() {
  int response;
  switch (rxLength) {
    case 16:
      response = rxMessage;
      break;
    case 0:
      response = DALI_RX_EMPTY;
      break;
    default:
      response = DALI_RX_ERROR;
  }
  rxLength = 0;
  return response;
}

#if defined(ARDUINO_ARCH_RP2040)
void __time_critical_func(DaliBusClass::timerISR()) {
#elif defined(ARDUINO_ARCH_ESP32) || defined(ARDUINO_ARCH_ESP8266)
void IRAM_ATTR DaliBusClass::timerISR() {
#elif defined(ARDUINO_ARCH_AVR)
void DaliBusClass::timerISR() {
#endif
  if (busIdleCount < 0xff) // increment idle counter avoiding overflow
    busIdleCount++;

  if (busIdleCount == 4 && getBusLevel == LOW) { // bus is low idle for more than 2 TE, something's pulling down for too long
    busState = SHORT;
    setBusLevel(HIGH);
    if(errorCallback != 0)
      errorCallback(DALI_PULLDOWN);
  }

  // timer state machine
  switch (busState) {
    case TX_START_1ST: // initiate transmission by setting bus low (1st half)
      if (busIdleCount >= 26) { // wait at least 9.17ms (22 TE) settling time before sending (little more for TCI compatibility)
        setBusLevel(LOW);
        busState = TX_START_2ND;
      }
      break;
    case TX_START_2ND: // send start bit (2nd half)
      setBusLevel(HIGH);
      txPos = 0;
      busState = TX_BIT_1ST;
      break;
    case TX_BIT_1ST: // prepare bus for bit (1st half)
      if (txMessage[txPos >> 3] & 1 << (7 - (txPos & 0x7)))
      {
        setBusLevel(LOW);
      } else {
        setBusLevel(HIGH);
      }
      busState = TX_BIT_2ND;
      break;
    case TX_BIT_2ND: // send bit (2nd half)
      if (txMessage[txPos >> 3] & 1 << (7 - (txPos & 0x7)))
      {
        setBusLevel(HIGH);
      } else {
        setBusLevel(LOW);
      }
      txPos++;
      if (txPos < txLength)
        busState = TX_BIT_1ST;
      else
        busState = TX_STOP_1ST;
      break;
    case TX_STOP_1ST: // 1st stop bit (1st half)
      setBusLevel(HIGH);
      busState = TX_STOP;
      break;
    case TX_STOP: // remaining stop half-bits
      if (busIdleCount >= 4) {
        busState = WAIT_RX;
        busIdleCount = 0;
      }   
      break;
    case WAIT_RX: // wait 9.17ms (22 TE) for a response
      if (busIdleCount > 22)
        busState = IDLE; // response timed out
      break;
    case RX_STOP:
      if (busIdleCount > 4) {
        // rx message incl stop bits finished. 
        busState = IDLE;
      }
      break;
    case RX_START:
    case RX_BIT:
    if (busIdleCount > 3) // bus has been inactive for too long
    {
      busState = IDLE; // rx has been interrupted, bus is idle
      if (rxLength > 16)
      {
        if (receivedCallback != 0)
        {
          uint8_t bitlen = (rxLength - (rxLength % 2)) / 2;
          uint8_t *data = new uint8_t[3]; // Allocate 3 bytes for safety.

          if (bitlen == 25)
          {
            uint8_t temp = rxCommand & 0xFF;
            rxCommand = (rxCommand >> 1) & 0xFFFF;
            rxCommand |= temp;
          }

          // Extract bytes from rxCommand
          uint8_t offset = bitlen - 8; // Start with bitlen - 8 for the first byte

          // Extract the first byte (always available if bitlen >= 16)
          data[0] = (rxCommand >> offset) & 0xFF;

          // Decrease offset and extract the second byte if bitlen >= 16
          offset -= 8;
          if (bitlen >= 16)
          {
            data[1] = (rxCommand >> offset) & 0xFF;
          }
          else
          {
            data[1] = 0; // Clear the second byte if it's not present
          }

          // Decrease offset and extract the third byte if bitlen >= 24
          offset -= 8;
          if (bitlen >= 24)
          {
            data[2] = (rxCommand >> offset) & 0xFF;
          }
          else
          {
            data[2] = 0; // Clear the third byte if it's not present
          }

          // Call the callback with the data and bit length
          receivedCallback(data, bitlen);

          // Cleanup: If you don't free the memory in the callback, free it here
          delete[] data;
        }
      }
    }
    break;
  }
}

#if defined(ARDUINO_ARCH_RP2040)
void __not_in_flash_func(DaliBusClass::pinchangeISR)() {
#elif defined(ARDUINO_ARCH_ESP32) || defined(ARDUINO_ARCH_ESP8266)
void IRAM_ATTR DaliBusClass::pinchangeISR() {
#else
void DaliBusClass::pinchangeISR() {
#endif
  byte busLevel = getBusLevel; // TODO: do we have to check if level actually changed?
  busIdleCount = 0;           // reset idle counter so timer knows that something's happening

  if(busLevel != 0 && activityCallback != 0)
    activityCallback();

  if (busState <= TX_STOP) {          // check if we are transmitting
#ifndef DALI_NO_COLLISSION_CHECK
    if (busLevel != txBusLevel) { // check for collision
      txCollision = 1;	           // signal collision
      if(errorCallback != 0)
        errorCallback(DALI_COLLISION);

      timerRestart(timer);
      busState = IDLE;	               // stop transmission
    }
#endif
    return;                        // no collision, ignore pin change
  }

  // logical bus level changed -> store timings
  unsigned long tmp_ts = micros();
  unsigned long delta = tmp_ts - rxLastChange; // store delta since last change
  rxLastChange = tmp_ts;                       // store timestamp

  // rx state machine
  switch (busState) {
    case WAIT_RX:
      if (busLevel == LOW) { // start of rx frame
        //Timer1.restart();    // sync timer
        timerRestart(timer);
        busState = RX_START;
        rxIsResponse = true;
      } else {
        busState = IDLE; // bus can't actually be high, reset
        if(errorCallback != 0)
          errorCallback(DALI_CANT_BE_HIGH);
      }
      break;
    case RX_START:
      if (busLevel == HIGH && isDeltaWithinTE(delta)) { // validate start bit
        rxLength = 0; // clear old rx message
        rxMessage = 0;
        busState = RX_BIT;
      } else {                                   // invalid start bit -> reset bus state
        tempBusLevel = busLevel;
        tempDelta = delta;
        rxLength = DALI_RX_ERROR;
        busState = RX_STOP;
        if(errorCallback != 0)
          errorCallback(DALI_INVALID_STARTBIT);
      }
      break;
    case RX_BIT:
      if (isDeltaWithinTE(delta)) {              // check if change is within time of a half-bit
        if (rxLength % 2)                        // if rxLength is odd (= actual bit change)
        {
          if(rxIsResponse)
            rxMessage = rxMessage << 1 | busLevel;   // shift in received bit
          else
            rxCommand = rxCommand << 1 | busLevel;
        }
        rxLength++;
      } else if (isDeltaWithin2TE(delta)) {       // check if change is within time of two half-bits
        if(rxIsResponse)
          rxMessage = rxMessage << 1 | busLevel;   // shift in received bit
        else
          rxCommand = rxCommand << 1 | busLevel;
        rxLength += 2;
      } else {
        rxLength = DALI_RX_ERROR;
        busState = RX_STOP; // timing error -> reset state
        tempDelta = delta;
        if(errorCallback != 0)
          errorCallback(DALI_ERROR_TIMING);
      }
      if (rxIsResponse && rxLength == 16) // check if all 8 bits have been received
        busState = RX_STOP;
      break;
    case SHORT:
      if (busLevel == HIGH)
        busState = IDLE; // recover from bus error
      break;
    case IDLE:
      if(busLevel == LOW) {
        busState = RX_START;
        rxIsResponse = false;
      }
      break;  // ignore, we didn't expect rx
  }
}
