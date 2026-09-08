#include "drv_ads131m04.h"
#include "hal_spi.h"
#include "hal_tim.h"
#include "hal_gpio.h"
#include "hal_systick.h"
#include "pin_config.h"
#include "config.h"
#include "math_crc.h"
#include "stm32g0xx_hal.h"   /* __disable_irq / __enable_irq */

/* Register addresses used here (datasheet Table 8-12, "Register Map"). */
#define REG_ID      0x00U
#define REG_STATUS  0x01U
#define REG_MODE    0x02U
#define REG_CLOCK   0x03U
#define REG_GAIN1   0x04U
#define REG_CFG     0x06U

/* WREG command word: 011a aaaa annn nnnn -- 011 prefix, 6-bit address,
 * 7-bit (count-1). This driver only ever writes one register at a time
 * (count-1 = 0), so the low 7 bits are always 0. */
#define WREG_CMD(addr)  (uint16_t)(0x6000U | (((addr) & 0x3FU) << 7))

/* RREG command word: 101a aaaa annn nnnn -- 101 prefix, same addr/count
 * fields. The register contents come back in word 0 of the NEXT frame. */
#define RREG_CMD(addr)  (uint16_t)(0xA000U | (((addr) & 0x3FU) << 7))

/* CLOCK register (datasheet Table 8-17): CH3_EN..CH0_EN=1 (all four
 * channels), TBM=0, OSR[2:0]=ADS131M04_OSR_FIELD (config.h — 000b = 128,
 * chosen for fDATA = fCLKIN/256), PWR[1:0]=10b (high-resolution mode,
 * required for fCLKIN=5.3333 MHz -- see the datasheet's Recommended
 * Operating Conditions, high-resolution mode's fCLKIN range is
 * 0.3-8.4 MHz). Reset default is 0F0Eh; this changes only the OSR field. */
#define CLOCK_REG_VALUE  (uint16_t)(0x0F00U | (ADS131M04_OSR_FIELD << 2) | 0x2U)

/* GAIN1 register (datasheet Table 8-18): PGAGAIN0..3[2:0] = 000b (gain
 * = 1) for all four channels -- this is already the POR/reset default
 * (0000h), written explicitly anyway for clarity and to not silently
 * depend on the reset value never changing. */
#define GAIN1_REG_VALUE  0x0000U

/* MODE register (datasheet Table 8-16): same as the 0510h reset default
 * except RESET (bit 10) cleared to 0 -- our own SYNC_RESET pulse below
 * sets this flag, and it must be explicitly cleared (writing 0 is the
 * documented way to clear the STATUS register's RESET bit). WLENGTH
 * stays at its default 01b (24-bit words, matching the ADC's native
 * resolution -- see the frame-buffer sizing below). */
#define MODE_REG_VALUE   0x0110U

/* On-the-wire frame size with WLENGTH=24-bit (default): 6 words x 3
 * bytes = [response][ch0][ch1][ch2][ch3][crc]. Datasheet "SPI
 * Communication Frames" -- fixed length regardless of command, the
 * device always shifts out its full response. */
#define FRAME_BYTES  18U
#define WORD_BYTES   3U

static Ads131m04SampleCb s_on_sample = 0;
/* s_running: written in task context (start/stop/init), read in the TIM7
 * ISR and the SysTick drain. s_xfer_active: written in the TIM7 ISR, also
 * read by drv_ads131m04_stop()'s bounded spin-wait in task context. Both
 * volatile so the compiler can't hoist those cross-context reads (this
 * file is built -O2 in every config — CMakeLists.txt). */
static volatile bool     s_running = false;
static volatile bool     s_xfer_active = false;
static Ads131m04Regs     s_regs;

static const uint8_t s_tx_zero[FRAME_BYTES] = { 0 };   /* NULL command, no CRC */

