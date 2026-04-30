#ifndef SVC_STORAGE_H
#define SVC_STORAGE_H

#include <stdint.h>
#include "drv_common.h"

DrvStatus svc_storage_init(void);
DrvStatus svc_storage_load_settings(void);
DrvStatus svc_storage_save_settings(void);

#endif /* SVC_STORAGE_H */
