/* CRC-16/CCITT-FALSE (DESIGN.md §5.1): the "seal" that detects damaged frames.
 * poly 0x1021, init 0xFFFF.  Check value: crc16("123456789") = 0x29B1. */
#ifndef CRC16_H
#define CRC16_H

#include <stdint.h>
#include <stddef.h>

/* Goal: calculate the CRC "seal" of some bytes, so the receiver can detect damaged data.
 * In:   data = pointer to the bytes, len = how many bytes
 * Out:  the 16-bit CRC, e.g. "123456789" -> 0x29B1 (0 bytes -> 0xFFFF) */
uint16_t crc16(const uint8_t *data, size_t len);

#endif /* CRC16_H */