/* --- Frame ring (docs/adc_acquisition_redesign.md, Phase 1) ---
 * Zero-copy: the SPI1 RX DMA writes each frame straight into the next
 * ring slot; on_trigger() (TIM7 ISR) just advances head and re-arms the
 * DMA at the following slot. drain_ring() (SysTick) does the sign-extend
 * + per-sample callback. SPSC: head is written only by the TIM7 ISR, tail
 * only by SysTick; 16-bit aligned index loads/stores are atomic on M0+,
 * so no locking. FRAME_RING_FRAMES is a power of two -> mask, not modulo. */
#define FRAME_RING_MASK  (ADC_FRAME_RING_FRAMES - 1U)
static uint8_t           s_ring[ADC_FRAME_RING_FRAMES][FRAME_BYTES];
static volatile uint16_t s_ring_head;   /* produced count (free-running) */
static volatile uint16_t s_ring_tail;   /* drained count  (free-running) */

static Ads131m04Integrity s_integ;
static uint32_t           s_start_ms;      /* hal_systick_get_ms() at start() */
static uint32_t           s_settle_ms;     /* run_ms when the deficit reference was taken */
static uint32_t           s_settle_frames; /* frames_produced at that point */
static bool               s_settled;
static uint16_t           s_deficit_hold;  /* ms the frame deficit has been past the limit */

/* Latch an integrity fault (first one wins). Called from on_trigger (TIM7
 * ISR, priority 0) and drain_ring (SysTick, lowest) — a plain byte store
 * is atomic on M0+, so fault_code never tears. The check-then-set is a
 * cross-ISR read-modify-write, though: TIM7 can preempt drain_ring
 * between its check and its store, so in the rare window where both
 * contexts fault in the same instant the *cause* reported may be the
 * second one, not the first. Acceptable — any fault stops acquisition
 * and gets logged; which of two simultaneous causes wins doesn't change
 * the response. */
static void integ_fault(Ads131m04Fault code)
{
    if (s_integ.fault_code == ADS_FAULT_NONE) {
        s_integ.fault_code = (uint8_t)code;
    }
}

/* Blocking, TX-only, single-register write -- used only during the
 * one-time init sequence below, never during streaming. Ignores
 * whatever the ADC shifts back on DOUT (settling/meaningless data
 * before OSR and gain are configured) -- acceptable for a boot-only
 * register write, unlike the streaming path which must capture every
 * response. Always sends a full FRAME_BYTES-sized frame: the device's
 * own output shift register needs that many clocks regardless of how
 * short the actual command is, per the datasheet's frame-length
 * description. */
static DrvStatus write_register(uint8_t addr, uint16_t value)
{
    uint8_t buf[FRAME_BYTES] = { 0 };
    uint16_t cmd = WREG_CMD(addr);

    buf[0] = (uint8_t)(cmd >> 8);
    buf[1] = (uint8_t)(cmd & 0xFFU);
    /* buf[2] = 0 -- 24-bit word LSB padding */
    buf[3] = (uint8_t)(value >> 8);
    buf[4] = (uint8_t)(value & 0xFFU);
    /* buf[5] = 0 -- 24-bit word LSB padding; remaining bytes already 0 */

    hal_spi_cs_assert(HAL_SPI_ADC);
    DrvStatus rc = hal_spi_write(HAL_SPI_ADC, buf, FRAME_BYTES);
    hal_spi_cs_deassert(HAL_SPI_ADC);
    return rc;
}

/* Blocking single-register read. RREG in one frame, then a NULL frame to
 * clock the response out of word 0. Init/diagnostic use only. The 16-bit
 * register value sits in the top 16 bits of the 24-bit response word. */
