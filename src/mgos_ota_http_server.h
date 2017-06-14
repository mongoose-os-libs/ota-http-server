/*
 * Copyright (c) 2014-2016 Cesanta Software Limited
 * All rights reserved
 */

#ifndef CS_MOS_LIBS_OTA_HTTP_SERVER_SRC_MGOS_OTA_HTTP_SERVER_H_
#define CS_MOS_LIBS_OTA_HTTP_SERVER_SRC_MGOS_OTA_HTTP_SERVER_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#if MGOS_ENABLE_UPDATER
bool mgos_ota_http_server_init(void);
#endif

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* CS_MOS_LIBS_OTA_HTTP_SERVER_SRC_MGOS_OTA_HTTP_SERVER_H_ */
