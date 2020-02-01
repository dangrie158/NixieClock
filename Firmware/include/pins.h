#ifndef __PINS_H__
#define __PINS_H__
// convert a chip and pin number to the corresponding bit
// in the serial output stream of the shift registers
// data is stored in host order (native)
#define P2B(CHIP, PIN) ((((CHIP)-1) * 8) + (PIN))

// latch pin of TPIC6B595 connected to D1
const uint8_t latchPin = 5;
// clock pin of TPIC6B595 connected to D2
const uint8_t clockPin = 4;
// data pin of TPIC6B595 connected to D3
const uint8_t dataPin = 0;

// number of HV shift registers on the serial data bus
const uint8_t numChips = 6;

// number of digit tubes in the clock
const uint8_t numDigits = 4;

#ifdef VARIANT
    #if VARIANT == 12
        #include "pins_in12.h"
    #elif VARIANT == 14
        #include "pins_IN14.h"
    #endif
#else
    #error You need to define the tube VARIANT (12 or 14)
#endif

#endif //__PINS_H__