static DrvStatus read_register(uint8_t addr, uint16_t *value)
{
    uint8_t tx[FRAME_BYTES] = { 0 };
    uint8_t rx[FRAME_BYTES] = { 0 };
    uint16_t cmd = RREG_CMD(addr);

    tx[0] = (uint8_t)(cmd >> 8);
    tx[1] = (uint8_t)(cmd & 0xFFU);

    hal_spi_cs_assert(HAL_SPI_ADC);
    DrvStatus rc = hal_spi_transmit_receive(HAL_SPI_ADC, tx, rx, FRAME_BYTES);
    hal_spi_cs_deassert(HAL_SPI_ADC);
    if (rc != DRV_OK) return rc;

    uint8_t nul[FRAME_BYTES] = { 0 };
    uint8_t resp[FRAME_BYTES] = { 0 };
    hal_spi_cs_assert(HAL_SPI_ADC);
    rc = hal_spi_transmit_receive(HAL_SPI_ADC, nul, resp, FRAME_BYTES);
    hal_spi_cs_deassert(HAL_SPI_ADC);
    if (rc != DRV_OK) return rc;

    *value = (uint16_t)(((uint16_t)resp[0] << 8) | resp[1]);
    return DRV_OK;
}

static void read_all_registers(void)
{
    s_regs.clock_expected = CLOCK_REG_VALUE;
    bool ok = true;
    ok &= (read_register(REG_ID,     &s_regs.id)     == DRV_OK);
    ok &= (read_register(REG_STATUS, &s_regs.status) == DRV_OK);
    ok &= (read_register(REG_MODE,   &s_regs.mode)   == DRV_OK);
    ok &= (read_register(REG_CLOCK,  &s_regs.clock)  == DRV_OK);
    ok &= (read_register(REG_GAIN1,  &s_regs.gain1)  == DRV_OK);
    ok &= (read_register(REG_CFG,    &s_regs.cfg)    == DRV_OK);
    s_regs.read_ok = ok;
}

const Ads131m04Regs *drv_ads131m04_get_regs(void)
{
    return &s_regs;
}

static int32_t sign_extend24(uint8_t msb, uint8_t mid, uint8_t lsb)
{
    uint32_t v = ((uint32_t)msb << 16) | ((uint32_t)mid << 8) | lsb;
    if (v & 0x00800000UL) {
        v |= 0xFF000000UL;   /* sign-extend bit 23 through bit 31 */
    }
    return (int32_t)v;
}

/* Frame-ring drain — process every frame the TIM7 ISR has queued. Called
 * from SysTick (1 kHz, see Core/Src/stm32g0xx_it.c) and, to flush the
 * tail, from drv_ads131m04_stop() with interrupts masked. A periodic tick
 * (not a re-pending soft IRQ) can't starve thread mode: ~21 frames/tick
 * at fDATA, ~40 us of work per 1 ms. Phase 1: sign-extend + per-sample
 * callback (DFT MAC / bulk store), plus record word0 / CRC bytes per run
 * for API read-back so Phase 2's integrity gate is built on confirmed
 * on-the-wire behaviour. */
