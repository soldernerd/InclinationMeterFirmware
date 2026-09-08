/* Host tests for the pure fixed-point transfer / decode functions that
 * were extracted out of the hardware-coupled drivers:
 *   drv_tmp236_mv_to_cdeg()   (Drivers_App/drv_tmp236.h)
 *   drv_lm35_mv_to_cdeg()     (Drivers_App/drv_lm35.h)
 *   drv_encoder_quad_step()   (Drivers_App/drv_encoder.h)
 */
#include "test.h"

#include "config.h"
#include "drv_tmp236.h"
#include "drv_lm35.h"
#include "drv_encoder.h"

/* the cal struct the firmware builds from the EEPROM defaults */
static const tmp236_cal_t TMP236_DEFAULTS = {
    .seg_boundary_mv = DEFAULT_TMP236_SEG_BOUNDARY_MV,
    .seg1_voffs_mv   = DEFAULT_TMP236_SEG1_VOFFS_MV,
    .seg1_num        = DEFAULT_TMP236_SEG1_NUM,
    .seg1_den        = DEFAULT_TMP236_SEG1_DEN,
    .seg2_voffs_mv   = DEFAULT_TMP236_SEG2_VOFFS_MV,
    .seg2_num        = DEFAULT_TMP236_SEG2_NUM,
    .seg2_den        = DEFAULT_TMP236_SEG2_DEN,
    .seg2_tinfl_cdeg = DEFAULT_TMP236_SEG2_TINFL_CDEG,
};

TEST(tmp236_datasheet_reference_points)
{
    const tmp236_cal_t *c = &TMP236_DEFAULTS;
    /* datasheet: 400 mV = 0 C, 2350 mV = 100 C (segment boundary) */
    CHECK_EQ(drv_tmp236_mv_to_cdeg(400u, c),    0);
    CHECK_EQ(drv_tmp236_mv_to_cdeg(2350u, c),   10000);
}

TEST(tmp236_segments_are_continuous_at_the_boundary)
{
    const tmp236_cal_t *c = &TMP236_DEFAULTS;
    /* 2350 -> segment 1 (v_mv <= boundary); the segment-2 formula
     * evaluated at 2350 gives (0)*num/den + 10000 == 10000 too. */
    int32_t seg1_at_boundary = drv_tmp236_mv_to_cdeg(2350u, c);
    int32_t seg2_formula_at_boundary =
        ((int32_t)2350 - (int32_t)c->seg2_voffs_mv) * (int32_t)c->seg2_num
        / (int32_t)c->seg2_den + (int32_t)c->seg2_tinfl_cdeg;
    CHECK_EQ(seg1_at_boundary, seg2_formula_at_boundary);
    CHECK_EQ(seg1_at_boundary, 10000);
    /* one LSB past the boundary is segment 2 */
    CHECK_EQ(drv_tmp236_mv_to_cdeg(2351u, c), 10005);
}

TEST(tmp236_midrange_and_negative)
{
    const tmp236_cal_t *c = &TMP236_DEFAULTS;
    /* (1200-400)*200/39 = 160000/39 = 4102 (trunc) */
    CHECK_EQ(drv_tmp236_mv_to_cdeg(1200u, c), 4102);
    /* below 400 mV -> negative C. C integer division truncates toward 0:
     * (200-400)*200/39 = -40000/39 = -1025 */
    CHECK_EQ(drv_tmp236_mv_to_cdeg(200u, c), -1025);
    CHECK_EQ(drv_tmp236_mv_to_cdeg(100u, c), -1538);
}

TEST(tmp236_high_end)
{
    const tmp236_cal_t *c = &TMP236_DEFAULTS;
    /* ~125 C region: (2842-2350)*1000/197 + 10000 = 492000/197 + 10000
     *  = 2497 + 10000 = 12497 */
    CHECK_EQ(drv_tmp236_mv_to_cdeg(2842u, c), 12497);
}

TEST(lm35_reference_points)
{
    CHECK_EQ(drv_lm35_mv_to_cdeg(0u,   DEFAULT_LM35_SCALE_MV_PER_C),   0);
    CHECK_EQ(drv_lm35_mv_to_cdeg(250u, DEFAULT_LM35_SCALE_MV_PER_C),   2500);   /* 25.00 C */
    CHECK_EQ(drv_lm35_mv_to_cdeg(1000u, DEFAULT_LM35_SCALE_MV_PER_C),  10000);  /* 100 C  */
    CHECK_EQ(drv_lm35_mv_to_cdeg(333u, DEFAULT_LM35_SCALE_MV_PER_C),   3330);
}

TEST(lm35_calibrated_scale)
{
    /* a non-nominal scale (e.g. bench-trimmed to 11 mV/C): 250*100/11 */
    CHECK_EQ(drv_lm35_mv_to_cdeg(250u, 11), 2272);
}

TEST(encoder_valid_gray_transitions)
{
    /* the 8 single-bit-change transitions: +1 one way, -1 the other */
    CHECK_EQ(drv_encoder_quad_step(0x0, 0x2), +1);
    CHECK_EQ(drv_encoder_quad_step(0x2, 0x3), +1);
    CHECK_EQ(drv_encoder_quad_step(0x3, 0x1), +1);
    CHECK_EQ(drv_encoder_quad_step(0x1, 0x0), +1);

    CHECK_EQ(drv_encoder_quad_step(0x0, 0x1), -1);
    CHECK_EQ(drv_encoder_quad_step(0x1, 0x3), -1);
    CHECK_EQ(drv_encoder_quad_step(0x3, 0x2), -1);
    CHECK_EQ(drv_encoder_quad_step(0x2, 0x0), -1);
}

TEST(encoder_no_change_and_double_step_are_zero)
{
    for (uint8_t s = 0; s < 4; ++s) {
        CHECK_EQ(drv_encoder_quad_step(s, s), 0);          /* repeat */
    }
    CHECK_EQ(drv_encoder_quad_step(0x0, 0x3), 0);          /* both bits flipped */
    CHECK_EQ(drv_encoder_quad_step(0x1, 0x2), 0);
    CHECK_EQ(drv_encoder_quad_step(0x2, 0x1), 0);
    CHECK_EQ(drv_encoder_quad_step(0x3, 0x0), 0);
}

TEST(encoder_full_detent_sums_to_plus_or_minus_four)
{
    /* one mechanical detent = 4 quadrature steps in one direction */
    int cw = drv_encoder_quad_step(0x0, 0x2) + drv_encoder_quad_step(0x2, 0x3)
           + drv_encoder_quad_step(0x3, 0x1) + drv_encoder_quad_step(0x1, 0x0);
    int ccw = drv_encoder_quad_step(0x0, 0x1) + drv_encoder_quad_step(0x1, 0x3)
            + drv_encoder_quad_step(0x3, 0x2) + drv_encoder_quad_step(0x2, 0x0);
    CHECK_EQ(cw, +4);
    CHECK_EQ(ccw, -4);
}

int main(void)
{
    printf("test_transfer:\n");
    RUN(tmp236_datasheet_reference_points);
    RUN(tmp236_segments_are_continuous_at_the_boundary);
    RUN(tmp236_midrange_and_negative);
    RUN(tmp236_high_end);
    RUN(lm35_reference_points);
    RUN(lm35_calibrated_scale);
    RUN(encoder_valid_gray_transitions);
    RUN(encoder_no_change_and_double_step_are_zero);
    RUN(encoder_full_detent_sums_to_plus_or_minus_four);
    return test_summary();
}
