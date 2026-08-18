#include "hal_uart.h"
#include "config.h"
#include "stm32g0xx_hal.h"
#include <string.h>

/* Two DMA ring-buffer UART instances, same engine, different peripheral:
 *   HAL_UART_BLE   - USART6 <-> RN4871 (WP5)
 *   HAL_UART_DEBUG - USART3 <-> STLINK VCP header, PD8/PD9 (WP5.1)
 * CubeMX must configure both identically: 115200 8N1, DMA RX CIRCULAR /
 * TX normal, NVIC global interrupt enabled. Generated handles live in
 * usart.c — extern below. No IDLE-line interrupt is armed on either
 * instance — see hal_uart_init()'s comment for why.
 *
 * ACTION NEEDED (as of this writing): Core/Src/usart.c currently has
 * hdma_usart3_rx.Init.Mode = DMA_NORMAL, not DMA_CIRCULAR like
 * hdma_usart6_rx — dma_write_pos() below assumes circular (continuously
 * self-reloading) reception on every instance. In Normal mode, once
 * RING_SIZE bytes have been received the DMA transfer completes and
 * stops (no HAL_UART_RxCpltCallback override restarts it here), so
 * reception on USART3 silently goes dead after the first RING_SIZE bytes
 * — this does NOT show up as a build or link error, only as receive
 * traffic quietly stopping. Fix in CubeMX: USART3's DMA settings ->
 * change USART3_RX from Normal to Circular mode, matching USART6_RX,
 * then regenerate. */
extern UART_HandleTypeDef huart6;
extern DMA_HandleTypeDef  hdma_usart6_rx;
extern UART_HandleTypeDef huart3;
extern DMA_HandleTypeDef  hdma_usart3_rx;

static UART_HandleTypeDef *const s_huart[HAL_UART_COUNT] = {
    [HAL_UART_BLE]   = &huart6,
    [HAL_UART_DEBUG] = &huart3,
};
static DMA_HandleTypeDef *const s_hdma_rx[HAL_UART_COUNT] = {
    [HAL_UART_BLE]   = &hdma_usart6_rx,
    [HAL_UART_DEBUG] = &hdma_usart3_rx,
};

#define RING_SIZE  HAL_UART_RX_RING_SIZE
#if (RING_SIZE & (RING_SIZE - 1)) != 0
#error "HAL_UART_RX_RING_SIZE must be a power of two"
#endif

/* DMA writes received bytes here in circular mode. The "head" is the
 * position the DMA controller will write next; we compute it from the
 * NDTR (number-of-data-to-transfer) register. The "tail" is the next
 * byte to be returned by hal_uart_read_byte — application-owned. */
static volatile uint8_t  s_rx_buf[HAL_UART_COUNT][RING_SIZE];
static volatile uint16_t s_rx_tail[HAL_UART_COUNT];

static volatile bool     s_tx_busy[HAL_UART_COUNT];

static inline uint16_t dma_write_pos(HalUartInstance instance)
{
    /* NDTR counts down from RING_SIZE to 1, then reloads. Position is
     * therefore (RING_SIZE - NDTR) & mask. */
    uint32_t ndtr = __HAL_DMA_GET_COUNTER(s_hdma_rx[instance]);
    return (uint16_t)((RING_SIZE - ndtr) & (RING_SIZE - 1U));
}

/* Instance -> array index for HAL_UART_TxCpltCallback/HAL_UART_ErrorCallback,
 * which only get a UART_HandleTypeDef* from the HAL, not a HalUartInstance.
 * HAL_UART_COUNT is small (2) so a linear scan is fine — these callbacks
 * aren't a hot path. Returns HAL_UART_COUNT (out of range) if huart matches
 * neither tracked instance — not reachable today (only huart3/huart6 exist
 * in this project, both tracked here), but harmless if a future USART
 * gets added to Core/Src/usart.c without a matching HalUartInstance. */
static HalUartInstance instance_of(const UART_HandleTypeDef *huart)
{
    for (HalUartInstance i = 0; i < HAL_UART_COUNT; ++i) {
        if (huart->Instance == s_huart[i]->Instance) {
            return i;
        }
    }
    return HAL_UART_COUNT;
}

