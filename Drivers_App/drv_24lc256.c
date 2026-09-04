#include "drv_24lc256.h"
#include "hal_i2c.h"
#include "hal_systick.h"
#include <string.h>

/* The 24LC256 expects:
 *   read  : write [addr_hi, addr_lo] then read N
 *   write : write [addr_hi, addr_lo, data...] up to one 64-byte page
 * After a write, the internal cycle takes up to 5 ms during which the
 * device NACKs all transactions. We poll IsDeviceReady to detect end. */

/* Internal write buffer holds 2-byte address + up to one page of data.
 * Also reused to hold the 2-byte address ahead of a DMA read. */
static uint8_t s_tx_buf[2U + EEPROM_PAGE_SIZE];

typedef enum {
    OP_IDLE = 0,
    OP_DMA_WRITE_ADDR,     /* writing the 2-byte address before a DMA read */
    OP_DMA_READ,
    OP_DMA_WRITE,
    OP_WRITE_CYCLE_POLL,
} eeprom_op_t;

static volatile eeprom_op_t s_op              = OP_IDLE;
static volatile bool        s_dma_done        = false;
static volatile bool        s_dma_success     = false;
static volatile bool        s_read_complete   = false;
static volatile bool        s_write_success   = false;
static volatile uint8_t     s_last_write_fail = 0U;   /* see drv_24lc256_last_write_fail() */
static uint8_t              *s_read_buf       = 0;
static uint16_t              s_read_len       = 0;
static uint32_t              s_poll_started_ms = 0;

static void on_dma(HalI2cInstance instance, bool success)
{
    if (instance != HAL_I2C_MAIN) return;
    s_dma_success = success;
    s_dma_done    = true;
}

void drv_24lc256_init(void)
{
    hal_i2c_register_dma_callback(HAL_I2C_MAIN, on_dma);
    s_op            = OP_IDLE;
    s_dma_done      = false;
    s_read_complete = false;
}

DrvStatus drv_24lc256_start_read(uint16_t addr, uint8_t *buf, uint16_t len)
{
    if (s_op != OP_IDLE)               return DRV_ERR_NOT_READY;
    if (buf == 0 || len == 0)          return DRV_ERR_INVALID;
    if (addr + len > EEPROM_TOTAL_BYTES) return DRV_ERR_INVALID;

    s_tx_buf[0] = (uint8_t)(addr >> 8);
    s_tx_buf[1] = (uint8_t)(addr & 0xFFU);
    s_read_buf  = buf;
    s_read_len  = len;

    s_dma_done      = false;
    s_dma_success   = false;
    s_read_complete = false;
    s_op            = OP_DMA_WRITE_ADDR;
    hal_i2c_write_dma(HAL_I2C_MAIN, EEPROM_I2C_ADDR, s_tx_buf, 2U);
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

    s_dma_done       = false;
    s_dma_success    = false;
    s_write_success  = false;
    s_last_write_fail = 0U;
    s_op             = OP_DMA_WRITE;
    hal_i2c_write_dma(HAL_I2C_MAIN, EEPROM_I2C_ADDR, s_tx_buf, (uint16_t)(2U + len));
    return DRV_OK;
}

bool drv_24lc256_write_complete(void)
{
    /* Only meaningful once !drv_24lc256_is_busy() confirms the write has
     * actually finished — same contract as drv_24lc256_read_complete(). */
    return s_write_success;
}

uint8_t drv_24lc256_last_write_fail(void)
{
    return s_last_write_fail;
}

bool drv_24lc256_is_busy(void)
{
    return s_op != OP_IDLE;
}

void drv_24lc256_abort(void)
{
    hal_i2c_abort(HAL_I2C_MAIN, EEPROM_I2C_ADDR);
    s_op            = OP_IDLE;
    s_dma_done      = false;
    s_dma_success   = false;
    s_read_buf      = 0;
    s_read_len      = 0;
    s_write_success = false;
}

void drv_24lc256_update(void)
{
    switch (s_op) {
        case OP_IDLE:
            break;

        case OP_DMA_WRITE_ADDR:
            if (s_dma_done) {
                if (s_dma_success) {
                    s_dma_done    = false;
                    s_dma_success = false;
                    s_op          = OP_DMA_READ;
                    hal_i2c_read_dma(HAL_I2C_MAIN, EEPROM_I2C_ADDR, s_read_buf, s_read_len);
                } else {
                    /* Address phase failed — s_read_complete stays at the
                     * false set in start_read(), correctly signalling
                     * failure to the next drv_24lc256_read_complete() call. */
                    s_op = OP_IDLE;
                }
            }
            break;

        case OP_DMA_READ:
            if (s_dma_done) {
                s_op            = OP_IDLE;
                s_read_complete = s_dma_success;
            }
            break;

        case OP_DMA_WRITE:
            if (s_dma_done) {
                if (s_dma_success) {
                    /* Address bytes + data have been clocked out. EEPROM now
                     * runs an internal write cycle (<= 5 ms) during which it
                     * NACKs all transactions. Poll until it ACKs again. */
                    s_op              = OP_WRITE_CYCLE_POLL;
                    s_poll_started_ms = hal_systick_get_ms();
                } else {
                    s_write_success   = false;
                    s_last_write_fail = 1U;   /* DMA transfer NAK'd */
                    s_op              = OP_IDLE;
                }
            }
            break;

        case OP_WRITE_CYCLE_POLL:
            if (hal_i2c_device_ready(HAL_I2C_MAIN, EEPROM_I2C_ADDR)) {
                s_write_success = true;
                s_op            = OP_IDLE;
            } else if (hal_systick_elapsed_ms(s_poll_started_ms) > 50U) {
                /* Stuck — give up rather than wedging the scheduler */
                s_write_success   = false;
                s_last_write_fail = 2U;   /* ACK-poll after write cycle timed out */
                s_op              = OP_IDLE;
            }
            break;

        default:
            s_op = OP_IDLE;
            break;
    }
}
