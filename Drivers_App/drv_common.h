#ifndef DRV_COMMON_H
#define DRV_COMMON_H

typedef enum {
    DRV_OK = 0,
    DRV_ERR_TIMEOUT,
    DRV_ERR_COMM,
    DRV_ERR_INVALID,
    DRV_ERR_NOT_READY,
} DrvStatus;

#endif /* DRV_COMMON_H */
