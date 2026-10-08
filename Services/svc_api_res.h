#ifndef SVC_API_RES_H
#define SVC_API_RES_H

/* Internal to the API resource handlers (svc_api_res_*.c). */

#include "svc_api.h"
#include "svc_api_defs.h"

bool api_bulk_active(void);               /* svc_api_res_stream.c */
void api_zero_cal_apply_if_ready(void);   /* svc_api_res_cmd.c: stores a finished flip calibration */

#endif /* SVC_API_RES_H */
