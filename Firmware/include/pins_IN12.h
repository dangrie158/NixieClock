#ifndef __PINS_IN12_H__
#define __PINS_IN12_H__

#include "pins.h"

// the mapping of [digit][number to display] to bitnumber in the serial output stream
const uint8_t digitsPinmap[numDigits][10]{
    {P2B(1, 5), P2B(1, 3), P2B(1, 2), P2B(1, 1), P2B(1, 0), P2B(1, 7), P2B(1, 6), P2B(2, 6), P2B(2, 5), P2B(2, 4)},
    {P2B(3, 4), P2B(2, 3), P2B(2, 2), P2B(2, 1), P2B(2, 7), P2B(2, 0), P2B(3, 0), P2B(3, 1), P2B(3, 2), P2B(3, 3)},
    {P2B(5, 7), P2B(4, 4), P2B(4, 3), P2B(4, 5), P2B(4, 2), P2B(4, 6), P2B(4, 1), P2B(4, 7), P2B(4, 0), P2B(5, 1)},
    {P2B(6, 0), P2B(5, 4), P2B(5, 3), P2B(5, 5), P2B(5, 6), P2B(5, 2), P2B(6, 5), P2B(6, 3), P2B(6, 2), P2B(6, 1)}};

// mapping of [led number] to bit in the serial output stream
const uint8_t ledsPinmap[2] = {P2B(3, 6), P2B(3, 7)};

// mapping of [number digit dot] to bit in the serial output stream
const uint8_t dotsPinmap[numDigits] = {P2B(1, 4), P2B(3, 5), P2B(5, 0), P2B(6, 4)};

// bitmask in the serial stream for the LEDs digits
const uint64_t ledsMask = (1ull << ledsPinmap[0]) | (1ull << ledsPinmap[1]);

// bitmask in the serial stream for the dots digits
const uint64_t dotsMask = (1ull << dotsPinmap[0]) | (1ull << dotsPinmap[1]) | (1ull << dotsPinmap[2]) | (1ull << dotsPinmap[3]);

// bitmask in the serial stream for all digits
const uint64_t digitsMask = ~(ledsMask | dotsMask);

#endif //__PINS_IN12_H__