static void drain_ring(void)
{
    uint16_t tail = s_ring_tail;
    uint16_t head = s_ring_head;                 /* volatile: snapshot once */

    /* Hard bound: never process more than one ring's worth per call, no
     * matter what the indices say. A larger gap means the producer lapped
     * us (already counted as ring_overflow) or an index was seen torn —
     * clamp rather than spin. */
    uint16_t gap = (uint16_t)(head - tail);
    if (gap > ADC_FRAME_RING_FRAMES) {
        if (gap > s_integ.drain_clamp_max) {
            s_integ.drain_clamp_max = gap;
        }
        s_integ.drain_clamped++;
        tail = (uint16_t)(head - ADC_FRAME_RING_FRAMES);
    }

    while (tail != head) {
        const uint8_t *f = s_ring[tail & FRAME_RING_MASK];

        /* Framing: word 0 is the ADS131M04 STATUS response for a no-command
         * frame — a fixed value. Anything else == byte-misalignment or a
         * device resync/reset. */
        uint16_t w0 = (uint16_t)((f[0] << 8) | f[1]);
        s_integ.word0_last = w0;
        if (w0 != ADS131M04_STATUS_WORD) {
            s_integ.framing_err++;
            integ_fault(ADS_FAULT_FRAMING);
        }

        /* CRC: the ADS appends CRC-16/CCITT (poly 0x1021, init 0xFFFF, no
         * reflect/xor-out) over words 0..4 (bytes 0..14), left-justified in
         * word 5. Byte range + polynomial bench-confirmed fw 0.9.18 —
         * crc_calc matched crc_rx every frame over a clean run. */
        uint16_t crc_rx   = (uint16_t)((f[5 * WORD_BYTES] << 8) | f[5 * WORD_BYTES + 1]);
        uint16_t crc_calc = math_crc16(f, 5U * WORD_BYTES);
        s_integ.crc_rx_last   = crc_rx;
        s_integ.crc_calc_last = crc_calc;
        if (crc_calc != crc_rx) {
            s_integ.crc_err++;
            integ_fault(ADS_FAULT_CRC);
        }

        if (s_on_sample != 0) {
            int32_t ch0 = sign_extend24(f[1 * WORD_BYTES], f[1 * WORD_BYTES + 1], f[1 * WORD_BYTES + 2]);
            int32_t ch1 = sign_extend24(f[2 * WORD_BYTES], f[2 * WORD_BYTES + 1], f[2 * WORD_BYTES + 2]);
            int32_t ch2 = sign_extend24(f[3 * WORD_BYTES], f[3 * WORD_BYTES + 1], f[3 * WORD_BYTES + 2]);
            int32_t ch3 = sign_extend24(f[4 * WORD_BYTES], f[4 * WORD_BYTES + 1], f[4 * WORD_BYTES + 2]);
            s_on_sample(ch0, ch1, ch2, ch3);
        }

        tail++;
        s_integ.frames_drained++;
    }
    s_ring_tail = tail;
}

/* Called from Core/Src/stm32g0xx_it.c's SysTick_Handler, once per ms. */
void drv_ads131m04_drain_tick(void)
{
    if (!s_running) {
        return;
    }
    drain_ring();

    /* Conversion-count integrity, referenced to the SysTick clock.
     * fDATA = SYSCLK / ADS131M04_FDATA_TIMER_TICKS and SysTick shares the
     * SYSCLK root, so over any interval the expected frame count is
     * elapsed_ms * ADC_FDATA_KHZ_NUM / ADC_FDATA_KHZ_DEN with no drift.
     * (tim7_fires is NOT the reference — the trigger ISR misses ~20 ppm
     * of fires under load without any sample being lost: bench fw 0.9.24.)
     *
     * The reference point is taken once, after ADC_SLIP_SETTLE_MS, so any
     * pipeline-startup offset is excluded and this measures pure drift.
     * deficit = expected_since_settle - frames_since_settle. A real lost
     * or duplicated conversion is a permanent +/-1 step; measured noise is
     * a few frames. Latch if it sits past the limit for
     * ADC_FRAME_DEFICIT_HOLD_MS (a transient SysTick stall recovers). */
    /* Once a fault has latched the producer is stopped; keeping the
     * deficit maths running just inflates frame_deficit into a large
     * meaningless number in the diagnostic (integ_fault() below is
     * already a no-op). Freeze the readout where the fault left it. */
    if (s_integ.fault_code != ADS_FAULT_NONE) {
        return;
    }

    uint32_t run_ms = hal_systick_get_ms() - s_start_ms;
    s_integ.run_ms = run_ms;
    if (run_ms < ADC_SLIP_SETTLE_MS) {
        return;
    }
    if (!s_settled) {
        s_settled       = true;
        s_settle_ms     = run_ms;
        s_settle_frames = s_integ.frames_produced;
        return;
    }

    uint32_t elapsed  = run_ms - s_settle_ms;
    uint32_t expected = (uint32_t)((uint64_t)elapsed * ADC_FDATA_KHZ_NUM
                                   / ADC_FDATA_KHZ_DEN);
    int32_t deficit = (int32_t)expected
                    - (int32_t)(s_integ.frames_produced - s_settle_frames);
    s_integ.frame_deficit = deficit;
    if (deficit > s_integ.frame_deficit_max) s_integ.frame_deficit_max = deficit;
    if (deficit < s_integ.frame_deficit_min) s_integ.frame_deficit_min = deficit;

    if (deficit >= ADC_FRAME_DEFICIT_LIMIT || deficit <= -ADC_FRAME_DEFICIT_LIMIT) {
        if (s_deficit_hold < ADC_FRAME_DEFICIT_HOLD_MS) {
            s_deficit_hold++;
        } else {
            integ_fault(ADS_FAULT_SLIP);
        }
    } else {
        s_deficit_hold = 0U;
    }
}

