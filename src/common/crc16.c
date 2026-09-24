/* CRC-16/CCITT-FALSE (DESIGN.md §5.1). Pure logic, no I/O. */
#include "crc16.h"

#define CRC16_POLY  0x1021
#define CRC16_INIT  0xFFFF

uint16_t crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = CRC16_INIT;

    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)(data[i] << 8);           /* mix the byte into the top of crc */

        for (int bit = 0; bit < 8; bit++) {         /* process its 8 bits */
            if (crc & 0x8000) {                     /* top bit is 1 */
                crc = (uint16_t)((crc << 1) ^ CRC16_POLY);
            } else {                                /* top bit is 0 */
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}
