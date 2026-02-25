#pragma once

#include <obs-module.h>
#include "recast-protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- OBS registration (called from plugin-main.c) ---- */
void recast_output_register(void);

#ifdef __cplusplus
}
#endif
