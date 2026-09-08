/* Host tests for Math/math_crc.c — CRC-16/CCITT-FALSE
 * (poly 0x1021, init 0xFFFF, no reflect, no xor-out). */
#include "test.h"
#include <string.h>

#include "../Math/math_crc.c"   /* single-TU test: pull the impl in directly */

TEST(ccitt_false_standard_check_vector)
{
    /* The canonical CRC-16/CCITT-FALSE check value for the ASCII string
     * "123456789" is 0x29B1 (catalogue of parametrised CRC algorithms). */
    CHECK_EQ(math_crc16((const uint8_t *)"123456789", 9), 0x29B1u);
}

TEST(empty_and_single_byte)
{
    CHECK_EQ(math_crc16((const uint8_t *)"", 0), 0xFFFFu);   /* init value, untouched */
    CHECK_EQ(math_crc16(0, 0), 0xFFFFu);                     /* NULL + 0 len is defined */
    CHECK_EQ(math_crc16((const uint8_t *)"\x00", 1), 0xE1F0u);
    CHECK_EQ(math_crc16((const uint8_t *)"A", 1), 0xB915u);
}

TEST(order_matters)
{
    CHECK(math_crc16((const uint8_t *)"AB", 2) != math_crc16((const uint8_t *)"BA", 2));
}

TEST(appending_a_byte_changes_crc)
{
    uint8_t buf[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    CHECK(math_crc16(buf, 7) != math_crc16(buf, 8));
}

TEST(matches_the_ads131m04_frame_crc_use)
{
    /* Drivers_App/drv_ads131m04.c computes the frame CRC over bytes 0..14
     * (5 words x 3 bytes) and compares to the ADS's own CRC word. This is
     * a real (device0.9.42 bench) frame + its trailing CRC word: */
    static const uint8_t frame15[15] = {
        0x01, 0x0F, 0x00,   /* word0: STATUS response */
        0x00, 0x00, 0x00,   /* ch0 */
        0x7F, 0xFF, 0xC0,   /* ch1 */
        0x80, 0x00, 0x40,   /* ch2 */
        0x00, 0x00, 0x00,   /* ch3 */
    };
    /* pinned; cross-checked against tests/oracle_crc.py (0x103B). */
    CHECK_EQ(math_crc16(frame15, 15), 0x103Bu);
}

TEST(table_build_is_idempotent)
{
    /* First call builds the lookup table lazily; a second call must give
     * the same answer (the ISR-safety pre-build in drv_ads131m04_init()
     * relies on this being stable). */
    uint16_t a = math_crc16((const uint8_t *)"deadbeef", 8);
    uint16_t b = math_crc16((const uint8_t *)"deadbeef", 8);
    CHECK_EQ(a, b);
}

int main(void)
{
    printf("test_math_crc:\n");
    RUN(ccitt_false_standard_check_vector);
    RUN(empty_and_single_byte);
    RUN(order_matters);
    RUN(appending_a_byte_changes_crc);
    RUN(matches_the_ads131m04_frame_crc_use);
    RUN(table_build_is_idempotent);
    return test_summary();
}
