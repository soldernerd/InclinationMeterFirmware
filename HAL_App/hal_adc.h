#ifndef HAL_ADC_H
#define HAL_ADC_H

#include <stdint.h>
#include <stdbool.h>

/* ADC1's fixed sequencer always converts CH3, CH13(VREFINT), CH15, CH16 in
 * that ascending-channel-number order — see hal_adc.c. */
typedef struct {
    uint16_t vbat_raw;      /* CH3  / PA3  / BATTERY_SENSE */
    uint16_t vrefint_raw;   /* CH13 / internal reference */
    uint16_t temp_ext_raw;  /* CH15 / PB11 / TEMP_SENSE_EXT */
    uint16_t temp_raw;      /* CH16 / PB12 / TEMP_SENSE */
    bool     valid;         /* true after first DMA scan completes */
} adc_results_t;

bool           hal_adc_init(void);       /* false if ADC calibration failed */
void           hal_adc_start(void);      /* trigger one DMA scan */
bool           hal_adc_is_ready(void);   /* true when DMA scan complete */

/* Returns a coherent snapshot of the latest scan, atomic with respect to
 * HAL_ADC_ConvCpltCallback (briefly disables interrupts while copying).
 * By-value, not a pointer to live storage — a caller that read a pointer's
 * fields at two different times could otherwise tear a read across two
 * different scans (e.g. one field from an old scan, another from a scan
 * that completed via ISR in between). */
adc_results_t  hal_adc_get_results(void);

/* Converts a raw 12-bit-equivalent ADC code into millivolts, ratiometric
 * against VREFINT's factory calibration. REV B ties VREF+ directly to the
 * 3V3_STANDBY rail rather than a fixed-voltage internal VREFBUF, so VDDA
 * can't be assumed constant — this compensates for the actual supply
 * voltage using a VREFINT reading. Both arguments must come from the SAME
 * hal_adc_get_results() snapshot (that's the whole point — mixing a
 * channel raw value with a VREFINT reading from a different scan silently
 * produces a plausible-but-wrong result). Returns 0 if vrefint_raw is 0
 * (no valid scan yet), avoiding a divide-by-zero. */
uint32_t       hal_adc_raw_to_mv(uint16_t channel_raw, uint16_t vrefint_raw);

uint16_t          hal_adc_read_vbat_raw(void);
uint16_t          hal_adc_read_temp_raw(void);
uint16_t          hal_adc_read_temp_ext_raw(void);
uint16_t          hal_adc_read_vrefint_raw(void);

#endif /* HAL_ADC_H */
