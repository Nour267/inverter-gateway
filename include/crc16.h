/* CRC-16/CCITT-FALSE (DESIGN.md §5.1): the "seal" that detects damaged frames.
 * poly 0x1021, init 0xFFFF.  Check value: crc16("123456789") = 0x29B1. */
#ifndef CRC16_H
#define CRC16_H

#include <stdint.h>
#include <stddef.h>

/* Calculate the CRC of len bytes starting at data.
 * In: bytes + their count.  Out: the 16-bit CRC (0xFFFF for 0 bytes). */
uint16_t crc16(const uint8_t *data, size_t len);

#endif /* CRC16_H */
