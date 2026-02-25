#pragma once

#include <obs-module.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RECAST_SERVICE_ID "recast_rtmp_service"

/* Register the custom service type with OBS (called from plugin-main.c) */
void recast_service_register(void);

#ifdef __cplusplus
}
#endif
