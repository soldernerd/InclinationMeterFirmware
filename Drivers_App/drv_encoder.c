#include "drv_encoder.h"
#include "hal_gpio.h"
#include "pin_config.h"

/* Full 4x quadrature decode: REV B's CubeMX config fires EXTI on both
 * edges of *both* A and B (Core/Src/gpio.c, GPIO_MODE_IT_RISING_FALLING
 * on all four pins) — unlike the REV A prototype, which only interrupted
 * on A and inferred direction from B's level. Whichever pin's edge fired,
 * we re-sample both A and B and look up the (old state, new state) pair
 * in the standard Gray-code transition table below. Signals are RC-
 * filtered + Schmitt-buffered CMOS (not inverted, unlike the ENC_1SW/
 * ENC_2SW push switches) — read directly, no polarity flip needed. */

typedef struct {
    GPIO_TypeDef *a_port;
    uint16_t      a_pin;
    GPIO_TypeDef *b_port;
    uint16_t      b_pin;
    volatile int32_t count;
    volatile uint8_t  ab_state;   /* last-seen (A<<1)|B, 2 bits */
} EncoderCtx;

static EncoderCtx s_enc[2];

/* Indexed by (old_state << 2) | new_state, each 2-bit state = (A<<1)|B.
 * +1 / -1 on the 8 valid single-step Gray-code transitions, 0 on a
 * repeated or torn (invalid double-step) transition. Sign convention
 * (CW = +1) is a naming choice, not yet confirmed against real hardware
 * rotation direction — swap the table's signs if bring-up shows it spins
 * backwards. */
static const int8_t QDEC_TABLE[16] = {
     0, -1,  1,  0,
     1,  0,  0, -1,
    -1,  0,  0,  1,
     0,  1, -1,  0,
};

static uint8_t read_ab_state(const EncoderCtx *e)
{
    uint8_t a = hal_gpio_get(e->a_port, e->a_pin) ? 1U : 0U;
    uint8_t b = hal_gpio_get(e->b_port, e->b_pin) ? 1U : 0U;
    return (uint8_t)((a << 1) | b);
}

static void encoder_update(EncoderInstance instance)
{
    EncoderCtx *e = &s_enc[instance];
    uint8_t new_state = read_ab_state(e);
    uint8_t idx = (uint8_t)((e->ab_state << 2) | new_state);
    e->count += QDEC_TABLE[idx];
    e->ab_state = new_state;
}

static void enc1_isr(void) { encoder_update(ENCODER_1); }
static void enc2_isr(void) { encoder_update(ENCODER_2); }

/* hal_gpio_exti_register() takes a 0-15 EXTI-line index, not a GPIO_PIN_x
 * bitmask; GPIO_PIN_x is one-hot (GPIO_PIN_0=0x0001 .. GPIO_PIN_15=0x8000),
 * so __builtin_ctz recovers the line index directly. */
static uint8_t exti_line_of(uint16_t gpio_pin)
{
    return (uint8_t)__builtin_ctz(gpio_pin);
}

/* s_enc[] has exactly one entry per EncoderInstance value; anything
 * outside that range is a caller bug, not a state this driver should
 * silently index past (no MPU on this MCU — an out-of-bounds static
 * array access is real undefined behavior, not a safe no-op). */
#define ENCODER_INSTANCE_COUNT  ((unsigned)(sizeof(s_enc) / sizeof(s_enc[0])))

DrvStatus drv_encoder_init(EncoderInstance instance)
{
    if ((unsigned)instance >= ENCODER_INSTANCE_COUNT) {
        return DRV_ERR_INVALID;
    }

    EncoderCtx *e = &s_enc[instance];

    if (instance == ENCODER_1) {
        e->a_port = ENC_1A_PORT; e->a_pin = ENC_1A_PIN;   /* PC4, EXTI4 */
        e->b_port = ENC_1B_PORT; e->b_pin = ENC_1B_PIN;   /* PB0, EXTI0 */
    } else {
        e->a_port = ENC_2A_PORT; e->a_pin = ENC_2A_PIN;   /* PB1, EXTI1 */
        e->b_port = ENC_2B_PORT; e->b_pin = ENC_2B_PIN;   /* PB2, EXTI2 */
    }

    /* Seed count/ab_state from the live pins BEFORE arming the EXTI
     * callbacks below. NVIC/EXTI for these lines is already globally
     * enabled by MX_GPIO_Init(), called well before this function runs
     * in main()'s boot sequence — registering the callback first would
     * let a real edge land while ab_state is still its zero-initialized
     * default, corrupting the very first decoded transition. */
    e->count    = 0;
    e->ab_state = read_ab_state(e);

    if (instance == ENCODER_1) {
        hal_gpio_exti_register(exti_line_of(ENC_1A_PIN), enc1_isr);
        hal_gpio_exti_register(exti_line_of(ENC_1B_PIN), enc1_isr);
    } else {
        hal_gpio_exti_register(exti_line_of(ENC_2A_PIN), enc2_isr);
        hal_gpio_exti_register(exti_line_of(ENC_2B_PIN), enc2_isr);
    }

    return DRV_OK;
}

int32_t drv_encoder_get_count(EncoderInstance instance)
{
    if ((unsigned)instance >= ENCODER_INSTANCE_COUNT) {
        return 0;
    }
    /* Single aligned 32-bit load — atomic on Cortex-M0+, no critical
     * section needed against the ISR's read-modify-write of the same
     * field (that read-modify-write can't itself be preempted, since
     * EXTI interrupts of the same priority don't nest). */
    return s_enc[instance].count;
}

void drv_encoder_reset(EncoderInstance instance)
{
    if ((unsigned)instance >= ENCODER_INSTANCE_COUNT) {
        return;
    }
    s_enc[instance].count = 0;   /* single aligned 32-bit store — atomic */
}
