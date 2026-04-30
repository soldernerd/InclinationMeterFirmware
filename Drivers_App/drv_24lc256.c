#include "drv_24lc256.h"
#include "hal_i2c.h"
#include "hal_systick.h"
#include <string.h>

/* The 24LC256 expects:
 *   read  : write [addr_hi, addr_lo] then read N
 *   write : write [addr_hi, addr_lo, data...] up to one 64-byte page
 * After a write, the internal cycle takes up to 5 ms during which the
 * device NACKs all transactions. We poll IsDeviceReady to detect end. */

/* Internal write buffer holds 2-byte address + up to one page of data. */
static uint8_t s_tx_buf[2U + EEPROM_PAGE_SIZE];

typedef enum {
    OP_IDLE = 0,
    OP_DMA_READ,
    OP_DMA_WRITE,
    OP_WRITE_CYCLE_POLL,
} EepromOp;

static volatile EepromOp s_op             = OP_IDLE;
static volatile bool     s_dma_done       = false;
static volatile bool     s_dma_success    = false;
static volatile bool     s_read_complete  = false;
static uint32_t          s_poll_started_ms = 0;

static void on_dma(HalI2cInstance instance, bool success)
{
    if (instance != HAL_I2C_MAIN) return;
    s_dma_success = success;
    s_dma_done    = true;
}

void drv_24lc256_init(void)
{
    hal_i2c_register_dma_callback(HAL_I2C_MAIN, on_dma);
    s_op             = OP_IDLE;
    s_dma_done       = false;
    s_read_complete  = false;
}

DrvStatus drv_24lc256_start_read(uint16_t addr, uint8_t *buf, uint16_t len)
{
    if (s_op != OP_IDLE)               return DRV_ERR_NOT_READY;
    if (buf == 0 || len == 0)          return DRV_ERR_INVALID;
    if (addr + len > EEPROM_TOTAL_BYTES) return DRV_ERR_INVALID;

    /* Phase 1: write 2-byte address (blocking — only 2 bytes) */
    s_tx_buf[0] = (uint8_t)(addr >> 8);
    s_tx_buf[1] = (uint8_t)(addr & 0xFFU);
    if (!hal_i2c_write(HAL_I2C_MAIN, EEPROM_I2C_ADDR, s_tx_buf, 2U)) {
        return DRV_ERR_COMM;
    }

    /* Phase 2: DMA read */
    s_dma_done       = false;
    s_dma_success    = false;
    s_read_complete  = false;
    s_op             = OP_DMA_READ;
    hal_i2c_read_dma(HAL_I2C_MAIN, EEPROM_I2C_ADDR, buf, len);
    return DRV_OK;
}

bool drv_24lc256_read_complete(void)
{
    bool done = s_read_complete;
    if (done) {
        s_read_complete = false;
    }
    return done;
}

DrvStatus drv_24lc256_start_write_page(uint16_t addr,
                                       const uint8_t *buf,
                                       uint16_t len)
{
    if (s_op != OP_IDLE)                          return DRV_ERR_NOT_READY;
    if (buf == 0 || len == 0)                     return DRV_ERR_INVALID;
    if (len > EEPROM_PAGE_SIZE)                   return DRV_ERR_INVALID;
    if (((uint32_t)addr + len) > EEPROM_TOTAL_BYTES) return DRV_ERR_INVALID;
    /* Page-boundary check: a write that spans pages wraps within a page
     * on the device, so reject those */
    if (((addr & (EEPROM_PAGE_SIZE - 1U)) + len) > EEPROM_PAGE_SIZE) {
        return DRV_ERR_INVALID;
    }

    s_tx_buf[0] = (uint8_t)(addr >> 8);
    s_tx_buf[1] = (uint8_t)(addr & 0xFFU);
    memcpy(&s_tx_buf[2], buf, len);

    s_dma_done    = false;
    s_dma_success = false;
    s_op          = OP_DMA_WRITE;
    hal_i2c_write_dma(HAL_I2C_MAIN, EEPROM_I2C_ADDR, s_tx_buf, (uint16_t)(2U + len));
    return DRV_OK;
}

bool drv_24lc256_is_busy(void)
{
    return s_op != OP_IDLE;
}

void drv_24lc256_update(void)
{
    switch (s_op) {
        case OP_IDLE:
            break;

        case OP_DMA_READ:
            if (s_dma_done) {
                s_op            = OP_IDLE;
                s_read_complete = s_dma_success;
            }
            break;

        case OP_DMA_WRITE:
            if (s_dma_done) {
                /* Address bytes + data have been clocked out. EEPROM now
                 * runs an internal write cycle (≤ 5 ms) during which it
                 * NACKs all transactions. Poll until it ACKs again. */
                s_op             = OP_WRITE_CYCLE_POLL;
                s_poll_started_ms = hal_systick_get_ms();
            }
            break;

        case OP_WRITE_CYCLE_POLL:
            if (hal_i2c_device_ready(HAL_I2C_MAIN, EEPROM_I2C_ADDR)) {
                s_op = OP_IDLE;
            } else if ((uint32_t)(hal_systick_get_ms() - s_poll_started_ms) > 50U) {
                /* Stuck — give up rather than wedging the scheduler */
                s_op = OP_IDLE;
            }
            break;

        default:
            s_op = OP_IDLE;
            break;
    }
}
