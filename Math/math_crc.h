#ifndef MATH_CRC_H
#define MATH_CRC_H

#include <stdint.h>

/* CRC16-CCITT (poly 0x1021, init 0xFFFF, no reflection, no XOR-out) */
uint16_t math_crc16(const uint8_t *data, uint16_t len);

#endif /* MATH_CRC_H */