bool hal_uart_init(HalUartInstance instance)
{
    if (instance >= HAL_UART_COUNT) return false;

    s_rx_tail[instance] = 0;
    s_tx_busy[instance] = false;

    /* Kick off circular DMA reception into the ring buffer. Reception is
     * consumed entirely via NDTR position polling (dma_write_pos()) — no
     * IDLE-line interrupt is armed. UART_IT_IDLE would need
     * HAL_UARTEx_ReceiveToIdle_DMA() (ReceptionType == HAL_UART_RECEPTION_TOIDLE)
     * to ever get cleared by the vendor HAL_UART_IRQHandler; with plain
     * HAL_UART_Receive_DMA() as used here, ReceptionType stays STANDARD and
     * the IRQ handler's IDLE-clearing branch is permanently skipped — an
     * enabled-but-never-cleared IDLE interrupt is a permanent NVIC
     * re-entry storm the moment the line goes idle. */
    return HAL_UART_Receive_DMA(s_huart[instance], (uint8_t *)s_rx_buf[instance], RING_SIZE) == HAL_OK;
}

DrvStatus hal_uart_write(HalUartInstance instance, const uint8_t *data, uint16_t len)
{
    if (instance >= HAL_UART_COUNT)  return DRV_ERR_INVALID;
    if (data == 0 || len == 0)       return DRV_ERR_INVALID;
    if (s_tx_busy[instance])         return DRV_ERR_NOT_READY;

    s_tx_busy[instance] = true;
    if (HAL_UART_Transmit_DMA(s_huart[instance], (uint8_t *)data, len) != HAL_OK) {
        s_tx_busy[instance] = false;
        return DRV_ERR_COMM;
    }
    return DRV_OK;
}

/* KNOWN LIMITATION: no overflow detection. The classic head==tail "empty"
 * test also can't distinguish "empty" from "completely full of unread
 * bytes" — if the DMA write position ever laps all the way around to
 * s_rx_tail (RING_SIZE bytes received since the last drain), this reports
 * empty and the whole buffer's contents are silently lost, same as if the
 * DMA had overwritten unread bytes mid-buffer. At 115200 baud that's
 * ~2.2ms/byte; RING_SIZE (256, see config.h) can be exceeded within one
 * scheduler tick under sustained throughput. Accepted rather than fixed
 * here: real traffic on either instance is normally well under raw UART
 * line rate, and each transport's poll period is a tunable EEPROM
 * setting if this proves to matter on real hardware — same
 * proportionality call as the other documented WP5 gaps. */
bool hal_uart_read_byte(HalUartInstance instance, uint8_t *byte)
{
    if (instance >= HAL_UART_COUNT || byte == 0) return false;
    uint16_t head = dma_write_pos(instance);
    if (head == s_rx_tail[instance]) {
        return false;
    }
    *byte = s_rx_buf[instance][s_rx_tail[instance]];
    s_rx_tail[instance] = (uint16_t)((s_rx_tail[instance] + 1U) & (RING_SIZE - 1U));
    return true;
}

uint16_t hal_uart_bytes_available(HalUartInstance instance)
{
    if (instance >= HAL_UART_COUNT) return 0;
    uint16_t head = dma_write_pos(instance);
    return (uint16_t)((head - s_rx_tail[instance]) & (RING_SIZE - 1U));
}

bool hal_uart_is_busy(HalUartInstance instance)
{
    if (instance >= HAL_UART_COUNT) return false;
    return s_tx_busy[instance];
}

/* HAL weak override — fires when DMA TX completes, for whichever
 * instance's peripheral just finished. */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    HalUartInstance i = instance_of(huart);
    if (i < HAL_UART_COUNT) {
        s_tx_busy[i] = false;
    }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    HalUartInstance i = instance_of(huart);
    if (i >= HAL_UART_COUNT) return;

    s_tx_busy[i] = false;
    /* Restart RX DMA if it stopped due to an overrun/framing error. Any
     * bytes already in the ring but not yet drained by the consuming
     * task are lost here (s_rx_tail snaps to the fresh DMA position) —
     * accepted, undiagnosed data loss on an already-rare error path;
     * HAL_App has no system_state escalation point (only App/Services
     * write g_system_state — see CLAUDE.md 8.1), and a genuine restart
     * failure below means the RX pipe is dead regardless, degrading to
     * "no more bytes arrive" rather than a crash. */
    if (HAL_UART_Receive_DMA(huart, (uint8_t *)s_rx_buf[i], RING_SIZE) == HAL_OK) {
        s_rx_tail[i] = dma_write_pos(i);
    }
}
