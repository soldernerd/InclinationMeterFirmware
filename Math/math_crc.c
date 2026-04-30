#include "math_crc.h"

/* CRC16-CCITT (polynomial 0x1021, initial value 0xFFFF, no reflection,
 * no final XOR). Table-driven for speed; table built once at first use. */

#define CRC_POLY     0x1021U
#define CRC_INIT     0xFFFFU
#define TABLE_SIZE   256

static uint16_t s_table[TABLE_SIZE];
static uint8_t  s_table_built = 0;

static void build_table(void)
{
    for (uint16_t i = 0; i < TABLE_SIZE; ++i) {
        uint16_t crc = (uint16_t)(i << 8);
        for (uint8_t b = 0; b < 8; ++b) {
            if (crc & 0x8000U) {
                crc = (uint16_t)((crc << 1) ^ CRC_POLY);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
        s_table[i] = crc;
    }
    s_table_built = 1;
}

uint16_t math_crc16(const uint8_t *data, uint16_t len)
{
    if (!s_table_built) {
        build_table();
    }
    uint16_t crc = CRC_INIT;
    if (data == 0) {
        return crc;
    }
    for (uint16_t i = 0; i < len; ++i) {
        uint8_t idx = (uint8_t)((crc >> 8) ^ data[i]);
        crc = (uint16_t)((crc << 8) ^ s_table[idx]);
    }
    return crc;
}