/* DRDY poll + raw-DMA frame read -- called from TIM7's lean ISR at 2x the
 * ADC data rate (config.h ADS131M04_TRIGGER_TIMER_PERIOD). DRDY is polled
 * as a level, not an edge (PA1/PB1 EXTI1 conflicts with the encoder --
 * pin_config.h's ADC_READY_PIN comment); in MODE register DRDY_FMT=0 it
 * stays low from the end of a conversion until the frame is read, so
 * "DRDY low" == "an unread conversion is waiting".
 *
 * State machine, one step per fire:
 *   - a frame in flight and finished  -> commit it
 *   - a frame in flight, not finished -> nothing to do this fire
 *   - idle and DRDY low               -> arm a new frame (zero-copy DMA
 *                                        straight into the next ring slot)
 * Fires ADC_TRIGGER_OVERSAMPLE x per conversion so a poll always lands
 * inside a conversion period. On an integrity fault the whole thing goes
 * quiet (no more arms) until stop()/start(). */
static void on_trigger(void)
{
    if (!s_running) {
        return;
    }
    s_integ.tim7_fires++;

    if (s_xfer_active) {
        if (!hal_spi_adc_stream_done()) {
            return;                       /* still shifting */
        }
        hal_spi_adc_stream_end();
        hal_spi_cs_deassert(HAL_SPI_ADC);
        s_xfer_active = false;

        /* The DMA has already written this frame into s_ring[head]. Commit
         * it by advancing head — unless the drain is a whole ring behind,
         * in which case leave head put (drop-newest) and fault. */
        uint16_t head = s_ring_head;
        if ((uint16_t)(head - s_ring_tail) >= ADC_FRAME_RING_FRAMES) {
            s_integ.ring_overflow++;
            integ_fault(ADS_FAULT_OVERRUN);
        } else {
            s_ring_head = head + 1U;
            s_integ.frames_produced++;
            /* Conversion-count integrity is checked against the SysTick
             * clock in drv_ads131m04_drain_tick(), not here — tim7_fires
             * is not a reliable conversion reference. */
        }
    }

    if (s_integ.fault_code != ADS_FAULT_NONE) {
        return;                           /* latched — acquisition halted */
    }

    if (hal_gpio_get(ADC_READY_PORT, ADC_READY_PIN)) {
        return;                           /* DRDY high -- nothing new */
    }

    hal_spi_cs_assert(HAL_SPI_ADC);
    hal_spi_adc_stream_begin(s_tx_zero, s_ring[s_ring_head & FRAME_RING_MASK],
                             FRAME_BYTES);
    s_xfer_active = true;
}

bool drv_ads131m04_faulted(void)
{
    return s_integ.fault_code != ADS_FAULT_NONE;
}

DrvStatus drv_ads131m04_init(void)
{
    s_on_sample     = 0;
    s_running       = false;
    s_xfer_active   = false;

    hal_spi_init(HAL_SPI_ADC);

    /* MCLK first -- the ADC's internal logic (including SYNC/RESET
     * handling below) is synchronous to it, same reasoning as the DAC's
     * drv_ad9833_init(). */
    hal_tim_adc_clock_start();

    /* Explicit hardware reset via SYNC/RESET rather than relying solely
     * on POR: guarantees a known starting state regardless of whatever
     * happened before this boot. Idle HIGH (see pin_config.h); pulse LOW
     * for >=2048 CLKIN cycles (datasheet t_w(RSL), ~384 us at our 5.3333
     * MHz CLKIN) -- 1 ms is a generous, simple margin for a one-time
     * boot operation, not a tight budget worth hand-timing. */
    hal_gpio_set(ADC_SYNC_RESET_PORT, ADC_SYNC_RESET_PIN, false);
    hal_systick_delay_ms(1);
    hal_gpio_set(ADC_SYNC_RESET_PORT, ADC_SYNC_RESET_PIN, true);
    /* t_REGACQ (5 us min) before communicating, per the datasheet's
     * SYNC/RESET Pin section -- same 1 ms margin reasoning as above. */
    hal_systick_delay_ms(1);

    if (write_register(REG_CLOCK, CLOCK_REG_VALUE) != DRV_OK) return DRV_ERR_COMM;
    if (write_register(REG_GAIN1, GAIN1_REG_VALUE) != DRV_OK) return DRV_ERR_COMM;
    if (write_register(REG_MODE,  MODE_REG_VALUE)  != DRV_OK) return DRV_ERR_COMM;

    /* Read the config back so a host can see whether the WREGs actually
     * landed (drv_ads131m04_get_regs()). */
    read_all_registers();

    /* Hand SPI1 + its DMA channels over to the raw streaming path (no more
     * blocking/HAL-SPI calls on this bus after this point). */
    hal_spi_adc_stream_init();

    /* Build the CRC-16 table now (task context) so the first per-frame
     * CRC in the SysTick drain doesn't pay for it. */
    (void)math_crc16(0, 0);

    /* The frame drain runs from SysTick (Core/Src/stm32g0xx_it.c) — a
     * periodic 1 kHz tick that always preempts thread mode but, being
     * periodic rather than a self-re-pending soft IRQ, cannot starve it.
     * SysTick keeps its default priority (lowest, below every peripheral
     * IRQ). No extra wiring here. */

    /* Trigger callback is registered here but the timer is left stopped —
     * drv_ads131m04_start() arms it. See the header comment. */
    hal_tim_adc_trigger_register_callback(on_trigger);

    return DRV_OK;
}

static void ring_reset(void)
{
    s_ring_head     = 0U;
    s_ring_tail     = 0U;
    s_deficit_hold  = 0U;
    s_settled       = false;
    for (uint32_t i = 0; i < sizeof s_integ; ++i) {
        ((uint8_t *)&s_integ)[i] = 0U;
    }
}

DrvStatus drv_ads131m04_start(void)
{
    if (s_running) {
        return DRV_OK;
    }
    s_xfer_active   = false;
    ring_reset();
    s_start_ms = hal_systick_get_ms();
    s_running = true;
    hal_tim_adc_trigger_start();
    return DRV_OK;
}

void drv_ads131m04_stop(void)
{
    if (!s_running) {
        return;
    }
    hal_tim_adc_trigger_stop();
    s_running = false;
    /* Let any frame kicked from on_trigger() drain before we park CS —
     * bounded wait, task context only. A raw frame is ~10 us on the wire. */
    for (uint32_t i = 0; i < 100000U && s_xfer_active && !hal_spi_adc_stream_done(); ++i) {
        __asm volatile("nop");
    }
    if (s_xfer_active) {
        hal_spi_adc_stream_end();
        s_xfer_active = false;
    }
    hal_spi_cs_deassert(HAL_SPI_ADC);

    /* Flush the tail with SysTick masked so its drain can't race this one
     * (the trigger is already stopped, so head is stable). */
    __disable_irq();
    drain_ring();
    __enable_irq();
}

bool drv_ads131m04_is_running(void)
{
    return s_running;
}

void drv_ads131m04_set_on_sample(Ads131m04SampleCb cb)
{
    s_on_sample = cb;
}

const Ads131m04Integrity *drv_ads131m04_get_integrity(void)
{
    return &s_integ;
}
